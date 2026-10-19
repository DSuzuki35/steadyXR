#include "spectator_effects_filter.hpp"
#include "../preview/visual_effects.hpp"
#include "../preview/visual_settings_io.hpp"
#include "openxr_source_settings.hpp"
#include <atomic>
#include <d3d11.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace {
using namespace vrst::preview;
using Microsoft::WRL::ComPtr;
struct Filter {
    obs_source_t *source{};
    std::mutex mutex;
    VisualSettings settings;
    std::string status{"Effects off | 0 allocations | 0 dispatches | 0 bytes"};
    // OBS graphics thread owns all GPU state.
    std::unique_ptr<VisualEffects> effects;
    gs_texrender_t *capture{};
    gs_texture_t *output{};
    obs_source_t *target{};
    unsigned width{}, height{};
    std::atomic<bool> rendered{false};
    ~Filter() {
        clear();
    }
    void clear() {
        effects.reset();
        gs_texrender_destroy(capture);
        gs_texture_destroy(output);
        capture = nullptr;
        output = nullptr;
        width = height = 0;
        target = nullptr;
    }
    void publish(const char *message) {
        const auto d = effects ? effects->diagnostics() : VisualDiagnostics{};
        std::lock_guard<std::mutex> lock(mutex);
        status = std::string(message) + " | " + std::to_string(d.allocations) + " allocations | " +
                 std::to_string(d.dispatches) + " dispatches | " + std::to_string(d.resource_bytes) +
                 " bytes";
    }
};
void update(void *data, obs_data_t *s) {
    auto &f = *static_cast<Filter *>(data);
    std::lock_guard<std::mutex> lock(f.mutex);
    f.settings = vrst_plugin::visual_settings(s);
}
void *create(obs_data_t *s, obs_source_t *source) {
    auto f = std::make_unique<Filter>();
    f->source = source;
    update(f.get(), s);
    return f.release();
}
void destroy(void *data) {
    obs_enter_graphics();
    delete static_cast<Filter *>(data);
    obs_leave_graphics();
}
void tick(void *data, float) {
    auto &f = *static_cast<Filter *>(data);
    bool active;
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        active = f.settings.active();
    }
    const bool rendered = f.rendered.exchange(false);
    if (!active || !rendered) {
        obs_enter_graphics();
        f.clear();
        f.publish(active ? "Waiting for visible source" : "Effects off");
        obs_leave_graphics();
    }
}
struct GraphicsScope {
    bool srgb{gs_framebuffer_srgb_enabled()};
    GraphicsScope() {
        gs_blend_state_push();
    }
    ~GraphicsScope() {
        gs_enable_framebuffer_srgb(srgb);
        gs_blend_state_pop();
    }
};
struct OutputBindingScope {
    ID3D11DeviceContext *context;
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView *depth{};
    explicit OutputBindingScope(ID3D11DeviceContext *c) : context(c) {
        // libobs updates OM targets lazily on the next draw. Ending texrender
        // changes its cache, but the captured RTV can still be actually bound.
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    ~OutputBindingScope() {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
        for (auto *target : targets)
            if (target)
                target->Release();
        if (depth)
            depth->Release();
    }
};
void render(void *data, gs_effect_t *) {
    auto &f = *static_cast<Filter *>(data);
    VisualSettings settings;
    {
        std::lock_guard<std::mutex> lock(f.mutex);
        settings = f.settings;
    }
    f.rendered = true;
    auto skip = [&](const char *message) {
        f.clear();
        f.publish(message);
        obs_source_skip_video_filter(f.source);
    };
    if (!settings.active()) {
        skip("Effects off");
        return;
    }
    auto *target = obs_filter_get_target(f.source);
    auto *parent = obs_filter_get_parent(f.source);
    const auto width = target ? obs_source_get_base_width(target) : 0;
    const auto height = target ? obs_source_get_base_height(target) : 0;
    const gs_color_space spaces[]{GS_CS_SRGB, GS_CS_SRGB_16F};
    if (!target || !parent || !width || !height) {
        skip("Waiting for source");
        return;
    }
    const auto space = obs_source_get_color_space(target, 2, spaces);
    if (space != GS_CS_SRGB && space != GS_CS_SRGB_16F) {
        skip("Unsupported HDR/color space: SDR effects bypassed");
        return;
    }
    if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
        skip("Effects require OBS Direct3D 11: source passed through");
        return;
    }
    if (f.target != target || f.width != width || f.height != height)
        f.clear();
    f.target = target;
    f.width = width;
    f.height = height;
    if (!f.capture)
        f.capture = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    if (!f.output)
        f.output = gs_texture_create(width, height, GS_RGBA16F, 1, nullptr, 0);
    if (!f.capture || !f.output) {
        skip("Effects allocation failed: source passed through");
        return;
    }
    GraphicsScope restore;
    gs_texrender_reset(f.capture);
    if (!gs_texrender_begin_with_color_space(f.capture, width, height, GS_CS_SRGB)) {
        skip("Effects capture unavailable: source passed through");
        return;
    }
    gs_enable_framebuffer_srgb(true);
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    vec4 zero{};
    gs_clear(GS_CLEAR_COLOR, &zero, 0, 0);
    gs_ortho(0, float(width), 0, float(height), -100, 100);
    const auto flags = obs_source_get_output_flags(target);
    if (target == parent && !(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC)))
        obs_source_default_render(target);
    else
        obs_source_video_render(target);
    gs_texrender_end(f.capture);
    // The float target contains linear premultiplied SDR. Explicitly unbind
    // its actual OM view below; the borrowed compute output is only copied.
    auto *input = gs_texrender_get_texture(f.capture);
    auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    settings.input_transfer = VisualTransfer::linear;
    settings.premultiplied_alpha = true;
    if (!f.effects)
        f.effects = std::make_unique<VisualEffects>();
    const auto time = obs_get_video_frame_time();
    auto *original = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(input));
    ID3D11Texture2D *processed;
    {
        OutputBindingScope unbind(context.Get());
        processed = f.effects->process(device, context.Get(), original, settings, time, time * 1e-9);
    }
    gs_texture_t *display = input;
    if (processed != original) {
        // Keep OBS's own resource cache coherent when replacing the copy target.
        gs_load_texture(nullptr, 0);
        context->CopyResource(static_cast<ID3D11Texture2D *>(gs_texture_get_obj(f.output)), processed);
        display = f.output;
    }
    gs_enable_framebuffer_srgb(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    auto *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), display);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(display, 0, width, height);
    gs_load_texture(nullptr, 0);
    const auto outcome = f.effects->diagnostics().outcome;
    f.publish(outcome == VisualOutcome::processed ? "SDR effects active"
              : outcome == VisualOutcome::unsupported_input
                  ? "Unsupported input: effects bypassed"
                  : "Effects unavailable: captured source passed through");
}
bool import_preset(obs_properties_t *, obs_property_t *, void *data) {
    auto *f = static_cast<Filter *>(data);
    if (!f)
        return false;
    auto *s = obs_source_get_settings(f->source);
    const auto *path = obs_data_get_string(s, "visual_preset_path");
    const auto n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wide(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), n);
    if (!wide.empty())
        wide.pop_back();
    const auto attr = GetFileAttributesW(wide.c_str());
    wchar_t section[32768]{};
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY) &&
        GetPrivateProfileSectionW(L"Preview", section, 32768, wide.c_str())) {
        vrst_plugin::visual_store(s, read_visual_settings(wide));
        obs_source_update(f->source, s);
        std::lock_guard<std::mutex> lock(f->mutex);
        f->status = "Visual preset imported";
    } else {
        std::lock_guard<std::mutex> lock(f->mutex);
        f->status = "Choose an existing SteadyXR INI with Preview settings";
    }
    obs_data_release(s);
    return true;
}
obs_properties_t *properties(void *data) {
    auto *p = obs_properties_create();
    vrst_plugin::visual_add_properties(p);
    std::string status{"Effects off | 0 allocations | 0 dispatches | 0 bytes"};
    if (data) {
        auto &f = *static_cast<Filter *>(data);
        std::lock_guard<std::mutex> lock(f.mutex);
        status = f.status;
    }
    obs_properties_add_text(p, "visual_status", status.c_str(), OBS_TEXT_INFO);
    obs_properties_add_button2(
        p, "visual_refresh", "Refresh effects status",
        [](obs_properties_t *properties, obs_property_t *, void *data) {
            if (!data)
                return false;
            auto &f = *static_cast<Filter *>(data);
            std::lock_guard<std::mutex> lock(f.mutex);
            obs_property_set_description(obs_properties_get(properties, "visual_status"), f.status.c_str());
            return true;
        },
        data);
    obs_properties_add_path(p, "visual_preset_path", "Visual preset (INI)", OBS_PATH_FILE,
                            "INI settings (*.ini)", nullptr);
    obs_properties_add_button2(p, "visual_import", "Import visual preset", import_preset, data);
    return p;
}
} // namespace
void register_spectator_effects_filter() {
    obs_source_info info{};
    info.id = "steadyxr_visual_effects";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = [](void *) { return "SteadyXR Visual Effects"; };
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = vrst_plugin::visual_defaults;
    info.get_properties = properties;
    info.video_render = render;
    info.video_tick = tick;
    info.video_get_color_space = [](void *data, size_t count, const gs_color_space *spaces) {
        auto *target = obs_filter_get_target(static_cast<Filter *>(data)->source);
        return target ? obs_source_get_color_space(target, count, spaces) : GS_CS_SRGB;
    };
    obs_register_source(&info);
}
