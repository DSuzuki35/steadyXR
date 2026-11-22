// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../preview/obs_output_protocol.hpp"
#include <obs.h>
#include <graphics/vec4.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace protocol = steadyxr::obs_output;
static void require(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
struct Producer {
    HANDLE mapping{}, mutex{};
    protocol::Metadata *view{};
    protocol::Metadata m;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<IDXGIKeyedMutex> keyed;
    Producer() {
        mutex = CreateMutexW(nullptr, FALSE, protocol::mutex_name);
        require(mutex && GetLastError() != ERROR_ALREADY_EXISTS, "isolated test: close SteadyXR and OBS first (IPC already exists)");
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(m), protocol::mapping_name);
        require(mapping && GetLastError() != ERROR_ALREADY_EXISTS, "isolated test mapping");
        view = static_cast<protocol::Metadata *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(m)));
        require(view, "map metadata");
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                 D3D11_SDK_VERSION, &device, nullptr, &context)), "producer D3D11");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        require(SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetDesc(&desc)), "producer adapter");
        m.adapter_luid_low = desc.AdapterLuid.LowPart;
        m.adapter_luid_high = desc.AdapterLuid.HighPart;
        m.producer_pid = GetCurrentProcessId();
        m.epoch = 1;
        m.flags = protocol::flag_active;
        resize(640, 360);
    }
    ~Producer() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
    void publish() {
        require(WaitForSingleObject(mutex, 2000) == WAIT_OBJECT_0, "publish lock");
        std::memcpy(view, &m, sizeof(m));
        ReleaseMutex(mutex);
    }
    void resize(unsigned w, unsigned h) {
        target.Reset(); keyed.Reset(); texture.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        require(SUCCEEDED(device->CreateTexture2D(&d, nullptr, &texture)), "shared texture");
        D3D11_RENDER_TARGET_VIEW_DESC rt{};
        rt.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        require(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), &rt, &target)), "producer sRGB target");
        ComPtr<IDXGIResource> resource;
        HANDLE handle{};
        require(SUCCEEDED(texture.As(&keyed)) && SUCCEEDED(texture.As(&resource)) &&
                SUCCEEDED(resource->GetSharedHandle(&handle)), "legacy GPU handle");
        m.shared_handle = reinterpret_cast<uint64_t>(handle);
        m.width = w; m.height = h; ++m.generation;
    }
    bool frame(float r, float g, float b) {
        HRESULT hr = keyed->AcquireSync(0, 0);
        if (hr == WAIT_TIMEOUT) hr = keyed->AcquireSync(1, 0);
        if (hr != S_OK) return false;
        const float color[4]{r, g, b, 1};
        context->ClearRenderTargetView(target.Get(), color);
        context->Flush();
        require(SUCCEEDED(keyed->ReleaseSync(1)), "release producer");
        ++m.frame_sequence;
        m.heartbeat_ms = GetTickCount64();
        publish();
        return true;
    }
};
struct Pixel { unsigned char r{}, g{}, b{}, a{}; };
static Pixel capture(obs_source_t *s) {
    obs_enter_graphics();
    auto *rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    require(gs_texrender_begin(rt, 32, 32), "capture target");
    vec4 clear{};
    gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
    gs_ortho(0, static_cast<float>(obs_source_get_width(s)), 0, static_cast<float>(obs_source_get_height(s)), -1, 1);
    obs_source_video_render(s);
    gs_texrender_end(rt);
    auto *stage = gs_stagesurface_create(32, 32, GS_RGBA);
    gs_stage_texture(stage, gs_texrender_get_texture(rt));
    uint8_t *bytes{}; uint32_t stride{};
    require(gs_stagesurface_map(stage, &bytes, &stride), "map captured image");
    Pixel result{};
    std::memcpy(&result, bytes + 16 * stride + 16 * 4, 4);
    gs_stagesurface_unmap(stage);
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(rt);
    obs_leave_graphics();
    return result;
}
template<class F> static void until(F predicate, const char *what, int timeout = 1800) {
    const auto end = GetTickCount64() + timeout;
    while (GetTickCount64() < end) {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error(what);
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    try {
        protocol::Metadata invalid;
        require(!protocol::valid(invalid, GetTickCount64()), "empty metadata rejected");
        std::filesystem::current_path(std::string(argv[1]) + "/bin/64bit");
        require(obs_startup("en-US", nullptr, nullptr), "libobs startup");
        obs_video_info vi{};
        const std::string graphics = std::string(argv[1]) + "/bin/64bit/libobs-d3d11.dll";
        vi.graphics_module = graphics.c_str(); vi.fps_num = 60; vi.fps_den = 1;
        vi.base_width = vi.output_width = 1280; vi.base_height = vi.output_height = 720;
        vi.output_format = VIDEO_FORMAT_RGBA; vi.colorspace = VIDEO_CS_709; vi.range = VIDEO_RANGE_FULL;
        vi.scale_type = OBS_SCALE_BILINEAR;
        require(obs_reset_video(&vi) == OBS_VIDEO_SUCCESS, "libobs D3D11 startup");
        obs_module_t *module{};
        require(obs_open_module(&module, argv[2], argv[3]) == MODULE_SUCCESS && obs_init_module(module), "plugin load");
        auto *source = obs_source_create("steadyxr_gpu_output", "SteadyXR test", nullptr, nullptr);
        require(source, "source registration");
        require(capture(source).a == 0, "disconnected source is transparent");
        {
            Producer producer;
            until([&] { producer.frame(.25f, .5f, .75f); return capture(source).a == 255; }, "first GPU frame");
            const auto pixel = capture(source);
            require(pixel.r >= 135 && pixel.r <= 139 && pixel.g >= 186 && pixel.g <= 190 && pixel.b >= 223 && pixel.b <= 227,
                    "sRGB pixel values preserve final canvas gamma");
            require(obs_source_get_width(source) == 640 && obs_source_get_height(source) == 360, "native dimensions");
            // A held texture must leave OBS rendering the previous frame without waiting.
            until([&] { return producer.keyed->AcquireSync(1, 0) == S_OK; }, "producer acquire after consumption");
            ++producer.m.frame_sequence; // Simulate notification while the next GPU frame is still owned.
            producer.publish();
            const auto start = GetTickCount64();
            for (int i = 0; i < 20; ++i) require(capture(source).r == pixel.r, "retain cached frame under GPU contention");
            require(GetTickCount64() - start < 1000, "consumer never waits for keyed mutex");
            producer.keyed->ReleaseSync(0);
            producer.resize(800, 450);
            until([&] { producer.frame(1, 0, 0); return obs_source_get_width(source) == 800 && capture(source).r == 255; },
                  "resize/generation reopen");
            auto *second = obs_source_create("steadyxr_gpu_output", "Second SteadyXR", nullptr, nullptr);
            until([&] { return capture(second).r == 255; }, "new source receives paused image without producer redraw");
            until([&] { producer.frame(0, 1, 0); return capture(second).g == 255; }, "multiple source readers");
            require(WaitForSingleObject(producer.mutex, 1000) == WAIT_OBJECT_0, "test metadata contention");
            const auto locked = GetTickCount64();
            for (int i = 0; i < 20; ++i) require(capture(second).g == 255, "metadata lock retains cached frame");
            ReleaseMutex(producer.mutex);
            require(GetTickCount64() - locked < 1000, "metadata lock cannot block video rendering");
            producer.m.flags = 0; producer.publish();
            until([&] { return capture(source).a == 0 && capture(second).a == 0; }, "inactive producer clears frames");
            producer.m.flags = protocol::flag_active;
            until([&] { producer.frame(0, 0, 1); return capture(source).b == 255; }, "same-session resume");
            producer.m.heartbeat_ms = GetTickCount64() - protocol::stale_after_ms - 1; producer.publish();
            until([&] { return capture(source).a == 0; }, "stale producer disconnect");
            ++producer.m.epoch;
            until([&] { producer.frame(1, 1, 0); return capture(source).r == 255 && capture(source).g == 255; }, "producer restart");
            producer.m.producer_pid = 0xffffffff; producer.publish();
            until([&] { return capture(source).a == 0; }, "dead producer PID rejected despite fresh heartbeat");
            producer.m.producer_pid = GetCurrentProcessId(); ++producer.m.epoch;
            until([&] { producer.frame(1, 0, 1); return capture(source).b == 255; }, "reconnect after process death");
            obs_source_release(second);
        }
        obs_source_release(source);
        obs_shutdown();
        std::cout << "PASS: GPU copy, sRGB, nonblocking contention, resize, multi-source, hide/resume, stale/dead/restarted producer\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
