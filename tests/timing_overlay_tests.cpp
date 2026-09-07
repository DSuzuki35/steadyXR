#include "vrst/timing_overlay.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>

namespace {
int checks{}, failures{};
void check(bool ok, const char *name) {
    ++checks;
    failures += !ok;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
}
std::string joined(const vrst::TimingOverlayData &data) {
    std::string out;
    for (const auto &line : vrst::timing_overlay_lines(data))
        out += line + "\n";
    return out;
}
} // namespace

int main() {
    using namespace vrst;
    check(format_timing_stamp(12345678901ULL) == "12.345", "timestamp shows exact truncated milliseconds");
    check(format_timing_stamp(999999999ULL) == "0.999", "timestamp never rounds into a later second");
    check(format_timing_stamp(UINT64_MAX) == "18446744073.709", "long uptime formatting stays exact");
    TimingOverlayData data;
    auto text = joined(data);
    check(text.find("WAITING FOR CLOCK") != std::string::npos && text.find("LATEST: --") != std::string::npos,
          "missing data has explicit unavailable labels rather than zero timestamps");
    data.sender_view = true;
    data.now_ns = 12350000000ULL;
    data.session = 42;
    data.latest_present = true;
    data.latest = {42, 123, 12345000000ULL};
    data.latest.flags = TrackingValid | FramePose;
    text = joined(data);
    check(text.find("GAME PC") != std::string::npos && text.find("12.350") != std::string::npos &&
              text.find("12.345") != std::string::npos && text.find("#123") != std::string::npos,
          "gaming panel uses the actually published pose and original clock");
    check(text.find("OBSERVED POSE") != std::string::npos &&
              text.find("NOT A GAME-FRAME TAG") != std::string::npos,
          "gaming panel does not claim exact image-pose metadata");
    data.now_ns += 300000000ULL;
    check(joined(data).find("STALE") != std::string::npos, "frozen sender tracking is visibly stale");
    data.latest.flags = DemoPose | TrackingValid;
    check(joined(data).find("DEMO") != std::string::npos, "synthetic tracking is visibly labeled");
    data.sender_view = false;
    data.connected = data.clock_ready = true;
    data.now_ns = 12350000000ULL;
    data.latest.flags = TrackingValid | FramePose;
    data.latest_receive_age_ns = 5000000;
    data.selected.valid = true;
    data.selected.session = 42;
    data.selected.time_ns = data.selected.sample_time_ns = 12230000000ULL;
    data.selected.sequence = 112;
    data.selected.flags = TrackingValid | FramePose;
    data.pose_applied = true;
    data.delay_ms = 120;
    data.buffer_age_ns = 16666667;
    text = joined(data);
    check(text.find("LATEST: 12.345") != std::string::npos &&
              text.find("USED:   12.230") != std::string::npos &&
              text.find("120.0 MS") != std::string::npos && text.find("16.7 MS") != std::string::npos,
          "stream panel separates latest receipt, displayed-frame pose, delay and buffer age");
    data.selected.interpolated = true;
    data.selected.sample_time_ns = 12235000000ULL;
    text = joined(data);
    check(text.find("BLEND AT: 12.235") != std::string::npos &&
              text.find("USED:   12.230") != std::string::npos,
          "interpolated effective time never masquerades as an original packet timestamp");
    data.pose_applied = false;
    data.comparison = true;
    check(joined(data).find("USED:   NONE - A/B") != std::string::npos,
          "comparison does not claim the selected correction was applied");
    data.comparison = false;
    data.image_only = true;
    check(joined(data).find("USED:   NONE - IMAGE ONLY") != std::string::npos,
          "image-only mode does not claim a used headset pose");
    data.image_only = false;
    data.calibrating = true;
    check(joined(data).find("USED:   NONE - CALIBRATION") != std::string::npos,
          "calibration preview does not claim a used headset pose");
    data.calibrating = false;
    data.pose_applied = true;
    data.selected.session = 41;
    check(joined(data).find("OLD SESSION:") != std::string::npos,
          "an older buffered session cannot appear aligned with the new session");
    check(joined(data).find("BLEND AT: 12.235") != std::string::npos,
          "an old-session warning preserves the actual interpolated sample time");
    data.selected.session = 42;
    data.latest_receive_age_ns = 300000000ULL;
    check(joined(data).find("STALE") != std::string::npos, "stale receiver packets stay clearly labeled");
    data.delay_ms = std::numeric_limits<double>::quiet_NaN();
    data.rtt_ms = std::numeric_limits<double>::infinity();
    text = joined(data);
    check(text.find("NAN") == std::string::npos && text.find("INF") == std::string::npos,
          "invalid diagnostic measurements display unavailable rather than nonfinite numbers");
    auto bitmap = render_timing_overlay(data, 2);
    check(bitmap.width > 500 && bitmap.width <= 1000 && bitmap.height > 100 && bitmap.height <= 300 &&
              bitmap.rgba.size() == size_t(bitmap.width) * bitmap.height * 4,
          "panel raster has bounded dimensions and exact RGBA storage");
    bool opaque = true, varied = false;
    for (size_t i = 0; i < bitmap.rgba.size(); i += 4) {
        opaque &= bitmap.rgba[i + 3] == 255;
        varied |= bitmap.rgba[i] > 100;
    }
    check(opaque && varied, "panel draws visible text with unambiguous opaque alpha");
    auto bounded = render_timing_overlay(data, 99999);
    check(bounded.width <= 2000 && bounded.height <= 600,
          "untrusted scale cannot allocate an unbounded panel");
    std::printf("%d/%d timing overlay checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
