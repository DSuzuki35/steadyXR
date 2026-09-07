#pragma once

#include "vrst/timing_overlay.hpp"
#include <obs-module.h>

namespace vrst_plugin {
// Graphics-thread only. Drawn after the final video warp, outside all capture
// and analysis textures. Allocations are created only while the display is on.
class TimingOverlay {
  public:
    void draw(const vrst::TimingOverlayData &, unsigned output_width, unsigned output_height);
    void destroy();

  private:
    bool load();
    gs_effect_t *effect_{};
    gs_eparam_t *image_{};
    gs_texture_t *texture_{};
    unsigned width_{}, height_{};
};
} // namespace vrst_plugin
