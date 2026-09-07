#include "vrst/timing_overlay.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace vrst {
namespace {
std::string milliseconds(double value) {
    if (!std::isfinite(value) || value < 0)
        return "--";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
std::string session_text(std::uint64_t session) {
    if (!session)
        return "--";
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << session;
    return out.str();
}
std::string pose_text(std::uint64_t time, std::uint64_t sequence) {
    return format_timing_stamp(time) + " S #" + std::to_string(sequence);
}
std::string tracking_mode(std::uint32_t flags) {
    if (flags & DemoPose)
        return "DEMO / SYNTHETIC";
    if (!(flags & TrackingValid))
        return "TRACKING UNAVAILABLE";
    return flags & FramePose ? "OBSERVED FRAME POSE" : "LIVE POSE";
}

// Original 5x7 bitmap glyphs. Keeping the small ASCII alphabet here avoids a
// font/Qt/OBS-text-plugin dependency and identical labels rasterize identically.
std::array<unsigned char, 7> glyph(char c) {
    switch (c) {
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {14, 17, 16, 16, 16, 17, 14};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {14, 17, 16, 23, 17, 17, 15};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 17, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 21, 10};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case ':':
        return {0, 4, 4, 0, 4, 4, 0};
    case '.':
        return {0, 0, 0, 0, 0, 6, 6};
    case ',':
        return {0, 0, 0, 0, 6, 4, 8};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '/':
        return {1, 1, 2, 4, 8, 16, 16};
    case '#':
        return {10, 10, 31, 10, 31, 10, 10};
    case '|':
        return {4, 4, 4, 4, 4, 4, 4};
    case '(':
        return {2, 4, 8, 8, 8, 4, 2};
    case ')':
        return {8, 4, 2, 2, 2, 4, 8};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case '=':
        return {0, 0, 31, 0, 31, 0, 0};
    case ' ':
        return {};
    default:
        return {14, 17, 1, 2, 4, 0, 4};
    }
}
} // namespace

std::string format_timing_stamp(std::uint64_t ns) {
    const auto ms = (ns / 1000000ULL) % 1000ULL;
    const auto fraction = std::to_string(ms);
    return std::to_string(ns / 1000000000ULL) + "." + std::string(3 - fraction.size(), '0') + fraction;
}

std::vector<std::string> timing_overlay_lines(const TimingOverlayData &d) {
    const bool clock = d.sender_view || d.clock_ready;
    const bool current = d.latest_present && d.latest.session == d.session;
    const bool valid_latest = current && (d.latest.flags & TrackingValid);
    const bool have_age = clock && current && d.now_ns >= d.latest.time_ns;
    const double pose_age = have_age ? (d.now_ns - d.latest.time_ns) * 1e-6 : -1;
    const bool stale = (!d.sender_view && (!d.connected || d.latest_receive_age_ns >= 250000000ULL)) ||
                       (have_age && pose_age >= 250);
    std::vector<std::string> lines;
    lines.emplace_back(d.sender_view ? "GAME PC / PHOTO TIMING" : "STREAM PC / PHOTO TIMING");
    lines.push_back(clock ? "NOW:    " + format_timing_stamp(d.now_ns) + " S" : "NOW:    WAITING FOR CLOCK");
    if (d.sender_view) {
        lines.push_back(current ? "POSE:   " + pose_text(d.latest.time_ns, d.latest.sequence) : "POSE:   --");
        lines.push_back("SESSION: " + session_text(d.session));
        lines.push_back(current ? tracking_mode(d.latest.flags) : "WAITING FOR A PUBLISHED POSE");
        lines.push_back("POSE AGE: " + milliseconds(pose_age) + " MS" + (stale ? " / STALE" : ""));
        lines.emplace_back("OBSERVED POSE - NOT A GAME-FRAME TAG");
        lines.emplace_back("DRAG WINDOW / KEEP OUTSIDE TRACKING REGION");
        return lines;
    }
    lines.push_back(current ? "LATEST: " + pose_text(d.latest.time_ns, d.latest.sequence) : "LATEST: --");
    const bool used = d.pose_applied && d.selected.valid && !d.comparison && !d.calibrating && !d.image_only;
    if (used) {
        lines.push_back("USED:   " + pose_text(d.selected.time_ns, d.selected.sequence));
        if (d.selected.session != d.session)
            lines.push_back("OLD SESSION: " + session_text(d.selected.session));
        if (d.selected.interpolated)
            lines.push_back("BLEND AT: " + format_timing_stamp(d.selected.sample_time_ns) + " S");
        else
            lines.push_back("USED MODE: " + tracking_mode(d.selected.flags));
    } else {
        const char *reason = d.calibrating  ? "CALIBRATION"
                             : d.comparison ? "A/B"
                             : d.image_only ? "IMAGE ONLY"
                                            : "NO VALID POSE";
        lines.push_back(std::string("USED:   NONE - ") + reason);
        lines.emplace_back("NO HEADSET CORRECTION APPLIED");
    }
    lines.push_back("DELAY: " + milliseconds(d.delay_ms) +
                    " MS | VIDEO BUFFER: " + milliseconds(d.buffer_age_ns * 1e-6) + " MS");
    lines.push_back("SESSION: " + session_text(d.session));
    lines.push_back(
        "RECEIVE AGE: " + (d.latest_present ? milliseconds(d.latest_receive_age_ns * 1e-6) : "--") +
        " MS | RTT: " + (clock ? milliseconds(d.rtt_ms) : "--") + " MS");
    lines.push_back(!clock     ? "WAITING FOR CLOCK"
                    : !current ? "WAITING FOR TRACKING"
                               : std::string(stale ? "STALE / " : "") +
                                     (valid_latest ? tracking_mode(d.latest.flags) : "TRACKING UNAVAILABLE"));
    lines.emplace_back("TIMING CHECK / GAME-FRAME MATCH NOT GUARANTEED");
    return lines;
}

