#pragma once
#include "vrst/core.hpp"
#include <memory>

namespace vrst::sender {
// Optional desktop diagnostic. All calls, including destruction, stay on the
// sender thread; the window never owns the tracking loop or its shutdown flag.
class DesktopOverlay {
  public:
    explicit DesktopOverlay(bool enabled);
    ~DesktopOverlay();
    DesktopOverlay(const DesktopOverlay &) = delete;
    DesktopOverlay &operator=(const DesktopOverlay &) = delete;
    void published(const Pose &pose);
    void refresh(std::uint64_t now_ns, std::uint64_t session);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::sender
