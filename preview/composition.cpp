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
Quat bounded_camera(const Warp &warp, Quat head, Quat desired, double allowance) {
    auto fits = [&](Quat q) {
        auto at = warp;
        at.correction = q;
        for (double x : {0., 1.})
            for (double y : {0., 1.}) {
                const auto ray = rotate(q, {warp.output.left + x * (warp.output.right - warp.output.left),
                                            warp.output.up + y * (warp.output.down - warp.output.up), -1});
                const auto uv = map_uv(at, x, y);
                if (ray.z >= -.05 || uv[0] < -allowance || uv[0] > 1 + allowance || uv[1] < -allowance ||
                    uv[1] > 1 + allowance)
                    return false;
            }
        return true;
    };
    auto limit = [&](Quat start, Quat end) {
        if (fits(end))
            return end;
        double lo = 0, hi = 1;
        for (unsigned n = 0; n < 24; ++n) {
            const double mid = (lo + hi) * .5;
            if (fits(slerp(start, end, mid)))
                lo = mid;
            else
                hi = mid;
        }
        return slerp(start, end, lo);
    };
    // Head following gets the available correction budget first; gaze uses only
    // what remains. The shader's finite mirror allowance is part of that budget.
    const auto safe_head = limit({}, head);
    return limit(safe_head, desired);
}
} // namespace
void apply_stream_comfort(Options &options) {
    options.smoothing = 60;
    options.horizon_lock = 50;
    options.adaptive_horizon = true;
    options.crop = 12;
    options.mirror = true;
    options.mirror_limit = 12;
    options.mirror_softness = 70;
    options.stabilize = true;
}
FramingRegion framing_region(double source_aspect, const Options &options, EdgeReserve reserve) {
    const auto edge = [](double v) { return std::isfinite(v) ? std::clamp(v, 0., 45.) / 100 : 0.; };
    FramingRegion r{edge(options.crop_left), 1 - edge(options.crop_right), edge(options.crop_top),
                    1 - edge(options.crop_bottom)};
    const double aspect = std::isfinite(options.aspect) ? std::clamp(options.aspect, .25, 4.) : 16. / 9.;
    double w = r.right - r.left, h = r.bottom - r.top;
    if (w * source_aspect / h > aspect)
        w = h * aspect / source_aspect;
    else
        h = w * source_aspect / aspect;
    // The shared pose/image engines reserve at most 40%. Apply the remaining
    // zoom in the actual output projection, keeping their legacy limits intact.
    if (!reserve.enabled && std::isfinite(options.crop) && options.crop > 40.) {
        const double scale = (100. - std::min(options.crop, framing_crop_max)) / 60.;
        w *= scale;
        h *= scale;
    }
    const double cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    FramingRegion fitted{cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2};
    if (reserve.enabled) {
        reserve = sanitize_edge_reserve(reserve);
        const double bx =
            reserve.left + reserve.right > 0 ? reserve.left / (reserve.left + reserve.right) : .5;
        const double by =
            reserve.top + reserve.bottom > 0 ? reserve.top / (reserve.top + reserve.bottom) : .5;
        const double left = r.left + (r.right - r.left - w) * bx;
        const double top = r.top + (r.bottom - r.top - h) * by;
        fitted = {left, left + w, top, top + h};
    }
    // Framing coordinates are relative to the reserved image. Placement spans
    // the full captured bounds, including stabilization reserve and edge trims.
    const auto edges = sanitize_edge_reserve(reserve);
    const double uniform = std::isfinite(options.crop) ? std::clamp(options.crop, 0., 40.) : 10.;
    auto place = [&](double &low, double &high, double value, double first, double last) {
        const double offset = std::isfinite(value) ? std::clamp(value, -100., 100.) / 100 : 0.;
        const double a = (edges.enabled ? first : uniform / 2) / 100;
        const double b = (edges.enabled ? last : uniform / 2) / 100;
        const double keep = 1 - a - b;
        const double slack = offset < 0 ? low + a / keep : 1 + b / keep - high;
        const double shift = offset * std::max(0., slack);
        low += shift;
        high += shift;
    };
    place(fitted.left, fitted.right, options.horizontal_offset, edges.left, edges.right);
    place(fitted.top, fitted.bottom, options.vertical_offset, edges.top, edges.bottom);
    return fitted;
}
Composition FrameStabilizer::update(const Frame &frame, const Options &options, const Quat *lookahead_target,
                                   double speed_kph) {
    Composition result;
    if (frame.view_count < 1 || frame.view_count > 2 || frame.display_time <= 0 || frame.eye > 1)
        return result;
    for (unsigned i = 0; i < frame.view_count; ++i) {
        const auto &v = frame.views[i];
        if (!valid(orientation(v)) || !valid(projection(v)) || v.rect_width <= 0 || v.rect_height <= 0)
            return result;
    }
    if (frame.session != previous_.session || frame.generation != previous_.generation ||
        frame.view_count != previous_.view_count || frame.reference_epoch != previous_.reference_epoch ||
        frame.producer_pid != previous_.producer_pid || frame.sequence < previous_.sequence ||
        frame.display_time < previous_.display_time || frame.eye != previous_.eye) {
        // A learning request can be armed before the first captured frame.
        if (previous_.session)
            reset();
        else {
            history_.clear();
            horizon_up_sign_ = 0;
            pitch_anchor_valid_ = false;
            director_.reset();
        }
    }
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
    if (!pitch_anchor_valid_) {
        const auto forward = rotate(orientation(frame.views[0]), {0, 0, -1});
        const double horizontal = std::hypot(forward.x, forward.z);
        // A vertical startup has no reliable heading; calibrate once the view
        // returns to an unambiguous forward direction, in either layer hemisphere.
        if (horizontal > .25) {
            pitch_anchor_ = std::atan2(forward.y, horizontal);
            pitch_anchor_valid_ = true;
        }
    }
    Settings settings;
    settings.automatic_fov = true;
    settings.stabilization_crop = options.crop;
    settings.mirror_edges = options.mirror;
    settings.output_16_9 = false;
    settings.edge_reserve = options.edge_reserve[frame.eye];
    const bool compare_camera = options.compare_original && !options.compare_effects;
    const bool director_enabled = !options.spectator.bypass &&
                                  (options.director.lookahead_enabled || options.director.auto_crop_enabled ||
                                   options.director.speed_fov_enabled);
    // Director state always sees the enhanced camera, even if the first draw
    // of this captured frame is the original side of an A/B comparison.
    const bool compare_before_director = compare_camera && !director_enabled;
    const bool stabilize = options.stabilize && !compare_before_director;
    const bool pitch_on = pitch_anchor_valid_ && stabilize && std::isfinite(options.pitch_lock) &&
                          options.pitch_lock > 0;
    const bool auto_crop = !options.spectator.bypass && options.director.auto_crop_enabled;
    const bool director_framing =
        auto_crop || (!options.spectator.bypass && options.director.speed_fov_enabled);
    // Standalone profiles may still contain the legacy natural-motion flag.
    // Ordinary PoseHistory smoothing remains the only head-motion filter.
    const bool camera_enabled = !options.spectator.bypass && options.spectator.gaze_enabled;
    const auto base_correction =
        options.director.lookahead_enabled && !options.spectator.bypass && lookahead_target &&
                valid(*lookahead_target)
            ? normalized(multiply(conjugate(orientation(frame.views[0])), *lookahead_target))
            : pose.correction;
    Quat correction = stabilize ? base_correction : Quat{};
    Quat head_correction = correction;
    if (camera_enabled || spectator_.calibration_active()) {
        SpectatorInput input;
        input.source_orientation = orientation(frame.views[0]);
        input.base_target_orientation =
            multiply(input.source_orientation, options.stabilize ? base_correction : Quat{});
        input.gaze = frame.gaze;
        input.time_ns = static_cast<std::uint64_t>(frame.display_time);
        input.session = frame.session;
        input.sequence = frame.sequence;
        input.reference_epoch = frame.reference_epoch;
        auto tuning = options.spectator;
        tuning.natural_enabled = false;
        const auto target = spectator_.update(input, tuning);
        spectator_active_ = true;
        head_correction =
            compare_before_director
                ? Quat{}
                : normalized(multiply(conjugate(input.source_orientation), spectator_.head_target()));
        if (camera_enabled)
            correction = compare_before_director
                             ? Quat{}
                             : normalized(multiply(conjugate(input.source_orientation), target));
    } else if (spectator_active_) {
        spectator_.suspend();
        spectator_active_ = false;
    }
    const bool final_framing = options.crop > 40. || director_framing || camera_enabled || pitch_on || settings.edge_reserve.enabled ||
                               (std::isfinite(options.horizontal_offset) && options.horizontal_offset != 0) ||
                               (std::isfinite(options.vertical_offset) && options.vertical_offset != 0);
    result.warp = make_warp(frame.views[0].rect_width, frame.views[0].rect_height, settings,
                            !final_framing ? correction : Quat{}, projection(frame.views[0]));
    if (final_framing)
        result.warp.correction = correction;
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
    const bool level_on =
        horizon_up_sign_ != 0 && stabilize && std::isfinite(options.horizon_lock) && options.horizon_lock > 0;
    auto level = [&](Quat target) {
        if (pitch_on) {
            const auto forward = rotate(target, {0, 0, -1});
            const double horizontal = std::hypot(forward.x, forward.z);
            const double fade = std::clamp((horizontal - .05) / .20, 0., 1.);
            if (fade > 0) {
                const double delta = std::atan2(forward.y, horizontal) - pitch_anchor_;
                constexpr double knee = 15 * pi / 180;
                // One spatial response after ordinary smoothing: small changes
                // stay near the anchor, deliberate larger looks follow smoothly.
                // Maximum correction is strength * knee/2, with no dead zone or
                // time-dependent anchor drift (including repeated draws/seeks).
                const double amount = delta * (std::clamp(options.pitch_lock, 0., 100.) / 100) * fade /
                                      (1 + delta * delta / (knee * knee));
                const Vec3 right{-forward.z / horizontal, 0, forward.x / horizontal};
                target = normalized(multiply(axis_angle(right, -amount), target));
            }
        }
        if (level_on) {
            const auto right = rotate(target, {1, 0, 0}), up = rotate(target, {0, 1, 0});
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
            target = normalized(multiply(axis_angle(rotate(target, {0, 0, -1}), roll), target));
        }
        return target;
    };
    if (level_on || pitch_on) {
        effective_target = level(effective_target);
        result.warp.correction =
            normalized(multiply(conjugate(orientation(frame.views[0])), effective_target));
    }
    auto &out = result.warp.output;
    const auto region =
        framing_region((out.right - out.left) / (out.up - out.down), options, settings.edge_reserve);
    const double x = out.left, y = out.up, w = out.right - out.left, h = out.down - out.up;
    out = {x + w * region.left, x + w * region.right, y + h * region.top, y + h * region.bottom};
    // This includes actual trims, aspect and vertical placement. Auto crop can
    // still protect gaze framing when pose stabilization itself is switched off.
    director_.update(result.warp, options.director, static_cast<std::uint64_t>(frame.display_time),
                     frame.sequence, speed_kph, options.spectator.bypass);
    if (final_framing) {
        // Reserve, extra framing and horizon leveling all describe the final
        // output rays. Limit those rays, not an earlier, wider intermediate.
        if (camera_enabled || pitch_on) {
            const auto head = level(multiply(orientation(frame.views[0]), head_correction));
            head_correction = normalized(multiply(conjugate(orientation(frame.views[0])), head));
            const double allowance = !auto_crop && options.mirror && std::isfinite(options.mirror_limit)
                                         ? std::clamp(options.mirror_limit, 0., 50.) / 100
                                         : 0;
            const auto requested = result.warp.correction;
            result.warp.correction = bounded_camera(result.warp, head_correction, requested, allowance);
            const double angle = angle_between({}, requested);
            if (angle > 1e-9)
                result.warp.correction_fraction =
                    std::clamp(angle_between({}, result.warp.correction) / angle, 0., 1.);
        } else
            limit_warp_correction(result.warp, result.warp.correction, !auto_crop && settings.mirror_edges);
        effective_target = multiply(orientation(frame.views[0]), result.warp.correction);
    }
    if (compare_camera && director_enabled) {
        result.warp.correction = {};
        effective_target = orientation(frame.views[0]);
    }
    result.focus_correction =
        multiply(conjugate(orientation(frame.views[frame.view_count - 1])), effective_target);
    result.valid = true;
    return result;
}
void FrameStabilizer::reset() {
    history_.clear();
    previous_ = {};
    horizon_up_sign_ = 0;
    pitch_anchor_valid_ = false;
    spectator_.reset();
    spectator_active_ = false;
    director_.reset();
}
void FrameStabilizer::reset_horizon() {
    horizon_up_sign_ = 0;
    pitch_anchor_valid_ = false;
}
} // namespace vrst::preview
