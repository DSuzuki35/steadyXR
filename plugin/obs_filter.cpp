#include <obs-module.h>

#include "image_filter.hpp"
#include "vrst/core.hpp"
#include "vrst/protocol.hpp"
#include "vrst/receiver.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vr-stabilizer", "en-US")
OBS_MODULE_AUTHOR("VR Stabilizer contributors")

namespace {
struct Configuration {
    vrst::Settings warp;
    vrst::ImageSettings image;
    bool image_mode{};
    std::uint64_t image_revision{};
    std::string host;
    std::uint16_t port{vrst::wire::default_port};
    std::uint64_t peer_revision{};
};

struct Filter {
    explicit Filter(obs_source_t *source) : source(source) {}

    obs_source_t *const source;
    vrst::Receiver receiver;
    std::mutex configuration_mutex;
    Configuration configuration;
    std::atomic<std::uint64_t> dimensions{0};
    std::atomic<double> applied_correction{1};
    std::atomic<bool> using_automatic_fov{false};
    std::atomic<bool> reset_image{false};
    std::mutex image_diagnostics_mutex;
    vrst::ImageResult image_diagnostics;

    // Owned by the graphics thread after create; UI callbacks never read these.
    ImageFilter image;
    bool image_active{};
    gs_effect_t *effect{};
    gs_eparam_t *input_projection{};
    gs_eparam_t *output_projection{};
    gs_eparam_t *correction{};
    gs_eparam_t *input_uv{};
    std::optional<vrst::Projection> last_projection;
    std::uint64_t rendered_peer_revision{};

    Configuration settings() {
        std::lock_guard<std::mutex> lock(configuration_mutex);
        return configuration;
    }

