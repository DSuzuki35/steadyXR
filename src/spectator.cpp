#include "vrst/spectator.hpp"
#include <algorithm>
#include <cmath>

namespace vrst {
namespace {
constexpr double rad = pi / 180;
double safe(double v, double fallback, double lo, double hi) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
double length(Vec3 v) { return std::hypot(v.x, std::hypot(v.y, v.z)); }
Vec3 unit(Vec3 v) { const auto n = length(v); return n > 1e-12 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{}; }
double separation(Vec3 a, Vec3 b) {
    return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1., 1.));
}
Vec3 rotation_vector(Quat q) {
    q = normalized(q);
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const double n = std::hypot(q.x, std::hypot(q.y, q.z));
    const double k = n > 1e-12 ? 2 * std::atan2(n, q.w) / n : 2;
    return {q.x * k, q.y * k, q.z * k};
}
Vec3 limited(Vec3 v, double max_length) {
    const auto n = length(v);
    const double scale = n > max_length ? max_length / n : 1;
    return {v.x * scale, v.y * scale, v.z * scale};
}
double natural_axis(double change, double error, double &velocity, double &speed, double dt,
                    double amount, const SpectatorSettings &s) {
    const double instant = std::clamp(change / dt, -4 * pi, 4 * pi);
    const double blend = 1 - std::exp(-dt / .12);
    velocity += blend * (instant - velocity);
    speed += blend * (std::abs(instant) - speed);
    const double coherence = speed > 1e-9 ? std::abs(velocity) / speed : 0;
    const double follow = s.natural_follow / 100 * std::clamp((coherence - .4) / .6, 0., 1.) *
                          std::clamp(std::abs(velocity) / (45 * rad), 0., 1.);
    const double suppression = s.natural_suppression * amount / 10000;
    if (suppression <= 0) return 0;
    const double tau = (.025 + .6 * suppression * suppression) * s.natural_settling_ms / 180;
    return error * std::exp(-dt * (1 + 14 * follow) / tau);
}
void ease(double &angle, double &velocity, double target, double dt, double response_ms) {
    // Scalar exponential response with speed and acceleration limits. Clamp the
    // last step to prevent overshoot in the small local gaze-offset domain.
    const double wanted = std::clamp((target - angle) / (response_ms * .001), -60 * rad, 60 * rad);
    velocity += std::clamp(wanted - velocity, -120 * rad * dt, 120 * rad * dt);
    const double step = velocity * dt;
    if ((target - angle) * step >= 0 && std::abs(step) >= std::abs(target - angle)) {
        angle = target; velocity = 0;
    } else angle += step;
}
double outside_neutral(double angle, double neutral) {
    return std::copysign(std::max(0., std::abs(angle) - neutral), angle);
}
}
SpectatorSettings sanitize_spectator_settings(SpectatorSettings s) {
    s.gaze_strength = safe(s.gaze_strength, 25, 0, 100);
    s.gaze_dwell_ms = safe(s.gaze_dwell_ms, 200, 0, 2000);
    s.gaze_neutral_degrees = safe(s.gaze_neutral_degrees, 3, 0, 20);
    s.gaze_horizontal_degrees = safe(s.gaze_horizontal_degrees, 8, 0, 30);
    s.gaze_vertical_degrees = safe(s.gaze_vertical_degrees, 3, 0, 20);
    s.gaze_response_ms = safe(s.gaze_response_ms, 250, 30, 2000);
    s.chat_width_degrees = safe(s.chat_width_degrees, 16, 1, 120);
    s.chat_height_degrees = safe(s.chat_height_degrees, 12, 1, 90);
    s.natural_suppression = safe(s.natural_suppression, 65, 0, 100);
    s.natural_follow = safe(s.natural_follow, 65, 0, 100);
    s.natural_settling_ms = safe(s.natural_settling_ms, 180, 30, 1000);
    s.natural_max_correction_degrees = safe(s.natural_max_correction_degrees, 15, 0, 45);
    s.natural_yaw = safe(s.natural_yaw, 100, 0, 100);
    s.natural_pitch = safe(s.natural_pitch, 100, 0, 100);
    s.natural_roll = safe(s.natural_roll, 100, 0, 100);
    return s;
}
void SpectatorController::begin_chat_calibration(std::uint64_t) {
    calibration_armed_ = true;
    calibration_start_ = 0;
    calibration_sums_ = {};
    calibration_counts_ = {};
    calibration_total_ = 0;
    status_.calibration_phase = ChatCalibrationPhase::countdown;
    status_.calibration_failure = ChatCalibrationFailure::none;
    status_.countdown_seconds = 3;
    status_.calibration_progress = 0;
    status_.calibration_samples = 0;
}
void SpectatorController::cancel_chat_calibration() {
    calibration_armed_ = false;
    status_.calibration_phase = ChatCalibrationPhase::idle;
    status_.calibration_failure = ChatCalibrationFailure::none;
    status_.calibration_progress = status_.countdown_seconds = 0;
    status_.calibration_samples = 0;
}
void SpectatorController::clear_chat_zone() {
    status_.zone_valid = status_.in_chat_zone = false;
    if (status_.gaze_state == GazeState::excluded) status_.gaze_state = GazeState::fixating;
    fixation_ = false;
}
void SpectatorController::reset() { *this = {}; }
Quat SpectatorController::update(const SpectatorInput &i, const SpectatorSettings &raw) {
    const auto s = sanitize_spectator_settings(raw);
    const Quat source = normalized(i.source_orientation), base = normalized(i.base_target_orientation);
    if (!valid(i.source_orientation) || !valid(i.base_target_orientation) || !i.time_ns || !i.session) {
        fixation_ = false;
        status_.gaze_state = s.gaze_enabled ? GazeState::untracked : GazeState::disabled;
        return s.bypass || (!s.gaze_enabled && !s.natural_enabled) ? i.base_target_orientation : base;
    }
    const bool reference_changed = initialized_ && (i.session != session_ || i.reference_epoch != epoch_);
    if (reference_changed) {
        clear_chat_zone();
        if (calibration_active()) {
            status_.calibration_phase = ChatCalibrationPhase::failed;
            status_.calibration_failure = ChatCalibrationFailure::reference_changed;
            status_.countdown_seconds = 0;
            calibration_armed_ = false;
        }
    }
    if (initialized_ && !reference_changed && (i.time_ns <= time_ || i.sequence <= sequence_))
        return s.bypass || (!s.gaze_enabled && !s.natural_enabled) ? i.base_target_orientation : output_;
    const bool discontinuity = !initialized_ || reference_changed || i.time_ns - time_ > 250000000ULL ||
                               angle_between(previous_source_, source) > 45 * rad;
    const double dt = discontinuity ? .01 : double(i.time_ns - time_) * 1e-9;
    if (discontinuity) {
        natural_target_ = source;
        velocity_ = speed_ = natural_step_velocity_ = {};
        yaw_ = pitch_ = yaw_velocity_ = pitch_velocity_ = 0;
        fixation_ = have_sample_ = false;
        status_.in_chat_zone = false;
    }
    if (s.natural_enabled) {
        if (!natural_active_ || discontinuity || s.natural_suppression <= 0) {
            natural_target_ = source;
            velocity_ = speed_ = natural_step_velocity_ = {};
        } else {
            const auto change = rotation_vector(multiply(conjugate(previous_source_), source));
            auto error = rotation_vector(multiply(conjugate(source), natural_target_));
            error.x = natural_axis(change.x, error.x, velocity_.x, speed_.x, dt, s.natural_pitch, s);
            error.y = natural_axis(change.y, error.y, velocity_.y, speed_.y, dt, s.natural_yaw, s);
            error.z = natural_axis(change.z, error.z, velocity_.z, speed_.z, dt, s.natural_roll, s);
            const double n = length(error), limit = s.natural_max_correction_degrees * rad;
            const auto candidate = normalized(multiply(source, axis_angle(error, std::min(n, limit))));
            const auto delta = rotation_vector(multiply(conjugate(natural_target_), candidate));
            auto wanted = limited(Vec3{delta.x / dt, delta.y / dt, delta.z / dt}, 360 * rad);
            const auto acceleration = limited(Vec3{wanted.x - natural_step_velocity_.x,
                wanted.y - natural_step_velocity_.y, wanted.z - natural_step_velocity_.z}, 1440 * rad * dt);
            natural_step_velocity_.x += acceleration.x;
            natural_step_velocity_.y += acceleration.y;
            natural_step_velocity_.z += acceleration.z;
            auto step = Vec3{natural_step_velocity_.x * dt, natural_step_velocity_.y * dt, natural_step_velocity_.z * dt};
            const double distance = length(delta);
            if (step.x * delta.x + step.y * delta.y + step.z * delta.z <= 0) {
                step = {}; natural_step_velocity_ = {};
            } else if (length(step) >= distance) {
                step = delta;
                natural_step_velocity_ = {step.x / dt, step.y / dt, step.z / dt};
            }
            const auto rotation = axis_angle(step, length(step));
            natural_target_ = normalized(multiply(natural_target_, rotation));
            natural_step_velocity_ = rotate(conjugate(rotation), natural_step_velocity_);
            // Image-margin/correction safety takes precedence over the motion
            // envelope when the source moves farther than the allowed lag.
            const double correction = angle_between(source, natural_target_);
            if (correction > limit) {
                natural_target_ = slerp(source, natural_target_, limit / correction);
                natural_step_velocity_ = {};
            }
            // Disabling an axis removes its correction, including any motion
            // retained by the shared speed/acceleration envelope.
            auto residual = rotation_vector(multiply(conjugate(source), natural_target_));
            if (s.natural_pitch <= 0) { residual.x = 0; natural_step_velocity_.x = 0; }
            if (s.natural_yaw <= 0) { residual.y = 0; natural_step_velocity_.y = 0; }
            if (s.natural_roll <= 0) { residual.z = 0; natural_step_velocity_.z = 0; }
            natural_target_ = normalized(multiply(source, axis_angle(residual, length(residual))));
        }
    }
    natural_active_ = s.natural_enabled;
    const Quat head = s.natural_enabled ? natural_target_ : base;

    const auto &g = i.gaze;
    const double ray_length = length(g.direction);
    const bool known = (g.flags & GazeTimeKnown) != 0;
    const bool tracked = (g.flags & (GazeValid | GazeTracked)) == (GazeValid | GazeTracked) &&
                         valid(g.reference_from_eye) && std::isfinite(ray_length) &&
                         std::abs(ray_length - 1) < .05 && g.reference_epoch == i.reference_epoch;
    const bool stale = known && (g.sample_time_ns <= 0 ||
                       static_cast<std::uint64_t>(g.sample_time_ns) > i.time_ns ||
                       i.time_ns - static_cast<std::uint64_t>(g.sample_time_ns) > 150000000ULL);
    const bool usable = tracked && !stale;
    const bool fresh = usable && (!known || !have_sample_ || g.sample_time_ns > sample_time_);
    const auto direction = usable ? unit(g.direction) : Vec3{0, 0, -1};
    status_.gaze_time_known = known;

    // Classify before transforming the ray into the submitted projection frame.
    if (usable && status_.zone_valid) {
        const auto center = zone_direction_;
        auto right = unit(Vec3{-center.z, 0, center.x});
        if (length(right) < .5) right = {1, 0, 0};
        const Vec3 up{right.y * center.z - right.z * center.y,
                      right.z * center.x - right.x * center.z,
                      right.x * center.y - right.y * center.x};
        const double forward = center.x * direction.x + center.y * direction.y + center.z * direction.z;
        const double x = right.x * direction.x + right.y * direction.y + right.z * direction.z;
        const double y = up.x * direction.x + up.y * direction.y + up.z * direction.z;
        const double hysteresis = status_.in_chat_zone ? 1.5 : 0;
        status_.in_chat_zone = forward > 0 &&
            std::abs(std::atan2(x, forward)) < (s.chat_width_degrees * .5 + hysteresis) * rad &&
            std::abs(std::atan2(y, forward)) < (s.chat_height_degrees * .5 + hysteresis) * rad;
    } else status_.in_chat_zone = false;

    if (calibration_active()) {
        if (calibration_armed_) {
            calibration_start_ = i.time_ns;
            calibration_armed_ = false;
        }
        const auto elapsed = i.time_ns - calibration_start_;
        status_.countdown_seconds = std::max(0., 3 - double(elapsed) * 1e-9);
        if (elapsed >= 3000000000ULL) {
            status_.calibration_phase = ChatCalibrationPhase::acquiring;
            status_.calibration_progress = std::clamp(double(elapsed - 3000000000ULL) / 1e9, 0., 1.);
            // Known, unique acquisition times inside the acquisition window.
            if (elapsed < 4000000000ULL && fresh && known &&
                static_cast<std::uint64_t>(g.sample_time_ns) >= calibration_start_ + 3000000000ULL) {
                // Three bounded clusters make the center insensitive to an
                // early outlier without keeping an unbounded sample history.
                ++calibration_total_;
                for (std::size_t k = 0; k < calibration_counts_.size(); ++k) {
                    if (!calibration_counts_[k] || separation(direction, unit(calibration_sums_[k])) <= 3 * rad) {
                        calibration_sums_[k].x += direction.x;
                        calibration_sums_[k].y += direction.y;
                        calibration_sums_[k].z += direction.z;
                        ++calibration_counts_[k];
                        break;
                    }
                }
                status_.calibration_samples = *std::max_element(calibration_counts_.begin(), calibration_counts_.end());
            }
            if (elapsed >= 4000000000ULL) {
                const bool stable = calibration_total_ - status_.calibration_samples <= status_.calibration_samples / 4;
                const bool enough = status_.calibration_samples >= 20;
                if (stable && enough) {
                    const auto best = std::max_element(calibration_counts_.begin(), calibration_counts_.end());
                    zone_direction_ = unit(calibration_sums_[static_cast<std::size_t>(best - calibration_counts_.begin())]);
                    status_.zone_valid = true;
                    status_.calibration_phase = ChatCalibrationPhase::succeeded;
                } else {
                    status_.calibration_phase = ChatCalibrationPhase::failed;
                    status_.calibration_failure = enough ? ChatCalibrationFailure::unstable :
                                                          ChatCalibrationFailure::insufficient_samples;
                }
            }
        }
    }

    double desired_yaw = 0, desired_pitch = 0;
    const bool excluded = s.chat_enabled && status_.zone_valid && status_.in_chat_zone;
    if (!s.gaze_enabled) {
        yaw_ = pitch_ = yaw_velocity_ = pitch_velocity_ = 0;
        fixation_ = false;
        status_.gaze_state = GazeState::disabled;
    } else if (!usable || excluded) {
        fixation_ = false;
        status_.gaze_state = excluded ? GazeState::excluded : stale ? GazeState::stale : GazeState::untracked;
    } else {
        if (fresh && (!fixation_ || separation(direction, fixation_direction_) > 2 * rad)) {
            fixation_direction_ = direction;
            fixation_start_ = known ? static_cast<std::uint64_t>(g.sample_time_ns) : i.time_ns;
            fixation_ = true;
        }
        const auto at = known ? static_cast<std::uint64_t>(g.sample_time_ns) : i.time_ns;
        const bool settled = fixation_ && at >= fixation_start_ &&
                             double(at - fixation_start_) * 1e-6 >= s.gaze_dwell_ms;
        status_.gaze_state = settled ? GazeState::valid : GazeState::fixating;
        if (settled) {
            const auto eye_ray = rotate(conjugate(g.reference_from_eye), direction);
            const auto projection_ray = rotate(source, eye_ray);
            const auto local = rotate(conjugate(head), projection_ray);
            if (local.z < -.01) {
                desired_yaw = std::clamp(outside_neutral(std::atan2(-local.x, -local.z),
                    s.gaze_neutral_degrees * rad) * s.gaze_strength / 100,
                    -s.gaze_horizontal_degrees * rad, s.gaze_horizontal_degrees * rad);
                desired_pitch = std::clamp(outside_neutral(std::atan2(local.y, std::hypot(local.x, local.z)),
                    s.gaze_neutral_degrees * rad) * s.gaze_strength / 100,
                    -s.gaze_vertical_degrees * rad, s.gaze_vertical_degrees * rad);
            }
        }
    }
    if (fresh && known) { have_sample_ = true; sample_time_ = g.sample_time_ns; }
    if (s.gaze_enabled) {
        // Substeps keep acceleration bounds stable at lower capture cadences.
        const int steps = std::max(1, static_cast<int>(std::ceil(dt / .01)));
        const double step_dt = dt / steps;
        for (int k = 0; k < steps; ++k) {
            ease(yaw_, yaw_velocity_, desired_yaw, step_dt, s.gaze_response_ms);
            ease(pitch_, pitch_velocity_, desired_pitch, step_dt, s.gaze_response_ms);
        }
        yaw_ = std::clamp(yaw_, -s.gaze_horizontal_degrees * rad, s.gaze_horizontal_degrees * rad);
        pitch_ = std::clamp(pitch_, -s.gaze_vertical_degrees * rad, s.gaze_vertical_degrees * rad);
    }
    output_ = normalized(multiply(head, multiply(axis_angle({0, 1, 0}, yaw_), axis_angle({1, 0, 0}, pitch_))));
    initialized_ = true;
    session_ = i.session; epoch_ = i.reference_epoch; time_ = i.time_ns; sequence_ = i.sequence;
    previous_source_ = source;
    return s.bypass || (!s.gaze_enabled && !s.natural_enabled) ? i.base_target_orientation : output_;
}
} // namespace vrst