OverlayBitmap render_timing_overlay(const TimingOverlayData &data, unsigned scale) {
    scale = std::clamp(scale, 1U, 4U);
    const auto lines = timing_overlay_lines(data);
    constexpr unsigned padding = 8, columns = 64, line_height = 11;
    const unsigned rows = unsigned(std::min(lines.size(), size_t(12)));
    OverlayBitmap out{(padding * 2 + columns * 6) * scale, (padding * 2 + rows * line_height) * scale, {}};
    out.rgba.resize(size_t(out.width) * out.height * 4);
    const std::array<std::uint8_t, 3> accent = data.sender_view ? std::array<std::uint8_t, 3>{246, 191, 66}
                                                                : std::array<std::uint8_t, 3>{82, 214, 234};
    const auto pixel = [&](unsigned x, unsigned y, const std::array<std::uint8_t, 3> &color) {
        const auto at = (size_t(y) * out.width + x) * 4;
        std::copy(color.begin(), color.end(), out.rgba.begin() + at);
        out.rgba[at + 3] = 255;
    };
    for (unsigned y = 0; y < out.height; ++y)
        for (unsigned x = 0; x < out.width; ++x)
            pixel(x, y,
                  x < 2 * scale || y < 2 * scale || x >= out.width - 2 * scale || y >= out.height - 2 * scale
                      ? accent
                      : std::array<std::uint8_t, 3>{14, 22, 34});
    for (unsigned row = 0; row < rows; ++row) {
        const auto &line = lines[row];
        const bool warning =
            line.find("STALE") != std::string::npos || line.find("WAITING") != std::string::npos ||
            line.find("UNAVAILABLE") != std::string::npos || line.find("OLD SESSION") != std::string::npos;
        const std::array<std::uint8_t, 3> color = !row      ? accent
                                                  : warning ? std::array<std::uint8_t, 3>{255, 191, 86}
                                                            : std::array<std::uint8_t, 3>{238, 242, 247};
        for (unsigned col = 0; col < std::min(size_t(columns), line.size()); ++col) {
            const auto shape = glyph(line[col]);
            for (unsigned y = 0; y < 7; ++y)
                for (unsigned x = 0; x < 5; ++x)
                    if (shape[y] & (1U << (4 - x)))
                        for (unsigned sy = 0; sy < scale; ++sy)
                            for (unsigned sx = 0; sx < scale; ++sx)
                                pixel((padding + col * 6 + x) * scale + sx,
                                      (padding + row * line_height + y) * scale + sy, color);
        }
    }
    return out;
}
} // namespace vrst
