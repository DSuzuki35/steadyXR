#include "openxr_source.hpp"
#include "../preview/build_expiry.hpp"
#include "openxr_source_settings.hpp"
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <wrl/client.h>

namespace {
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
struct Source {
    obs_source_t *source{};
    std::mutex settings_mutex;
    vrst_plugin::OpenXRSettings settings;
    std::uint64_t revision{1};
    std::atomic<bool> reset_requested{false};
    std::atomic<std::uint64_t> dimensions{(std::uint64_t(3840) << 32) | 2160};
    // Graphics resources and capture state belong to the OBS graphics thread.
    Channel channel;
    std::unique_ptr<Renderer> renderer;
    gs_texture_t *shared{}, *output{};
    std::uint64_t handle{}, generation{}, applied_revision{}, last_received{}, output_pending_since{},
        draw_failure_since{};
    unsigned eye{};
    bool active{}, expired{}, valid_output{}, output_pending{}, request_fresh{};
    LUID adapter{};
    std::wstring message, attach_error, capture_error, channel_name;
    void fallback_card(const std::wstring &text, unsigned width, unsigned height) {
        // This remains available when the offscreen D3D device cannot initialize.
        // GDI draws only a small status card; live game pixels never cross the CPU.
        constexpr unsigned w = 512, h = 64;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = w;
        bitmap.bmiHeader.biHeight = -int(h);
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void *bits{};
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP dib = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !dib || !bits) {
            if (dib)
                DeleteObject(dib);
            if (dc)
                DeleteDC(dc);
            return;
        }
        auto old = SelectObject(dc, dib);
        RECT rect{0, 0, w, h};
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        auto *pixels = static_cast<std::uint32_t *>(bits);
        for (unsigned i = 0; i < w * h; ++i)
            pixels[i] |= 0xff000000u;
        const uint8_t *bytes = static_cast<const uint8_t *>(bits);
        gs_texture_destroy(output);
        output = gs_texture_create(w, h, GS_RGBA, 1, &bytes, 0);
        SelectObject(dc, old);
        DeleteObject(dib);
        DeleteDC(dc);
        valid_output = output != nullptr;
        dimensions.store((std::uint64_t(width) << 32) | height);
    }
    void clear_textures() {
        gs_texture_destroy(shared);
        shared = nullptr;
        gs_texture_destroy(output);
        output = nullptr;
        handle = generation = 0;
        valid_output = false;
        output_pending = false;
        output_pending_since = 0;
    }
    bool attach() {
        auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> dxgi_adapter;
        DXGI_ADAPTER_DESC desc{};
        if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11 || !device ||
            FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
            FAILED(dxgi_device->GetAdapter(&dxgi_adapter)) || FAILED(dxgi_adapter->GetDesc(&desc))) {
            attach_error = L"SteadyXR requires the OBS Direct3D 11 renderer";
            return false;
        }
        adapter = desc.AdapterLuid;
        renderer = std::make_unique<Renderer>();
        if (!renderer->attach_offscreen(&adapter)) {
            attach_error = renderer->error();
            renderer.reset();
            return false;
        }
        attach_error.clear();
        return true;
    }
    // Every draw publishes key 1, even a status card. Drain it before another
    // draw, and retain only a private OBS texture after releasing the writer.
    bool copy_output(bool copy = true) {
        if (!renderer || !output_pending)
            return true;
        const auto next_handle = renderer->output_handle(), next_generation = renderer->output_generation();
        if (!shared || handle != next_handle || generation != next_generation) {
            gs_texture_destroy(shared);
            shared = nullptr;
            handle = next_handle;
            generation = next_generation;
            shared = gs_texture_open_shared(static_cast<uint32_t>(handle));
        }
        if (!shared)
            return false;
        const int acquired = gs_texture_acquire_sync(shared, 1, 0);
        if (acquired != 0)
            return false;
        bool copied = !copy;
        if (copy) {
            const auto w = gs_texture_get_width(shared), h = gs_texture_get_height(shared);
            if (!output || gs_texture_get_width(output) != w || gs_texture_get_height(output) != h) {
                gs_texture_destroy(output);
                output = gs_texture_create(w, h, GS_RGBA, 1, nullptr, 0);
                valid_output = false;
            }
            if (output) {
                gs_copy_texture(output, shared);
                gs_flush();
                copied = true;
            }
        }
        const int released = gs_texture_release_sync(shared, 0);
        output_pending = released != 0;
        if (!output_pending)
            output_pending_since = 0;
        valid_output = copy && copied && released == 0;
        return copied && released == 0;
    }
};
void update(void *data, obs_data_t *settings) {
    auto &s = *static_cast<Source *>(data);
    auto next = vrst_plugin::openxr_settings(settings);
    std::lock_guard<std::mutex> lock(s.settings_mutex);
    s.settings = next;
    ++s.revision;
}
void *create(obs_data_t *settings, obs_source_t *source) {
    auto s = std::make_unique<Source>();
    s->source = source;
    update(s.get(), settings);
    s->expired = build_expired();
    // The override is only for isolated integration tests / explicit instances.
    wchar_t channel_name[1024]{};
    const auto length = GetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_CHANNEL", channel_name, 1024);
    s->channel_name = length && length < 1024 ? channel_name : default_channel;
    s->channel.open(true, s->channel_name);
    return s.release();
}
void destroy(void *data) {
    auto *s = static_cast<Source *>(data);
    obs_enter_graphics();
    s->channel.release_consumer();
    s->copy_output(false);
    s->clear_textures();
    s->renderer.reset();
    s->channel.close();
    obs_leave_graphics();
    delete s;
}
void tick(void *data, float) {
    auto &s = *static_cast<Source *>(data);
    vrst_plugin::OpenXRSettings c;
    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(s.settings_mutex);
        c = s.settings;
        revision = s.revision;
    }
    obs_enter_graphics();
    const auto now = GetTickCount64();
    s.expired = s.expired || build_expired();
    const bool requested = obs_source_showing(s.source) && !c.paused && !s.expired;
    if (requested && !s.channel.data())
        s.channel.open(true, s.channel_name);
    const bool claimed = requested && s.channel.heartbeat(c.options.eye, c.fps);
    // heartbeat's zero-timeout mutex probe can fail while our existing claim
    // remains valid. Skip consumption that tick without resetting pose history.
    const bool owned = requested && (claimed || s.channel.owns_consumer());
    const bool changed = revision != s.applied_revision;
    const bool source_changed = owned != s.active || c.options.eye != s.eye;
    bool dirty = changed || source_changed || !s.valid_output;
    s.dimensions.store((std::uint64_t(c.options.output_width) << 32) | c.options.output_height);
    if (!requested)
        s.channel.release_consumer();
    if (!s.renderer && !s.attach()) {
        if (s.message != s.attach_error || !s.valid_output) {
            s.message = s.attach_error;
            s.fallback_card(s.message, c.options.output_width, c.options.output_height);
            blog(LOG_ERROR, "SteadyXR OpenXR source could not initialize its OBS-adapter renderer");
        }
        obs_leave_graphics();
        return;
    }
    if (source_changed) {
        s.renderer->source_changed();
        s.last_received = 0;
        s.valid_output = false;
        s.capture_error.clear();
        if (c.options.eye != s.eye)
            s.request_fresh = true;
    }
    if (claimed && s.request_fresh && s.channel.lock()) {
        if (s.channel.owns_consumer() && s.channel.data()->consumer_pid == GetCurrentProcessId()) {
            s.channel.data()->ready = 2;
            s.request_fresh = false;
        }
        s.channel.unlock();
    }
    if (s.reset_requested.exchange(false)) {
        s.renderer->reset();
        dirty = true;
    }
    s.active = owned;
    s.eye = c.options.eye;
    s.applied_revision = revision;
    std::wstring message;
    if (s.expired)
        message = L"SteadyXR build expired - install a fresh build";
    else if (c.paused)
        message = L"SteadyXR capture paused";
    else if (!requested)
        message = L"SteadyXR capture inactive";
    else if (!s.channel.data())
        message = L"Could not open the SteadyXR capture channel";
    else if (!owned)
        message = L"SteadyXR capture busy - close the other capture";
    else {
        // A reconnect request invalidates the previous image immediately.
        bool reconnect = false;
        if (claimed && s.channel.lock()) {
            reconnect = s.channel.data()->ready == 2;
            s.channel.unlock();
        }
        if (reconnect && s.renderer->has_frame()) {
            s.renderer->source_changed();
            s.last_received = 0;
            s.valid_output = false;
            dirty = true;
        }
        if (claimed && !s.request_fresh && s.renderer->consume(s.channel, true)) {
            s.last_received = now;
            s.capture_error.clear();
            dirty = true;
        } else if (!s.renderer->error().empty())
            s.capture_error = s.renderer->error();
        if (s.last_received && now - s.last_received >= 2000) {
            s.renderer->source_changed();
            s.last_received = 0;
            s.valid_output = false;
            dirty = true;
        }
        if (!s.renderer->has_frame())
            message = s.capture_error.empty() ? L"Waiting for OpenXR frames" : s.capture_error;
    }
    if (message != s.message) {
        s.message = message;
        dirty = true;
        s.valid_output = false;
    }
    if (s.output_pending && !s.copy_output()) {
        s.valid_output = false;
        // An output-open/device failure must not strand writer key 1 forever.
        // Replacing both ends discards that generation and retries next tick.
        if (now - s.output_pending_since >= 500) {
            s.clear_textures();
            s.renderer.reset();
            s.last_received = 0;
            s.active = false;
        }
        obs_leave_graphics();
        return;
    }
    if (dirty) {
        const bool drawn = s.renderer->draw(c.options, false, message.empty() ? nullptr : message.c_str());
        // Failed draws return key 0 without publishing a partial output.
        if (drawn && s.renderer->output_handle()) {
            s.draw_failure_since = 0;
            s.output_pending = true;
            s.output_pending_since = now;
            if (!s.copy_output())
                s.valid_output = false;
        } else {
            s.valid_output = false;
            s.capture_error = s.renderer->error();
            s.renderer->source_changed();
            if (!s.draw_failure_since)
                s.draw_failure_since = now;
            else if (now - s.draw_failure_since >= 500) {
                s.clear_textures();
                s.renderer.reset();
                s.last_received = s.draw_failure_since = 0;
                s.active = false;
            }
        }
    }
    obs_leave_graphics();
}
void render(void *data, gs_effect_t *) {
    auto &s = *static_cast<Source *>(data);
    const auto dims = s.dimensions.load();
    if (!s.valid_output || !s.output) {
        auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
        vec4 black{};
        black.w = 1;
        gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &black);
        while (gs_effect_loop(effect, "Solid"))
            gs_draw_sprite(nullptr, 0, uint32_t(dims >> 32), uint32_t(dims));
        return;
    }
    const bool previous = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(true);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), s.output);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(s.output, 0, uint32_t(dims >> 32), uint32_t(dims));
    gs_enable_framebuffer_srgb(previous);
}
bool reset(obs_properties_t *, obs_property_t *, void *data) {
    if (data)
        static_cast<Source *>(data)->reset_requested.store(true);
    return false;
}
bool comfort(obs_properties_t *, obs_property_t *, void *data) {
    if (!data)
        return false;
    auto *source = static_cast<Source *>(data)->source;
    auto *settings = obs_source_get_settings(source);
    auto c = vrst_plugin::openxr_settings(settings);
    apply_stream_comfort(c.options);
    vrst_plugin::openxr_store(settings, c);
    obs_source_update(source, settings);
    obs_data_release(settings);
    return true;
}
bool import_settings(obs_properties_t *props, obs_property_t *, void *data) {
    if (!data)
        return false;
    auto *source = static_cast<Source *>(data)->source;
    auto *settings = obs_source_get_settings(source);
    const char *path = obs_data_get_string(settings, "import_path");
    const int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(length ? length : 1, L'\0');
    if (length)
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), length);
    if (!wide.empty())
        wide.pop_back();
    std::string error;
    if (vrst_plugin::openxr_import(settings, wide, error)) {
        obs_source_update(source, settings);
        error = "Settings imported into this OBS source. Extra render area stays unchanged.";
    }
    obs_property_set_description(obs_properties_get(props, "import_status"), error.c_str());
    obs_data_release(settings);
    return true;
}
obs_properties_t *properties(void *data) {
    auto *p = obs_properties_create();
    obs_properties_add_text(p, "help",
                            "Capture OpenXR directly. Enable the SteadyXR OpenXR layer before starting the "
                            "game. One active capture per channel.",
                            OBS_TEXT_INFO);
    const auto *deadline = build_expiry_text();
    const int count = WideCharToMultiByte(CP_UTF8, 0, deadline, -1, nullptr, 0, nullptr, nullptr);
    std::string expiry(count > 0 ? count : 1, '\0');
    if (count > 0)
        WideCharToMultiByte(CP_UTF8, 0, deadline, -1, expiry.data(), count, nullptr, nullptr);
    expiry.pop_back();
    const auto expiry_label = std::string("SteadyXR 0.7.0 build expires: ") + expiry;
    obs_properties_add_text(p, "build_expiry", expiry_label.c_str(), OBS_TEXT_INFO);
    vrst_plugin::openxr_add_properties(p);
    obs_properties_add_button2(p, "reset", "Reset stabilization", reset, data);
    obs_properties_add_button2(p, "comfort", "Stream Comfort", comfort, data);
    obs_properties_add_path(p, "import_path", "SteadyXR settings or preset", OBS_PATH_FILE,
                            "INI settings (*.ini)", nullptr);
    obs_properties_add_button2(p, "import", "Import settings", import_settings, data);
    obs_properties_add_text(p, "import_status",
                            "Import reads the INI file without changing it or the game's extra render area.",
                            OBS_TEXT_INFO);
    return p;
}
const char *name(void *) {
    return "SteadyXR OpenXR Capture";
}
uint32_t width(void *data) {
    return uint32_t(static_cast<Source *>(data)->dimensions.load() >> 32);
}
uint32_t height(void *data) {
    return uint32_t(static_cast<Source *>(data)->dimensions.load());
}
} // namespace
void register_openxr_source() {
    obs_source_info info{};
    info.id = "steadyxr_openxr_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.get_defaults = vrst_plugin::openxr_defaults;
    info.get_properties = properties;
    info.update = update;
    info.video_tick = tick;
    info.video_render = render;
    info.get_width = width;
    info.get_height = height;
    obs_register_source(&info);
}