    template <typename Warp> void set_dimensions(const Warp &warp) {
        dimensions.store((std::uint64_t(warp.width) << 32) | warp.height, std::memory_order_relaxed);
    }
};

const char *text(const char *key) {
    return obs_module_text(key);
}

// Settings can also arrive from scene JSON, outside the property widget's bounds.
double number(obs_data_t *settings, const char *key, double fallback, double low, double high) {
    const double value = obs_data_get_double(settings, key);
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

void update(void *data, obs_data_t *settings) {
    auto *filter = static_cast<Filter *>(data);
    if (!filter)
        return;
    Configuration next;
    next.image_mode = obs_data_get_int(settings, "mode") == 1;
    next.host = obs_data_get_string(settings, "gaming_pc");
    const auto first = next.host.find_first_not_of(" \t\r\n");
    next.host = first == std::string::npos
                    ? ""
                    : next.host.substr(first, next.host.find_last_not_of(" \t\r\n") - first + 1);
    next.port =
        static_cast<std::uint16_t>(std::clamp<std::int64_t>(obs_data_get_int(settings, "port"), 1, 65535));
    auto &s = next.warp;
    s.horizontal_fov = number(settings, "horizontal_fov", 100, 10, 170);
    s.vertical_fov = number(settings, "vertical_fov", 90, 10, 170);
    s.automatic_fov = obs_data_get_bool(settings, "automatic_fov");
    s.delay_ms = number(settings, "delay_ms", 80, 0, 1000);
    s.smoothing = number(settings, "smoothing", 40, 0, 100);
    s.horizon_level = number(settings, "horizon_level", 0, 0, 100);
    s.horizon_max_degrees = number(settings, "horizon_max_degrees", 45, 0, 60);
    next.image.smoothing = s.smoothing;
    next.image.max_shift_percent = number(settings, "image_max_shift", 1, 0, 5);
    next.image.max_roll_degrees = number(settings, "image_max_roll", 0.5, 0, 5);
    next.image.track_roll = obs_data_get_bool(settings, "image_track_roll");
    next.image.region_left = number(settings, "region_left", 10, 0, 90);
    next.image.region_right = number(settings, "region_right", 10, 0, 90);
    next.image.region_top = number(settings, "region_top", 50, 0, 90);
    next.image.region_bottom = number(settings, "region_bottom", 10, 0, 90);
    // Keep a nonempty tracking region even for imported scene settings.
    next.image.region_right = std::min(next.image.region_right, 94 - next.image.region_left);
    next.image.region_bottom = std::min(next.image.region_bottom, 94 - next.image.region_top);
    s.stabilization_crop = number(settings, "stabilization_crop", 10, 0, 40);
    s.framing_zoom = number(settings, "framing_zoom", 0, 0, 40);
    s.output_16_9 = obs_data_get_bool(settings, "output_16_9");
    s.framing_x = number(settings, "framing_x", 0, -100, 100);
    s.framing_y = number(settings, "framing_y", 0, -100, 100);
    s.input_left = number(settings, "input_left", 0, 0, 45);
    s.input_right = number(settings, "input_right", 0, 0, 45);
    s.input_top = number(settings, "input_top", 0, 0, 45);
    s.input_bottom = number(settings, "input_bottom", 0, 0, 45);

    std::lock_guard<std::mutex> lock(filter->configuration_mutex);
    const bool peer_changed = next.host != filter->configuration.host ||
                              next.port != filter->configuration.port ||
                              next.image_mode != filter->configuration.image_mode;
    next.peer_revision = filter->configuration.peer_revision + (peer_changed ? 1 : 0);
    const auto &old = filter->configuration;
    const bool image_changed =
        next.image_mode != old.image_mode || s.input_left != old.warp.input_left ||
        s.input_right != old.warp.input_right || s.input_top != old.warp.input_top ||
        s.input_bottom != old.warp.input_bottom || next.image.region_left != old.image.region_left ||
        next.image.region_right != old.image.region_right || next.image.region_top != old.image.region_top ||
        next.image.region_bottom != old.image.region_bottom;
    next.image_revision = old.image_revision + (image_changed ? 1 : 0);
    filter->configuration = next;
    if (peer_changed)
        filter->receiver.configure(next.image_mode ? "" : next.host, next.port);
}

void enabled_changed(void *data, calldata_t *) {
    static_cast<Filter *>(data)->reset_image.store(true, std::memory_order_relaxed);
}

void destroy(void *data) {
    auto *filter = static_cast<Filter *>(data);
    if (!filter)
        return;
    signal_handler_disconnect(obs_source_get_signal_handler(filter->source), "enable", enabled_changed,
                              filter);
    obs_enter_graphics();
    filter->image.destroy();
    gs_effect_destroy(filter->effect);
    obs_leave_graphics();
    delete filter; // Receiver destructor stops and joins its worker and closes the socket.
}

void *create(obs_data_t *settings, obs_source_t *source) {
    std::unique_ptr<Filter> filter;
    try {
        filter = std::make_unique<Filter>(source);
        char *path = obs_module_file("stabilize.effect");
        char *errors = nullptr;
        obs_enter_graphics();
        if (path)
            filter->effect = gs_effect_create_from_file(path, &errors);
        const bool image_loaded = filter->image.load();
        obs_leave_graphics();
        bfree(path);
        if (!filter->effect || !image_loaded) {
            blog(LOG_ERROR, "[vr-stabilizer] Unable to load stabilize.effect: %s",
                 errors ? errors : "missing shader file");
            bfree(errors);
            destroy(filter.release());
            return nullptr;
        }
        bfree(errors);
        filter->input_projection = gs_effect_get_param_by_name(filter->effect, "input_projection");
        filter->output_projection = gs_effect_get_param_by_name(filter->effect, "output_projection");
        filter->correction = gs_effect_get_param_by_name(filter->effect, "correction");
        filter->input_uv = gs_effect_get_param_by_name(filter->effect, "input_uv");
        if (!filter->input_projection || !filter->output_projection || !filter->correction ||
            !filter->input_uv) {
            blog(LOG_ERROR, "[vr-stabilizer] Shader is missing required warp parameters");
            destroy(filter.release());
            return nullptr;
        }
        update(filter.get(), settings);
        signal_handler_connect(obs_source_get_signal_handler(source), "enable", enabled_changed,
                               filter.get());
        return filter.release();
    } catch (const std::exception &error) {
        blog(LOG_ERROR, "[vr-stabilizer] Cannot create filter: %s", error.what());
        destroy(filter.release());
        return nullptr;
    }
}

void defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, "mode", 0);
    obs_data_set_default_double(settings, "horizon_level", 0);
    obs_data_set_default_double(settings, "horizon_max_degrees", 45);
    obs_data_set_default_double(settings, "image_max_shift", 1);
    obs_data_set_default_double(settings, "image_max_roll", 0.5);
    obs_data_set_default_bool(settings, "image_track_roll", true);
    obs_data_set_default_double(settings, "region_left", 10);
    obs_data_set_default_double(settings, "region_right", 10);
    obs_data_set_default_double(settings, "region_top", 50);
    obs_data_set_default_double(settings, "region_bottom", 10);
    obs_data_set_default_string(settings, "gaming_pc", "");
    obs_data_set_default_int(settings, "port", vrst::wire::default_port);
    obs_data_set_default_double(settings, "horizontal_fov", 100);
    obs_data_set_default_double(settings, "vertical_fov", 90);
    obs_data_set_default_bool(settings, "automatic_fov", false);
    obs_data_set_default_double(settings, "delay_ms", 80);
    obs_data_set_default_double(settings, "smoothing", 40);
    obs_data_set_default_double(settings, "stabilization_crop", 10);
    obs_data_set_default_double(settings, "framing_zoom", 0);
    obs_data_set_default_bool(settings, "output_16_9", true);
    for (const char *key :
         {"framing_x", "framing_y", "input_left", "input_right", "input_top", "input_bottom"})
        obs_data_set_default_double(settings, key, 0);
}

