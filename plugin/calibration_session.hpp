#pragma once
#include "vrst/calibration.hpp"
#include "vrst/image.hpp"
#include "vrst/receiver.hpp"
#include <mutex>
namespace vrst_plugin {
struct CalibrationState {
    bool active{};
    double seconds{};
    unsigned poses{}, motions{};
    vrst::CalibrationResult result;
    std::string message{"Park the car, then start calibration."};
};
class CalibrationSession {
  public:
    void start(std::uint64_t time);
    bool invalidate(const std::string &reason);
    bool active() const;
    void pose(const vrst::TrackingSnapshot &, std::uint64_t now);
    void image(const vrst::ImageResult &, std::uint64_t capture_time);
    void source(std::uintptr_t identity, unsigned width, unsigned height);
    void finish();
    CalibrationState state() const;

  private:
    mutable std::mutex mutex_;
    CalibrationState state_;
    std::uint64_t started_{}, revision_{}, image_time_{}, capture_time_{};
    std::uintptr_t source_{};
    unsigned width_{}, height_{};
    std::vector<vrst::CalibrationPose> poses_;
    std::vector<vrst::CalibrationMotion> motions_;
    void discard_locked(const std::string &reason);
};
} // namespace vrst_plugin
