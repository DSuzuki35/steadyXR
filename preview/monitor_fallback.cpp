#include "monitor_fallback.hpp"
#include <algorithm>

namespace vrst::preview {
namespace {
HMONITOR selected_monitor(const std::wstring &name) {
    if (name.empty())
        return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    struct Search { const std::wstring &name; HMONITOR found{}; } search{name};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM arg) -> BOOL {
        auto &search = *reinterpret_cast<Search *>(arg);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && !_wcsicmp(info.szDevice, search.name.c_str())) {
            search.found = monitor;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}
}

void MonitorFallback::close_capture() {
    capture_.stop();
    renderer_.clear_monitor();
    device_ = nullptr;
    has_picture_ = pending_frame_ = false;
    hold_until_ = last_poll_ = next_attempt_ = last_message_ = 0;
    status_.clear();
}
void MonitorFallback::stop() {
    close_capture();
    topology_dirty_ = true;
    monitor_ = nullptr;
}
void MonitorFallback::tick(HWND output, const std::wstring &device_name,
                           const Options &options, unsigned fps, std::uint64_t now, bool repaint) {
    if (topology_dirty_ || now >= next_check_) {
        const auto selected = selected_monitor(device_name);
        if (selected != monitor_) {
            close_capture();
            monitor_ = selected;
        } else if (topology_dirty_) {
            capture_.stop();
            pending_frame_ = false;
            next_attempt_ = 0;
            if (has_picture_ && !hold_until_) hold_until_ = now + 2000;
        }
        topology_dirty_ = false;
        next_check_ = now + 1000;
    }
    if (device_ && renderer_.device() != device_)
        close_capture();
    if (!monitor_) {
        status_ = L"Fallback monitor disconnected";
    } else {
        if (!capture_.running() && now >= next_attempt_) {
            next_attempt_ = now + 250;
            if (!renderer_.device())
                renderer_.attach(output);
            if (renderer_.device() && capture_.start_monitor(renderer_.device(), monitor_))
                device_ = renderer_.device();
        }
        if (capture_.running() && (repaint || now - last_poll_ >= 1000 / std::max(1u, fps))) {
            last_poll_ = now;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            std::uint64_t time{};
            capture_.poll(texture, time);
            // A busy swapchain needs a retry even if the desktop stays static.
            // Keep the last presented picture on screen until the new one is ready.
            pending_frame_ = pending_frame_ || texture;
            if (capture_.running() && pending_frame_ &&
                renderer_.draw_monitor(texture.Get(), options.output_width, options.output_height)) {
                has_picture_ = true;
                pending_frame_ = false;
                hold_until_ = 0;
                status_.clear();
            } else if (repaint && has_picture_ && !pending_frame_) {
                renderer_.draw_monitor(nullptr, options.output_width, options.output_height);
            }
            if (!capture_.running()) {
                next_attempt_ = now + 250;
                pending_frame_ = false;
                if (has_picture_ && !hold_until_) hold_until_ = now + 2000;
            }
        }
        if (!capture_.running()) {
            const auto error = capture_.error();
            if (!error.empty()) status_ = error;
        }
        if (has_picture_ && (!hold_until_ || now < hold_until_))
            return;
        if (has_picture_) {
            if (!pending_frame_) renderer_.clear_monitor();
            has_picture_ = false;
        }
        if (status_.empty())
            status_ = L"Waiting for fallback monitor";
    }
    if (repaint || !last_message_ || now - last_message_ >= 250) {
        renderer_.draw(options, true, monitor_ ? L"Waiting for fallback monitor" : L"Fallback monitor disconnected");
        last_message_ = now;
    }
}
} // namespace vrst::preview
