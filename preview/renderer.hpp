#pragma once
#include "director.hpp"
#include "transport.hpp"
#include "visual_effects.hpp"
#include <memory>
#include <vector>
#include <vrst/core.hpp>
struct ID3D11Device;
struct ID3D11Texture2D;

namespace vrst::preview {
struct Options {
    SpectatorSettings spectator{};
    VisualSettings visual{};
    DirectorSettings director{};
    bool compare_effects{}; // Persisted comparison scope; compare_original activates it transiently.
    OverlaySelection overlays{};
    double smoothing{40}, crop{10};
    bool mirror{true}, stabilize{true};
    bool compare_original{}; // Transient A/B bypass; keep framing and continue motion history.
    unsigned compare_mode{}; // 0 original, 1 same-framing split, 2 fitted side-by-side; inactive normally.
    double crop_left{}, crop_right{}, crop_top{}, crop_bottom{};
    double aspect{16. / 9.};
    unsigned output_width{3840}, output_height{2160};
    double horizon_lock{}, pitch_lock{}, mirror_limit{10}; // Pitch 0..100; off by default.
    bool adaptive_horizon{true};
    double mirror_softness{60};
    double mirror_stretch{0}; // Mirror-to-edge extension, percent; zero preserves reflection.
    EdgeReserve edge_reserve[2]{};
    unsigned eye{};           // Window capture profile; OpenXR uses the eye stored with its frame.
    double horizontal_offset{}; // -100 leftmost, +100 rightmost within full captured bounds.
    double vertical_offset{}; // -100 highest, +100 lowest within full captured bounds.
};
void apply_stream_comfort(Options &);
struct FramingRegion {
    double left{}, right{1}, top{}, bottom{1};
};
FramingRegion framing_region(double source_aspect, const Options &, EdgeReserve reserve = {});
struct Composition {
    bool valid{};
    Warp warp{};
    Quat focus_correction{};
};
class FrameStabilizer {
  public:
    Composition update(const Frame &, const Options &, const Quat *lookahead_target = nullptr,
                       double speed_kph = -1);
    void reset();
    void reset_horizon(); // Re-select upright hemisphere and pitch anchor; retain smoothing history.
    void learn_chat() {
        spectator_.begin_chat_calibration(0);
    }
    void cancel_chat() {
        spectator_.cancel_chat_calibration();
    }
    void clear_chat() {
        spectator_.clear_chat_zone();
    }
    SpectatorStatus spectator_status() const {
        return spectator_.status();
    }
    bool calibration_active() const {
        return spectator_.calibration_active();
    }
    DirectorStatus director_status() const {
        return director_.status();
    }
    void reset_director() {
        director_.reset();
    }

  private:
    PoseHistory history_;
    Frame previous_{};
    double horizon_up_sign_{};
    double pitch_anchor_{};
    bool pitch_anchor_valid_{};
    SpectatorController spectator_;
    bool spectator_active_{};
    DirectorController director_;
};

class Renderer {
  public:
    Renderer();
    ~Renderer();
    bool attach(HWND window);
    bool attach_offscreen(const LUID *adapter = nullptr);
    // Offscreen output: acquire key 1 before reading, release key 0 afterward.
    // Handles remain valid only for their output generation. Opened resources
    // may be retained by the reader while a resize replaces the writer output.
    std::uint64_t output_handle() const;
    std::uint64_t output_generation() const;
    bool consume(Channel &channel, bool require_consumer = false);
    ID3D11Device *device() const;
    bool consume_window(ID3D11Texture2D *texture, std::uint64_t time_ns, const Options &);
    void source_changed();
    bool demo(double seconds, bool quad = true);
    bool draw(const Options &, bool present = true, const wchar_t *waiting_text = nullptr);
    void reset();
    void reset_horizon();
    void learn_chat();
    void cancel_chat();
    void clear_chat();
    SpectatorStatus spectator_status() const;
    DirectorStatus director_status() const;
    bool calibration_active() const;
    unsigned gaze_capability() const;
    const VisualDiagnostics &visual_diagnostics() const;
    std::wstring error() const;
    bool has_frame() const;
    std::uint64_t sequence() const;
    unsigned views() const;
    // Diagnostic readback is used only by tests, never the live preview path.
    // For offscreen output this consumes key 1 and returns key 0, like a reader.
    bool readback(std::vector<std::uint32_t> &, unsigned &, unsigned &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
