#pragma once
#include "vrst/image.hpp"

namespace vrst {
struct CropSettings {
    bool enabled{};
    double max_extra{10}, speed{3}; // Percentage points, percentage points/second.
};
class CropController {
  public:
    void reset();
    double update(double required, double base, const CropSettings &, std::uint64_t time_ns);

  private:
    double crop_{}, base_{};
    std::uint64_t time_ns_{};
};
// Return total stabilization crop, bounded by base + max_extra and the 40% ceiling.
double required_pose_crop(unsigned width, unsigned height, const Settings &, Quat,
                          std::optional<Projection>, double max_extra);
double required_image_crop(unsigned width, unsigned height, const Settings &, const ImageResult &,
                           double max_extra);
} // namespace vrst