std::string html_escape(const std::string &value) {
    std::string result;
    for (char c : value) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
        }
    }
    return result;
}

void refresh_diagnostics(obs_properties_t *properties, Filter *filter) {
    auto *property = obs_properties_get(properties, "diagnostics");
    if (!property)
        return;
    obs_property_text_set_info_word_wrap(property, true);
    if (!filter) {
        obs_property_set_description(property, text("Status.NotCreated"));
        return;
    }
    const auto configuration = filter->settings();
    if (configuration.image_mode) {
        vrst::ImageResult result;
        {
            std::lock_guard<std::mutex> lock(filter->image_diagnostics_mutex);
            result = filter->image_diagnostics;
        }
        const char *status = "Image.Warming";
        switch (result.status) {
        case vrst::ImageStatus::tracking:
            status = "Image.Tracking";
            break;
        case vrst::ImageStatus::weak:
            status = "Image.Weak";
            break;
        case vrst::ImageStatus::scene_change:
            status = "Image.SceneChange";
            break;
        case vrst::ImageStatus::invalid:
            status = "Image.Invalid";
            break;
        default:
            break;
        }
        const auto size = filter->dimensions.load(std::memory_order_relaxed);
        std::ostringstream message;
        message << std::fixed << std::setprecision(1) << "<b>" << text("Mode.Image") << "</b><br/>"
                << text(status) << "<br/>" << text("Image.Confidence") << ": " << result.confidence * 100
                << "% | " << text("Image.Inliers") << ": " << result.inliers << " / " << result.tracked
                << "<br/>" << text("Image.Processing") << ": " << result.processing_ms << " ms | "
                << result.width << " x " << result.height << "<br/>" << text("Image.Delay") << "<br/>"
                << text("Status.Output") << ": " << (size >> 32) << " x " << (size & 0xffffffffu) << " | "
                << text("Status.Correction") << ": "
                << filter->applied_correction.load(std::memory_order_relaxed) * 100 << '%' << "<br/>"
                << text("Status.RefreshHint");
        obs_property_set_description(property, message.str().c_str());
        obs_property_text_set_info_type(property, result.status == vrst::ImageStatus::tracking
                                                      ? OBS_TEXT_INFO_NORMAL
                                                      : OBS_TEXT_INFO_WARNING);
        return;
    }
    const auto snapshot =
        filter->receiver.snapshot(configuration.warp.delay_ms, configuration.warp.smoothing);
    std::ostringstream message;
    message << std::fixed << std::setprecision(1) << "<b>" << text("Status.Connection") << ":</b> "
            << html_escape(snapshot.status) << "<br/>";
    if (!configuration.host.empty())
        message << html_escape(configuration.host) << ':' << configuration.port << "<br/>";
    message << text("Status.Clock") << ": " << text(snapshot.clock_ready ? "Status.Ready" : "Status.Waiting");
    if (snapshot.clock_ready)
        message << " | " << text("Status.RTT") << ": " << snapshot.rtt_ms << " ms";
    message << "<br/>" << text("Status.Tracking") << ": "
            << text(snapshot.pose.valid ? "Status.Valid" : "Status.NoCorrection");
    if (snapshot.pose.valid)
        message << " | " << text("Status.Age") << ": " << snapshot.pose_age_ms << " ms";
    message << "<br/>" << text("Status.Mode") << ": ";
    if (snapshot.pose.flags & vrst::DemoPose)
        message << text("Status.Demo");
    else if (snapshot.pose.valid)
        message << text((snapshot.pose.flags & vrst::FramePose) ? "Status.Frame" : "Status.Live");
    else
        message << text("Status.Waiting");
    message << "<br/>" << text("Status.Packets") << ": " << snapshot.received << " | "
            << text("Status.Rejected") << ": " << snapshot.rejected;
    const auto size = filter->dimensions.load(std::memory_order_relaxed);
    message << "<br/>" << text("Status.Output") << ": " << (size >> 32) << " x " << (size & 0xffffffffu)
            << " | " << text("Status.Correction") << ": "
            << filter->applied_correction.load(std::memory_order_relaxed) * 100 << '%';
    if (configuration.warp.automatic_fov)
        message << "<br/>"
                << text(filter->using_automatic_fov.load(std::memory_order_relaxed)
                            ? "Status.AutoFOV"
                            : "Status.ManualFallback");
    message << "<br/>" << text("Status.RefreshHint");
    obs_property_set_description(property, message.str().c_str());
    obs_property_text_set_info_type(property,
                                    snapshot.pose.valid ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_WARNING);
}

