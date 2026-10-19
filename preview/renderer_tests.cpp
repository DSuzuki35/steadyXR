#include "renderer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <iostream>
#include <limits>
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
void overlay_tests() {
    Renderer renderer;
    require(renderer.attach_offscreen(), "overlay renderer initialize");
    OutputReader producer(renderer.device());
    Channel channel;
    require(channel.open(true, L"Local\\HorizonLock-overlay-GPU-" + std::to_wstring(GetCurrentProcessId())),
            "overlay channel");
    Frame frame{};
    frame.generation = frame.session = frame.sequence = frame.reference_epoch = 1;
    frame.producer_pid = GetCurrentProcessId();
    frame.display_time = 1000000000;
    frame.view_count = frame.overlay_count = 1;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    producer.device.As(&dxgi);
    dxgi->GetAdapter(&adapter);
    adapter->GetDesc(&adapter_desc);
    frame.adapter = adapter_desc.AdapterLuid;
    ComPtr<ID3D11Texture2D> images[3];
    ComPtr<IDXGIKeyedMutex> keys[3];
    for (unsigned i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 64;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        require(SUCCEEDED(producer.device->CreateTexture2D(&d, nullptr, &images[i])),
                "overlay fixture image");
        ComPtr<IDXGIResource> resource;
        HANDLE handle{};
        images[i].As(&resource);
        images[i].As(&keys[i]);
        resource->GetSharedHandle(&handle);
        auto &v = i ? frame.overlays[i - 1].image : frame.views[0];
        v.texture_handle = reinterpret_cast<std::uint64_t>(handle);
        v.width = v.height = v.rect_width = v.rect_height = 64;
        v.format = d.Format;
    }
    auto &overlay = frame.overlays[0];
    overlay.type = 1;
    overlay.flags = 3;
    overlay.width = overlay.height = 1;
    overlay.position[2] = -1;
    auto publish = [&](std::uint32_t overlay_color = 0x800000ffu, std::uint32_t second_color = 0x8000ff00u,
                       bool bordered = false) {
        for (unsigned i = 0; i < 1 + frame.overlay_count; ++i) {
            require(keys[i]->AcquireSync(0, 0) == S_OK, "overlay producer owns every image");
            std::vector<std::uint32_t> pixels(64 * 64,
                                              i ? (i == 1 ? overlay_color : second_color) : 0xffff0000u);
            if (i && bordered)
                for (unsigned y = 0; y < 64; ++y)
                    for (unsigned x = 0; x < 64; ++x)
                        if (x < 16 || x >= 48 || y < 16 || y >= 48)
                            pixels[y * 64 + x] = 0xff0000ffu;
            producer.context->UpdateSubresource(images[i].Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
        }
        producer.context->Flush();
        for (unsigned i = 0; i < 1 + frame.overlay_count; ++i)
            require(SUCCEEDED(keys[i]->ReleaseSync(1)), "publish overlay reader key");
        ++frame.sequence;
        frame.display_time += 16666667;
        require(channel.lock(), "overlay publication lock");
        channel.data()->frame = frame;
        channel.data()->ready = 1;
        channel.unlock();
    };
    Options options;
    options.output_width = 640;
    options.output_height = 360;
    options.crop = 0;
    options.stabilize = options.mirror = false;
    options.overlays.mode = 1;
    std::vector<std::uint32_t> pixels;
    unsigned w{}, h{};
    auto draw = [&] {
        require(renderer.draw(options) && renderer.readback(pixels, w, h), "overlay draw/readback");
    };
    publish();
    require(renderer.consume(channel), "consume base and overlay atomically");
    draw();
    const auto center = pixels[180 * w + 320];
    require(std::abs(int(center & 255) - 188) <= 2 && std::abs(int((center >> 16) & 255) - 187) <= 2,
            "straight alpha quad blends red over blue in linear light");
    require(pixels[180 * w + 50] == 0xffff0000u, "quad geometry clips outside physical size");
    options.visual.exposure = true;
    options.visual.exposure_ev = -2;
    draw();
    require(std::abs(int(pixels[180 * w + 320] & 255) - 188) <= 2 &&
                std::abs(int((pixels[180 * w + 320] >> 16) & 255) - 99) <= 2,
            "effects darken game before overlay; overlay colors and alpha stay unchanged");
    options.visual = {};
    options.overlays.mode = 0;
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u, "disabling overlays immediately hides retained image");
    options.overlays.mode = 1;
    draw();
    require(pixels[180 * w + 320] == center, "retained overlay metadata and pixels redraw together");
    options.overlays.mode = 2;
    options.overlays.mask = 1;
    options.overlays.producer_pid = frame.producer_pid;
    options.overlays.session = frame.session + 1;
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u, "selected overlay slots cannot cross producer sessions");
    options.overlays.session = frame.session;
    draw();
    require(pixels[180 * w + 320] == center, "matching session selected slot draws");
    options.overlays.mode = 1;
    require(keys[1]->AcquireSync(0, 0) == S_OK, "overwrite released transport overlay");
    std::vector<std::uint32_t> newer_pixels(64 * 64, 0xff00ff00u);
    producer.context->UpdateSubresource(images[1].Get(), 0, nullptr, newer_pixels.data(), 64 * 4, 0);
    producer.context->Flush();
    keys[1]->ReleaseSync(0);
    draw();
    require(pixels[180 * w + 320] == center, "producer overwrite cannot mutate retained private overlay");
    // A busy overlay must leave every retained image and its pose intact, and
    // return any acquired base image to the pending reader key for retry.
    overlay.position[0] = .7f;
    publish(0xff00ff00u);
    require(keys[1]->AcquireSync(1, 0) == S_OK, "hold overlay reader key");
    const auto retained_sequence = renderer.sequence();
    const auto busy_begin = std::chrono::steady_clock::now();
    require(!renderer.consume(channel) && renderer.sequence() == retained_sequence,
            "busy overlay never replaces retained base or metadata");
    require(std::chrono::steady_clock::now() - busy_begin < std::chrono::milliseconds(100),
            "overlay acquisition never blocks the game");
    require(keys[0]->AcquireSync(1, 0) == S_OK, "partial acquisition restores base reader key");
    keys[0]->ReleaseSync(1);
    draw();
    require(pixels[180 * w + 320] == center, "busy overlay preserves complete previous composition");
    keys[1]->ReleaseSync(1);
    require(renderer.consume(channel), "retry atomically consumes pending overlay");
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u && pixels[180 * w + 544] == 0xff00ff00u,
            "space-converted translation places overlay on the right");
    overlay.position[0] = 0;
    overlay.flags = 1;
    publish(0x80000080u);
    require(renderer.consume(channel), "consume premultiplied alpha fixture");
    draw();
    require(std::abs(int(pixels[180 * w + 320] & 255) - 188) <= 2 &&
                std::abs(int((pixels[180 * w + 320] >> 16) & 255) - 187) <= 2,
            "premultiplied source is not multiplied twice");
    overlay.flags = 0;
    publish(0x000000ffu);
    require(renderer.consume(channel), "consume ignored-alpha fixture");
    draw();
    require(pixels[180 * w + 320] == 0xff0000ffu, "OpenXR no-alpha flag treats zero alpha as opaque");
    overlay.flags = 3;
    publish(0x000000ffu);
    require(renderer.consume(channel), "consume transparent fixture");
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u, "transparent straight-alpha pixels preserve the scene");
    overlay.orientation[1] = 1;
    overlay.orientation[3] = 0;
    publish(0xff0000ffu);
    require(renderer.consume(channel), "consume back-facing quad");
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u, "back-facing quad is culled");
    overlay.orientation[1] = 0;
    overlay.orientation[3] = 1;
    overlay.type = 2;
    overlay.position[2] = 0;
    overlay.radius = 1;
    overlay.central_angle = 1;
    overlay.aspect_ratio = 1;
    publish(0xff00ff00u);
    require(renderer.consume(channel), "consume finite cylinder");
    draw();
    require(pixels[180 * w + 320] == 0xff00ff00u && pixels[180 * w + 50] == 0xffff0000u,
            "cylinder arc clips outside its angle");
    overlay.position[0] = .5f;
    publish(0xff00ff00u);
    require(renderer.consume(channel), "consume translated finite cylinder");
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u && pixels[180 * w + 480] == 0xff00ff00u,
            "finite cylinder intersection accounts for the translated eye origin");
    overlay.radius = 0;
    overlay.position[0] = 100;
    publish(0xff00ff00u);
    require(renderer.consume(channel), "consume infinite cylinder");
    draw();
    require(pixels[180 * w + 320] == 0xff00ff00u,
            "infinite cylinder ignores translation and retains direction");
    overlay.type = 3;
    overlay.position[0] = 0;
    overlay.image.fov[0] = overlay.image.fov[3] = -.2f;
    overlay.image.fov[1] = overlay.image.fov[2] = .2f;
    publish(0xff0000ffu);
    require(renderer.consume(channel), "consume projection overlay");
    draw();
    require(pixels[180 * w + 320] == 0xff0000ffu && pixels[180 * w + 450] == 0xffff0000u,
            "projection overlay clips to its own FOV");
    overlay.orientation[1] = std::sin(-.25f);
    overlay.orientation[3] = std::cos(-.25f);
    publish(0xff0000ffu);
    require(renderer.consume(channel), "consume rotated projection overlay");
    draw();
    require(pixels[180 * w + 320] == 0xffff0000u && pixels[180 * w + 495] == 0xff0000ffu,
            "projection orientation rotates coverage relative to the context eye");
    overlay.type = 1;
    overlay.position[0] = .35f;
    overlay.position[2] = -1;
    overlay.orientation[1] = 0;
    overlay.orientation[3] = 1;
    overlay.width = overlay.height = .45f;
    frame.views[0].orientation[2] = std::sin(.3f);
    frame.views[0].orientation[3] = std::cos(.3f);
    options.stabilize = true;
    options.horizon_lock = 100;
    options.adaptive_horizon = false;
    publish(0xff00ff00u);
    require(renderer.consume(channel), "consume horizon correction overlay fixture");
    draw();
    const auto stabilized = pixels;
    options.compare_original = true;
    draw();
    const auto original = pixels;
    require(original != stabilized, "overlay follows game horizon correction");
    auto green_center_y = [&](const std::vector<std::uint32_t> &image) {
        double total = 0, count = 0;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
                if (image[y * w + x] == 0xff00ff00u) {
                    total += y;
                    ++count;
                }
        require(count > 100, "corrected overlay has visible geometry");
        return total / count;
    };
    require(std::abs(green_center_y(original) - green_center_y(stabilized)) > 30,
            "horizon correction moves the overlay itself with the game's rays");
    options.compare_mode = 1;
    draw();
    for (unsigned y = 0; y < h; y += 7)
        for (unsigned x = 0; x < w; x += 7)
            require(pixels[y * w + x] == (x < w / 2 ? stabilized : original)[y * w + x],
                    "A/B split applies matching stabilized and original overlay rays");
    options.compare_mode = 2;
    draw();
    require(pixels[50 * w + 160] == 0xff000000u && pixels[50 * w + 480] == 0xff000000u,
            "side-by-side overlays do not escape letterboxed panels");
    // Sample interiors, away from resampling boundaries: both complete images
    // are scaled to half size and centered vertically without changing geometry.
    for (unsigned panel = 0; panel < 2; ++panel)
        for (unsigned y = 100; y < 260; y += 11)
            for (unsigned x = 10; x < 310; x += 11) {
                const auto &reference = panel ? original : stabilized;
                const unsigned rx = x * 2, ry = (y - 90) * 2;
                const auto expected = reference[ry * w + rx];
                if (reference[ry * w + rx + 1] == expected && reference[(ry + 1) * w + rx] == expected &&
                    reference[(ry + 1) * w + rx + 1] == expected)
                    require(pixels[y * w + panel * 320 + x] == expected,
                            "side-by-side overlay remapping matches complete reference images");
            }
    options.compare_original = false;
    options.compare_mode = 0;
    options.stabilize = false;
    overlay.position[0] = 0;
    overlay.width = overlay.height = 1;
    overlay.image.x = overlay.image.y = 16;
    overlay.image.rect_width = overlay.image.rect_height = 32;
    publish(0xff00ff00u, 0, true);
    require(renderer.consume(channel), "consume overlay subrect resize on same handle");
    draw();
    require(pixels[180 * w + 320] == 0xff00ff00u && pixels[180 * w + 165] == 0xff00ff00u,
            "private overlay subrect excludes contrasting outer texels including edge sampling");
    const auto second_image = frame.overlays[1].image;
    frame.overlays[1] = overlay;
    frame.overlays[1].image = second_image;
    frame.overlays[1].slot = 1;
    frame.overlay_count = 2;
    publish();
    require(renderer.consume(channel), "consume multiple overlay layers");
    draw();
    auto color = pixels[180 * w + 320];
    require(std::abs(int(color & 255) - 137) <= 2 && std::abs(int((color >> 8) & 255) - 188) <= 2 &&
                std::abs(int((color >> 16) & 255) - 136) <= 2,
            "submission order composites green over red over base blue");
    std::swap(frame.overlays[0], frame.overlays[1]);
    publish();
    require(renderer.consume(channel), "consume reordered overlay resources");
    draw();
    color = pixels[180 * w + 320];
    require(std::abs(int(color & 255) - 188) <= 2 && std::abs(int((color >> 8) & 255) - 137) <= 2,
            "reordered overlay metadata opens matching resources and reverses composition order");
    require(channel.lock(), "bad overlay metadata lock");
    channel.data()->frame = frame;
    channel.data()->frame.overlays[0].width = -1;
    channel.data()->ready = 1;
    channel.unlock();
    require(!renderer.consume(channel) && !renderer.has_frame(),
            "invalid overlay geometry rejects mixed frames");
    std::cout << "Native GPU: overlay composition and selection passed\n";
}
void visual_host_tests() {
    Renderer renderer;
    require(renderer.attach_offscreen() && renderer.demo(0), "visual host fixture");
    Options options;
    options.output_width = 640;
    options.output_height = 360;
    options.stabilize = false;
    options.crop = 0;
    std::vector<std::uint32_t> pixels;
    unsigned w{}, h{};
    auto draw = [&] {
        require(renderer.draw(options) && renderer.readback(pixels, w, h), "visual host draw");
        return pixels;
    };
    const auto original = draw();
    require(renderer.visual_diagnostics().allocations == 0 && renderer.visual_diagnostics().dispatches == 0,
            "default preview allocates no effects resources and dispatches no work");
    options.visual.exposure = true;
    options.visual.exposure_ev = -2;
    const auto enhanced = draw();
    const auto allocations = renderer.visual_diagnostics().allocations;
    require(allocations > 0 && renderer.visual_diagnostics().frame_dispatches == 1,
            "manual exposure uses one GPU compute pass");
    require(draw() == enhanced && renderer.visual_diagnostics().allocations == allocations,
            "effects reuse GPU resources on redraw");
    require((enhanced[180 * w + 320] & 255) < (original[180 * w + 320] & 255) - 30,
            "local preview applies exposure to game image");
    options.compare_effects = options.compare_original = true;
    require(draw() == original, "effects original A/B preserves exact geometry and source colors");
    options.compare_mode = 1;
    draw();
    require(pixels[180 * w + 200] == enhanced[180 * w + 200] &&
                pixels[180 * w + 440] == original[180 * w + 440], "effects split uses same framing");
    options.compare_mode = 2;
    draw();
    require(pixels[10 * w + 160] == 0xff000000u &&
                pixels[180 * w + 160] == enhanced[180 * w + 320] &&
                pixels[180 * w + 480] == original[180 * w + 320], "effects fitted side-by-side");
    options.compare_original = false;
    options.visual.exposure_ev = 0;
    draw();
    for (std::size_t i = 0; i < pixels.size(); ++i)
        for (unsigned shift : {0u, 8u, 16u, 24u})
            require(std::abs(int((pixels[i] >> shift) & 255) - int((original[i] >> shift) & 255)) <= 1,
                    "neutral effects preserve SDR and alpha with one transfer encode");
    options.visual.bloom = options.visual.adaptation = options.visual.color = true;
    draw();
    options.spectator.bypass = true;
    require(draw() == original, "master bypass restores original pixels");
    options.spectator.bypass = false;
    options.visual = {};
    require(draw() == original, "switching every effect off restores original pixels");
    require(renderer.visual_diagnostics().resource_bytes == 0 && renderer.visual_diagnostics().dispatches == 0,
            "all-off frees effects resources and stops GPU work");
    std::cout << "Native GPU: preview exposure, transfer, A/B and bypass passed\n";
}
int main(int argc, char **argv) {
    try {
        visual_host_tests();
        overlay_tests();
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
        // A/B must sample the very same captured quad-view frame in both halves.
        // Distinct context/focus colors also catch bypassing the focus pose.
        {
            auto comparison = options;
            comparison.output_width = 640;
            comparison.output_height = 360;
            comparison.stabilize = true;
            comparison.smoothing = 100;
            comparison.horizon_lock = 75;
            require(renderer.demo(.05, true), "moving quad comparison fixture");
            require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h),
                    "stabilized A frame");
            const auto stable = pixels;
            comparison.compare_original = true;
            require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h), "original B frame");
            const auto original = pixels;
            require(stable != original, "comparison fixture must have visible stabilization");
            comparison.compare_mode = 1;
            require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h), "split comparison");
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x)
                    require(
                        pixels[y * w + x] == (x < w / 2 ? stable[y * w + x] : original[y * w + x]),
                        "50/50 must keep stabilized pixels left and original pixels right without reframing");
            comparison.compare_mode = 2;
            require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h),
                    "side by side comparison");
            require((pixels[w * 8 + 8] & 0xffffff) == 0 && (pixels[(h - 9) * w + w - 9] & 0xffffff) == 0,
                    "side-by-side complete views must preserve aspect with black letterboxing");
            // Produce half-size references with identical optical framing.
            const auto side_by_side = pixels;
            const unsigned full_w = w, full_h = h;
            comparison.output_width /= 2;
            comparison.output_height /= 2;
            comparison.compare_mode = 0;
            for (bool b : {false, true}) {
                comparison.compare_original = b;
                require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h),
                        "half-size comparison reference");
                for (unsigned y = 0; y < h; ++y)
                    for (unsigned x = 0; x < w; ++x) {
                        const auto actual = side_by_side[(y + full_h / 4) * full_w + x + (b ? w : 0)],
                                   expected = pixels[y * w + x];
                        for (unsigned shift : {0u, 8u, 16u})
                            if (std::abs(int((actual >> shift) & 255) - int((expected >> shift) & 255)) > 1) {
                                std::cerr << "panel=" << b << " x=" << x << " y=" << y
                                          << " actual=" << std::hex << actual << " expected=" << expected
                                          << std::dec << "\n";
                                require(
                                    false,
                                    "side-by-side must show both complete views at their original aspect");
                            }
                    }
            }
            comparison.output_width = full_w;
            comparison.output_height = full_h;
            comparison.compare_original = false;
            require(renderer.draw(comparison, false) && renderer.readback(pixels, w, h) && pixels == stable,
                    "ending comparison must restore identical stable output with no smoothing reset");
            require(renderer.demo(0, true) && renderer.draw(options, false) &&
                        renderer.readback(pixels, w, h),
                    "restore original quad fixture after comparison");
        }
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
        // A green image border around a red interior exposes whether an extra
        // region reflects the interior or extends the boundary's real pixels.
        require(keyed->AcquireSync(0, 0) == S_OK, "stretch fixture producer key");
        std::fill(input.begin(), input.end(), 0xff0000ffu);
        for (unsigned y = 128; y < 384; ++y)
            for (unsigned x = 128; x < 384; ++x)
                if (x < 140 || x >= 372 || y < 140 || y >= 372)
                    input[y * 512 + x] = 0xff00ff00u;
        context->UpdateSubresource(texture.Get(), 0, nullptr, input.data(), 512 * 4, 0);
        context->Flush();
        keyed->ReleaseSync(1);
        require(channel.lock(), "stretch fixture publication");
        channel.data()->frame.sequence++;
        channel.data()->frame.display_time += 16666667;
        channel.data()->ready = 1;
        channel.unlock();
        require(renderer.consume(channel), "consume edge stretch fixture");
        options.mirror_softness = 0;
        auto stretch_draw = [&] {
            require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "edge stretch draw");
        };
        stretch_draw();
        const auto legacy_mirror = pixels;
        options.mirror_stretch = 100;
        stretch_draw();
        const auto stretched = pixels;
        unsigned extended = 0, preserved = 0;
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
                const auto index = std::size_t(y) * w + x;
                if (u > .01 && u < .99 && v > .01 && v < .99) {
                    require(stretched[index] == legacy_mirror[index],
                            "stretch preserves original pixels exactly");
                    ++preserved;
                }
                if (legacy_mirror[index] == 0xff0000ffu && std::max({-u, u - 1, -v, v - 1}) > .05) {
                    require(stretched[index] == 0xff00ff00u,
                            "full stretch extends the green edge instead of reflecting red interior");
                    ++extended;
                }
            }
        require(extended > 100 && preserved > 1000, "stretch fixture exercises edge extension and interior");
        for (double strength :
             {0., -100., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            options.mirror_stretch = strength;
            stretch_draw();
            require(pixels == legacy_mirror, "zero or invalid stretch preserves the legacy mirror exactly");
        }
        options.mirror_stretch = 150;
        stretch_draw();
        require(pixels == stretched, "stretch above 100 clamps to full strength");
        options.mirror_stretch = 100;
        options.compare_original = true;
        options.compare_mode = 0;
        stretch_draw();
        const auto stretch_original = pixels;
        options.mirror_stretch = 0;
        stretch_draw();
        require(pixels == stretch_original, "original bypass preserves in-bounds pixels with stretch");
        options.mirror_stretch = 100;
        options.compare_mode = 1;
        stretch_draw();
        for (unsigned y = 0; y < h; y += 7)
            for (unsigned x = 0; x < w; x += 7)
                require(pixels[y * w + x] == (x < w / 2 ? stretched : stretch_original)[y * w + x],
                        "stretch split matches stabilized and original reference images");
        options.compare_mode = 2;
        stretch_draw();
        require(pixels[w / 4] == 0xff000000u && pixels[3 * w / 4] == 0xff000000u,
                "stretch stays inside fitted side-by-side panels");
        for (unsigned panel_index = 0; panel_index < 2; ++panel_index)
            for (unsigned y = h / 4 + 1; y < 3 * h / 4 - 1; y += 19)
                for (unsigned x = 1; x < w / 2 - 1; x += 23) {
                    const auto &reference = panel_index ? stretch_original : stretched;
                    const unsigned rx = x * 2, ry = (y - h / 4) * 2;
                    const auto expected = reference[ry * w + rx];
                    if (reference[ry * w + rx + 1] == expected && reference[(ry + 1) * w + rx] == expected &&
                        reference[(ry + 1) * w + rx + 1] == expected)
                        require(pixels[y * w + panel_index * w / 2 + x] == expected,
                                "stretch side-by-side panels preserve full reference framing");
                }
        options.compare_original = false;
        options.compare_mode = 0;
        options.mirror_limit = 1;
        stretch_draw();
        const auto limited_stretch = pixels;
        options.mirror_stretch = 0;
        stretch_draw();
        for (std::size_t i = 0; i < pixels.size(); ++i)
            require((pixels[i] == 0xff000000u) == (limited_stretch[i] == 0xff000000u),
                    "edge stretch cannot extend the permitted mirror limit");
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
        // Window capture uses 2D image-warp rows, including the original half.
        const auto framed_window = pixels;
        options.compare_original = true;
        options.compare_mode = 1;
        require(
            renderer.draw(options, false) && renderer.readback(pixels, w, h) && pixels == framed_window,
            "split window capture with stabilization off must preserve both halves of asymmetric framing");
        options.compare_mode = 2;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "side-by-side window capture");
        const auto letterbox_y = unsigned((h - w / options.aspect) / 2) + 8;
        require((pixels[letterbox_y * w + 8] & 0xffffff) == 0,
                "side-by-side window capture must fit complete views without stretching");
        options.compare_original = false;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h) && pixels == framed_window,
                "stored comparison layout must not affect normal window capture");
        renderer.source_changed();
        options = Options{};
        options.output_width = 1280;
        options.output_height = 720;
        options.crop = 0;
        options.smoothing = 100;
        options.mirror_limit = 2;
        options.mirror_softness = 0;
        // Track a real translated window image so its image-warp branch exposes
        // extra pixels. The same mirror transition must work without pose input.
        for (unsigned frame_index = 0; frame_index < 2; ++frame_index) {
            for (unsigned y = 0; y < 288; ++y)
                for (unsigned x = 0; x < 512; ++x) {
                    const double px = double(x) - frame_index * 8, py = y;
                    const auto gray = std::uint32_t(
                        255 *
                        (.5 + .13 * std::sin(.19 * px + .03 * py) + .12 * std::cos(.23 * py - .01 * px) +
                         .11 * std::sin(.13 * px + .21 * py) + .10 * std::cos(.29 * px - .17 * py)));
                    window_pixels[y * 512 + x] = 0xff000000u | gray | (gray << 8) | (gray << 16);
                }
            capture_context->UpdateSubresource(window_image.Get(), 0, nullptr, window_pixels.data(), 512 * 4,
                                               0);
            processed = renderer.consume_window(window_image.Get(), 5000000000ULL + frame_index * 16666667ULL,
                                                options);
            for (int i = 0; i < 200 && !processed; ++i) {
                Sleep(1);
                processed = renderer.consume_window(nullptr, 0, options);
            }
            require(processed, "translated window analysis completes");
        }
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h),
                "window reflection reference");
        const auto window_reflection = pixels;
        options.mirror_stretch = 100;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "window edge extension");
        unsigned window_extended = 0;
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const auto index = y * w + x;
                if (x > w / 4 && x < 3 * w / 4 && y > h / 4 && y < 3 * h / 4)
                    require(pixels[index] == window_reflection[index],
                            "window stretch preserves image interior");
                if (pixels[index] != window_reflection[index])
                    ++window_extended;
            }
        require(window_extended > 100, "window image-warp path applies edge stretch to tracked overrun");
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
