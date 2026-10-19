#pragma once
#include "renderer.hpp"
#include "theme.hpp"
#include <functional>
#include <memory>

namespace vrst::preview {
// One modeless panel; camera and visual pages share native keyboard navigation.
class SpectatorControls {
  public:
    enum Id { page_id = 3000, camera_bool_id = 3010, camera_number_id = 3030,
              visual_bool_id = 3060, visual_number_id = 3080, region_id = 3110,
              learn_id = 3120, cancel_id, clear_id, natural_id, cinematic_id,
              filmic_id, neutral_id, compare_scope_id, status_id };
    SpectatorControls();
    ~SpectatorControls();
    void initialize(HWND, Theme &, Options &, Renderer &, std::function<void()> changed,
                    std::function<void()> learn);
    void open(bool visible = true);
    void refresh();
    HWND handle() const;
    bool route(MSG &);
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
