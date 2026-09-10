#pragma once
#include "transport.hpp"
#include <memory>
#include <vector>
#include <vrst/core.hpp>
struct ID3D11Device;
struct ID3D11Texture2D;

namespace vrst::preview {
struct Options {
    double smoothing{40}, crop{10};
    bool mirror{true}, stabilize{true};
};
struct Composition {
    bool valid{};
    Warp warp{};
    Quat focus_correction{};
};
class FrameStabilizer {
  public:
    Composition update(const Frame &, const Options &);
    void reset();

  private:
    PoseHistory history_;
    Frame previous_{};
};

class Renderer {
  public:
    Renderer();
    ~Renderer();
    bool attach(HWND window);
    bool consume(Channel &channel);
    ID3D11Device *device() const;
    bool consume_window(ID3D11Texture2D *texture, std::uint64_t time_ns, const Options &);
    void source_changed();
    bool demo(double seconds, bool quad = true);
    bool draw(const Options &, bool present = true);
    void reset();
    std::wstring error() const;
    bool has_frame() const;
    std::uint64_t sequence() const;
    unsigned views() const;
    // Diagnostic readback is used only by tests, never the live preview path.
    bool readback(std::vector<std::uint32_t> &, unsigned &, unsigned &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
