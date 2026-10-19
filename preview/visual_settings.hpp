#pragma once
#include <algorithm>
#include <cmath>

namespace vrst::preview {
enum class VisualTransfer { srgb, linear, unsupported };
struct VisualRegion {
    float left = 0, top = 0, right = 1, bottom = 1;
};
struct VisualSettings {
    bool exposure = false, adaptation = false, bloom = false, color = false, bypass = false;
    VisualTransfer input_transfer = VisualTransfer::srgb;
    bool premultiplied_alpha = false;
    float exposure_ev = 0, min_gain = .25f, max_gain = 4, target_luminance = .18f;
    float brighten_rate = 1, darken_rate = 3, transition_strength = 0, transition_recovery = 3;
    float bloom_threshold = .8f, bloom_knee = .2f, bloom_strength = .1f, bloom_radius = 12;
    float temperature = 0, tint = 0, contrast = 1, saturation = 1, shadows = 0, highlights = 0;
    VisualRegion metering_region{};
    // Transient host crop: intersected with the user region, never persisted.
    VisualRegion content_region{};
    bool active() const {
        return !bypass && (exposure || adaptation || bloom || color);
    }
    VisualSettings bounded() const;
};
inline VisualSettings VisualSettings::bounded() const {
    auto v = *this;
    auto bound = [](float x, float lo, float hi, float neutral) {
        return std::isfinite(x) ? std::clamp(x, lo, hi) : neutral;
    };
    v.exposure_ev = bound(v.exposure_ev, -6, 6, 0);
    v.min_gain = bound(v.min_gain, .0625f, 16, .25f);
    v.max_gain = bound(v.max_gain, v.min_gain, 16, std::max(4.f, v.min_gain));
    v.target_luminance = bound(v.target_luminance, .01f, .8f, .18f);
    v.brighten_rate = bound(v.brighten_rate, .01f, 20, 1);
    v.darken_rate = bound(v.darken_rate, .01f, 20, 3);
    v.transition_strength = bound(v.transition_strength, 0, .3f, 0);
    v.transition_recovery = bound(v.transition_recovery, .1f, 20, 3);
    v.bloom_threshold = bound(v.bloom_threshold, 0, 4, .8f);
    v.bloom_knee = bound(v.bloom_knee, 0, 1, .2f);
    v.bloom_strength = bound(v.bloom_strength, 0, 1, .1f);
    v.bloom_radius = bound(v.bloom_radius, 0, 64, 12);
    v.temperature = bound(v.temperature, -1, 1, 0);
    v.tint = bound(v.tint, -1, 1, 0);
    v.contrast = bound(v.contrast, 0, 2, 1);
    v.saturation = bound(v.saturation, 0, 2, 1);
    v.shadows = bound(v.shadows, -1, 1, 0);
    v.highlights = bound(v.highlights, -1, 1, 0);
    auto region = [&](VisualRegion r) {
        r.left = bound(r.left, 0, .999f, 0);
        r.top = bound(r.top, 0, .999f, 0);
        r.right = bound(r.right, r.left + .001f, 1, 1);
        r.bottom = bound(r.bottom, r.top + .001f, 1, 1);
        return r;
    };
    v.metering_region = region(v.metering_region);
    v.content_region = region(v.content_region);
    return v;
}
} // namespace vrst::preview
