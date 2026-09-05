#pragma once

#include "vrst/image.hpp"
#include <obs-module.h>

// Graphics state is used only from OBS's graphics thread/context. UI callbacks
// communicate configuration revisions through the enclosing filter.
class ImageFilter {
  public:
    bool load();
    void destroy();
    void clear();
    void tick();
    bool render(obs_source_t *source, unsigned width, unsigned height, const vrst::Settings &,
                const vrst::ImageSettings &, std::uint64_t revision);
    bool fallback(obs_source_t *source, unsigned width, unsigned height, const vrst::Settings &);
    const vrst::ImageResult &result() const {
        return result_;
    }
    const vrst::ImageWarp &warp() const {
        return warp_;
    }

  private:
    struct Slot {
        gs_texrender_t *full{};
        gs_texrender_t *small{};
        gs_stagesurf_t *stage{};
        std::uint64_t time{};
        bool captured{}, staged{};
    };
    bool allocate(unsigned width, unsigned height, const vrst::Settings &);
    bool capture(Slot &, obs_source_t *, const vrst::Settings &);
    bool read(Slot &);
    bool draw();
    gs_effect_t *effect_{};
    gs_eparam_t *image_{}, *row_x_{}, *row_y_{}, *trim_{}, *footprint_{};
    Slot slots_[2];
    vrst::ImageStabilizer stabilizer_;
    vrst::GrayImage gray_;
    vrst::ImageResult result_;
    vrst::ImageWarp warp_;
    unsigned width_{}, height_{}, small_width_{}, small_height_{};
    unsigned next_{}, display_{};
    std::uint64_t revision_{};
    bool processed_{}, have_display_{};
};
