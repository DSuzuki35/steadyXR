#pragma once
#include "render_settings.hpp"
#include "renderer.hpp"
#include "theme.hpp"
#include <functional>
#include <memory>

namespace vrst::preview {
void read_edge_settings(const std::wstring &ini, Options &);
bool write_edge_settings(const std::wstring &ini, const Options &);
class AdvancedSettings {
  public:
    enum Id {
        eye_id = 2001,
        separate_id,
        left_id,
        right_id,
        top_id,
        bottom_id,
        vertical_id,
        render_id,
        render_left_id,
        render_right_id,
        inward_id,
        save_render_id,
        center_id,
        stretch_id
    };
    AdvancedSettings();
    ~AdvancedSettings();
    void initialize(HWND parent, Theme &, Options &, const std::wstring &ini, std::function<void()> changed,
                    std::function<void(std::wstring)> notice);
    void open(unsigned eye, bool visible = true);
    HWND handle() const;
    bool route(MSG &);
    RenderSettings draft() const;
    void reload(const std::wstring &ini);
    void refresh_live_controls();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
