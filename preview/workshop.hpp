#pragma once
#include "renderer.hpp"
#include "theme.hpp"
#include <filesystem>
#include <functional>
#include <memory>
namespace vrst::preview {
class Workshop {
  public:
    struct Callbacks {
        std::function<bool()> source_ready;
        std::function<void(bool)> pause_live;
        std::function<void(const Options &)> apply;
        std::function<Options()> options;
    };
    Workshop();
    ~Workshop();
    void initialize(HWND, Theme &, Renderer &, const std::filesystem::path &ini, Callbacks);
    void open();
    void open_clip(const std::filesystem::path &);
    void tick();
    void on_frame();
    bool route(MSG &);
    bool editing() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
