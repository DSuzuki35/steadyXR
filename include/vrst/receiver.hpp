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
    std::string status;
};
class Receiver {
  public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;
    void configure(const std::string &host, std::uint16_t port);
    TrackingSnapshot snapshot(double delay_ms, double smoothing);
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst
