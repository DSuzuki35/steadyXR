#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <windows.h>

namespace vrst::preview {
class Theme;
enum class ShortcutAction {
    center_camera,
    edit_framing,
    pause_resume,
    next_preset,
    compare,
    learn_chat,
    count
};

class ShortcutController {
  public:
    ShortcutController();
    ~ShortcutController();
    void initialize(HWND owner, Theme &, const std::filesystem::path &preview_ini,
                    std::function<void(ShortcutAction)> trigger);
    void open();
    void tick();
    bool route(MSG &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