bool refresh_clicked(obs_properties_t *properties, obs_property_t *, void *data) {
    refresh_diagnostics(properties, static_cast<Filter *>(data));
    return true;
}

bool reset_clicked(obs_properties_t *properties, obs_property_t *, void *data) {
    auto *filter = static_cast<Filter *>(data);
    if (filter) {
        filter->receiver.reset();
        filter->reset_image.store(true, std::memory_order_relaxed);
    }
    refresh_diagnostics(properties, filter);
    return true;
}

bool mode_changed(obs_properties_t *properties, obs_property_t *, obs_data_t *settings) {
    const bool image = obs_data_get_int(settings, "mode") == 1;
    for (const char *key : {"gaming_pc", "port", "automatic_fov", "horizontal_fov", "vertical_fov",
                            "delay_ms", "horizon_level", "horizon_max_degrees"})
        obs_property_set_visible(obs_properties_get(properties, key), !image);
    for (const char *key : {"image_max_shift", "image_max_roll", "image_track_roll", "region_left",
                            "region_right", "region_top", "region_bottom"})
        obs_property_set_visible(obs_properties_get(properties, key), image);
    return true;
}

bool automatic_changed(obs_properties_t *properties, obs_property_t *, obs_data_t *settings) {
    const bool manual = !obs_data_get_bool(settings, "automatic_fov");
    obs_property_set_enabled(obs_properties_get(properties, "horizontal_fov"), manual);
    obs_property_set_enabled(obs_properties_get(properties, "vertical_fov"), manual);
    return true;
}

obs_property_t *slider(obs_properties_t *properties, const char *key, const char *label, double low,
                       double high, double step, const char *suffix, const char *description) {
    auto *property = obs_properties_add_float_slider(properties, key, text(label), low, high, step);
    obs_property_float_set_suffix(property, suffix);
    obs_property_set_long_description(property, text(description));
    return property;
}

