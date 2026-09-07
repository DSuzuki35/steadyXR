#include "image_filter.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

namespace {
void vector(gs_eparam_t *parameter, double x, double y, double z, double w) {
    const float value[] = {float(x), float(y), float(z), float(w)};
    gs_effect_set_val(parameter, value, sizeof(value));
}
struct BlendScope {
    BlendScope() {
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    }
    ~BlendScope() {
        gs_blend_state_pop();
    }
};
struct SrgbScope {
    bool previous{gs_framebuffer_srgb_enabled()};
    explicit SrgbScope(bool enabled) {
        gs_enable_framebuffer_srgb(enabled);
    }
    ~SrgbScope() {
        gs_enable_framebuffer_srgb(previous);
    }
};
double crop_value(double value, double fallback = 10) {
    return std::isfinite(value) ? std::clamp(value, 0., 40.) : fallback;
}
double stage_crop(double total) {
    return 100 * (1 - std::sqrt(1 - crop_value(total) / 100));
}
vrst::Settings pose_settings(vrst::Settings settings) {
    settings.stabilization_crop = stage_crop(settings.stabilization_crop);
    settings.output_16_9 = false;
    settings.framing_zoom = settings.framing_x = settings.framing_y = 0;
    return settings;
}
vrst::Settings residual_settings(vrst::Settings settings) {
    settings.stabilization_crop = stage_crop(settings.stabilization_crop);
    settings.input_left = settings.input_right = settings.input_top = settings.input_bottom = 0;
    return settings;
}
vrst::Quat pose_correction(const vrst::Settings &settings, const ImageRenderOptions &options) {
    if (!options.pose.valid || !vrst::valid(options.pose.correction))
        return {};
    return settings.horizon_level > 0
               ? vrst::horizon_correction(options.pose.source_orientation, options.pose.target_orientation,
                                          settings.horizon_level, settings.horizon_max_degrees)
               : options.pose.correction;
}
} // namespace

bool ImageFilter::load() {
    char *path = obs_module_file("image-stabilize.effect");
    char *errors = nullptr;
    if (path)
        effect_ = gs_effect_create_from_file(path, &errors);
    bfree(path);
    if (!effect_)
        blog(LOG_ERROR, "[vr-stabilizer] Unable to load image-stabilize.effect: %s",
             errors ? errors : "missing shader file");
    bfree(errors);
    if (!effect_)
        return false;
    image_ = gs_effect_get_param_by_name(effect_, "image");
    row_x_ = gs_effect_get_param_by_name(effect_, "row_x");
    row_y_ = gs_effect_get_param_by_name(effect_, "row_y");
    trim_ = gs_effect_get_param_by_name(effect_, "trim_uv");
    footprint_ = gs_effect_get_param_by_name(effect_, "footprint");
    pose_input_ = gs_effect_get_param_by_name(effect_, "pose_input");
    pose_output_ = gs_effect_get_param_by_name(effect_, "pose_output");
    pose_correction_ = gs_effect_get_param_by_name(effect_, "pose_correction");
    pose_uv_ = gs_effect_get_param_by_name(effect_, "pose_uv");
    if (!image_ || !row_x_ || !row_y_ || !trim_ || !footprint_ || !pose_input_ || !pose_output_ ||
        !pose_correction_ || !pose_uv_)
        return false;
    // OBS's effect translator may retain pose uniforms in the legacy analysis
    // technique too. Initialize them even before the first hybrid frame.
    set_pose(vrst::Warp{});
    return true;
}

void ImageFilter::set_pose(const vrst::Warp &warp, bool comparison) {
    vector(pose_input_, warp.input.left, warp.input.right, warp.input.up, warp.input.down);
    vector(pose_output_, warp.output.left, warp.output.right, warp.output.up, warp.output.down);
    const auto q = comparison ? vrst::Quat{} : warp.correction;
    vector(pose_correction_, q.x, q.y, q.z, q.w);
    vector(pose_uv_, warp.uv_left, warp.uv_right, warp.uv_top, warp.uv_bottom);
}

void ImageFilter::tick() {
    // If OBS did not render this filter in the preceding tick (for example a
    // hidden preview), the buffered texture is older than one video frame.
    // Keep allocations, but restart with fresh video when it becomes visible.
    if (!processed_) {
        for (auto &slot : slots_)
            slot.captured = slot.staged = false;
        have_display_ = false;
        stabilizer_.reset();
        crop_controller_.reset();
        have_pose_state_ = false;
        result_ = {};
    }
    processed_ = false;
}

void ImageFilter::clear() {
    for (auto &slot : slots_) {
        gs_stagesurface_destroy(slot.stage);
        gs_texrender_destroy(slot.small);
        gs_texrender_destroy(slot.full);
        slot = {};
    }
    stabilizer_.reset();
    crop_controller_.reset();
    result_ = {};
    warp_ = comparison_warp_ = {};
    effective_crop_ = 0;
    width_ = height_ = small_width_ = small_height_ = 0;
    next_ = display_ = 0;
    processed_ = have_display_ = false;
    have_pose_state_ = pose_valid_ = false;
}

