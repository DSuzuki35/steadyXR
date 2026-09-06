#include "calibration_session.hpp"
namespace vrst_plugin {
void CalibrationSession::discard_locked(const std::string &reason) {
    ++revision_;
    const bool had_data = state_.active || state_.result.valid || !poses_.empty() || !motions_.empty();
    state_.active = false;
    state_.result = {};
    state_.poses = state_.motions = 0;
    state_.seconds = 0;
    if (had_data)
        state_.message = reason;
    poses_.clear();
    motions_.clear();
    source_ = 0;
    width_ = height_ = 0;
    started_ = image_time_ = capture_time_ = 0;
}
void CalibrationSession::source(std::uintptr_t identity, unsigned width, unsigned height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active && poses_.empty() && motions_.empty() && !state_.result.valid)
        return;
    if (!identity || !width || !height ||
        (source_ && (source_ != identity || width_ != width || height_ != height))) {
        discard_locked("Capture source or size changed. Start a new calibration.");
        return;
    }
    if (state_.active) {
        source_ = identity;
        width_ = width;
        height_ = height;
    }
}
void CalibrationSession::start(std::uint64_t time) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++revision_;
    started_ = time;
    image_time_ = capture_time_ = 0;
    source_ = 0;
    width_ = height_ = 0;
    poses_.clear();
    motions_.clear();
    state_ = {};
    state_.active = true;
    state_.message = "Parked car: make varied small left/right movements, then up/down. Continue 15–25 "
                     "seconds; click Finish and analyze.";
}
bool CalibrationSession::invalidate(const std::string &reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool was_active = state_.active;
    discard_locked(reason);
    return was_active;
}
bool CalibrationSession::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.active;
}
void CalibrationSession::pose(const vrst::TrackingSnapshot &p, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.active)
        return;
    state_.seconds = now >= started_ ? (now - started_) * 1e-9 : 0;
    if (state_.seconds >= 30) {
        state_.active = false;
        state_.message = "30 seconds collected. Click Finish and analyze.";
        return;
    }
    if (p.pose.valid && !(p.pose.flags & vrst::DemoPose) && p.local_pose_time_ns &&
        (poses_.empty() || p.local_pose_time_ns > poses_.back().time_ns) && poses_.size() < 10000 &&
        p.local_pose_time_ns >= started_ && p.local_pose_time_ns <= now)
        poses_.push_back({p.local_pose_time_ns, p.pose.source_orientation});
    state_.poses = unsigned(poses_.size());
}
void CalibrationSession::image(const vrst::ImageResult &r, std::uint64_t captured) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (r.status == vrst::ImageStatus::scene_change && captured >= started_ && captured > capture_time_ &&
        (state_.active || state_.result.valid || !motions_.empty())) {
        discard_locked("Scene change detected. Keep the car parked and start a new calibration.");
        return;
    }
    if (!state_.active || !captured || captured < started_ || captured - started_ > 30000000000ULL ||
        !r.time_ns || captured <= capture_time_)
        return;
    if (r.status == vrst::ImageStatus::tracking && r.previous_time_ns == image_time_ && capture_time_ &&
        captured - capture_time_ <= 200000000ULL && r.width && r.height && motions_.size() < 4000)
        motions_.push_back(
            {capture_time_, captured, r.motion.x / r.width, r.motion.y / r.height, r.confidence});
    image_time_ = r.time_ns;
    capture_time_ = captured;
    state_.motions = unsigned(motions_.size());
}
void CalibrationSession::finish() {
    std::vector<vrst::CalibrationPose> poses;
    std::vector<vrst::CalibrationMotion> motions;
    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.active = false;
        revision = ++revision_;
        poses = poses_;
        motions = motions_;
        state_.message = "Analyzing collected motion...";
    }
    const auto result = vrst::calibrate(poses, motions);
    std::lock_guard<std::mutex> lock(mutex_);
    if (revision != revision_)
        return;
    state_.result = result;
    state_.message = result.valid ? "Suggestion ready. Review it, then Apply or recalibrate." : result.reason;
}
CalibrationState CalibrationSession::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
} // namespace vrst_plugin