obs_properties_t *properties(void *data) {
    auto *props = obs_properties_create();
    auto *p = obs_properties_add_list(props, "mode", text("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, text("Mode.Pose"), 0);
    obs_property_list_add_int(p, text("Mode.Image"), 1);
    obs_property_set_modified_callback(p, mode_changed);
    p = obs_properties_add_text(props, "gaming_pc", text("GamingPC"), OBS_TEXT_DEFAULT);
    obs_property_set_long_description(p, text("GamingPC.Description"));
    p = obs_properties_add_int(props, "port", text("Port"), 1, 65535, 1);
    obs_property_set_long_description(p, text("Port.Description"));
    p = obs_properties_add_bool(props, "automatic_fov", text("AutomaticFOV"));
    obs_property_set_long_description(p, text("AutomaticFOV.Description"));
    obs_property_set_modified_callback(p, automatic_changed);
    slider(props, "horizontal_fov", "HorizontalFOV", 10, 170, 0.1, "°", "ManualFOV.Description");
    slider(props, "vertical_fov", "VerticalFOV", 10, 170, 0.1, "°", "ManualFOV.Description");
    slider(props, "delay_ms", "Delay", 0, 1000, 1, " ms", "Delay.Description");
    slider(props, "smoothing", "Smoothing", 0, 100, 1, "%", "Smoothing.Description");
    slider(props, "horizon_level", "HorizonLevel", 0, 100, 1, "%", "HorizonLevel.Description");
    slider(props, "horizon_max_degrees", "HorizonMax", 0, 60, 1, "°", "HorizonMax.Description");
    slider(props, "image_max_shift", "Image.MaxShift", 0, 5, 0.1, "%", "Image.MaxShift.Description");
    slider(props, "image_max_roll", "Image.MaxRoll", 0, 5, 0.1, "°", "Image.MaxRoll.Description");
    p = obs_properties_add_bool(props, "image_track_roll", text("Image.TrackRoll"));
    obs_property_set_long_description(p, text("Image.TrackRoll.Description"));
    slider(props, "region_left", "Image.RegionLeft", 0, 90, 1, "%", "Image.Region.Description");
    slider(props, "region_right", "Image.RegionRight", 0, 90, 1, "%", "Image.Region.Description");
    slider(props, "region_top", "Image.RegionTop", 0, 90, 1, "%", "Image.Region.Description");
    slider(props, "region_bottom", "Image.RegionBottom", 0, 90, 1, "%", "Image.Region.Description");
    slider(props, "stabilization_crop", "StabilizationCrop", 0, 40, 0.1, "%",
           "StabilizationCrop.Description");
    slider(props, "framing_zoom", "FramingZoom", 0, 40, 0.1, "%", "FramingZoom.Description");
    p = obs_properties_add_bool(props, "output_16_9", text("Output16x9"));
    obs_property_set_long_description(p, text("Output16x9.Description"));
    slider(props, "framing_x", "FramingX", -100, 100, 1, "%", "FramingX.Description");
    slider(props, "framing_y", "FramingY", -100, 100, 1, "%", "FramingY.Description");
    slider(props, "input_left", "InputLeft", 0, 45, 0.1, "%", "InputTrim.Description");
    slider(props, "input_right", "InputRight", 0, 45, 0.1, "%", "InputTrim.Description");
    slider(props, "input_top", "InputTop", 0, 45, 0.1, "%", "InputTrim.Description");
    slider(props, "input_bottom", "InputBottom", 0, 45, 0.1, "%", "InputTrim.Description");
    obs_properties_add_text(props, "diagnostics", "", OBS_TEXT_INFO);
    obs_properties_add_button2(props, "refresh_diagnostics", text("RefreshDiagnostics"), refresh_clicked,
                               data);
    p = obs_properties_add_button2(props, "reset_smoothing", text("ResetSmoothing"), reset_clicked, data);
    obs_property_set_long_description(p, text("ResetSmoothing.Description"));
    auto *settings = data ? obs_source_get_settings(static_cast<Filter *>(data)->source) : obs_data_create();
    mode_changed(props, nullptr, settings);
    automatic_changed(props, nullptr, settings);
    obs_data_release(settings);
    refresh_diagnostics(props, static_cast<Filter *>(data));
    return props;
}

void tick(void *data, float) {
    auto *filter = static_cast<Filter *>(data);
    const auto configuration = filter->settings();
    filter->image.tick();
    const bool image_active = configuration.image_mode && obs_source_enabled(filter->source);
    if ((!image_active && filter->image_active) ||
        filter->reset_image.exchange(false, std::memory_order_relaxed)) {
        obs_enter_graphics();
        filter->image.clear();
        obs_leave_graphics();
        std::lock_guard<std::mutex> lock(filter->image_diagnostics_mutex);
        filter->image_diagnostics = {};
    }
    filter->image_active = image_active;
    auto *target = obs_filter_get_target(filter->source);
    if (!target) {
        filter->reset_image.store(true, std::memory_order_relaxed);
        filter->dimensions.store(0, std::memory_order_relaxed);
        return;
    }
    const auto source_width = obs_source_get_base_width(target);
    const auto source_height = obs_source_get_base_height(target);
    if (!source_width || !source_height) {
        filter->reset_image.store(true, std::memory_order_relaxed);
        filter->dimensions.store(0, std::memory_order_relaxed);
        return;
    }
    if (configuration.image_mode)
        filter->set_dimensions(vrst::make_image_warp(source_width, source_height, configuration.warp, {}));
    else
        filter->set_dimensions(vrst::make_warp(source_width, source_height, configuration.warp));
}

void set_vector(gs_eparam_t *parameter, double x, double y, double z, double w) {
    // gs_effect_set_val copies the float4; no dependency on OBS's SIMD vector headers.
    const float value[] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z),
                           static_cast<float>(w)};
    gs_effect_set_val(parameter, value, sizeof(value));
}