void ImageFilter::destroy() {
    clear();
    gs_effect_destroy(effect_);
    effect_ = nullptr;
}

bool ImageFilter::allocate(unsigned width, unsigned height, const vrst::Settings &settings) {
    width_ = width;
    height_ = height;
    const double trimmed_width = width * (1 - (settings.input_left + settings.input_right) / 100);
    const double trimmed_height = height * (1 - (settings.input_top + settings.input_bottom) / 100);
    const double scale = std::min({1.0, 480.0 / trimmed_width, 270.0 / trimmed_height});
    small_width_ = std::max(1u, unsigned(std::floor(trimmed_width * scale)));
    small_height_ = std::max(1u, unsigned(std::floor(trimmed_height * scale)));
    gray_.width = small_width_;
    gray_.height = small_height_;
    gray_.pixels.resize(std::size_t(small_width_) * small_height_);
    for (auto &slot : slots_) {
        slot.full = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        slot.small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
        slot.stage = gs_stagesurface_create(small_width_, small_height_, GS_RGBA);
        if (!slot.full || !slot.small || !slot.stage) {
            clear();
            return false;
        }
    }
    return true;
}

bool ImageFilter::capture(Slot &slot, obs_source_t *source, const vrst::Settings &settings,
                          const ImageRenderOptions &options) {
    auto *target = obs_filter_get_target(source);
    auto *parent = obs_filter_get_parent(source);
    slot.captured = slot.staged = false;
    if (!target || !parent)
        return false;
    slot.local_time = vrst::monotonic_ns();
    slot.settings = settings;
    slot.hybrid = options.hybrid;
    slot.pose = options.pose;
    slot.pose_valid = options.pose.valid && vrst::valid(options.pose.correction);
    if (slot.hybrid)
        slot.pose_warp = vrst::make_warp(width_, height_, pose_settings(settings),
                                         pose_correction(settings, options), options.automatic);
    BlendScope blend;
    gs_texrender_reset(slot.full);
    if (!gs_texrender_begin_with_color_space(slot.full, width_, height_, GS_CS_SRGB))
        return false;
    // Match obs_source_process_filter_begin: sources such as Color Source
    // inherit this state and emit straight alpha. Store premultiplied RGB for
    // the later ONE / INVSRCALPHA draw of the captured texture.
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0, 0);
    gs_ortho(0, float(width_), 0, float(height_), -100, 100);
    const auto flags = obs_source_get_output_flags(target);
    if (target == parent && !(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC)))
        obs_source_default_render(target);
    else
        obs_source_video_render(target);
    gs_texrender_end(slot.full);
    slot.captured = true;
    slot.time = obs_get_video_frame_time();

    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    gs_texrender_reset(slot.small);
    if (!gs_texrender_begin(slot.small, small_width_, small_height_))
        return true; // Full video remains usable when analysis allocation fails.
    {
        SrgbScope srgb(false); // Store linear luminance directly in the 8-bit stage.
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0, 0);
        gs_ortho(0, float(small_width_), 0, float(small_height_), -100, 100);
        gs_texture_t *texture = gs_texrender_get_texture(slot.full);
        gs_effect_set_texture_srgb(image_, texture);
        const double left = settings.input_left / 100, top = settings.input_top / 100;
        const double span_x = 1 - left - settings.input_right / 100;
        const double span_y = 1 - top - settings.input_bottom / 100;
        vector(trim_, left, top, span_x, span_y);
        vector(footprint_, span_x / small_width_, span_y / small_height_, 0, 0);
        set_pose(slot.hybrid ? slot.pose_warp : vrst::Warp{});
        if (slot.hybrid)
            vector(footprint_, 1. / small_width_, 1. / small_height_, 0, 0);
        while (gs_effect_loop(effect_, slot.hybrid ? "AnalyzeHybrid" : "Analyze"))
            gs_draw_sprite(texture, 0, small_width_, small_height_);
    }
    gs_texrender_end(slot.small);
    if (auto *texture = gs_texrender_get_texture(slot.small)) {
        gs_stage_texture(slot.stage, texture);
        slot.staged = true;
    }
    return true;
}

bool ImageFilter::read(Slot &slot) {
    std::uint8_t *pixels = nullptr;
    std::uint32_t stride = 0;
    if (!slot.staged || !gs_stagesurface_map(slot.stage, &pixels, &stride))
        return false;
    for (unsigned y = 0; y < small_height_; ++y)
        for (unsigned x = 0; x < small_width_; ++x)
            gray_.pixels[std::size_t(y) * small_width_ + x] = pixels[y * stride + 4 * x] / 255.0f;
    gs_stagesurface_unmap(slot.stage);
    return true;
}

