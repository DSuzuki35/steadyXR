#pragma once
#include "renderer.hpp"
#include "theme.hpp"
#include <functional>

namespace vrst::preview {
class FramingEditor {
  public:
    FramingEditor();
    ~FramingEditor();
    void initialize(HWND owner, Theme &, Renderer &, std::function<Options()> current,
                    std::function<void(const Options &)> apply, std::function<bool()> can_capture = {});
    HWND create_monitor(RECT bounds, int id);
    void open();
    void capture(); // A shortcut can capture in the background while the game has focus.
    void tick();
    bool route(MSG &);
    const std::wstring &status() const;
    bool has_still() const;
    std::uint64_t still_revision() const;
    std::wstring still_description() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
