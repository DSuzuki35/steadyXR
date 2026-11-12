#pragma once
#include <cstdint>
#include <string>
#include <windows.h>
#include <vrst/spectator.hpp>

namespace vrst::preview {
constexpr std::uint32_t ipc_magic = 0x50564c48;
constexpr std::uint32_t ipc_version = 4;
constexpr const wchar_t *default_channel = L"Local\\SteadyXRPreview-v4";
constexpr unsigned max_overlay_layers = 16;
constexpr unsigned max_overlay_images = 8;
// Disabled by default. Selected slots are scoped to one producer PID/session.
struct OverlaySelection {
    std::uint32_t mode{}; // 0 none, 1 all supported, 2 selected slots.
    std::uint32_t mask{}, producer_pid{};
    std::uint64_t session{};
};
struct OverlayInfo {
    std::uint32_t slot{}, type{}, supported{}, width{}, height{};
    wchar_t label[128]{};
};
struct OverlayCatalog {
    std::uint32_t producer_pid{}, count{};
    std::uint64_t session{}, updated_ms{};
    OverlayInfo layers[max_overlay_layers]{};
};

// Shared data uses Channel's named mutex except gaze_requested, whose aligned
// LONG is read with Windows Interlocked operations on the runtime input path.
// No C++ atomics cross processes. GPU textures use writer key 0 / reader key 1.
struct View {
    std::uint64_t texture_handle{};
    std::uint32_t width{}, height{}, format{};
    std::int32_t x{}, y{}, rect_width{}, rect_height{};
    float orientation[4]{0, 0, 0, 1};
    float fov[4]{-1, 1, 1, -1}; // left, right, up, down tangents
};
struct OverlayImage {
    std::uint32_t slot{}, type{}, flags{}; // 1 quad, 2 cylinder, 3 projection. flags: 1 alpha, 2 unpremultiplied.
    View image{};
    // Overlay pose relative to the captured context eye at display_time.
    float orientation[4]{0, 0, 0, 1}, position[3]{};
    float width{}, height{}, radius{}, central_angle{}, aspect_ratio{};
};
struct Frame {
    std::uint64_t generation{}, sequence{}, session{}, reference_epoch{};
    std::int64_t display_time{};
    std::uint64_t published_ms{};
    std::uint32_t producer_pid{}, view_count{}, eye{};
    LUID adapter{};
    View views[2]{};
    vrst::GazeSample gaze{};
    std::uint32_t overlay_count{};
    OverlayImage overlays[max_overlay_images]{};
};
// ready: 0 = idle, 1 = pending GPU frame, 2 = consumer requests fresh textures.
struct Shared {
    std::uint32_t magic{}, version{}, size{}, ready{};
    std::uint32_t consumer_pid{}, paused{1}, eye{}, fps{60};
    LONG gaze_requested{}; // Atomic permission bit; writes still require consumer mutex ownership.
    std::uint32_t gaze_capability{};
    std::uint64_t heartbeat_ms{};
    std::uint32_t producer_pid{}, status_code{};
    std::uint32_t render_enabled{};
    float render_left[2]{}, render_right[2]{}; // Active instance snapshot, in percent.
    std::uint64_t producer_heartbeat_ms{}, published_frames{}, dropped_frames{};
    wchar_t application[128]{};
    wchar_t status[192]{};
    Frame frame{};
    OverlaySelection overlay_selection{};
    OverlayCatalog overlay_catalog{};
};

bool valid_view(const View &view);
bool valid_frame(const Frame &frame);
bool valid_overlay(const OverlayImage &overlay);
bool overlay_selected(const OverlaySelection &, std::uint32_t producer_pid, std::uint64_t session,
                      unsigned slot);
bool viewer_active(const Shared &shared, std::uint64_t now_ms);

// A phase-preserving capture schedule, independent of game frame cadence.
struct RateLimiter {
    std::uint64_t next_ns{}, early_ns{};
    unsigned fps{};
    bool due(std::uint64_t now_ns, unsigned requested_fps);
    void published(std::uint64_t now_ns, unsigned requested_fps);
    void reset();
};

class Channel {
  public:
    Channel() = default;
    ~Channel();
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    bool open(bool viewer, const std::wstring &name = default_channel);
    void close();
    // One active renderer per channel; separate OBS sources cannot steal frames.
    bool heartbeat(unsigned eye, unsigned fps);
    bool request_gaze(bool); // Only the current owning consumer can request runtime work.
    bool gaze_requested() const; // Atomic read for the runtime's input cycle; no mutex/syscall.
    bool select_overlays(const OverlaySelection &); // Only the active owning consumer can write.
    OverlayCatalog overlay_catalog(); // Read-only snapshot; empty if unavailable/busy.
    void release_consumer();
    bool owns_consumer() const;
    // Validate ownership under the same mutex as frame consumption when requested.
    bool lock(bool require_consumer = false); // zero timeout; WAIT_ABANDONED treated as acquired
    void unlock();
    Shared *data() const {
        return shared_;
    }

  private:
    HANDLE mapping_{}, mutex_{};
    Shared *shared_{};
    bool locked_{};
    std::wstring name_;
};
} // namespace vrst::preview
