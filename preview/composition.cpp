#include "renderer.hpp"
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
    result.warp = make_warp(frame.views[0].rect_width, frame.views[0].rect_height, settings,
                            options.stabilize ? pose.correction : Quat{}, projection(frame.views[0]));
    const auto effective_target = multiply(orientation(frame.views[0]), result.warp.correction);
    result.focus_correction =
        multiply(conjugate(orientation(frame.views[frame.view_count - 1])), effective_target);
    result.valid = true;
    return result;
}
void FrameStabilizer::reset() {
    history_.clear();
    previous_ = {};
}
} // namespace vrst::preview
