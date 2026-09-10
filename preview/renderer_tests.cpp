#include "renderer.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <wrl/client.h>
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
void require(bool value, const char *what) {
    if (!value)
        throw std::runtime_error(what);
}
int main(int argc, char **argv) {
    try {
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
        options.crop = 0;
        options.stabilize = false;
        require(renderer.demo(0, true) && renderer.draw(options, false), "quad demo draw");
        std::vector<std::uint32_t> pixels;
        unsigned w, h;
        require(renderer.readback(pixels, w, h), "GPU readback");
        require(w == 3840 && h == 2160, "default rendering must be exactly 4K");
        auto center = pixels[(h / 2) * w + w / 2];
        auto side = pixels[(h / 2) * w + w / 8];
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
        options.mirror_limit = 1;
        require(renderer.draw(options, false) && renderer.readback(pixels, w, h), "small mirror draw");
        require((pixels[0] & 0xffffff) == 0, "overrun beyond mirror limit must be black");
        options = Options{};
        options.crop = 0;
        options.stabilize = false;
        options.output_width = 1280;
        options.output_height = 720;
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
