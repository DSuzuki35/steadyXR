#pragma once
#include "renderer.hpp"
#include "window_capture.hpp"

namespace vrst::preview {
// The capture session and its last image belong exclusively to the monitor route.
class MonitorFallback {
  public:
    void stop();
    void tick(HWND output, const std::wstring &device_name,
              const Options &options, unsigned fps, std::uint64_t now, bool repaint);
    void display_changed() { topology_dirty_ = true; }
    bool has_picture() const { return has_picture_; }
    bool capturing() const { return capture_.running(); }
    const std::wstring &status() const { return status_; }
    explicit MonitorFallback(Renderer &renderer) : renderer_(renderer) {}

  private:
    void close_capture();
    Renderer &renderer_;
    WindowCapture capture_;
    HMONITOR monitor_{};
    ID3D11Device *device_{}; // Identity only. The capture owns a reference while active.
    bool topology_dirty_{true}, has_picture_{}, pending_frame_{};
    std::uint64_t next_attempt_{}, next_check_{}, last_poll_{}, last_message_{};
    std::wstring status_;
};
} // namespace vrst::preview
