#pragma once
#include <functional>
#include <memory>
#include <string>
#include <windows.h>

namespace vrst::preview {
class Theme;
enum class FallbackMode { waiting, hide, monitor };
struct OutputFallbackSettings {
    FallbackMode mode{FallbackMode::waiting};
    bool cockpit_only{};
    // An empty name follows the Windows primary display. Others are MONITORINFOEXW device names.
    std::wstring monitor_name;
};

OutputFallbackSettings read_output_fallback_settings(const std::wstring &ini);
bool write_output_fallback_settings(const std::wstring &ini, const OutputFallbackSettings &settings);

class OutputFallbackDialog {
  public:
    OutputFallbackDialog();
    ~OutputFallbackDialog();
    void open(HWND owner, Theme &theme, const OutputFallbackSettings &settings,
              std::function<void(OutputFallbackSettings)> applied);
    bool route(MSG &message);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
