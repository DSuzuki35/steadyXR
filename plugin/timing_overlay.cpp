#include "timing_overlay.hpp"

#include <algorithm>
#include <cmath>

namespace vrst_plugin {
namespace {
constexpr char effect_source[] = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
sampler_state timing_sampler {
    Filter = Point;
    AddressU = Clamp;
    AddressV = Clamp;
};
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v) {
    v.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    return v;
}
float4 PSPanel(VertData v) : TARGET { return image.Sample(timing_sampler, v.uv); }
technique Draw {
    pass { vertex_shader = VSDefault(v); pixel_shader = PSPanel(v); }
}
)";
} // namespace

bool TimingOverlay::load() {
    if (effect_)
        return true;
    char *errors = nullptr;
    effect_ = gs_effect_create(effect_source, "vrst-photo-timing", &errors);
    if (!effect_)
        blog(LOG_WARNING, "[vr-stabilizer] Photo timing panel unavailable: %s", errors ? errors : "");
    bfree(errors);
    image_ = effect_ ? gs_effect_get_param_by_name(effect_, "image") : nullptr;
    return image_ != nullptr;
}

void TimingOverlay::destroy() {
    gs_texture_destroy(texture_);
    gs_effect_destroy(effect_);
    texture_ = nullptr;
    effect_ = nullptr;
    image_ = nullptr;
    width_ = height_ = 0;
}

void TimingOverlay::draw(const vrst::TimingOverlayData &data, unsigned output_width, unsigned output_height) {
    if (!output_width || !output_height || !load())
        return;
    const auto bitmap = vrst::render_timing_overlay(data);
    if (!bitmap.width || !bitmap.height ||
        bitmap.rgba.size() != std::size_t(bitmap.width) * bitmap.height * 4)
        return;
    if (!texture_ || width_ != bitmap.width || height_ != bitmap.height) {
        gs_texture_destroy(texture_);
        width_ = bitmap.width;
        height_ = bitmap.height;
        texture_ = gs_texture_create(width_, height_, GS_RGBA, 1, nullptr, GS_DYNAMIC);
    }
    if (!texture_)
        return;
    gs_texture_set_image(texture_, bitmap.rgba.data(), width_ * 4, false);

    // Leave the upper half for the gaming-PC clock captured in the original
    // picture. Bound both dimensions even for portrait/tiny filter outputs.
    const unsigned margin = std::min({24u, output_width / 50, output_height / 50});
    const double scale =
        std::min({std::max(1., output_height / 720.), double(output_width - 2 * margin) / width_,
                  output_height * .46 / height_});
    const unsigned panel_width = std::max(1u, unsigned(std::floor(width_ * scale)));
    const unsigned panel_height = std::max(1u, unsigned(std::floor(height_ * scale)));
    const bool srgb = gs_framebuffer_srgb_enabled();
    auto *vertex_shader = gs_get_vertex_shader();
    auto *pixel_shader = gs_get_pixel_shader();
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_enable_framebuffer_srgb(true);
    gs_effect_set_texture_srgb(image_, texture_);
    gs_matrix_push();
    gs_matrix_translate3f(float(output_width - margin - panel_width),
                          float(output_height - margin - panel_height), 0);
    while (gs_effect_loop(effect_, "Draw"))
        gs_draw_sprite(texture_, 0, panel_width, panel_height);
    gs_matrix_pop();
    gs_enable_framebuffer_srgb(srgb);
    gs_blend_state_pop();
    gs_load_vertexshader(vertex_shader);
    gs_load_pixelshader(pixel_shader);
}
} // namespace vrst_plugin
