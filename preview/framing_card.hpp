#pragma once
#include "renderer.hpp"

namespace vrst::preview {
RECT framing_image_rect(RECT available, double source_aspect);
RECT framing_canvas_image(RECT bounds, double aspect, bool dashboard);
RECT framing_crop_rect(RECT image, double source_aspect, const Options &, unsigned eye);

class FramingCard {
  public:
    FramingCard();
    ~FramingCard();
    void paint(HDC, RECT bounds, const FramingStill &, std::uint64_t revision, const Options &,
               double source_aspect, unsigned eye, bool dashboard = true);

  private:
    struct Cache;
    std::unique_ptr<Cache> cache_;
};
} // namespace vrst::preview
