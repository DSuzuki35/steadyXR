#pragma once
#include "transport.hpp"
#include <array>
#include <d3d11.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace vrst::preview {
// Clips contain the entire submitted source rectangle, including a focus view
// when present. The proxy changes resolution only; it never changes the FOV.
struct CaptureOptions {
    unsigned duration_seconds{30}, fps{30}, longest_edge{2048};
};
enum class CaptureState { idle, preparing, recording, finishing, ready, failed, cancelled };
struct CaptureStatus {
    CaptureState state{CaptureState::idle};
    unsigned recorded_frames{}, dropped_frames{};
    double elapsed_seconds{};
    std::filesystem::path path;
    std::wstring message;
};
struct ClipFrame {
    Frame frame{}; // Original source dimensions/subrect; every texture handle is zero.
    double speed_kph{-1};
    std::int64_t media_time{}; // 100 ns, relative to first accepted source frame.
};
struct ClipInfo {
    CaptureOptions options{};
    unsigned width[2]{}, height[2]{}, dropped_frames{};
    bool partial{};
    std::wstring stop_reason;
    std::uint64_t video_bytes[2]{};
    std::array<unsigned char, 32> video_hash[2]{};
    std::vector<ClipFrame> frames;
};
// Public pure validation helpers also exercise the on-disk trust boundary.
bool validate_clip_metadata(const ClipInfo &, std::wstring &error);
bool compatible_clip_source(const Frame &initial, const Frame &next, std::wstring &reason);
bool write_clip_metadata(const std::filesystem::path &, const ClipInfo &, std::wstring &error);
bool read_clip_metadata(const std::filesystem::path &, ClipInfo &, std::wstring &error);

class ClipRecorder {
  public:
    ClipRecorder();
    ~ClipRecorder();
    ClipRecorder(const ClipRecorder &) = delete;
    ClipRecorder &operator=(const ClipRecorder &) = delete;
    // start/submit/stop/cancel are called on the rendering thread. Start prepares
    // bounded GPU surfaces; hardware encoder/file initialization runs separately.
    bool start(ID3D11Device *, const std::filesystem::path &, const Frame &source,
               const CaptureOptions & = {});
    // Call only after shared-reader access has ended. Each consumer-owned texture
    // must contain the full source subrect, with no crop/stabilization/effects.
    // Textures need D3D11_BIND_RENDER_TARGET (plus any shader-resource binding)
    // so D3D11 video processing can consume them without an extra source copy.
    // No CPU pixel readback, file I/O or waits occur here. Busy slots drop samples.
    bool submit(const Frame &, ID3D11Texture2D *const views[2], double speed_kph = -1);
    void stop(const std::wstring &reason = L"Recording stopped");
    void cancel();
    // Poll on the rendering thread. Terminal polls nonblockingly reap capture
    // resources once the worker has exited and sample callbacks have released
    // their surfaces; the last status/path/counters remain available afterward.
    CaptureStatus status();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
struct DecodedFrame {
    Frame frame{}; // Matched metadata, normalized to decoded proxy dimensions.
    double speed_kph{-1};
    std::size_t index{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[2];
};
class ClipReader {
  public:
    ClipReader();
    ~ClipReader();
    ClipReader(const ClipReader &) = delete;
    ClipReader &operator=(const ClipReader &) = delete;
    bool open(ID3D11Device *, const std::filesystem::path &);
    void close();
    // Exact index, decoded from the beginning for deterministic association.
    // Stateful render/effect warmup: seek(0), reset renderer, next()/draw without
    // presenting through the desired index. Metadata-only warmup cannot warm FX.
    bool seek(std::size_t index);
    bool next(DecodedFrame &);
    std::size_t count() const;
    const ClipFrame *metadata(std::size_t index) const;
    const ClipInfo &info() const;
    std::wstring error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vrst::preview
