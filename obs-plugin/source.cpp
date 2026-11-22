// SPDX-License-Identifier: GPL-2.0-or-later
#include "../preview/obs_output_protocol.hpp"
#include "lifecycle.hpp"
#include <obs-module.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("steadyxr-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
    return "SteadyXR 1.1.0 final GPU output source";
}
MODULE_EXPORT const char *obs_module_name(void) { return "SteadyXR"; }

namespace {
namespace protocol = steadyxr::obs_output;
using Microsoft::WRL::ComPtr;
steadyxr::obs_plugin::Lifecycle lifecycle;

std::string default_application() {
    char *path = obs_module_file("app-path.txt");
    std::string app;
    if (path) {
        std::ifstream file(std::filesystem::path(steadyxr::obs_plugin::wide(path)), std::ios::binary);
        std::getline(file, app);
        bfree(path);
    }
    if (app.compare(0, 3, "\xef\xbb\xbf") == 0) app.erase(0, 3);
    if (!app.empty() && app.back() == '\r') app.pop_back();
    return app;
}

struct Source {
    HANDLE mapping{}, metadata_mutex{}, process{};
    const protocol::Metadata *view{};
    protocol::Metadata opened{};
    gs_texture_t *shared{}, *cached{};
    std::atomic_uint32_t width{1920}, height{1080};
    std::uint64_t next_connect{}, next_open{}, last_heartbeat{}, last_sequence{};
    bool has_frame{}, warned_adapter{};

