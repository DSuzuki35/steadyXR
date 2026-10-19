#pragma once
#include "vrst/core.hpp"
#include <cstdint>

namespace vrst {
enum GazeFlags : std::uint32_t { GazeValid = 1, GazeTracked = 2, GazeTimeKnown = 4 };
struct GazeSample {
    Quat reference_from_eye{};
    Vec3 direction{0, 0, -1};      // Unit ray in the runtime's stable reference frame.
    std::int64_t sample_time_ns{}; // Same clock as SpectatorInput::time_ns when known.
    std::uint64_t reference_epoch{};
    std::uint32_t flags{};
};
struct SpectatorSettings {
    bool bypass{}, gaze_enabled{}, chat_enabled{}, natural_enabled{};
    double gaze_strength{25}, gaze_dwell_ms{200}, gaze_neutral_degrees{3};
    double gaze_horizontal_degrees{8}, gaze_vertical_degrees{3}, gaze_response_ms{250};
    double chat_width_degrees{16}, chat_height_degrees{12};
    double natural_suppression{65}, natural_follow{65}, natural_settling_ms{180};
    double natural_max_correction_degrees{15};
    double natural_yaw{100}, natural_pitch{100}, natural_roll{100};
};
SpectatorSettings sanitize_spectator_settings(SpectatorSettings);
struct SpectatorInput {
    // Source and base target are in the submitted projection's frame, which need
    // not be the gaze reference frame. The host applies its final image-margin
    // limiter and horizon correction after this controller.
    Quat source_orientation{}, base_target_orientation{};
    GazeSample gaze{};
    std::uint64_t time_ns{}, session{}, sequence{}, reference_epoch{};
};
enum class ChatCalibrationPhase { idle, countdown, acquiring, succeeded, failed };
enum class ChatCalibrationFailure { none, insufficient_samples, unstable, reference_changed };
enum class GazeState { disabled, untracked, stale, fixating, valid, excluded };
struct SpectatorStatus {
    ChatCalibrationPhase calibration_phase{ChatCalibrationPhase::idle};
    ChatCalibrationFailure calibration_failure{ChatCalibrationFailure::none};
    double countdown_seconds{}, calibration_progress{};
    std::uint32_t calibration_samples{};
    bool zone_valid{}, in_chat_zone{}, gaze_time_known{};
    GazeState gaze_state{GazeState::disabled};
};
class SpectatorController {
  public:
    Quat update(const SpectatorInput &, const SpectatorSettings &);
    Quat head_target() const {
        return head_target_;
    }
    void suspend(); // Stop transient framing/diagnostics, retain learned reference anchor.
    // Arms against the next acquired frame's clock, independent of UI clock.
    // now_ns is the caller's request timestamp; it is never compared with XR time.
    void begin_chat_calibration(std::uint64_t now_ns);
    void cancel_chat_calibration();
    void clear_chat_zone();
    void reset();
    SpectatorStatus status() const {
        return status_;
    }
    bool calibration_active() const {
        return status_.calibration_phase == ChatCalibrationPhase::countdown ||
               status_.calibration_phase == ChatCalibrationPhase::acquiring;
    }

  private:
    SpectatorStatus status_{};
    bool initialized_{}, calibration_armed_{}, fixation_{}, have_sample_{}, natural_active_{};
    std::uint64_t session_{}, epoch_{}, time_{}, sequence_{}, calibration_start_{}, fixation_start_{};
    std::int64_t sample_time_{};
    Quat output_{}, previous_source_{}, natural_target_{}, head_target_{};
    Vec3 velocity_{}, speed_{}, natural_step_velocity_{}, fixation_direction_{}, zone_direction_{};
    std::array<Vec3, 3> calibration_sums_{};
    std::array<std::uint32_t, 3> calibration_counts_{};
    std::uint32_t calibration_total_{};
    double yaw_{}, pitch_{}, yaw_velocity_{}, pitch_velocity_{};
};
} // namespace vrst
