#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <wrl/client.h>
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
void require(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
template <class T, class = void> struct HasOffscreen : std::false_type {};
template <class T>
struct HasOffscreen<T, std::void_t<decltype(std::declval<T &>().attach_offscreen())>> : std::true_type {};

// This reader uses an independent hardware device, exactly as an OBS consumer does.
struct OutputReader {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    explicit OutputReader(ID3D11Device *writer) {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        require(SUCCEEDED(writer->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
                    SUCCEEDED(dxgi->GetAdapter(&adapter)),
                "offscreen writer adapter");
        require(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &device, nullptr, &context)),
                "independent output reader device");
    }
    void open(std::uint64_t handle) {
        mutex.Reset();
        texture.Reset();
        require(handle &&
                    SUCCEEDED(device->OpenSharedResource(reinterpret_cast<HANDLE>(handle),
                                                         IID_PPV_ARGS(&texture))) &&
                    SUCCEEDED(texture.As(&mutex)),
                "open keyed offscreen output");
    }
    std::vector<std::uint32_t> pixels(unsigned width, unsigned height) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        require(desc.Width == width && desc.Height == height, "offscreen output dimensions");
        require(desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS, "OBS-compatible RGBA output");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "reader staging texture");
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "reader GPU map");
        std::vector<std::uint32_t> result(std::size_t(width) * height);
        for (unsigned y = 0; y < height; ++y)
            std::memcpy(result.data() + std::size_t(y) * width,
                        static_cast<const char *>(mapped.pData) + y * mapped.RowPitch, width * 4);
        context->Unmap(staging.Get(), 0);
        return result;
    }
};

void abandon_output(OutputReader &reader, std::uint64_t handle) {
    HRESULT acquired = E_FAIL;
    // The owning thread exits without returning the GPU reader key, then its
    // reader device is destroyed. Thread exit alone does not abandon a D3D11
    // mutex because ownership belongs to the device. No GPU TDR is requested.
    std::thread owner([&] { acquired = reader.mutex->AcquireSync(1, 0); });
    owner.join();
    require(acquired == S_OK, "abandonment fixture acquires the published output");
    OutputReader probe(reader.device.Get());
    probe.open(handle);
    reader.mutex.Reset();
    reader.texture.Reset();
    reader.context.Reset();
    reader.device.Reset();
    const HRESULT abandoned = probe.mutex->AcquireSync(0, 0);
    require(abandoned == WAIT_ABANDONED, "destroyed owner must abandon the native output mutex");
}