    void close_texture() {
        gs_texture_destroy(shared);
        shared = nullptr;
        gs_texture_destroy(cached);
        cached = nullptr;
        has_frame = false;
        last_sequence = 0;
        opened = {};
    }
    void disconnect() {
        close_texture();
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (metadata_mutex) CloseHandle(metadata_mutex);
        if (process) CloseHandle(process);
        mapping = metadata_mutex = process = nullptr;
        view = nullptr;
        last_heartbeat = 0;
    }
    bool connect(std::uint64_t now) {
        if (view) return true;
        if (now < next_connect) return false;
        next_connect = now + 250;
        metadata_mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, protocol::mutex_name);
        if (!metadata_mutex) return false;
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, protocol::mapping_name);
        if (mapping)
            view = static_cast<const protocol::Metadata *>(
                MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(protocol::Metadata)));
        if (!view) { disconnect(); return false; }
        return true;
    }
    bool same_adapter(const protocol::Metadata &m) {
        if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) return false;
        auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
               SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
               desc.AdapterLuid.LowPart == m.adapter_luid_low && desc.AdapterLuid.HighPart == m.adapter_luid_high;
    }
    void tick() {
        const auto now = GetTickCount64();
        if (!connect(now)) return;
        const auto lock = WaitForSingleObject(metadata_mutex, 0);
        if (lock != WAIT_OBJECT_0) {
            if (lock == WAIT_ABANDONED) { ReleaseMutex(metadata_mutex); disconnect(); }
            else if (last_heartbeat && now - last_heartbeat > protocol::stale_after_ms) disconnect();
            return;
        }
        protocol::Metadata m;
        std::memcpy(&m, view, sizeof(m));
        ReleaseMutex(metadata_mutex);
        if (!protocol::valid(m, now)) { disconnect(); return; }
        last_heartbeat = m.heartbeat_ms;
        const bool new_session = opened.epoch != m.epoch || opened.producer_pid != m.producer_pid;
        if (new_session || !process) {
            if (process) CloseHandle(process);
            process = OpenProcess(SYNCHRONIZE, FALSE, m.producer_pid);
        }
        if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) { disconnect(); return; }
        const bool changed = new_session || opened.generation != m.generation || opened.shared_handle != m.shared_handle;
        if (changed || !shared) {
            if (now < next_open) return;
            next_open = now + 1000;
            if (!same_adapter(m)) {
                if (!warned_adapter)
                    blog(LOG_WARNING, "[SteadyXR] OBS and SteadyXR must use the same Direct3D 11 GPU.");
                warned_adapter = true;
                return;
            }
            warned_adapter = false;
            auto *candidate = gs_texture_open_shared(static_cast<uint32_t>(m.shared_handle));
            if (!candidate) return;
            if (gs_texture_get_width(candidate) != m.width || gs_texture_get_height(candidate) != m.height ||
                gs_texture_get_color_format(candidate) != GS_RGBA) {
                gs_texture_destroy(candidate);
                return;
            }
            auto *copy = gs_texture_create(m.width, m.height, GS_RGBA, 1, nullptr, 0);
            if (!copy) { gs_texture_destroy(candidate); return; }
            close_texture();
            shared = candidate;
            cached = copy;
            opened = m;
            next_open = 0;
        }
        if (has_frame && last_sequence == m.frame_sequence) return;
        const auto acquired = gs_texture_acquire_sync(shared, protocol::consumer_key, 0);
        if (acquired == ETIMEDOUT) return; // Keep our previous GPU image, without owning producer's mutex.
        if (acquired != 0) { close_texture(); next_open = now + 250; return; }
        gs_copy_texture(cached, shared);
        gs_flush(); // Submit the copy before returning ownership to the producer.
        if (gs_texture_release_sync(shared, protocol::consumer_key) != 0) {
            close_texture(); next_open = now + 250; return;
        }
        has_frame = true;
        last_sequence = m.frame_sequence;
        width.store(m.width, std::memory_order_relaxed);
        height.store(m.height, std::memory_order_relaxed);
    }
};
void update(void *data, obs_data_t *settings) {
    const bool enabled = obs_data_get_bool(settings, "auto_start");
    const char *path = obs_data_get_string(settings, "application");
    if (!lifecycle.update(data, enabled, path ? path : ""))
        blog(LOG_WARNING, "[SteadyXR] Could not start the selected SteadyXR application. Check its executable path.");
}
void *create(obs_data_t *settings, obs_source_t *) {
    auto *source = new Source;
    update(source, settings);
    return source;
}
void destroy(void *data) {
    auto *s = static_cast<Source *>(data);
    lifecycle.update(s, false, "");
    obs_enter_graphics();
    s->disconnect();
    obs_leave_graphics();
    delete s;
}
void tick(void *data, float) {
    obs_enter_graphics();
    static_cast<Source *>(data)->tick();
    obs_leave_graphics();
}
void render(void *data, gs_effect_t *) {
    const auto *s = static_cast<Source *>(data);
    if (!s->has_frame || !s->cached) return;
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    const bool srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), s->cached);
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(s->cached, 0, 0, 0);
    gs_enable_framebuffer_srgb(srgb);
}
obs_properties_t *properties(void *) {
    auto *props = obs_properties_create();
    obs_properties_add_text(props, "instructions", obs_module_text("Instructions"), OBS_TEXT_INFO);
    obs_properties_add_bool(props, "auto_start", obs_module_text("AutoStart"));
    obs_properties_add_path(props, "application", obs_module_text("Application"), OBS_PATH_FILE,
                            "SteadyXR (SteadyXR.exe)", nullptr);
    obs_properties_add_text(props, "lifecycle_hint", obs_module_text("LifecycleHint"), OBS_TEXT_INFO);
    return props;
}
} // namespace

bool obs_module_load(void) {
    obs_source_info info{};
    info.id = "steadyxr_gpu_output";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = [](void *) { return obs_module_text("SourceName"); };
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = [](obs_data_t *settings) {
        obs_data_set_default_bool(settings, "auto_start", false);
        const auto path = default_application();
        obs_data_set_default_string(settings, "application", path.c_str());
    };
    info.get_properties = properties;
    info.get_width = [](void *p) { return static_cast<Source *>(p)->width.load(std::memory_order_relaxed); };
    info.get_height = [](void *p) { return static_cast<Source *>(p)->height.load(std::memory_order_relaxed); };
    info.video_tick = tick;
    info.video_render = render;
    info.video_get_color_space = [](void *, size_t, const gs_color_space *) { return GS_CS_SRGB; };
    obs_register_source(&info);
    blog(LOG_INFO, "[SteadyXR] Direct GPU source 1.1.0 loaded");
    return true;
}
void obs_module_unload(void) { lifecycle.shutdown(); }