void render(void *data, gs_effect_t *) {
    auto *filter = static_cast<Filter *>(data);
    auto *target = obs_filter_get_target(filter->source);
    if (!target || !filter->effect) {
        obs_source_skip_video_filter(filter->source);
        return;
    }
    const auto width = obs_source_get_base_width(target);
    const auto height = obs_source_get_base_height(target);
    if (!width || !height) {
        filter->dimensions.store(0, std::memory_order_relaxed);
        obs_source_skip_video_filter(filter->source);
        return;
    }
    const auto configuration = filter->settings();
    if (configuration.image_mode) {
        try {
            if (filter->image.render(filter->source, width, height, configuration.warp, configuration.image,
                                     configuration.image_revision)) {
                filter->set_dimensions(filter->image.warp());
                filter->applied_correction.store(filter->image.warp().correction_fraction,
                                                 std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(filter->image_diagnostics_mutex);
                filter->image_diagnostics = filter->image.result();
                return;
            }
        } catch (const std::exception &error) {
            blog(LOG_WARNING, "[vr-stabilizer] Image pipeline reset: %s", error.what());
            filter->image.clear();
        }
        // Preserve the same framing and dimensions even if staging or capture
        // allocation fails, with no correction from an earlier frame.
        if (filter->image.fallback(filter->source, width, height, configuration.warp)) {
            filter->set_dimensions(filter->image.warp());
            std::lock_guard<std::mutex> lock(filter->image_diagnostics_mutex);
            filter->image_diagnostics = filter->image.result();
        } else
            obs_source_skip_video_filter(filter->source);
        return;
    }
    const auto snapshot =
        filter->receiver.snapshot(configuration.warp.delay_ms, configuration.warp.smoothing);
    if (configuration.peer_revision != filter->rendered_peer_revision) {
        filter->last_projection.reset();
        filter->rendered_peer_revision = configuration.peer_revision;
    }
    if (snapshot.pose.valid && vrst::valid(snapshot.pose.projection))
        filter->last_projection = snapshot.pose.projection;
    const auto automatic = configuration.warp.automatic_fov ? filter->last_projection : std::nullopt;
    const auto warp =
        vrst::make_warp(width, height, configuration.warp,
                        snapshot.pose.valid ? vrst::horizon_correction(snapshot.pose.source_orientation,
                                                                       snapshot.pose.target_orientation,
                                                                       configuration.warp.horizon_level,
                                                                       configuration.warp.horizon_max_degrees)
                                            : vrst::Quat{},
                        automatic);
    filter->set_dimensions(warp);
    filter->applied_correction.store(warp.correction_fraction, std::memory_order_relaxed);
    filter->using_automatic_fov.store(automatic.has_value(), std::memory_order_relaxed);

    // OBS captures the complete target at its base dimensions. The final sprite
    // uses warp.width/height, but retains the complete texture's normalized UVs.
    // Disallow direct rendering because a capture-card/preceding filter source
    // can have arbitrary vertex geometry and our output dimensions may differ.
    if (!obs_source_process_filter_begin(filter->source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
        return;
    set_vector(filter->input_projection, warp.input.left, warp.input.right, warp.input.up, warp.input.down);
    set_vector(filter->output_projection, warp.output.left, warp.output.right, warp.output.up,
               warp.output.down);
    set_vector(filter->correction, warp.correction.x, warp.correction.y, warp.correction.z,
               warp.correction.w);
    set_vector(filter->input_uv, warp.uv_left, warp.uv_right, warp.uv_top, warp.uv_bottom);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(filter->source, filter->effect, warp.width, warp.height);
    gs_blend_state_pop();
}

uint32_t width(void *data) {
    return static_cast<std::uint32_t>(
        static_cast<Filter *>(data)->dimensions.load(std::memory_order_relaxed) >> 32);
}
uint32_t height(void *data) {
    return static_cast<std::uint32_t>(
        static_cast<Filter *>(data)->dimensions.load(std::memory_order_relaxed));
}
const char *name(void *) {
    return text("FilterName");
}
} // namespace

const char *obs_module_name(void) {
    return "VR Stabilizer";
}
const char *obs_module_description(void) {
    return "VR video stabilization using SteamVR tracking or image-based microjitter correction.";
}

bool obs_module_load(void) {
    obs_source_info info{};
    info.id = "vr-stabilizer";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.video_tick = tick;
    info.video_render = render;
    info.get_width = width;
    info.get_height = height;
    obs_register_source(&info);
    return true;
}