template <class T> void offscreen_tests() {
    if constexpr (!HasOffscreen<T>::value) {
        require(false, "renderer must support offscreen attachment without an HWND");
    } else {
        T renderer;
        require(renderer.attach_offscreen(), "offscreen initialize without creating a window");
        Options options;
        options.output_width = 640;
        options.output_height = 360;
        options.crop = 0;
        options.stabilize = false;
        require(renderer.draw(options), "offscreen black waiting draw");
        OutputReader reader(renderer.device());
        reader.open(renderer.output_handle());
        const auto first_generation = renderer.output_generation();
        require(first_generation != 0, "published output has a generation");
        require(reader.mutex->AcquireSync(1, 0) == S_OK, "waiting frame published on reader key");
        const auto waiting = reader.pixels(640, 360);
        require(std::all_of(waiting.begin(), waiting.end(), [](auto p) { return p == 0xff000000u; }),
                "offscreen waiting image is opaque black");
        const auto begin = std::chrono::steady_clock::now();
        require(!renderer.draw(options), "reader ownership prevents overwriting output");
        require(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100),
                "busy output draw must never wait for the reader");
        require(reader.pixels(640, 360) == waiting, "busy draw preserves the held image");
        require(SUCCEEDED(reader.mutex->ReleaseSync(0)), "reader returns writer key");
        require(renderer.demo(0) && renderer.draw(options), "offscreen quad demo draw");
        require(renderer.output_generation() == first_generation, "ordinary draws retain generation");
        require(reader.mutex->AcquireSync(1, 0) == S_OK, "demo published after reader releases");
        const auto demo = reader.pixels(640, 360);
        const auto center = demo[180 * 640 + 320];
        // Linear demo RGB (32,208,32) is sRGB-encoded by the output target.
        require(std::abs(int(center & 255) - 99) <= 2 && std::abs(int((center >> 8) & 255) - 233) <= 2 &&
                    std::abs(int((center >> 16) & 255) - 99) <= 2,
                "shared output preserves quad focus colors");

        options.output_width = 800;
        options.output_height = 450;
        require(renderer.draw(options), "resize can replace output while reader holds old image");
        require(renderer.output_generation() != first_generation, "resize changes output generation");
        require(reader.pixels(640, 360) == demo, "resize preserves reader-owned old texture");
        require(SUCCEEDED(reader.mutex->ReleaseSync(0)), "release retired output");
        reader.open(renderer.output_handle());
        require(reader.mutex->AcquireSync(1, 0) == S_OK, "resized output publishes reader key");
        require(reader.pixels(800, 450)[225 * 800 + 400] == center, "resized output correct colors");
        require(SUCCEEDED(reader.mutex->ReleaseSync(0)), "release resized output");

        require(renderer.demo(-2), "create invalid display-time fixture");
        require(!renderer.draw(options), "invalid composition fails after writer acquires output");
        require(reader.mutex->AcquireSync(1, 0) == WAIT_TIMEOUT,
                "failed draw never publishes a partial frame");
        require(renderer.demo(.1) && renderer.draw(options), "failed draw returns writer key for retry");
        require(reader.mutex->AcquireSync(1, 0) == S_OK, "successful retry publishes output");
        const auto generation = renderer.output_generation();
        require(renderer.attach_offscreen() && renderer.draw(options), "replace offscreen device");
        require(renderer.output_generation() != generation, "device replacement changes generation");
        require(reader.pixels(800, 450)[225 * 800 + 400] == center,
                "device replacement preserves independently opened old output");
        require(SUCCEEDED(reader.mutex->ReleaseSync(0)), "release old-device output");
        std::vector<std::uint32_t> diagnostic;
        unsigned diagnostic_width{}, diagnostic_height{};
        require(renderer.readback(diagnostic, diagnostic_width, diagnostic_height) &&
                    diagnostic_width == 800 && diagnostic_height == 450 &&
                    diagnostic[225 * 800 + 400] == 0xff000000u,
                "diagnostic readback consumes the new device waiting output");
        require(renderer.draw(options), "diagnostic readback returns output ownership to writer");

        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc{};
        renderer.device()->QueryInterface(IID_PPV_ARGS(&dxgi));
        dxgi->GetAdapter(&adapter);
        adapter->GetDesc(&adapter_desc);
        require(renderer.attach_offscreen(&adapter_desc.AdapterLuid), "attach fixed OBS adapter");
        auto *fixed_device = renderer.device();
        Channel channel;
        require(channel.open(true,
                             L"Local\\HorizonLock-offscreen-test-" + std::to_wstring(GetCurrentProcessId())),
                "offscreen adapter channel");
        require(channel.lock(), "offscreen adapter packet lock");
        auto &frame = channel.data()->frame;
        frame = {};
        frame.generation = frame.session = frame.sequence = frame.reference_epoch = 1;
        frame.producer_pid = GetCurrentProcessId();
        frame.display_time = 1000000000;
        frame.adapter = adapter_desc.AdapterLuid;
        frame.adapter.LowPart ^= 0x80000000u;
        frame.view_count = 1;
        frame.views[0].texture_handle = 1;
        frame.views[0].width = frame.views[0].height = 512;
        frame.views[0].rect_width = frame.views[0].rect_height = 512;
        frame.views[0].format = DXGI_FORMAT_R8G8B8A8_UNORM;
        channel.data()->ready = 1;
        channel.unlock();
        require(!renderer.consume(channel), "different game adapter must fail for fixed OBS adapter");
        require(renderer.device() == fixed_device, "adapter mismatch preserves fixed renderer device");
        require(renderer.error().find(L"adapter") != std::wstring::npos, "clear adapter mismatch error");
        require(renderer.draw(options), "fixed adapter remains usable after rejecting game adapter");
        for (bool diagnostic_acquire : {false, true}) {
            OutputReader abandoned_reader(renderer.device());
            abandoned_reader.open(renderer.output_handle());
            const auto abandoned_generation = renderer.output_generation();
            abandon_output(abandoned_reader, renderer.output_handle());
            require(SUCCEEDED(renderer.device()->GetDeviceRemovedReason()),
                    "abandoned reader must leave the writer device healthy");
            require(diagnostic_acquire ? !renderer.readback(diagnostic, diagnostic_width, diagnostic_height)
                                       : !renderer.draw(options),
                    "abandoned output acquisition must fail");
            require(renderer.output_handle() == 0 && renderer.output_generation() != abandoned_generation,
                    "abandoned output is invalidated and its generation retired");
            require(renderer.device() == fixed_device, "abandonment retains the fixed writer device");
            require(renderer.draw(options), "next draw recreates output after reader abandonment");
            OutputReader recovered_reader(renderer.device());
            recovered_reader.open(renderer.output_handle());
            require(recovered_reader.mutex->AcquireSync(1, 0) == S_OK,
                    "replacement output publishes a readable frame");
            require(recovered_reader.pixels(800, 450)[225 * 800 + 400] == 0xff000000u,
                    "replacement output has correct waiting pixels");
            require(SUCCEEDED(recovered_reader.mutex->ReleaseSync(0)), "release recovered output");
            require(renderer.draw(options), "publish next frame after abandoned output recovery");
        }
        std::cout << "Native GPU: abandoned reader draw/readback recovery without device removal passed\n";
        std::cout << "Native GPU: offscreen waiting/demo, keyed output, busy reader, resize, failed draw, "
                     "device replacement and fixed adapter passed\n";
    }
}
int main(int argc, char **argv) {
    try {
        offscreen_tests<Renderer>();
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HorizonLockRenderTest";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"Render test", WS_POPUP, 0, 0, 960, 540, nullptr,
                                    nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "test window");
        Renderer renderer;
        require(renderer.attach(window), "native renderer initialize/shader compilation");
        Options options;
        options.mirror_softness = 0; // Legacy mirror tests compare the exact unsoftened image.
        options.crop = 0;
        options.stabilize = false;
        require(renderer.demo(0, true) && renderer.draw(options, false), "quad demo draw");
        std::vector<std::uint32_t> pixels;
        unsigned w, h;
        require(renderer.readback(pixels, w, h), "GPU readback");
        require(w == 3840 && h == 2160, "default rendering must be exactly 4K");
        auto center = pixels[(h / 2) * w + w / 2];
        auto side = pixels[(h / 2) * w + w / 8];
        require(renderer.draw(options, true), "present live frame before capture stalls");
        require(renderer.draw(options, true, L"Waiting for OpenXR capture…") &&
                    renderer.readback(pixels, w, h),
                "present waiting frame through the GPU swapchain");
        unsigned ink = 0;
        for (auto pixel : pixels)
            if (pixel & 0xffffff)
                ++ink;
        require(ink > 100 && ink < w * h / 20 && (pixels[0] & 0xffffff) == 0,
                "waiting output must be black with a small visible text label");
        require(renderer.draw(options, true) && renderer.readback(pixels, w, h),
                "resume live output after waiting");
        require(pixels[(h / 2) * w + w / 2] == center, "waiting screen must not alter retained live image");
        require(((center >> 8) & 255) > ((center) & 255) + 80, "foveal inset must replace context at center");
        require(((side >> 16) & 255) > ((side >> 8) & 255), "outside inset must keep context");
        if (argc > 1) {
            std::ofstream out(argv[1], std::ios::binary);
            out << "P6\n" << w << ' ' << h << "\n255\n";
            for (auto pixel : pixels) {
                char color[3] = {char(pixel), char(pixel >> 8), char(pixel >> 16)};
                out.write(color, 3);
            }
        }
        SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER);
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "resize and letterbox");
        require(w == 3840 && h == 2160, "window resize must preserve render dimensions");
        options.output_width = 1280;
        options.output_height = 720;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h) && w == 1280 && h == 720,
                "chosen render resolution");
        require((pixels[0] & 0xffffff) != (pixels[(h / 2) * w + w / 2] & 0xffffff),
                "letterbox differs from focus");
        SetWindowPos(window, nullptr, 0, 0, 960, 540, SWP_NOMOVE | SWP_NOZORDER);

        const auto name = L"Local\\HorizonLock-render-test-" + std::to_wstring(GetCurrentProcessId());
        Channel channel;
        require(channel.open(true, name), "test IPC");
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                            D3D11_SDK_VERSION, &device, nullptr, &context)),
                "producer device");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc{};
        device.As(&dxgi);
        dxgi->GetAdapter(&adapter);
        adapter->GetDesc(&adapter_desc);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 512;
        desc.MipLevels = desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "shared texture");
        ComPtr<IDXGIResource> resource;
        ComPtr<IDXGIKeyedMutex> keyed;
        texture.As(&resource);
        texture.As(&keyed);
        HANDLE handle{};
        resource->GetSharedHandle(&handle);
        std::vector<std::uint32_t> input(512 * 512, 0xffff0000u);
        for (int y = 128; y < 384; ++y)
            for (int x = 128; x < 384; ++x)
                input[y * 512 + x] = 0xff0000ffu;
        require(keyed->AcquireSync(0, 0) == S_OK, "producer key");
        context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), 512 * 4, 0);
        context->Flush();
        keyed->ReleaseSync(1);
        require(channel.lock(), "publish lock");
        Frame f{};
        f.generation = 22;
        f.session = f.sequence = f.reference_epoch = 1;
        f.producer_pid = GetCurrentProcessId();
        f.adapter = adapter_desc.AdapterLuid;
        f.display_time = 2000000000;
        f.view_count = 1;
        auto &view = f.views[0];
        view.texture_handle = reinterpret_cast<std::uint64_t>(handle);
        view.width = view.height = 512;
        view.x = view.y = 128;
        view.rect_width = view.rect_height = 256;
        view.format = desc.Format;
        channel.data()->frame = f;
        channel.data()->ready = 1;
        channel.unlock();
        require(renderer.consume(channel), "consume keyed frame on another device");
        require(keyed->AcquireSync(0, 0) == S_OK, "consumer must return producer key");
        // Overwrite the transport texture immediately. Renderer must retain its
        // private copy and must never display the newer pixels with old metadata.
        std::fill(input.begin(), input.end(), 0xff00ff00u);
        context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), 512 * 4, 0);
        context->Flush();
        keyed->ReleaseSync(0);
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "draw retained snapshot");
        for (auto pixel : pixels)
            require((pixel & 255) > 240 && ((pixel >> 8) & 255) < 3 && ((pixel >> 16) & 255) < 3,
                    "subrect or snapshot ownership mismatch");
        require(keyed->AcquireSync(0, 0) == S_OK, "republish key");
        keyed->ReleaseSync(1);
        require(channel.lock(), "rolled frame lock");
        auto roll = vrst::axis_angle({0, 0, 1}, .7);
        channel.data()->frame.sequence++;
        channel.data()->frame.display_time += 16666667;
        channel.data()->frame.views[0].orientation[2] = float(roll.z);
        channel.data()->frame.views[0].orientation[3] = float(roll.w);
        channel.data()->ready = 1;
        channel.unlock();
        require(renderer.consume(channel), "consume rolled frame");
        options.stabilize = true;
        options.smoothing = 0;
        options.horizon_lock = 100;
        options.mirror_limit = 0;
        renderer.reset();
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "zero mirror draw");
        require((pixels[0] & 0xffffff) == 0, "zero mirror limit must black outside source");
        options.mirror_limit = 10;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "limited mirror draw");
        require(((pixels[0] >> 8) & 255) > 240, "ten percent mirror must cover known six percent overrun");
        const auto unsoftened = pixels;
        options.mirror_softness = 70;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "soft mirror draw");
        require(((pixels[0] >> 8) & 255) < ((unsoftened[0] >> 8) & 255) - 10 &&
                    ((pixels[0] >> 8) & 255) > 100,
                "softened mirror must be subtly darker away from the seam");
        require(pixels[(h / 2) * w + w / 2] == unsoftened[(h / 2) * w + w / 2],
                "soft mirrors must leave the original image interior untouched");
        // A textured fixture distinguishes real softening from darkening alone.
        require(keyed->AcquireSync(0, 0) == S_OK, "textured mirror producer key");
        std::uint32_t noise = 12345;
        for (auto &pixel : input) {
            noise = noise * 1664525u + 1013904223u;
            const auto gray = noise >> 24;
            pixel = 0xff000000u | gray | (gray << 8) | (gray << 16);
        }
        context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), 512 * 4, 0);
        context->Flush();
        keyed->ReleaseSync(1);
        require(channel.lock(), "textured mirror frame lock");
        channel.data()->frame.sequence++;
        channel.data()->frame.display_time += 16666667;
        const auto textured_frame = channel.data()->frame;
        channel.data()->ready = 1;
        channel.unlock();
        require(renderer.consume(channel), "consume textured mirror fixture");
        options.mirror_softness = 0;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "legacy textured mirror");
        const auto sharp = pixels;
        options.mirror_softness = 100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "soft textured mirror");
        FrameStabilizer mirror_geometry;
        const auto projection = mirror_geometry.update(textured_frame, options).warp;
        double sharp_sum = 0, soft_sum = 0, sharp_square = 0, soft_square = 0;
        unsigned mirror_count = 0, seam_count = 0, interior_count = 0;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                auto ray = vrst::rotate(
                    projection.correction,
                    {projection.output.left +
                         (projection.output.right - projection.output.left) * (x + .5) / w,
                     projection.output.up + (projection.output.down - projection.output.up) * (y + .5) / h,
                     -1});
                const double u = (ray.x / -ray.z - projection.input.left) /
                                 (projection.input.right - projection.input.left);
                const double v =
                    (projection.input.up - ray.y / -ray.z) / (projection.input.up - projection.input.down);
                const double distance = std::max({-u, u - 1, -v, v - 1});
                const auto index = std::size_t(y) * w + x;
                const double a = sharp[index] & 255, b = pixels[index] & 255;
                if (distance < -.02) {
                    require(sharp[index] == pixels[index],
                            "mirror softness must preserve every interior pixel");
                    ++interior_count;
                } else if (distance > 0 && distance < .0005) {
                    require(std::abs(a - b) <= 3,
                            "soft mirror transition must meet the original without a seam");
                    ++seam_count;
                } else if (distance > .035 && distance < .07) {
                    sharp_sum += a;
                    soft_sum += b;
                    sharp_square += a * a;
                    soft_square += b * b;
                    ++mirror_count;
                }
            }
        require(interior_count > 1000 && seam_count > 20 && mirror_count > 100,
                "fixture must exercise interior, seam and mirror region");
        const double sharp_variance = sharp_square / mirror_count - std::pow(sharp_sum / mirror_count, 2);
        const double soft_variance = soft_square / mirror_count - std::pow(soft_sum / mirror_count, 2);
        require(soft_variance < sharp_variance * .65,
                "soft mirror must reduce texture contrast beyond darkening alone");
        options.mirror_softness = 0;
        options.mirror_limit = 1;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "small mirror draw");
        require((pixels[0] & 0xffffff) == 0, "overrun beyond mirror limit must be black");
        options = Options{};
        options.crop = 0;
        options.stabilize = false;
        options.output_width = 1280;
        options.output_height = 720;
        // Move and resize the selected eye inside the same stereo texture.
        // No pixel outside the submitted subrect may leak into the private copy.
        require(keyed->AcquireSync(0, 0) == S_OK, "resize producer key");
        std::fill(input.begin(), input.end(), 0xffff0000u);
        for (int y = 64; y < 192; ++y)
            for (int x = 256; x < 384; ++x)
                input[y * 512 + x] = 0xff0000ffu;
        context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), 512 * 4, 0);
        context->Flush();
        keyed->ReleaseSync(1);
        require(channel.lock(), "resize subrect lock");
        auto &resized = channel.data()->frame;
        resized.sequence++;
        resized.display_time += 16666667;
        resized.views[0].x = 256;
        resized.views[0].y = 64;
        resized.views[0].rect_width = resized.views[0].rect_height = 128;
        channel.data()->ready = 1;
        channel.unlock();
        require(renderer.consume(channel), "consume resized eye without replacing producer texture");
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "draw resized eye");
        for (auto pixel : pixels)
            require((pixel & 255) > 240 && ((pixel >> 8) & 255) < 3 && ((pixel >> 16) & 255) < 3,
                    "resized private eye must retain the exact submitted subrect");
        // Replacing the GPU generation may fail before its first acquisition.
        // Old pose metadata must not survive destruction of the matching image.
        require(channel.lock(), "replacement lock");
        channel.data()->frame.generation++;
        channel.data()->frame.views[0].texture_handle = 0x1234;
        channel.data()->ready = 1;
        channel.unlock();
        require(!renderer.consume(channel), "invalid replacement must fail");
        require(!renderer.has_frame(), "discarded retained texture must also discard its old metadata");
        require(channel.lock(), "recovery lock");
        require(channel.data()->ready == 2, "failed transport must request a fresh producer generation");
        channel.unlock();
        renderer.source_changed();
        D3D11_TEXTURE2D_DESC window_desc{};
        window_desc.Width = 512;
        window_desc.Height = 288;
        window_desc.MipLevels = window_desc.ArraySize = 1;
        window_desc.SampleDesc.Count = 1;
        window_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        window_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> window_image;
        std::vector<std::uint32_t> window_pixels(512 * 288, 0xff804020u);
        D3D11_SUBRESOURCE_DATA window_data{window_pixels.data(), 512 * 4, 0};
        require(SUCCEEDED(renderer.device()->CreateTexture2D(&window_desc, &window_data, &window_image)),
                "window test texture");
        bool processed = renderer.consume_window(window_image.Get(), 4000000000ULL, options);
        for (int i = 0; i < 200 && !processed; ++i) {
            Sleep(1);
            processed = renderer.consume_window(nullptr, 0, options);
        }
        if (!processed)
            std::wcerr << renderer.error() << L'\n';
        require(processed, "window frame analysis must complete without blocking map");
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "window image draw");
        auto window_center = pixels[(h / 2) * w + w / 2];
        require(std::abs(int(window_center & 255) - 128) <= 2 &&
                    std::abs(int((window_center >> 8) & 255) - 64) <= 2 &&
                    std::abs(int((window_center >> 16) & 255) - 32) <= 2,
                "window capture must preserve SDR colors");
        require(!renderer.consume_window(window_image.Get(), 4000000000ULL, options),
                "repeated window timestamp must not advance tracking");
        for (unsigned y = 0; y < 288; ++y)
            for (unsigned x = 0; x < 512; ++x)
                window_pixels[y * 512 + x] = 0xff000000u | ((x * 255 / 511) << 16) | ((y * 255 / 287) << 8);
        ComPtr<ID3D11DeviceContext> capture_context;
        renderer.device()->GetImmediateContext(&capture_context);
        capture_context->UpdateSubresource(window_image.Get(), 0, nullptr, window_pixels.data(), 512 * 4, 0);
        processed = renderer.consume_window(window_image.Get(), 4100000000ULL, options);
        for (int i = 0; i < 200 && !processed; ++i) {
            Sleep(1);
            processed = renderer.consume_window(nullptr, 0, options);
        }
        options.crop_left = 10;
        options.crop_right = 20;
        options.crop_top = 5;
        options.crop_bottom = 15;
        options.aspect = 1;
        require(processed && renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "asymmetric image crop draw");
        auto cropped = pixels[(h / 2) * w + w / 2];
        require(std::abs(int(cropped & 255) - 114) <= 2 && std::abs(int((cropped >> 8) & 255) - 114) <= 2,
                "image crop must move sampled center to (45%,45%) without stretching");
        options.crop_left = options.crop_right = options.crop_top = options.crop_bottom = 0;
        options.edge_reserve[0] = {true, 2, 20, 5, 5};
        options.edge_reserve[1] = {true, 20, 2, 5, 5};
        options.eye = 0;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "left window reserve draw");
        auto reserve_center = pixels[(h / 2) * w + w / 2];
        require(std::abs(int(reserve_center & 255) - 76) <= 2,
                "window aspect fit must visibly favor scenery beside its two-percent left reserve");
        options.eye = 1;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "right window reserve draw");
        reserve_center = pixels[(h / 2) * w + w / 2];
        require(std::abs(int(reserve_center & 255) - 179) <= 2,
                "window capture must change reserve profiles immediately with selected eye");
        options.edge_reserve[0].enabled = options.edge_reserve[1].enabled = false;
        options.stabilize = false;
        options.crop = 20;
        options.aspect = 16. / 9.;
        options.vertical_offset = -100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "reserve-only high window");
        const auto reserve_high = pixels[(h / 2) * w + w / 2];
        options.vertical_offset = 100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "reserve-only low window");
        const auto reserve_low = pixels[(h / 2) * w + w / 2];
        require(int((reserve_low >> 8) & 255) - int((reserve_high >> 8) & 255) >= 48,
                "Y slider must use hidden uniform reserve in a16:9 window, with no extra aspect crop");
        options.crop = 0;
        options.aspect = 4;
        options.vertical_offset = -100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "highest window framing");
        auto high_center = pixels[(h / 2) * w + w / 2];
        options.vertical_offset = 100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "lowest window framing");
        auto low_center = pixels[(h / 2) * w + w / 2];
        require(std::abs(int((high_center >> 8) & 255) - 57) <= 2 &&
                    std::abs(int((low_center >> 8) & 255) - 198) <= 2,
                "window vertical offset must select top and bottom aspect-fit scenery");
        options.edge_reserve[1] = {true, 0, 0, 2, 20};
        options.vertical_offset = -100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "highest window framing with separate reserves");
        high_center = pixels[(h / 2) * w + w / 2];
        options.vertical_offset = 100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "lowest window framing with separate reserves");
        low_center = pixels[(h / 2) * w + w / 2];
        require(std::abs(int((high_center >> 8) & 255) - 57) <= 2 &&
                    std::abs(int((low_center >> 8) & 255) - 198) <= 2,
                "window vertical offset must use captured pixels behind the enabled edge reserves");
        renderer.source_changed();
        require(!renderer.has_frame(), "window source change must discard pending history");
        std::cout << "Native GPU: window analysis/color, quad composition, resize, subrect bounds, keyed "
                     "transfer and retained "
                     "snapshot passed\n";
        DestroyWindow(window);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
