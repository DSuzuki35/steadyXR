#include "renderer.hpp"
#include "frame_buffer.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <wrl/client.h>
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        check(FrameBuffer::capacity(64ull * 1024 * 1024, 4) == 3, "budget includes every retained texture slot");
        check(FrameBuffer::capacity(128ull * 1024 * 1024, 4) == 1, "two-frame budget boundary");
        check(FrameBuffer::capacity(128ull * 1024 * 1024 + 1, 4) == 0, "oversized frames bypass buffering");
        check(FrameBuffer::capacity(0, 4) == 0 && FrameBuffer::capacity(1, 100) == 4, "bounded frame count");
        Renderer renderer;
        check(renderer.attach_offscreen(), "initialize renderer");
        ComPtr<ID3D11DeviceContext> context;
        renderer.device()->GetImmediateContext(&context);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapter_desc{};
        check(SUCCEEDED(renderer.device()->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
                  SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
                  SUCCEEDED(adapter->GetDesc(&adapter_desc)), "adapter");
        Channel channel;
        check(channel.open(true, L"Local\\SteadyXR-director-test-" +
                                     std::to_wstring(GetCurrentProcessId())), "channel");
        Frame frame{};
        frame.generation = frame.session = frame.reference_epoch = 1;
        frame.producer_pid = GetCurrentProcessId();
        frame.adapter = adapter_desc.AdapterLuid;
        frame.view_count = frame.overlay_count = 1;
        frame.display_time = 1000000000;
        frame.overlays[0].type = 1;
        frame.overlays[0].width = frame.overlays[0].height = 1;
        frame.overlays[0].position[2] = -1;
        ComPtr<ID3D11Texture2D> textures[2];
        ComPtr<IDXGIKeyedMutex> keys[2];
        for (unsigned i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = d.Height = 64;
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            check(SUCCEEDED(renderer.device()->CreateTexture2D(&d, nullptr, &textures[i])), "source texture");
            ComPtr<IDXGIResource> resource;
            HANDLE handle{};
            check(SUCCEEDED(textures[i].As(&resource)) && SUCCEEDED(textures[i].As(&keys[i])) &&
                      SUCCEEDED(resource->GetSharedHandle(&handle)), "source share");
            auto &v = i ? frame.overlays[0].image : frame.views[0];
            v.texture_handle = reinterpret_cast<std::uint64_t>(handle);
            v.width = v.height = v.rect_width = v.rect_height = 64;
            v.format = d.Format;
        }
        auto publish = [&](std::uint32_t color, std::uint32_t overlay, bool patterned = false) {
            for (unsigned i = 0; i < 2; ++i) {
                check(keys[i]->AcquireSync(0, 0) == S_OK, "acquire source");
                std::vector<std::uint32_t> pixels(64 * 64, i ? overlay : color);
                if (patterned && i == 0)
                    for (unsigned y = 0; y < 64; ++y)
                        for (unsigned x = 0; x < 64; ++x)
                            pixels[y * 64 + x] = x < 32 ? 0xff0000ff : 0xff00ff00;
                context->UpdateSubresource(textures[i].Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
            }
            context->Flush();
            for (auto &key : keys)
                check(SUCCEEDED(key->ReleaseSync(1)), "release source");
            ++frame.sequence;
            frame.display_time += 10000000;
            check(channel.lock(), "packet lock");
            channel.data()->frame = frame;
            channel.data()->ready = 1;
            channel.unlock();
            check(renderer.consume(channel), "consume source");
        };
        Options options;
        options.output_width = 320;
        options.output_height = 180;
        options.crop = 0;
        options.mirror = false;
        options.overlays.mode = 1;
        std::vector<std::uint32_t> pixels;
        unsigned w{}, h{};
        auto draw = [&] {
            check(renderer.draw(options) && renderer.readback(pixels, w, h), "draw/readback");
        };
        auto expect = [&](std::uint32_t background, std::uint32_t overlay) {
            check(pixels[90 * w + 10] == background, "delayed background pixels");
            check(pixels[90 * w + 160] == overlay, "overlay uses the same delayed frame");
        };
        publish(0xff0000ff, 0xff00ff00);
        draw();
        expect(0xff0000ff, 0xff00ff00);
        check(renderer.director_status().buffered_bytes == 0, "disabled path has no GPU history");
        {
            ComPtr<ID3D11Texture2D> input[FrameBuffer::max_images];
            ComPtr<ID3D11ShaderResourceView> views[FrameBuffer::max_images];
            for (unsigned i = 0; i < 2; ++i) {
                check(keys[i]->AcquireSync(0, 0) == S_OK, "buffer fixture source ownership");
                input[i] = textures[i];
                check(SUCCEEDED(renderer.device()->CreateShaderResourceView(input[i].Get(), nullptr, &views[i])),
                      "buffer fixture view");
            }
            FrameBuffer buffer;
            auto moving = frame;
            buffer.update(renderer.device(), context.Get(), moving, input, views, 2, -1, 0);
            check(buffer.target() != nullptr, "warmup supplies a stable target immediately");
            const auto held_target = *buffer.target();
            for (unsigned i = 0; i < 2; ++i) {
                ++moving.sequence;
                moving.display_time += 10000000;
                moving.views[0].orientation[1] = .1f * (i + 1);
                moving.views[0].orientation[3] = std::sqrt(1 - moving.views[0].orientation[1] *
                                                                moving.views[0].orientation[1]);
                buffer.update(renderer.device(), context.Get(), moving, input, views, 2, -1, 0);
                check(vrst::angle_between(held_target, *buffer.target()) < 1e-8,
                      "future warmup poses cannot change the held output target");
            }
            ++moving.sequence;
            moving.display_time += 10000000;
            buffer.update(renderer.device(), context.Get(), moving, input, views, 2, -1, 0);
            check(vrst::angle_between(held_target, *buffer.target()) > .01,
                  "next displayed frame uses future poses");
            context->Flush();
            for (auto &key : keys)
                check(SUCCEEDED(key->ReleaseSync(0)), "release buffer fixture ownership");
        }
        options.director.lookahead_enabled = true;
        options.director.lookahead_frames = 2;
        draw();
        check(renderer.director_status().buffered_bytes > 0, "enabled history allocates");
        publish(0xff00ff00, 0xffff0000);
        draw();
        expect(0xff0000ff, 0xff00ff00);
        publish(0xffff0000, 0xffffffff);
        draw();
        expect(0xff0000ff, 0xff00ff00);
        check(renderer.director_status().delay_ms == 20, "actual timestamp delay");
        const auto allocated = renderer.director_status().buffered_bytes;
        draw();
        expect(0xff0000ff, 0xff00ff00);
        check(renderer.director_status().buffered_bytes == allocated, "redraw does not grow history");
        publish(0xffffffff, 0xff0000ff);
        draw();
        expect(0xff00ff00, 0xffff0000);
        check(renderer.director_status().buffered_bytes == allocated, "steady state recycles GPU slots");
        ++frame.reference_epoch;
        publish(0xffff0000, 0xff00ff00);
        draw();
        expect(0xffff0000, 0xff00ff00);
        check(renderer.director_status().delay_ms == 0, "recenter flushes old frames");
        options.director.lookahead_enabled = false;
        draw();
        check(renderer.director_status().buffered_bytes == 0, "disable releases GPU history");
        options.director.lookahead_enabled = true;
        draw();
        check(renderer.director_status().buffered_bytes > 0, "reenable");
        options.spectator.bypass = true;
        draw();
        check(renderer.director_status().buffered_bytes == 0, "master bypass releases GPU history");
        options.spectator.bypass = false;
        draw();
        check(renderer.draw(options, true, L"Waiting") && renderer.readback(pixels, w, h), "waiting frame");
        check(renderer.director_status().buffered_bytes == 0, "source wait releases GPU history");
        draw();
        renderer.source_changed();
        check(renderer.director_status().buffered_bytes == 0, "source change releases history");
        options.crop = 30;
        options.smoothing = 0;
        options.overlays.mode = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto q = vrst::axis_angle({0, 1, 0}, .2 * i);
            frame.views[0].orientation[1] = float(q.y);
            frame.views[0].orientation[3] = float(q.w);
            publish(0xff0000ff, 0xff00ff00, true);
            draw();
        }
        const auto enhanced_pixels = pixels;
        options.compare_original = true;
        draw();
        unsigned changed_pixels{};
        for (std::size_t i = 0; i < pixels.size(); ++i)
            changed_pixels += pixels[i] != enhanced_pixels[i];
        check(changed_pixels > 100, "future pose changes real rendered geometry versus original camera");
        check(renderer.director_status().delay_ms == 20, "A/B retains video delay");
        options.compare_original = false;
        renderer.source_changed();
        D3D11_TEXTURE2D_DESC window_desc{};
        window_desc.Width = window_desc.Height = 64;
        window_desc.MipLevels = window_desc.ArraySize = window_desc.SampleDesc.Count = 1;
        window_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        window_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<std::uint32_t> window_pixels(64 * 64, 0xff00ff00);
        D3D11_SUBRESOURCE_DATA initial{window_pixels.data(), 64 * 4, 0};
        ComPtr<ID3D11Texture2D> window_texture;
        check(SUCCEEDED(renderer.device()->CreateTexture2D(&window_desc, &initial, &window_texture)),
              "window fixture texture");
        options.director.auto_crop_enabled = true;
        // Image analysis readback is intentionally asynchronous; wait only in this test.
        for (unsigned i = 0; i < 100 && !renderer.has_frame(); ++i) {
            renderer.consume_window(window_texture.Get(), 2000000000ull + i * 10000000ull, options);
            Sleep(1);
        }
        check(renderer.has_frame(), "window analysis completes");
        draw();
        check(pixels[90 * w + 160] == 0xff00ff00, "window director keeps source pixels");
        check(renderer.director_status().buffered_bytes == 0, "window capture bypasses XR lookahead");
        std::cout << "Director GPU: delayed image/overlay association, redraw, recycle, recenter, bypass and release passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
