#include "renderer.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vrst;
using namespace vrst::preview;
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
Frame frame(std::uint64_t sequence, double angle = 0) {
    Frame f{};
    f.generation = f.session = f.reference_epoch = 1;
    f.sequence = sequence;
    f.display_time = 1000000000LL + sequence * 16666667;
    f.producer_pid = 1;
    f.view_count = 2;
    const auto q = axis_angle({0, 1, 0}, angle);
    for (auto &v : f.views) {
        v.texture_handle = 1;
        v.width = v.height = v.rect_width = v.rect_height = 512;
        v.format = 28;
        v.orientation[0] = float(q.x);
        v.orientation[1] = float(q.y);
        v.orientation[2] = float(q.z);
        v.orientation[3] = float(q.w);
    }
    f.views[1].fov[0] = f.views[1].fov[3] = -.25f;
    f.views[1].fov[1] = f.views[1].fov[2] = .25f;
    return f;
}
int main() {
    try {
        FrameStabilizer s;
        Options options;
        check(s.update(frame(1), options).valid, "valid first frame must compose");
        auto moving = s.update(frame(2, .08), options);
        check(angle_between(moving.warp.correction, {}) > .01, "motion must be smoothed");
        auto repeat = s.update(frame(2, .08), options);
        check(angle_between(moving.warp.correction, repeat.warp.correction) < 1e-6,
              "redraw must not advance smoothing");
        auto reset_frame = frame(3, .15);
        reset_frame.reference_epoch++;
        check(angle_between(s.update(reset_frame, options).warp.correction, {}) < 1e-6,
              "reference reset must discard old correction");
        auto focus = frame(4, .18);
        auto fq = axis_angle({0, 1, 0}, .21);
        focus.views[1].orientation[1] = float(fq.y);
        focus.views[1].orientation[3] = float(fq.w);
        auto composed = s.update(focus, options);
        const auto target = multiply({focus.views[0].orientation[0], focus.views[0].orientation[1],
                                      focus.views[0].orientation[2], focus.views[0].orientation[3]},
                                     composed.warp.correction);
        check(angle_between(composed.focus_correction, multiply(conjugate(fq), target)) < 1e-6,
              "inset must use its own submitted orientation");
        options.stabilize = false;
        check(angle_between(s.update(frame(5, .2), options).warp.correction, {}) < 1e-6,
              "bypass must preserve source orientation");
        auto invalid = frame(6);
        invalid.views[0].fov[0] = NAN;
        check(!s.update(invalid, options).valid, "invalid projection must not render");
        std::cout << "7 composition checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
