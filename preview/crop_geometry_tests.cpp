#include "crop_geometry.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vrst::preview;
namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool close(double a, double b) { return std::abs(a - b) < 1e-9; }
bool same(FramingRegion a, FramingRegion b) {
    return close(a.left, b.left) && close(a.right, b.right) && close(a.top, b.top) && close(a.bottom, b.bottom);
}
void valid_crop(FramingRegion r, double source, double output) {
    check(r.left >= -1e-9 && r.right <= 1 + 1e-9 && r.top >= -1e-9 && r.bottom <= 1 + 1e-9 &&
              r.right > r.left && r.bottom > r.top,
          "crop must stay finite and inside source bounds");
    check(close((r.right - r.left) * source / (r.bottom - r.top), output),
          "crop resizing must preserve the optical output aspect");
    const double max_width = std::min(1., output / source);
    check(r.right - r.left >= max_width * framing_min_scale - 1e-9,
          "crop resize must respect the supported zoom limit");
}
}
int main() {
    try {
        Options options;
        options.crop = 20;
        options.aspect = 16. / 9.;
        check(same(crop_from_options(2, options), {13. / 90., 77. / 90., .1, .9}),
              "crop rectangle must represent the actual uniform crop and optical aspect fit");
        options.horizontal_offset = 100;
        options.vertical_offset = -100;
        check(same(crop_from_options(2, options), {13. / 45., 1, 0, .8}),
              "source-space crop must include both full-boundary placement controls");
        options.aspect = 1;
        crop_to_options({.2, .8, .25, .85}, 1, options);
        check(close(options.crop, 40) && close(options.horizontal_offset, 0) &&
                  close(options.vertical_offset, 25),
              "canonical options must encode source-space zoom and signed available-slack position");
        check(same(crop_from_options(1, options), {.2, .8, .25, .85}),
              "crop conversion must reproduce a hand-checked off-center rectangle");
        check(same(move_crop({.1, .9, .2, .8}, 2, -2), {.2, 1, 0, .6}),
              "dragging beyond borders must clamp position while retaining width and height");
        const FramingRegion square{.1, .9, .1, .9};
        check(same(resize_crop(square, CropHandle::left, .1, 0, 1, 1), {.2, .9, .15, .85}),
              "edge resizing must anchor the opposite edge and preserve transverse center");
        check(same(resize_crop(square, CropHandle::right, .8, 0, 1, 1), {.1, 1, .05, .95}),
              "edge resizing at a border must retain its opposite-edge anchor");
        check(same(resize_crop(square, CropHandle::top_left, .1, .1, 1, 1), {.2, .9, .2, .9}),
              "corner resizing must anchor the opposite corner");
        check(same(resize_crop(square, CropHandle::top_left, 10, 10, 1, 1), {.87, .9, .87, .9}),
              "corner resizing must stop at supported zoom without crossing its anchor");
        for (double source : {.5, 1., 2.4}) {
            for (double aspect : {.25, 1., 16. / 9., 4.}) {
                options = {};
                options.aspect = aspect;
                options.pitch_lock = 37;
                options.edge_reserve[1] = {true, 2, 20, 5, 7};
                for (double crop : {0., 7.8, 20., 40., 45.8368, 80., 97.}) {
                    for (double x : {-100., -23., 0., 81., 100.}) {
                        options.crop = crop;
                        options.horizontal_offset = x;
                        options.vertical_offset = -x;
                        const auto r = crop_from_options(source, options);
                        valid_crop(r, source, aspect);
                        auto converted = options;
                        crop_to_options(r, source, converted);
                        check(same(crop_from_options(source, converted), r),
                              "numeric and visual crop edits must roundtrip at corners and zoom limits");
                        check(converted.pitch_lock == 37 && converted.edge_reserve[1].enabled &&
                                  converted.edge_reserve[1].right == 20,
                              "crop editing must retain camera settings and the other eye reserve");
                        for (auto handle : {CropHandle::left, CropHandle::right, CropHandle::top,
                                            CropHandle::bottom, CropHandle::top_left, CropHandle::top_right,
                                            CropHandle::bottom_left, CropHandle::bottom_right}) {
                            for (double delta : {-10., -.1, 0., .1, 10.})
                                valid_crop(resize_crop(r, handle, delta, -delta, source, aspect), source,
                                           aspect);
                        }
                    }
                }
            }
        }
        options = {};
        options.aspect = 1.5;
        options.crop_left = 3;
        options.crop_right = 5;
        options.edge_reserve[0] = {true, 2, 12, 4, 8};
        options.horizontal_offset = 22;
        options.vertical_offset = -15;
        const auto advanced = crop_from_options(2, options);
        auto migrated = options;
        migrated.edge_reserve[1] = {true, 1, 2, 4, 8};
        check(migrate_legacy_framing(migrated, 2, 0) &&
                  same(crop_from_options(2, migrated), advanced) &&
                  !migrated.edge_reserve[0].enabled && !migrated.edge_reserve[1].enabled &&
                  !migrate_legacy_framing(migrated, 2, 0),
              "legacy migration preserves the active eye and clears obsolete controls once");
        auto converted = options;
        crop_to_options(advanced, 2, converted);
        check(same(crop_from_options(2, converted), advanced),
              "canonicalizing asymmetric reserve and trims must retain their visible crop");
        check(converted.crop_left == 0 && converted.crop_right == 0 &&
                  converted.crop_top == 0 && converted.crop_bottom == 0 && !converted.edge_reserve[0].enabled,
              "visual crop editing must have one unambiguous canonical framing representation");
        Options dan;
        dan.crop = 19.4;
        dan.crop_right = 32.8;
        dan.horizontal_offset = -11.7;
        dan.vertical_offset = -35;
        const auto original = crop_from_options(1.035, dan);
        check(migrate_legacy_framing(dan, 1.035, 0) && close(dan.crop, 45.8368) &&
              same(original, crop_from_options(1.035, dan)),
              "Dan's 0.16 framing must migrate without enlarging his selected source rectangle");
        Frame input{};
        input.session = input.sequence = input.display_time = input.view_count = 1;
        input.views[0].width = input.views[0].rect_width = 1000;
        input.views[0].height = input.views[0].rect_height = 1000;
        input.views[0].fov[0] = -1.035f;
        input.views[0].fov[1] = 1.035f;
        FrameStabilizer compositor;
        const auto composition = compositor.update(input, dan);
        const auto tl = map_uv(composition.warp, 0., 0.), br = map_uv(composition.warp, 1., 1.);
        check(composition.valid && same(crop_from_options(input.views[0].fov[1], dan),
                                        {tl[0], br[0], tl[1], br[1]}),
              "screenshot selection and live output rays share the migrated rectangle");
        for (double bad : {double(NAN), double(INFINITY), -100.}) {
            const auto r = constrain_crop({bad, bad, bad, bad}, bad, bad);
            valid_crop(r, 1, 16. / 9.);
        }
        std::cout << "Crop geometry: source mapping, roundtrip, bounds and anchored fixed-aspect resize passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
