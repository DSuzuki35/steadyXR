#pragma once
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace vrst::preview {
struct CaptureWindow {
    HWND handle{};
    std::wstring title;
};
// Start, poll, stop and destruction run on the owning UI thread. poll returns a
// private GPU texture, valid until the next successful poll or stop.
class WindowCapture {
  public:
    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture &) = delete;
    WindowCapture &operator=(const WindowCapture &) = delete;
    bool start(ID3D11Device *, HWND);
    void stop();
    bool poll(Microsoft::WRL::ComPtr<ID3D11Texture2D> &texture, std::uint64_t &time_ns);
    bool running() const;
    std::wstring error() const;
    static std::vector<CaptureWindow> windows();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
