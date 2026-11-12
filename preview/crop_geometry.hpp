#pragma once
#include "renderer.hpp"
#include <algorithm>
#include <cmath>

namespace vrst::preview {
enum class CropHandle { left, right, top, bottom, top_left, top_right, bottom_left, bottom_right };
namespace crop_detail {
inline double source_aspect(double value) {
    return std::isfinite(value) && value > 0 ? std::clamp(value, 1e-6, 1e6) : 1.;
}
inline double output_aspect(double value) {
    return std::isfinite(value) && value > 0 ? std::clamp(value, .25, 4.) : 16. / 9.;
}
inline bool valid(FramingRegion r) {
    return std::isfinite(r.left) && std::isfinite(r.right) && std::isfinite(r.top) &&
           std::isfinite(r.bottom) && r.right > r.left && r.bottom > r.top;
}
inline double finite_delta(double value) { return std::isfinite(value) ? std::clamp(value, -2., 2.) : 0.; }
} // namespace crop_detail

// All rectangles/deltas are full-source UVs, not coordinates within the reserved
// image. source_aspect is optical tangent width/height, as used by composition:
// (fov.right-fov.left)/(fov.up-fov.down), which can differ from texture aspect.
// The editable range includes every previously valid combination of reserve
// and trims. This is framing, before pose correction.
inline FramingRegion constrain_crop(FramingRegion r, double source_aspect, double output_aspect) {
    const double ratio = crop_detail::output_aspect(output_aspect) / crop_detail::source_aspect(source_aspect);
    const double max_width = std::min(1., ratio);
    if (!crop_detail::valid(r))
        r = {};
    const double width = std::clamp(std::min(r.right - r.left, (r.bottom - r.top) * ratio),
                                    max_width * framing_min_scale, max_width);
    const double height = width / ratio;
    const double cx = std::clamp(r.left / 2 + r.right / 2, width / 2, 1 - width / 2);
    const double cy = std::clamp(r.top / 2 + r.bottom / 2, height / 2, 1 - height / 2);
    return {cx - width / 2, cx + width / 2, cy - height / 2, cy + height / 2};
}

inline FramingRegion crop_from_options(double source_aspect, const Options &options, unsigned eye = 0) {
    const auto edges = sanitize_edge_reserve(options.edge_reserve[std::min(eye, 1U)]);
    const double uniform = std::isfinite(options.crop) ? std::clamp(options.crop, 0., 40.) : 10.;
    const double left = (edges.enabled ? edges.left : uniform / 2) / 100;
    const double right = (edges.enabled ? edges.right : uniform / 2) / 100;
    const double top = (edges.enabled ? edges.top : uniform / 2) / 100;
    const double bottom = (edges.enabled ? edges.bottom : uniform / 2) / 100;
    const double width = 1 - left - right, height = 1 - top - bottom;
    const auto r = framing_region(crop_detail::source_aspect(source_aspect) * width / height, options, edges);
    return {left + width * r.left, left + width * r.right,
            top + height * r.top, top + height * r.bottom};
}

// Convert a visual edit into one canonical framing representation: uniform
// crop, zero edge trims, selected-eye reserve disabled, signed placement.
// Existing asymmetric settings are readable above; an edit within the supported
// zoom range preserves their visible rectangle. Other-eye reserve and
// non-framing settings survive.
// Offsets are percent of the available travel from centered framing to that
// full-source boundary: -100 touches left/top; +100 touches right/bottom.
inline void crop_to_options(FramingRegion r, double source_aspect, Options &options, unsigned eye = 0) {
    options.aspect = crop_detail::output_aspect(options.aspect);
    r = constrain_crop(r, source_aspect, options.aspect);
    const double max_width = std::min(1., options.aspect / crop_detail::source_aspect(source_aspect));
    const double width = r.right - r.left, height = r.bottom - r.top;
    options.crop = std::clamp((1 - width / max_width) * 100, 0., framing_crop_max);
    options.crop_left = options.crop_right = options.crop_top = options.crop_bottom = 0;
    options.edge_reserve[std::min(eye, 1U)].enabled = false;
    options.horizontal_offset = width < 1 - 1e-12
                                    ? std::clamp((r.left + r.right - 1) / (1 - width) * 100, -100., 100.) : 0;
    options.vertical_offset = height < 1 - 1e-12
                                  ? std::clamp((r.top + r.bottom - 1) / (1 - height) * 100, -100., 100.) : 0;
}

// The new profile has one framing for both eyes. Match the selected eye at its
// submitted FOV, or pass the last known source aspect when migrating offline.
inline bool migrate_legacy_framing(Options &options, double source_aspect, unsigned eye) {
    if (!options.edge_reserve[0].enabled && !options.edge_reserve[1].enabled &&
        !options.crop_left && !options.crop_right && !options.crop_top && !options.crop_bottom)
        return false;
    crop_to_options(crop_from_options(source_aspect, options, eye), source_aspect, options, eye);
    options.edge_reserve[0] = options.edge_reserve[1] = {};
    return true;
}

inline FramingRegion move_crop(FramingRegion r, double dx, double dy) {
    if (!crop_detail::valid(r) || r.right - r.left > 1 || r.bottom - r.top > 1)
        return {};
    dx = std::clamp(crop_detail::finite_delta(dx), -r.left, 1 - r.right);
    dy = std::clamp(crop_detail::finite_delta(dy), -r.top, 1 - r.bottom);
    return {r.left + dx, r.right + dx, r.top + dy, r.bottom + dy};
}

// Deltas are measured from the pointer-down rectangle. Corners anchor the
// opposite corner; edges anchor the opposite edge and the transverse center.
// A corner's pointer movement projects onto its fixed-aspect diagonal.
inline FramingRegion resize_crop(FramingRegion r, CropHandle handle, double dx, double dy,
                                 double source_aspect, double output_aspect) {
    r = constrain_crop(r, source_aspect, output_aspect);
    dx = crop_detail::finite_delta(dx);
    dy = crop_detail::finite_delta(dy);
    const double ratio = crop_detail::output_aspect(output_aspect) / crop_detail::source_aspect(source_aspect);
    const double min_width = std::min(1., ratio) * framing_min_scale;
    const double cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    double width = r.right - r.left, height = r.bottom - r.top;
    if (handle == CropHandle::left || handle == CropHandle::right) {
        const bool left = handle == CropHandle::left;
        const double anchor = left ? r.right : r.left;
        const double limit = std::min(left ? anchor : 1 - anchor, 2 * std::min(cy, 1 - cy) * ratio);
        width = std::clamp(width + (left ? -dx : dx), min_width, std::max(min_width, limit));
        height = width / ratio;
        return {left ? anchor - width : anchor, left ? anchor : anchor + width,
                cy - height / 2, cy + height / 2};
    }
    if (handle == CropHandle::top || handle == CropHandle::bottom) {
        const bool top = handle == CropHandle::top;
        const double anchor = top ? r.bottom : r.top;
        const double limit = std::min(top ? anchor : 1 - anchor, 2 * std::min(cx, 1 - cx) / ratio);
        height = std::clamp(height + (top ? -dy : dy), min_width / ratio,
                            std::max(min_width / ratio, limit));
        width = height * ratio;
        return {cx - width / 2, cx + width / 2,
                top ? anchor - height : anchor, top ? anchor : anchor + height};
    }
    const bool left = handle == CropHandle::top_left || handle == CropHandle::bottom_left;
    const bool top = handle == CropHandle::top_left || handle == CropHandle::top_right;
    const double ax = left ? r.right : r.left, ay = top ? r.bottom : r.top;
    const double limit = std::min((left ? ax : 1 - ax) / ratio, top ? ay : 1 - ay);
    const double delta_height = ((left ? -dx : dx) * ratio + (top ? -dy : dy)) / (ratio * ratio + 1);
    height = std::clamp(height + delta_height, min_width / ratio, std::max(min_width / ratio, limit));
    width = height * ratio;
    return {left ? ax - width : ax, left ? ax : ax + width,
            top ? ay - height : ay, top ? ay : ay + height};
}
} // namespace vrst::preview
