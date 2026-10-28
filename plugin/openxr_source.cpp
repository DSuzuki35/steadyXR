#include "openxr_source.hpp"
#include "../preview/build_expiry.hpp"
#include "../preview/overlay_settings.hpp"
#include "openxr_source_settings.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <mutex>
#include <wrl/client.h>

namespace {
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
enum class Action {
    reset,
    reset_horizon,
    pause,
    compare,
    learn_chat,
    cancel_chat,
    clear_chat,
    natural,
    comfort,
    strong,
    preset_1,
    preset_2,
    preset_3
};
struct ActionInfo {
    const char *name, *label;
    Action action;
};
constexpr ActionInfo actions[]{
    {"reset", "Reset stabilization", Action::reset},
    {"reset_horizon", "Reset horizon", Action::reset_horizon},
    {"pause", "Pause / resume capture", Action::pause},
    {"compare", "Compare A/B (20 seconds)", Action::compare},
    {"learn_chat", "Learn chat zone (look at chat and hold steady)", Action::learn_chat},
    {"cancel_chat", "Cancel chat learning", Action::cancel_chat},
    {"clear_chat", "Clear learned chat zone", Action::clear_chat},
    {"natural", "Natural", Action::natural},
    {"comfort", "Stream Comfort", Action::comfort},
    {"strong", "Strong leveling", Action::strong},
    {"preset_1", "Load saved preset 1", Action::preset_1},
    {"preset_2", "Load saved preset 2", Action::preset_2},
    {"preset_3", "Load saved preset 3", Action::preset_3}};
struct Source {
    obs_source_t *source{};
    std::mutex settings_mutex;
    vrst_plugin::OpenXRSettings settings;
    std::uint64_t revision{1};
    std::atomic<bool> reset_requested{false};
    std::atomic<bool> horizon_reset_requested{false};
    enum class ChatCommand { none, learn, cancel, clear };
    std::atomic<ChatCommand> chat_command{ChatCommand::none};
    std::atomic<std::uint64_t> compare_until{0};
    std::array<obs_hotkey_id, std::size(actions)> hotkeys{};
    std::mutex status_mutex;
    std::string capture_status{"Inactive | 0.0 FPS / 60 requested"};
    std::string spectator_status{"Gaze disabled"};
    std::string director_status{"Director: disabled"};
    std::string action_status{"Choose an INI preset to import into this source."};
    std::atomic<std::uint64_t> dimensions{(std::uint64_t(3840) << 32) | 2160};
    // Graphics resources and capture state belong to the OBS graphics thread.
    Channel channel;
    std::unique_ptr<Renderer> renderer;
    gs_texture_t *shared{}, *output{};
    std::uint64_t handle{}, generation{}, applied_revision{}, last_received{}, output_pending_since{},
        draw_failure_since{};
    unsigned eye{};
    bool active{}, expired{}, valid_output{}, output_pending{}, request_fresh{};
    bool comparing{};
    unsigned requested_fps{60}, received_frames{};
    std::uint64_t fps_since{};
    double captured_fps{};
    std::string connection{"Inactive"};
    LUID adapter{};
    std::wstring message, attach_error, capture_error, channel_name;
    void feedback(const std::string &text, bool refresh_properties) {
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            action_status = text;
        }
        if (refresh_properties)
            obs_source_update_properties(source);
    }
    void publish_status() {
        const auto now = GetTickCount64();
        if (!fps_since)
            fps_since = now;
        if (now - fps_since >= 1000) {
            captured_fps = 1000. * received_frames / (now - fps_since);
            received_frames = 0;
            fps_since = now;
        }
        if (connection != "Connected") {
            captured_fps = 0;
            received_frames = 0;
        }
        char rate[96]{};
        std::snprintf(rate, sizeof(rate), " | %.1f FPS / %u requested", captured_fps, requested_fps);
        std::string text = connection + rate;
        std::string camera = "Gaze disabled / no OpenXR eye data";
        std::string director = "Director: inactive";
        if (renderer) {
            const auto d = renderer->director_status();
            char detail[256]{};
            const char *games[]{"no speed data", "iRacing", "Le Mans Ultimate"};
            std::snprintf(detail, sizeof(detail),
                          "Director: %.1f ms video delay / %u frames / %.1f MiB | Crop %.1f%% | Speed crop %.1f%% | %s",
                          d.delay_ms, d.buffered_frames, double(d.buffered_bytes) / (1024 * 1024),
                          d.auto_crop_percent, d.speed_crop_percent,
                          d.speed_available ? games[std::min(d.telemetry_game, 2u)] : games[0]);
            director = detail;
            if (d.buffer_limited)
                director += " | Lookahead reduced by memory limit";
            const char *capabilities[]{"Gaze disabled / no OpenXR eye data",
                                       "Eye tracking unsupported by runtime",
                                       "Waiting for game action set attachment",
                                       "Eye tracking inactive",
                                       "Eye tracking available",
                                       "Eye tracking has unknown sample time"};
            const auto status = renderer->spectator_status();
            camera = capabilities[std::min(renderer->gaze_capability(), 5u)];
            const char *states[]{"disabled", "untracked", "stale", "fixating", "valid", "excluded"};
            camera += std::string(" | Gaze: ") + states[std::min(unsigned(status.gaze_state), 5u)];
            if (status.calibration_phase == vrst::ChatCalibrationPhase::countdown) {
                char countdown[96]{};
                std::snprintf(countdown, sizeof(countdown), " | Look at chat; learning starts in %.1f s",
                              status.countdown_seconds);
                camera += countdown;
            } else if (status.calibration_phase == vrst::ChatCalibrationPhase::acquiring)
                camera += " | Learning: " + std::to_string(int(status.calibration_progress * 100)) + "% (" +
                          std::to_string(status.calibration_samples) + " samples)";
            else if (status.calibration_phase == vrst::ChatCalibrationPhase::failed)
                camera += status.calibration_failure == vrst::ChatCalibrationFailure::unstable
                              ? " | Learning failed: gaze moved"
                          : status.calibration_failure == vrst::ChatCalibrationFailure::reference_changed
                              ? " | Learning reset: tracking reference changed"
                              : " | Learning failed: insufficient valid eye samples";
            else
                camera +=
                    status.zone_valid ? " | Chat zone learned for this session" : " | No learned chat zone";
            if (status.gaze_state == vrst::GazeState::excluded)
                camera += " | Chat gaze excluded; head following continues";
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            capture_status = std::move(text);
            spectator_status = std::move(camera);
            director_status = std::move(director);
        }
        // The properties text is an explicit snapshot. Periodic OBS property
        // reloads recreate every widget and interrupt sliders/numeric editing.
    }
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
void hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool);
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
    for (std::size_t i = 0; i < std::size(actions); ++i) {
        const auto name = std::string("steadyxr.") + actions[i].name;
        s->hotkeys[i] = obs_hotkey_register_source(source, name.c_str(), actions[i].label, hotkey, s.get());
    }
    return s.release();
}
void destroy(void *data) {
    auto *s = static_cast<Source *>(data);
    // Unregistration waits for an in-flight hotkey callback before data is freed.
    for (const auto id : s->hotkeys)
        obs_hotkey_unregister(id);
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
    struct PublishStatus {
        Source &s;
        ~PublishStatus() {
            s.publish_status();
        }
    } publish{s};
    vrst_plugin::OpenXRSettings c;
    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(s.settings_mutex);
        c = s.settings;
        revision = s.revision;
    }
    obs_enter_graphics();
    const auto now = GetTickCount64();
    s.requested_fps = c.fps;
    const bool comparing = s.compare_until.load() > now;
    s.expired = s.expired || build_expired();
    const bool requested = obs_source_showing(s.source) && !c.paused && !s.expired;
    if (requested && !s.channel.data())
        s.channel.open(true, s.channel_name);
    const bool claimed = requested && s.channel.heartbeat(c.options.eye, c.fps);
    if (claimed)
        s.channel.select_overlays(c.options.overlays);
    // heartbeat's zero-timeout mutex probe can fail while our existing claim
    // remains valid. Skip consumption that tick without resetting pose history.
    const bool owned = requested && (claimed || s.channel.owns_consumer());
    const bool changed = revision != s.applied_revision;
    const bool source_changed = owned != s.active || c.options.eye != s.eye;
    bool dirty = changed || source_changed || !s.valid_output || comparing != s.comparing;
    s.comparing = comparing;
    c.options.compare_original = comparing;
    s.dimensions.store((std::uint64_t(c.options.output_width) << 32) | c.options.output_height);
    if (!requested)
        s.channel.release_consumer();
    if (!s.renderer && !s.attach()) {
        s.connection = "Renderer unavailable";
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
    if (s.horizon_reset_requested.exchange(false)) {
        s.renderer->reset_horizon();
        dirty = true;
    }
    switch (s.chat_command.exchange(Source::ChatCommand::none)) {
    case Source::ChatCommand::learn:
        s.renderer->learn_chat();
        dirty = true;
        break;
    case Source::ChatCommand::cancel:
        s.renderer->cancel_chat();
        dirty = true;
        break;
    case Source::ChatCommand::clear:
        s.renderer->clear_chat();
        dirty = true;
        break;
    default:
        break;
    }
    if (owned)
        s.channel.request_gaze(!c.options.spectator.bypass &&
                               (c.options.spectator.gaze_enabled || s.renderer->calibration_active()));
    s.active = owned;
    s.eye = c.options.eye;
    s.applied_revision = revision;
    std::wstring message;
    if (s.expired) {
        s.connection = "Build expired";
        message = L"SteadyXR build expired - install a fresh build";
    } else if (c.paused) {
        s.connection = "Paused";
        message = L"SteadyXR capture paused";
    } else if (!requested) {
        s.connection = "Inactive";
        message = L"SteadyXR capture inactive";
    } else if (!s.channel.data()) {
        s.connection = "Channel unavailable";
        message = L"Could not open the SteadyXR capture channel";
    } else if (!owned) {
        s.connection = "Capture busy";
        message = L"SteadyXR capture busy - close the other capture";
    } else {
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
            ++s.received_frames;
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
        s.connection = message.empty()           ? "Connected"
                       : s.capture_error.empty() ? "Waiting for OpenXR"
                                                 : "Capture error";
    }
    if (message != s.message) {
        s.message = message;
        dirty = true;
        s.valid_output = false;
    }
    if (s.output_pending && !s.copy_output()) {
        s.connection = "Output unavailable";
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
            s.connection = "Output unavailable";
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
bool import_path(Source &s, const char *key, bool refresh_properties) {
    auto *settings = obs_source_get_settings(s.source);
    const char *path = obs_data_get_string(settings, key);
    const int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(length ? length : 1, L'\0');
    if (length)
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), length);
    if (!wide.empty())
        wide.pop_back();
    std::string error;
    const bool imported = vrst_plugin::openxr_import(settings, wide, error);
    if (imported) {
        obs_source_update(s.source, settings);
        error = "Settings imported into this OBS source. Extra render area stays unchanged.";
    }
    obs_data_release(settings);
    s.feedback(error, refresh_properties);
    return imported;
}
void perform(Source &s, Action action, bool refresh_properties) {
    if (action == Action::learn_chat || action == Action::cancel_chat || action == Action::clear_chat) {
        s.chat_command.store(action == Action::learn_chat    ? Source::ChatCommand::learn
                             : action == Action::cancel_chat ? Source::ChatCommand::cancel
                                                             : Source::ChatCommand::clear);
        s.feedback(
            action == Action::learn_chat
                ? "Chat learning requested. Look at chat and hold steady; Refresh status for countdown."
            : action == Action::cancel_chat ? "Chat learning cancellation requested."
                                            : "Chat zone clear requested.",
            refresh_properties);
        return;
    }
    if (action == Action::reset) {
        s.reset_requested.store(true);
        s.feedback("Stabilization reset requested.", refresh_properties);
        return;
    }
    if (action == Action::reset_horizon) {
        s.horizon_reset_requested.store(true);
        s.feedback("Horizon reset requested. Keep the headset approximately upright.", refresh_properties);
        return;
    }
    if (action == Action::compare) {
        const auto now = GetTickCount64();
        auto previous = s.compare_until.load();
        while (!s.compare_until.compare_exchange_weak(previous, previous > now ? 0 : now + 20000)) {
        }
        s.feedback(previous > now ? "A/B comparison ended."
                                  : "A/B comparison started for 20 seconds; press again to return sooner.",
                   refresh_properties);
        return;
    }
    if (action >= Action::preset_1) {
        const auto key =
            std::string("preset_") + std::to_string(int(action) - int(Action::preset_1) + 1) + "_path";
        import_path(s, key.c_str(), refresh_properties);
        return;
    }
    auto *settings = obs_source_get_settings(s.source);
    auto c = vrst_plugin::openxr_settings(settings);
    if (action == Action::pause)
        c.paused = !c.paused;
    else if (action == Action::comfort)
        apply_stream_comfort(c.options);
    else {
        const bool strong = action == Action::strong;
        auto &o = c.options;
        o.smoothing = strong ? 70 : 25;
        o.horizon_lock = strong ? 85 : 25;
        o.adaptive_horizon = true;
        o.crop = strong ? 15 : 8;
        o.mirror = o.stabilize = true;
        o.mirror_limit = strong ? 12 : 10;
        o.mirror_softness = strong ? 75 : 60;
    }
    vrst_plugin::openxr_store(settings, c);
    obs_source_update(s.source, settings);
    obs_data_release(settings);
    if (refresh_properties)
        obs_source_update_properties(s.source);
}
void hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed) {
    if (!pressed)
        return;
    auto &s = *static_cast<Source *>(data);
    for (std::size_t i = 0; i < s.hotkeys.size(); ++i)
        if (s.hotkeys[i] == id) {
            perform(s, actions[i].action, true);
            break;
        }
}
void refresh_status(obs_properties_t *properties, Source *s) {
    std::string capture_status = "Inactive | 0.0 FPS / 60 requested", action_status;
    std::string spectator_status = "Gaze disabled";
    std::string director_status = "Director: inactive";
    if (s) {
        {
            std::lock_guard<std::mutex> lock(s->status_mutex);
            capture_status = s->capture_status;
            action_status = s->action_status;
            spectator_status = s->spectator_status;
            director_status = s->director_status;
        }
        const auto now = GetTickCount64(), deadline = s->compare_until.load();
        if (deadline > now)
            capture_status +=
                " | Comparison (" + std::to_string((deadline - now + 999) / 1000) + "s remaining)";
    }
    capture_status = "Status snapshot: " + capture_status;
    obs_property_set_description(obs_properties_get(properties, "capture_status"), capture_status.c_str());
    obs_property_set_description(obs_properties_get(properties, "import_status"), action_status.c_str());
    obs_property_set_description(obs_properties_get(properties, "spectator_status"),
                                 spectator_status.c_str());
    obs_property_set_description(obs_properties_get(properties, "director_status"),
                                 director_status.c_str());
}
bool refresh_status_button(obs_properties_t *properties, obs_property_t *, void *data) {
    refresh_status(properties, static_cast<Source *>(data));
    return true;
}
bool action_button(obs_properties_t *properties, obs_property_t *property, void *data) {
    if (!data)
        return false;
    for (const auto &action : actions)
        if (std::strcmp(obs_property_name(property), action.name) == 0) {
            // OBS queues RefreshProperties after this callback returns true.
            // Signalling update_properties here would synchronously destroy
            // the button's WidgetInfo while its ButtonClicked is on the stack.
            perform(*static_cast<Source *>(data), action.action, false);
            refresh_status(properties, static_cast<Source *>(data));
            return true;
        }
    return false;
}
void overlay_properties(obs_properties_t *, Source *);
bool import_settings(obs_properties_t *properties, obs_property_t *, void *data) {
    if (!data)
        return false;
    import_path(*static_cast<Source *>(data), "import_path", false);
    overlay_properties(properties, static_cast<Source *>(data));
    refresh_status(properties, static_cast<Source *>(data));
    return true;
}
// The property context owns a catalog snapshot so an old checkbox cannot target
// a different producer after a restart. No property callback emits update_properties.
struct OverlayProperties {
    Source *source{};
    OverlayCatalog catalog{};
};
OverlayCatalog read_catalog(Source *s) {
    Channel reader;
    if (!s || !reader.open(false, s->channel_name))
        return {};
    return reader.overlay_catalog();
}
bool overlay_modified(void *data, obs_properties_t *properties, obs_property_t *property,
                      obs_data_t *settings) {
    auto unchanged = [&] {
        obs_data_unset_user_value(settings, obs_property_name(property));
        return false;
    };
    auto *ctx = static_cast<OverlayProperties *>(data);
    if (!ctx || !ctx->source)
        return unchanged();
    const auto current = read_catalog(ctx->source);
    if (!overlay_catalog_live(current) || current.producer_pid != ctx->catalog.producer_pid ||
        current.session != ctx->catalog.session)
        return unchanged();
    unsigned slot{};
    if (sscanf_s(obs_property_name(property), "overlay_slot_%u", &slot) != 1 || slot >= max_overlay_layers)
        return unchanged();
    bool supported = false;
    for (unsigned i = 0; i < std::min(current.count, max_overlay_layers); ++i)
        if (current.layers[i].slot == slot)
            supported = current.layers[i].supported != 0;
    if (!supported)
        return unchanged();
    auto config = vrst_plugin::openxr_settings(settings);
    auto &selected = config.options.overlays;
    const bool checked = obs_data_get_bool(settings, obs_property_name(property));
    const bool expected =
        selected.mode == 1 ||
        (selected.mode == 2 && overlay_scope_matches(selected, current) && (selected.mask & (1u << slot)));
    if (checked == expected)
        return unchanged(); // OBS also calls this while applying initial settings.
    std::uint32_t mask = selected.mode == 2 && overlay_scope_matches(selected, current) ? selected.mask : 0;
    if (selected.mode == 1)
        for (unsigned i = 0; i < std::min(current.count, max_overlay_layers); ++i)
            if (current.layers[i].supported && current.layers[i].slot < max_overlay_layers)
                mask |= 1u << current.layers[i].slot;
    if (checked)
        mask |= 1u << slot;
    else
        mask &= ~(1u << slot);
    selected = {2, mask, current.producer_pid, current.session};
    vrst_plugin::openxr_store(settings, config);
    for (unsigned i = 0; i < max_overlay_layers; ++i) {
        const auto key = "overlay_slot_" + std::to_string(i);
        obs_data_set_default_bool(settings, key.c_str(), (mask & (1u << i)) != 0);
        obs_data_unset_user_value(settings, key.c_str());
    }
    // OBS invokes modified callbacks after its immediate source update. Apply
    // the derived selection now, without signalling a properties reload.
    obs_source_update(ctx->source->source, settings);
    obs_property_set_description(
        obs_properties_get(properties, "overlay_help"),
        "Selected layers in this game session. OpenXR only; keep the source active and Refresh layers. "
        "Layer order: game > overlay injector > SteadyXR > runtime. "
        "Update the connection and restart the game for 0.10.0 (IPC 4).");
    return true;
}
void overlay_properties(obs_properties_t *, Source *);
bool refresh_overlay_button(obs_properties_t *p, obs_property_t *, void *data) {
    overlay_properties(p, static_cast<Source *>(data));
    return true;
}
bool overlay_mode_button(obs_properties_t *p, obs_property_t *property, void *data) {
    auto *s = static_cast<Source *>(data);
    if (!s)
        return false;
    auto *settings = obs_source_get_settings(s->source);
    auto c = vrst_plugin::openxr_settings(settings);
    c.options.overlays = {};
    c.options.overlays.mode = std::strcmp(obs_property_name(property), "overlays_all") == 0 ? 1 : 0;
    vrst_plugin::openxr_store(settings, c);
    obs_source_update(s->source, settings);
    obs_data_release(settings);
    overlay_properties(p, s);
    return true;
}
void overlay_properties(obs_properties_t *p, Source *s) {
    auto *ctx = static_cast<OverlayProperties *>(obs_properties_get_param(p));
    if (!ctx) {
        ctx = new OverlayProperties{};
        obs_properties_set_param(p, ctx, [](void *v) { delete static_cast<OverlayProperties *>(v); });
    }
    ctx->source = s;
    ctx->catalog = read_catalog(s);
    for (unsigned i = 0; i < max_overlay_layers; ++i)
        obs_properties_remove_by_name(p, ("overlay_slot_" + std::to_string(i)).c_str());
    auto *settings = s ? obs_source_get_settings(s->source) : obs_data_create();
    const auto selected = vrst_plugin::openxr_settings(settings).options.overlays;
    const auto &catalog = ctx->catalog;
    const bool live = overlay_catalog_live(catalog), scope = overlay_scope_matches(selected, catalog);
    std::string help =
        "OpenXR only. Activate this source and Refresh layers. Layer order: game > overlay injector > "
        "SteadyXR > runtime. Update the connection and restart the game for 0.10.0 (IPC 4). ";
    if (selected.mode == 2 && !scope)
        help += "Saved selection belongs to another game session; reselect layers. ";
    if (!live || !catalog.count)
        help += "No live additional layers: check source activity and layer order.";
    else
        help += selected.mode == 1   ? "All supported layers, including new layers."
                : selected.mode == 0 ? "Additional layers disabled."
                                     : "Selected layers in this game session.";
    if (auto *existing = obs_properties_get(p, "overlay_help"))
        obs_property_set_description(existing, help.c_str());
    else
        obs_properties_add_text(p, "overlay_help", help.c_str(), OBS_TEXT_INFO);
    if (!obs_properties_get(p, "overlays_all")) {
        obs_properties_add_button2(p, "overlays_all", "All supported overlays", overlay_mode_button, s);
        obs_properties_add_button2(p, "overlays_none", "No overlays", overlay_mode_button, s);
        obs_properties_add_button2(p, "refresh_overlays", "Refresh layers", refresh_overlay_button, s);
    }
    for (unsigned i = 0; i < std::min(catalog.count, max_overlay_layers); ++i) {
        const auto &item = catalog.layers[i];
        if (item.slot >= max_overlay_layers)
            continue;
        const auto key = "overlay_slot_" + std::to_string(item.slot);
        const int length = int(wcsnlen(item.label, 128));
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, item.label, length, nullptr, 0, nullptr, nullptr);
        std::string label(bytes, '\0');
        if (bytes)
            WideCharToMultiByte(CP_UTF8, 0, item.label, length, label.data(), bytes, nullptr, nullptr);
        if (!item.supported)
            label += " (unsupported)";
        auto *property = obs_properties_add_bool(p, key.c_str(), label.c_str());
        obs_data_set_default_bool(
            settings, key.c_str(),
            item.supported &&
                (selected.mode == 1 || (selected.mode == 2 && scope && (selected.mask & (1u << item.slot)))));
        obs_data_unset_user_value(settings, key.c_str());
        obs_property_set_enabled(property, live && item.supported);
        obs_property_set_modified_callback2(property, overlay_modified, ctx);
    }
    obs_data_release(settings);
}
obs_properties_t *properties(void *data) {
    auto *p = obs_properties_create();
    obs_properties_add_text(p, "help",
                            "Capture OpenXR directly. Enable the SteadyXR OpenXR layer before starting the "
                            "game. One active capture per channel.",
                            OBS_TEXT_INFO);
    obs_properties_add_text(p, "capture_status", "Status snapshot", OBS_TEXT_INFO);
    obs_properties_add_text(p, "spectator_status", "Gaze disabled", OBS_TEXT_INFO);
    obs_properties_add_text(p, "director_status", "Director: inactive", OBS_TEXT_INFO);
    obs_properties_add_button2(p, "refresh_status", "Refresh status", refresh_status_button, data);
    obs_properties_add_text(
        p, "hotkey_help",
        "Assign source hotkeys in OBS Settings > Hotkeys. For Reset horizon, keep the "
        "headset approximately upright. Comparison returns after 20 seconds, or press again "
        "to return sooner.",
        OBS_TEXT_INFO);
    obs_properties_add_text(p, "build_expiry", "SteadyXR 0.14.0 | No expiry", OBS_TEXT_INFO);
    vrst_plugin::openxr_add_properties(p);
    overlay_properties(p, static_cast<Source *>(data));
    for (const auto &action : actions)
        if (action.action < Action::preset_1 && action.action != Action::pause)
            obs_properties_add_button2(p, action.name, action.label, action_button, data);
    auto *slots = obs_properties_create();
    for (unsigned slot = 1; slot <= 3; ++slot) {
        const auto key = std::string("preset_") + std::to_string(slot);
        const auto label = "Saved preset " + std::to_string(slot) + " (INI)";
        obs_properties_add_path(slots, (key + "_path").c_str(), label.c_str(), OBS_PATH_FILE,
                                "INI settings (*.ini)", nullptr);
        obs_properties_add_button2(slots, key.c_str(), ("Load saved preset " + std::to_string(slot)).c_str(),
                                   action_button, data);
    }
    obs_properties_add_group(p, "saved_presets", "Saved preset hotkey slots", OBS_GROUP_NORMAL, slots);
    obs_properties_add_path(p, "import_path", "SteadyXR settings or preset", OBS_PATH_FILE,
                            "INI settings (*.ini)", nullptr);
    obs_properties_add_button2(p, "import", "Import settings", import_settings, data);
    obs_properties_add_text(p, "import_status", "", OBS_TEXT_INFO);
    obs_properties_add_text(
        p, "import_help",
        "Preset slots and Import read INI files without changing them or the game's extra "
        "render area. Slot paths and tuning are saved with this OBS source.",
        OBS_TEXT_INFO);
    refresh_status(p, static_cast<Source *>(data));
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
