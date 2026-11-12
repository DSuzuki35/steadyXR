#pragma once
#include "theme.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace vrst::preview {
class MonitorWidget {
  public:
    MonitorWidget();
    ~MonitorWidget();
    bool create(HWND parent, Theme &, const std::filesystem::path &layout_ini,
                std::function<void(HDC, RECT)> paint_canvas, int id = 0);
    HWND window() const;
    void set_caption(const std::wstring &);
    void invalidate();
    void reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
