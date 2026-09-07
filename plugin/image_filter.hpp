#pragma once

#include "vrst/advanced.hpp"
#include "vrst/image.hpp"
#include <obs-module.h>

struct ImageRenderOptions {
    bool hybrid{}, comparison{};
    vrst::PoseResult pose;
    std::optional<vrst::Projection> automatic;
    vrst::CropSettings crop;
};

// Graphics state is used only from OBS's graphics thread/context. UI callbacks
// communicate configuration revisions through the enclosing filter.
class ImageFilter {
  public:
    bool load();
    void destroy();
    void clear();
    void tick();
    bool render(obs_source_t *source, unsigned width, unsigned height, const vrst::Settings &,
                const vrst::ImageSettings &, std::uint64_t revision, const ImageRenderOptions &options = {});
    bool fallback(obs_source_t *source, unsigned width, unsigned height, const vrst::Settings &);
    const vrst::ImageResult &result() const {
        return result_;
    }
    const vrst::ImageWarp &warp() const {
        return warp_;
    }
    std::uint64_t capture_time_ns() const {
        // Calibration needs the source capture time, never analysis completion.
        return have_display_ ? slots_[display_].local_time : 0;
    }
    vrst::PoseResult displayed_pose() const {
        return have_display_ && slots_[display_].hybrid ? slots_[display_].pose : vrst::PoseResult{};
    }
    double effective_crop() const {
        return effective_crop_;
    }

  private:
    struct Slot {
        gs_texrender_t *full{};
        gs_texrender_t *small{};
        gs_stagesurf_t *stage{};
        std::uint64_t time{}, local_time{};
        vrst::Warp pose_warp;
        vrst::PoseResult pose;
        vrst::Settings settings;
        bool captured{}, staged{}, pose_valid{}, hybrid{};
    };
    bool allocate(unsigned width, unsigned height, const vrst::Settings &);
    bool capture(Slot &, obs_source_t *, const vrst::Settings &, const ImageRenderOptions &);
    bool read(Slot &);
    bool draw(bool comparison);
    void set_pose(const vrst::Warp &, bool comparison = false);
    gs_effect_t *effect_{};
    gs_eparam_t *image_{}, *row_x_{}, *row_y_{}, *trim_{}, *footprint_{};
    gs_eparam_t *pose_input_{}, *pose_output_{}, *pose_correction_{}, *pose_uv_{};
    gs_eparam_t *mirror_edges_{}, *source_texel_{};
    Slot slots_[2];
    vrst::ImageStabilizer stabilizer_;
    vrst::GrayImage gray_;
    vrst::ImageResult result_;
    vrst::ImageWarp warp_;
    vrst::ImageWarp comparison_warp_;
    vrst::CropController crop_controller_;
    double effective_crop_{};
    unsigned width_{}, height_{}, small_width_{}, small_height_{};
    unsigned next_{}, display_{};
    std::uint64_t revision_{};
    bool processed_{}, have_display_{}, hybrid_{}, have_pose_state_{}, pose_valid_{};
};
