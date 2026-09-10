#include "renderer.hpp"
#include <algorithm>
#include <cmath>
namespace vrst::preview {
namespace {
Quat orientation(const View &v) {
    return {v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]};
}
Projection projection(const View &v) {
    return {v.fov[0], v.fov[1], v.fov[2], v.fov[3]};
}
} // namespace
FramingRegion framing_region(double source_aspect, const Options &options) {
    const auto edge = [](double v) { return std::isfinite(v) ? std::clamp(v, 0., 45.) / 100 : 0.; };
    FramingRegion r{edge(options.crop_left), 1 - edge(options.crop_right), edge(options.crop_top),
                    1 - edge(options.crop_bottom)};
    const double aspect = std::isfinite(options.aspect) ? std::clamp(options.aspect, .25, 4.) : 16. / 9.;
    double w = r.right - r.left, h = r.bottom - r.top;
    if (w * source_aspect / h > aspect)
        w = h * aspect / source_aspect;
    else
        h = w * source_aspect / aspect;
    const double cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    return {cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2};
}
Composition FrameStabilizer::update(const Frame &frame, const Options &options) {
    Composition result;
    if (frame.view_count < 1 || frame.view_count > 2 || frame.display_time <= 0)
        return result;
    for (unsigned i = 0; i < frame.view_count; ++i) {
        const auto &v = frame.views[i];
        if (!valid(orientation(v)) || !valid(projection(v)) || v.rect_width <= 0 || v.rect_height <= 0)
            return result;
    }
    if (frame.session != previous_.session || frame.generation != previous_.generation ||
        frame.view_count != previous_.view_count || frame.reference_epoch != previous_.reference_epoch ||
        frame.producer_pid != previous_.producer_pid || frame.sequence < previous_.sequence ||
        frame.display_time < previous_.display_time)
        reset();
    history_.set_smoothing(options.smoothing);
    if (frame.sequence != previous_.sequence || history_.size() == 0) {
        Pose pose;
        pose.session = frame.session;
        pose.sequence = frame.sequence;
        pose.time_ns = static_cast<std::uint64_t>(frame.display_time);
        pose.orientation = orientation(frame.views[0]);
        pose.projection = projection(frame.views[0]);
        pose.flags = TrackingValid | FramePose;
        if (!history_.push(pose))
            return result;
        previous_ = frame;
    }
    const auto pose = history_.sample(static_cast<std::uint64_t>(frame.display_time));
    if (!pose.valid)
        return result;
    Settings settings;
    settings.automatic_fov = true;
    settings.stabilization_crop = options.crop;
    settings.mirror_edges = options.mirror;
    settings.output_16_9 = false;
    result.warp = make_warp(frame.views[0].rect_width, frame.views[0].rect_height, settings,
                            options.stabilize ? pose.correction : Quat{}, projection(frame.views[0]));
    auto effective_target = multiply(orientation(frame.views[0]), result.warp.correction);
    // Projection poses are expressed in the application's layer space, not
    // necessarily gravity-aligned runtime space. iRacing can submit an upright
    // image with eye-up pointing along -Y. Choose the upright hemisphere from
    // the source once, even with leveling disabled, and keep it until reset.
    // Re-selecting every frame would flip the horizon during a large head tilt.
    if (horizon_up_sign_ == 0) {
        const double source_up_y = rotate(orientation(frame.views[0]), {0, 1, 0}).y;
        if (std::abs(source_up_y) > .25)
            horizon_up_sign_ = source_up_y > 0 ? 1 : -1;
    }
    if (horizon_up_sign_ != 0 && options.stabilize && std::isfinite(options.horizon_lock) &&
        options.horizon_lock > 0) {
        const auto right = rotate(effective_target, {1, 0, 0}), up = rotate(effective_target, {0, 1, 0});
        const double fade = std::clamp((std::hypot(up.y, right.y) - .05) / .20, 0., 1.);
        const double tilt = std::atan2(horizon_up_sign_ * right.y, horizon_up_sign_ * up.y);
        const double remaining = 1 - std::clamp(options.horizon_lock, 0., 100.) / 100;
        double response = remaining;
        if (options.adaptive_horizon) {
            // Suppress small tilts strongly, then continuously approach the
            // familiar linear response on larger movements. The rational blend
            // has no threshold or dead zone; sign and slider endpoints survive.
            constexpr double knee = 15 * pi / 180;
            const double blend = tilt * tilt / (tilt * tilt + knee * knee);
            const double gentle = remaining * remaining * remaining;
            response *= gentle + (1 - gentle) * blend;
        }
        const double roll = tilt * (1 - response) * fade;
        effective_target =
            normalized(multiply(axis_angle(rotate(effective_target, {0, 0, -1}), roll), effective_target));
        result.warp.correction =
            normalized(multiply(conjugate(orientation(frame.views[0])), effective_target));
    }
    auto &out = result.warp.output;
    const auto region = framing_region((out.right - out.left) / (out.up - out.down), options);
    const double x = out.left, y = out.up, w = out.right - out.left, h = out.down - out.up;
    out = {x + w * region.left, x + w * region.right, y + h * region.top, y + h * region.bottom};
    result.focus_correction =
        multiply(conjugate(orientation(frame.views[frame.view_count - 1])), effective_target);
    result.valid = true;
    return result;
}
void FrameStabilizer::reset() {
    history_.clear();
    previous_ = {};
    horizon_up_sign_ = 0;
}
} // namespace vrst::preview
