#pragma once
#include "vrst/core.hpp"
#include <array>
#include <memory>
#include <vector>

namespace vrst {
struct GrayImage {
    unsigned width{}, height{};
    std::vector<float> pixels; // row-major linear luminance in [0, 1]
};
struct ImageSettings {
    double smoothing{40};
    MotionSettings motion{}; // Common amount is taken from smoothing above.
    double max_shift_percent{1.0}, max_roll_degrees{0.5};
    bool track_roll{true};
    // Margins as percentages of the trimmed analysis image. Together < 100 per axis.
    double region_left{10}, region_right{10}, region_top{50}, region_bottom{10};
};
struct ImageTransform {
    // Inverse sampling transform centered on the analysis image: input = R * output + t.
    // x right, y down, translations in analysis pixels, positive angle clockwise.
    double x{}, y{}, angle{};
};
enum class ImageStatus { warming, tracking, weak, scene_change, invalid };
struct ImageResult {
    ImageStatus status{ImageStatus::warming};
    // Weak tracking may carry a bounded fade-out; it is not a motion measurement.
    ImageTransform correction{};
    ImageTransform motion{}; // Measured previous-to-current transform, only when tracking.
    std::uint64_t time_ns{}, previous_time_ns{};
    double confidence{}, processing_ms{};
    unsigned tracked{}, inliers{}, width{}, height{};
};
class ImageStabilizer {
  public:
    ImageStabilizer();
    ~ImageStabilizer();
    ImageStabilizer(const ImageStabilizer &) = delete;
    ImageStabilizer &operator=(const ImageStabilizer &) = delete;
    ImageResult process(const GrayImage &, std::uint64_t time_ns, const ImageSettings &);
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct ImageWarp {
    // Original, untrimmed texture UV = row * (output_u, output_v, 1).
    std::array<double, 3> row_x{1, 0, 0}, row_y{0, 1, 0};
    std::uint32_t width{}, height{};
    double correction_fraction{1};
};
// Cropping/framing uses Settings; FOV, automatic_fov and tracking delay are ignored.
ImageWarp make_image_warp(std::uint32_t source_width, std::uint32_t source_height, const Settings &,
                          const ImageResult &);
} // namespace vrst
