#include "openxr_source_settings.hpp"
#include "../preview/overlay_settings.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

namespace vrst_plugin {
namespace {
using Options = vrst::preview::Options;
struct Number {
    const char *key, *label;
    double Options::*member;
    double low, high;
};
constexpr Number numbers[]{
    {"smoothing", "Smoothing (%)", &Options::smoothing, 0, 100},
    {"crop", "Stabilization reserve (%)", &Options::crop, 0, 35},
    {"horizon_lock", "Horizon lock (%)", &Options::horizon_lock, 0, 100},
    {"mirror_limit", "Mirror reach (%)", &Options::mirror_limit, 0, 50},
    {"mirror_softness", "Mirror softness (%)", &Options::mirror_softness, 0, 100},
    {"mirror_stretch", "Edge stretch (%)", &Options::mirror_stretch, 0, 100},
    {"crop_left", "Crop left (%)", &Options::crop_left, 0, 45},
    {"crop_right", "Crop right (%)", &Options::crop_right, 0, 45},
    {"crop_top", "Crop top (%)", &Options::crop_top, 0, 45},
    {"crop_bottom", "Crop bottom (%)", &Options::crop_bottom, 0, 45},
    {"aspect", "Output aspect ratio (width / height)", &Options::aspect, .25, 4},
    {"vertical_offset", "Vertical framing offset (%)", &Options::vertical_offset, -100, 100}};
struct Flag {
    const char *key, *label;
    bool Options::*member;
};
constexpr Flag flags[]{{"stabilize", "Enable stabilization", &Options::stabilize},
                       {"mirror", "Mirror exposed edges", &Options::mirror},
                       {"adaptive_horizon", "Adaptive horizon", &Options::adaptive_horizon}};
const char *edges[]{"left", "right", "top", "bottom"};
const wchar_t *ini_edges[]{L"LeftTenths", L"RightTenths", L"TopTenths", L"BottomTenths"};
std::string reserve_key(unsigned eye, const char *edge) {
    return std::string(eye ? "right_reserve_" : "left_reserve_") + edge;
}
double number(obs_data_t *s, const char *key, double fallback, double low, double high) {
    if (!obs_data_has_user_value(s, key) && !obs_data_has_default_value(s, key))
        return fallback;
    const double v = obs_data_get_double(s, key);
    return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
}
bool flag(obs_data_t *s, const char *key, bool fallback) {
    return obs_data_has_user_value(s, key) || obs_data_has_default_value(s, key) ? obs_data_get_bool(s, key)
                                                                                 : fallback;
}
} // namespace
void openxr_defaults(obs_data_t *s) {
    const Options o;
    for (const auto &n : numbers)
        obs_data_set_default_double(s, n.key, o.*n.member);
    for (const auto &f : flags)
        obs_data_set_default_bool(s, f.key, o.*f.member);
    obs_data_set_default_int(s, "output_width", o.output_width);
    obs_data_set_default_int(s, "output_height", o.output_height);
    obs_data_set_default_int(s, "eye", 0);
    obs_data_set_default_int(s, "compare_mode", 0);
    obs_data_set_default_int(s, "overlay_mode", 0);
    obs_data_set_default_int(s, "overlay_mask", 0);
    obs_data_set_default_int(s, "overlay_producer_pid", 0);
    obs_data_set_default_string(s, "overlay_session", "0");
    obs_data_set_default_int(s, "fps", 60);
    obs_data_set_default_bool(s, "paused", false);
    for (unsigned eye = 0; eye < 2; ++eye) {
        obs_data_set_default_bool(s, reserve_key(eye, "enabled").c_str(), false);
        for (auto edge : edges)
            obs_data_set_default_double(s, reserve_key(eye, edge).c_str(), 5);
    }
}
OpenXRSettings openxr_settings(obs_data_t *s) {
    OpenXRSettings c;
    for (const auto &n : numbers)
        c.options.*n.member = number(s, n.key, c.options.*n.member, n.low, n.high);
    for (const auto &f : flags)
        c.options.*f.member = flag(s, f.key, c.options.*f.member);
    c.options.output_width = unsigned(number(s, "output_width", 3840, 320, 7680));
    c.options.output_height = unsigned(number(s, "output_height", 2160, 180, 4320));
    c.options.eye = unsigned(number(s, "eye", 0, 0, 1));
    c.options.compare_mode = unsigned(number(s, "compare_mode", 0, 0, 2));
    c.options.overlays.mode = unsigned(number(s, "overlay_mode", 0, 0, 2));
    c.options.overlays.mask = unsigned(obs_data_get_int(s, "overlay_mask")) & 0xffff;
    c.options.overlays.producer_pid = unsigned(obs_data_get_int(s, "overlay_producer_pid"));
    const char *session = obs_data_get_string(s, "overlay_session");
    char *end{};
    c.options.overlays.session =
        session && *session >= '0' && *session <= '9' ? std::strtoull(session, &end, 10) : 0;
    if (end && *end)
        c.options.overlays.session = 0;
    c.fps = unsigned(number(s, "fps", 60, 30, 90));
    if (c.fps != 30 && c.fps != 60 && c.fps != 90)
        c.fps = 60;
    c.paused = flag(s, "paused", false);
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto &r = c.options.edge_reserve[eye];
        r.enabled = flag(s, reserve_key(eye, "enabled").c_str(), false);
        double *values[]{&r.left, &r.right, &r.top, &r.bottom};
        for (unsigned i = 0; i < 4; ++i)
            *values[i] = number(s, reserve_key(eye, edges[i]).c_str(), 5, 0, 35);
    }
    return c;
}
void openxr_store(obs_data_t *s, const OpenXRSettings &c) {
    for (const auto &n : numbers)
        obs_data_set_double(s, n.key, c.options.*n.member);
    for (const auto &f : flags)
        obs_data_set_bool(s, f.key, c.options.*f.member);
    obs_data_set_int(s, "output_width", c.options.output_width);
    obs_data_set_int(s, "output_height", c.options.output_height);
    obs_data_set_int(s, "eye", c.options.eye);
    obs_data_set_int(s, "compare_mode", c.options.compare_mode);
    obs_data_set_int(s, "overlay_mode", c.options.overlays.mode);
    obs_data_set_int(s, "overlay_mask", c.options.overlays.mask);
    obs_data_set_int(s, "overlay_producer_pid", c.options.overlays.producer_pid);
    obs_data_set_string(s, "overlay_session", std::to_string(c.options.overlays.session).c_str());
    obs_data_set_int(s, "fps", c.fps);
    obs_data_set_bool(s, "paused", c.paused);
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto &r = c.options.edge_reserve[eye];
        obs_data_set_bool(s, reserve_key(eye, "enabled").c_str(), r.enabled);
        const double values[]{r.left, r.right, r.top, r.bottom};
        for (unsigned i = 0; i < 4; ++i)
            obs_data_set_double(s, reserve_key(eye, edges[i]).c_str(), values[i]);
    }
}
void openxr_add_properties(obs_properties_t *p) {
    auto *eye = obs_properties_add_list(p, "eye", "Captured eye", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(eye, "Left eye", 0);
    obs_property_list_add_int(eye, "Right eye", 1);
    auto *fps =
        obs_properties_add_list(p, "fps", "Capture frame rate", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    for (auto value : {30, 60, 90})
        obs_property_list_add_int(fps, std::to_string(value).c_str(), value);
    obs_properties_add_bool(p, "paused", "Pause capture");
    auto *compare = obs_properties_add_list(p, "compare_mode", "Comparison mode", OBS_COMBO_TYPE_LIST,
                                            OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(compare, "Original", 0);
    obs_property_list_add_int(compare, "50/50 split (left stabilized / right original)", 1);
    obs_property_list_add_int(compare, "Side by side (both full images)", 2);
    for (const auto &f : flags)
        obs_properties_add_bool(p, f.key, f.label);
    for (const auto &n : numbers)
        obs_properties_add_float_slider(p, n.key, n.label, n.low, n.high,
                                        n.member == &Options::aspect ? .001 : .1);
    obs_properties_add_text(
        p, "mirror_stretch_help",
        "Edge stretch: 0% keeps mirroring; 100% strongly stretches pixels outside the captured image. "
        "Mirror reach still caps the effect; enable Mirror exposed edges to use it.",
        OBS_TEXT_INFO);
    obs_properties_add_int(p, "output_width", "Output width", 320, 7680, 1);
    obs_properties_add_int(p, "output_height", "Output height", 180, 4320, 1);
    for (unsigned eye_index = 0; eye_index < 2; ++eye_index) {
        auto *group = obs_properties_create();
        for (auto edge : edges) {
            const auto key = reserve_key(eye_index, edge);
            const std::string label = std::string("Reserve ") + edge + " (%)";
            obs_properties_add_float_slider(group, key.c_str(), label.c_str(), 0, 35, .1);
        }
        obs_properties_add_group(p, reserve_key(eye_index, "enabled").c_str(),
                                 eye_index ? "Separate right-eye reserves" : "Separate left-eye reserves",
                                 OBS_GROUP_CHECKABLE, group);
    }
}
bool openxr_import(obs_data_t *s, const std::wstring &path, std::string &error) {
    error.clear();
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "Choose an existing SteadyXR INI file.";
        return false;
    }
    wchar_t section[32768]{};
    if (!GetPrivateProfileSectionW(L"Preview", section, 32768, path.c_str())) {
        error = "The INI file has no Preview settings.";
        return false;
    }
    auto value = [&](const wchar_t *section_name, const wchar_t *key, int fallback, int low, int high) {
        return std::clamp(int(GetPrivateProfileIntW(section_name, key, fallback, path.c_str())), low, high);
    };
    auto v = [&](const wchar_t *key, int fallback, int low, int high) {
        return value(L"Preview", key, fallback, low, high);
    };
    OpenXRSettings c;
    auto &o = c.options;
    o.overlays = vrst::preview::read_overlay_settings(path);
    o.smoothing = v(L"SmoothingTenths", v(L"Smoothing", 40, 0, 100) * 10, 0, 1000) / 10.;
    o.crop = v(L"CropTenths", v(L"Crop", 10, 0, 35) * 10, 0, 350) / 10.;
    o.crop_left = v(L"CropLeftTenths", 0, 0, 450) / 10.;
    o.crop_right = v(L"CropRightTenths", 0, 0, 450) / 10.;
    o.crop_top = v(L"CropTopTenths", 0, 0, 450) / 10.;
    o.crop_bottom = v(L"CropBottomTenths", 0, 0, 450) / 10.;
    o.horizon_lock = v(L"HorizonTenths", 0, 0, 1000) / 10.;
    o.adaptive_horizon = v(L"AdaptiveHorizon", 1, 0, 1) != 0;
    o.mirror_limit = v(L"MirrorLimitTenths", 100, 0, 500) / 10.;
    o.mirror_softness = v(L"MirrorSoftnessTenths", 600, 0, 1000) / 10.;
    o.mirror_stretch = v(L"MirrorStretchTenths", 0, 0, 1000) / 10.;
    o.output_width = v(L"OutputWidth", 3840, 320, 7680);
    o.output_height = v(L"OutputHeight", 2160, 180, 4320);
    o.aspect = double(v(L"AspectNumerator", 16, 1, 10000)) / v(L"AspectDenominator", 9, 1, 10000);
    if (o.aspect < .25 || o.aspect > 4)
        o.aspect = 16. / 9.;
    o.mirror = v(L"Mirror", 1, 0, 1) != 0;
    o.stabilize = v(L"Stabilize", 1, 0, 1) != 0;
    o.eye = v(L"Eye", 0, 0, 1);
    o.compare_mode = v(L"CompareMode", 0, 0, 2);
    o.vertical_offset = v(L"VerticalOffsetTenths", 0, -1000, 1000) / 10.;
    c.fps = v(L"FPS", 60, 30, 90);
    if (c.fps != 30 && c.fps != 60 && c.fps != 90)
        c.fps = 60;
    c.paused = flag(s, "paused", false);
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto &r = o.edge_reserve[eye];
        const auto name = eye ? L"RightEyeReserve" : L"LeftEyeReserve";
        r.enabled = value(name, L"Enabled", 0, 0, 1) != 0;
        double *values[]{&r.left, &r.right, &r.top, &r.bottom};
        for (unsigned i = 0; i < 4; ++i)
            *values[i] = value(name, ini_edges[i], int(o.crop * 5), 0, 350) / 10.;
    }
    openxr_store(s, c);
    return true;
}
} // namespace vrst_plugin