bool ImageFilter::draw(bool comparison) {
    if (!have_display_ || !effect_)
        return false;
    auto *texture = gs_texrender_get_texture(slots_[display_].full);
    if (!texture)
        return false;
    SrgbScope srgb(true);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_effect_set_texture_srgb(image_, texture);
    const auto &warp = comparison ? comparison_warp_ : warp_;
    vector(row_x_, warp.row_x[0], warp.row_x[1], warp.row_x[2], 0);
    vector(row_y_, warp.row_y[0], warp.row_y[1], warp.row_y[2], 0);
    const auto &slot = slots_[display_];
    if (slot.hybrid)
        set_pose(slot.pose_warp, comparison);
    while (gs_effect_loop(effect_, slot.hybrid ? "DrawHybrid" : "Draw"))
        gs_draw_sprite(texture, 0, warp.width, warp.height);
    gs_blend_state_pop();
    return true;
}

bool ImageFilter::fallback(obs_source_t *source, unsigned width, unsigned height,
                           const vrst::Settings &settings) {
    have_display_ = false;
    result_ = {};
    result_.status = vrst::ImageStatus::invalid;
    warp_ = vrst::make_image_warp(width, height, settings, result_);
    if (!effect_ || !obs_source_process_filter_begin(source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
        return false;
    vector(row_x_, warp_.row_x[0], warp_.row_x[1], warp_.row_x[2], 0);
    vector(row_y_, warp_.row_y[0], warp_.row_y[1], warp_.row_y[2], 0);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_process_filter_end(source, effect_, warp_.width, warp_.height);
    gs_blend_state_pop();
    return true;
}

bool ImageFilter::render(obs_source_t *source, unsigned width, unsigned height,
                         const vrst::Settings &settings, const vrst::ImageSettings &image_settings,
                         std::uint64_t revision, const ImageRenderOptions &options) {
    // A second view may render this filter again in the same video tick. Reuse
    // both the selected video texture and its exact correction.
    if (processed_ && revision == revision_ && width == width_ && height == height_ &&
        options.hybrid == hybrid_)
        return draw(options.comparison);
    if (width != width_ || height != height_ || revision != revision_ || options.hybrid != hybrid_) {
        clear();
        revision_ = revision;
        hybrid_ = options.hybrid;
    }
    if (!effect_ || (!width_ && !allocate(width, height, settings)))
        return false;
    processed_ = true;
    const unsigned previous = 1 - next_;
    auto capture_settings = settings;
    const double base = crop_value(settings.stabilization_crop);
    double required = base;
    if (options.crop.enabled) {
        if (options.hybrid) {
            const auto first = pose_settings(settings), second = residual_settings(settings);
            const double stage_extra = stage_crop(std::min(40., base + crop_value(options.crop.max_extra))) -
                                       first.stabilization_crop;
            const auto frame = vrst::make_warp(width, height, first);
            const double pose_required = vrst::required_pose_crop(
                width, height, first, pose_correction(settings, options), options.automatic, stage_extra);
            const double image_required =
                vrst::required_image_crop(frame.width, frame.height, second, result_, stage_extra);
            required = 100 * (1 - std::pow(1 - std::max(pose_required, image_required) / 100, 2));
        } else
            required = vrst::required_image_crop(width, height, settings, result_, options.crop.max_extra);
    }
    // The controller advances once per captured frame. Its crop travels with
    // that frame, so analysis, residual limiting and A/B use identical geometry.
    capture_settings.stabilization_crop =
        crop_controller_.update(required, base, options.crop, vrst::monotonic_ns());
    if (!capture(slots_[next_], source, capture_settings, options)) {
        clear();
        return false;
    }
    display_ = slots_[previous].captured ? previous : next_;
    have_display_ = true;
    auto &slot = slots_[display_];
    if (slot.hybrid && (!have_pose_state_ || slot.pose_valid != pose_valid_))
        stabilizer_.reset();
    have_pose_state_ = true;
    pose_valid_ = slot.pose_valid;
    result_ = {};
    // Warm-up renders the current texture once without reading it back. Every
    // later tick reads and displays the preceding slot, adding one video frame.
    if (display_ == previous) {
        try {
            if (read(slots_[previous]))
                result_ = stabilizer_.process(gray_, slots_[previous].time, image_settings);
            else {
                stabilizer_.reset();
                result_.status = vrst::ImageStatus::invalid;
            }
        } catch (const std::exception &) {
            stabilizer_.reset();
            result_.status = vrst::ImageStatus::invalid;
        }
    }
    effective_crop_ = slot.settings.stabilization_crop;
    if (slot.hybrid) {
        const auto final = residual_settings(slot.settings);
        warp_ = vrst::make_image_warp(slot.pose_warp.width, slot.pose_warp.height, final, result_);
        comparison_warp_ = vrst::make_image_warp(slot.pose_warp.width, slot.pose_warp.height, final, {});
        // Preserve OBS's advertised dimensions even when an intermediate trim
        // has a fractional edge that rounds to an even pose-stage dimension.
        const auto dimensions = vrst::make_image_warp(width, height, slot.settings, {});
        warp_.width = comparison_warp_.width = dimensions.width;
        warp_.height = comparison_warp_.height = dimensions.height;
    } else {
        warp_ = vrst::make_image_warp(width, height, slot.settings, result_);
        comparison_warp_ = vrst::make_image_warp(width, height, slot.settings, {});
    }
    next_ = previous;
    return draw(options.comparison);
}
