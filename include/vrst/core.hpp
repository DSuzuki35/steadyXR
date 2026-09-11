#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <optional>

namespace vrst {
constexpr double pi = 3.14159265358979323846;
struct Vec3 {
    double x{}, y{}, z{};
};
struct Quat {
    double x{}, y{}, z{}, w{1};
};
Quat normalized(Quat q);
Quat conjugate(Quat q);
Quat multiply(Quat a, Quat b);
Quat slerp(Quat a, Quat b, double t);
Quat axis_angle(Vec3 axis, double radians);
// Removes target roll relative to tracking-space up, preserving its look direction.
// Returns inverse sampling correction conjugate(source) * leveled_target.
Quat horizon_correction(Quat source, Quat target, double strength_percent, double max_degrees = 45);
Vec3 rotate(Quat q, Vec3 v);
double angle_between(Quat a, Quat b);
bool valid(Quat q);

// Right-handed eye space: X right, Y up, -Z forward. Tangents are signed.
struct Projection {
    double left{-1}, right{1}, up{1}, down{-1};
};
bool valid(Projection p);
Projection symmetric_projection(double horizontal_degrees, double vertical_degrees);

enum PoseFlags : std::uint32_t { TrackingValid = 1, FramePose = 2, DemoPose = 4 };
struct Pose {
    std::uint64_t session{}, sequence{}, time_ns{};
    Quat orientation{};
    Projection projection{};
    std::uint32_t flags{TrackingValid};
};
struct PoseResult {
    bool valid{};
    Quat correction{}; // conjugate(source eye orientation) * smoothed eye orientation
    Quat source_orientation{}, target_orientation{};
    Projection projection{};
    std::uint64_t time_ns{}, sequence{};
    std::uint32_t flags{};
    std::uint64_t session{};
    std::uint64_t sample_time_ns{}; // effective source time; time_ns keeps the earlier packet stamp
    bool interpolated{};
};
struct MotionSettings {
    double amount{40};
    bool separate_axes{};
    double yaw{40}, pitch{40}, roll{40};
    double turn_follow{}, turn_speed{90}; // percent, degrees/second at full follow
};
class PoseHistory {
  public:
    bool push(const Pose &pose);
    void clear();
    void set_smoothing(double percent);
    void set_smoothing(const MotionSettings &);
    PoseResult sample(std::uint64_t time_ns) const;
    std::size_t size() const {
        return entries_.size();
    }
    std::uint64_t latest_time() const;

  private:
    struct Entry {
        Pose pose;
        Quat smooth;
        Vec3 follow_velocity{}, follow_speed{};
    };
    std::deque<Entry> entries_;
    double smoothing_{40};
    MotionSettings motion_{};
    void recompute();
};

struct EdgeReserve {
    bool enabled{};
    double left{5}, right{5}, top{5}, bottom{5}; // Percent of the source extent per edge.
};
EdgeReserve sanitize_edge_reserve(EdgeReserve);
struct Settings {
    double horizontal_fov{100}, vertical_fov{90};
    bool automatic_fov{false};
    double delay_ms{80}, smoothing{40}, stabilization_crop{10}, framing_zoom{0};
    double horizon_level{0}, horizon_max_degrees{45};
    bool output_16_9{true};
    double framing_x{0}, framing_y{0}; // -100..100, within available framing margin
    double input_left{0}, input_right{0}, input_top{0}, input_bottom{0}; // percent per edge
    bool mirror_edges{false}; // Allow correction beyond the trimmed image, filled by reflection.
    EdgeReserve edge_reserve{};
};
struct Warp {
    Projection input{}, output{};
    Quat correction{};
    double uv_left{}, uv_right{1}, uv_top{}, uv_bottom{1};
    std::uint32_t width{}, height{};
    double correction_fraction{1};
};
Warp make_warp(std::uint32_t width, std::uint32_t height, const Settings &settings, Quat correction = {},
               std::optional<Projection> automatic = std::nullopt);
// Maps final output normalized coordinates to coordinates in the original input.
std::array<double, 2> map_uv(const Warp &warp, double u, double v);
bool corners_inside(const Warp &warp);
// Re-evaluate a correction after changing output framing.
void limit_warp_correction(Warp &, Quat correction, bool mirror_edges);

// Four timestamps: client sent, server received, server sent, client received.
class ClockSync {
  public:
    bool observe(std::uint64_t t1, std::uint64_t t2, std::uint64_t t3, std::uint64_t t4);
    bool ready() const {
        return !samples_.empty();
    }
    double offset_ns() const; // server minus client
    double rtt_ms() const;
    void clear() {
        samples_.clear();
    }

  private:
    struct Sample {
        double offset, rtt;
        std::uint64_t at;
    };
    std::deque<Sample> samples_;
};
std::uint64_t monotonic_ns();
} // namespace vrst
