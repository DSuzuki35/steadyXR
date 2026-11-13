#pragma once
#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace vrst::preview {
struct CaptureWindow {
    HWND handle{};
    std::wstring title;
    DWORD pid{};
};
// Duplication surfaces use native panel orientation. An optional separate
// hardware pointer travels with the private GPU frame to the clone shader.
struct MonitorFrameInfo {
    DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_IDENTITY};
    POINT pointer{};
    unsigned pointer_type{}, pointer_width{}, pointer_height{};
};
inline constexpr GUID monitor_info_guid{
    0xd4320ac1, 0xd5fc, 0x426f, {0x86, 0xdc, 0xfb, 0x04, 0xea, 0x42, 0x08, 0x6f}};
inline constexpr GUID monitor_pointer_guid{
    0x32189664, 0x2625, 0x4870, {0x88, 0xfb, 0x6c, 0xe1, 0x71, 0x2e, 0xa3, 0x09}};
// Start, poll, stop and destruction run on the owning UI thread. poll returns a
// private GPU texture, valid until the next successful poll or stop.
class WindowCapture {
  public:
    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture &) = delete;
    WindowCapture &operator=(const WindowCapture &) = delete;
    bool start(ID3D11Device *, HWND);
    bool start_monitor(ID3D11Device *, HMONITOR);
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
