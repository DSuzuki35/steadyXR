#pragma once
#include "visual_settings.hpp"
#include <cstdint>
#include <memory>
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace vrst::preview {
enum class VisualOutcome { bypass, processed, unsupported_input, resource_failure };
struct VisualDiagnostics {
    VisualOutcome outcome = VisualOutcome::bypass;
    VisualTransfer output_transfer = VisualTransfer::srgb;
    std::uint64_t dispatches = 0, allocations = 0, temporal_resets = 0;
    unsigned frame_dispatches = 0, metering_samples = 0;
    std::uint64_t resource_bytes = 0;
    double gpu_ms = 0;
    bool gpu_timing_valid = false;
};
// Immediate D3D11 context, FL11+. Input must belong to device and must not be
// bound as any graphics/compute output. The previous returned effects texture
// must also be unbound from ALL shader stages before process (it becomes a UAV).
// Preserves all touched CS bindings and
// leaves graphics state untouched. Host serializes calls with its render thread.
// Disabled/bypass: exact input pointer, no allocations, dispatches or queries.
// Enabled: borrowed RGBA16F linear SDR (clamped [0,1]), valid until next process/
// reset/destruction. Sample through a LINEAR view and encode exactly once at the
// host's SDR output. Alpha convention matches settings; premultiplied input is
// unpremultiplied BEFORE transfer decoding, then premultiplied in linear output.
// Supports 1-mip non-MSAA RGBA/BGRA8 UNORM/SRGB/TYPELESS and linear RGBA16F.
// No HDR, YUV, limited-range conversion or implicit HDR assumptions: unsupported
// inputs return original and report unsupported_input. No CPU pixel readback.
class VisualEffects {
  public:
    VisualEffects();
    ~VisualEffects();
    VisualEffects(const VisualEffects &) = delete;
    VisualEffects &operator=(const VisualEffects &) = delete;
    ID3D11Texture2D *process(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *, const VisualSettings &,
                             std::uint64_t frame_id, double timestamp_seconds);
    void reset();
    void set_timing_enabled(bool);
    const VisualDiagnostics &diagnostics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
