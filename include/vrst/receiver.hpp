#pragma once
#include "vrst/core.hpp"
#include <memory>
#include <string>

namespace vrst {
struct TrackingSnapshot {
    PoseResult pose;
    bool connected{}, clock_ready{};
    double rtt_ms{}, pose_age_ms{};
    std::uint64_t received{}, rejected{};
    std::uint64_t local_pose_time_ns{}; // selected raw pose stamp mapped into receiver's monotonic clock
    std::string status;
    Pose latest_pose; // last accepted raw packet, independent of selected delay and tracking-valid flag
    bool latest_received{}; // retained when stale; cleared with history/session/connection resets
    std::uint64_t local_time_ns{}, remote_now_ns{}; // receiver now; sender now (zero without current sync)
    std::uint64_t latest_receive_time_ns{}; // local monotonic time of latest_pose reception
};
class Receiver {
  public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;
    void configure(const std::string &host, std::uint16_t port);
    TrackingSnapshot snapshot(double delay_ms, double smoothing);
    TrackingSnapshot snapshot(double delay_ms, const MotionSettings &smoothing);
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst
