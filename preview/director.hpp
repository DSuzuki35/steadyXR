#pragma once
#include <cstddef>
#include <cstdint>
#include <vrst/core.hpp>
#include <vrst/image.hpp>

namespace vrst::preview {
struct Frame;
struct DirectorSettings {
    bool lookahead_enabled{}, auto_crop_enabled{}, speed_fov_enabled{};
    double lookahead_frames{3}, auto_crop_max_percent{15}, auto_crop_release_ms{1200};
    double speed_fov_percent{6}, speed_fov_full_kph{220}, speed_fov_response_ms{1000};
    unsigned telemetry_game{};
};
DirectorSettings sanitize_director_settings(DirectorSettings);
struct DirectorStatus {
    unsigned buffered_frames{};
    std::uint64_t buffered_bytes{};
    double delay_ms{}, auto_crop_percent{}, speed_crop_percent{}, speed_kph{};
    bool speed_available{}, buffer_limited{};
    unsigned telemetry_game{};
};
// The caller selects a centered chronological window around the displayed
// image. At most nine captured poses are averaged; antipodes represent one pose.
Quat centered_orientation(const Frame *frames, std::size_t count);

class DirectorController {
  public:
    // Changes final output rays, after aspect fit/trims/offset. Call before the
    // normal finite-correction fallback. Time advances only for a new frame.
    void update(Warp &, const DirectorSettings &, std::uint64_t time_ns, std::uint64_t sequence,
                double speed_kph = -1, bool bypass = false);
    // Supply the same framing without motion so an exhausted crop budget can
    // reduce correction instead of requesting pixels outside the captured image.
    void update_image(ImageWarp &, const DirectorSettings &, std::uint64_t time_ns,
                      std::uint64_t sequence, double speed_kph = -1, bool bypass = false,
                      const ImageWarp *uncorrected = nullptr);
    DirectorStatus status() const {
        return status_;
    }
    void reset();

  private:
    DirectorStatus status_{};
    std::uint64_t previous_time_{}, previous_sequence_{};
    bool initialized_{};
};
} // namespace vrst::preview
