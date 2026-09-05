#include "pose_math.hpp"
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok)
            ++failed;
    };
    using namespace vrst::sender;
    const Rotation identity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const Rotation rotate_x{{{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}};
    const Rotation rotate_y{{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}};
    const Rotation rotate_z{{{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}}};
    const Rotation turn_y{{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}};
    auto matches = [](const std::optional<vrst::Quat> &got, vrst::Quat want) {
        return got && vrst::angle_between(*got, want) < 1e-7;
    };
    check(matches(rotation_quaternion(identity), {0, 0, 0, 1}), "identity matrix conversion");
    check(matches(rotation_quaternion(rotate_x), {1, 0, 0, 0}), "180-degree X trace branch");
    check(matches(rotation_quaternion(rotate_y), {0, 1, 0, 0}), "180-degree Y trace branch");
    check(matches(rotation_quaternion(rotate_z), {0, 0, 1, 0}), "180-degree Z trace branch");
    check(matches(rotation_quaternion(turn_y), {0, .7071067811865475, 0, .7071067811865475}),
          "90-degree yaw matrix sign");
    auto eye = rotation_quaternion(turn_y);
    if (eye) {
        auto direction = vrst::rotate(vrst::multiply({1, 0, 0, 0}, *eye), {0, 0, -1});
        check(std::abs(direction.x + 1) < 1e-9 && std::abs(direction.z) < 1e-9,
              "eye rotation composes after head rotation");
    } else
        check(false, "eye rotation composes after head rotation");
    check(!rotation_quaternion({}), "degenerate matrix rejected");
    auto malformed = identity;
    malformed[0][0] = std::numeric_limits<double>::quiet_NaN();
    check(!rotation_quaternion(malformed), "nonfinite matrix rejected");
    auto reflection = identity;
    reflection[0][0] = -1;
    check(!rotation_quaternion(reflection), "reflection matrix rejected");
    FrameSelector frames;
    check(frames.select(true, 500, true, 1000000000) == Source::frame, "first valid frame selected");
    check(frames.select(true, 500, true, 1005000000) == Source::wait, "duplicate frame not retimestamped");
    check(frames.select(false, 501, true, 1010000000) == Source::invalid,
          "current tracking loss suppresses historical frame");
    check(frames.select(true, 501, true, 1020000000) == Source::wait,
          "tracking recovery does not reuse old frame");
    check(frames.select(true, 0, true, 1030000000) == Source::frame, "frame index reset accepted");
    check(frames.select(true, 0, true, 1200000000) == Source::live,
          "stuck frame falls back to current live pose");
    check(frames.select(true, std::nullopt, false, 1210000000) == Source::live,
          "missing frame API falls back to live");
    check(frames.select(true, 1, false, 1220000000) == Source::live, "invalid frame pose falls back to live");
    return failed ? 1 : 0;
}
