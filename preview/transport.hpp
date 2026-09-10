#pragma once
#include <cstdint>
#include <string>
#include <windows.h>

namespace vrst::preview {
constexpr std::uint32_t ipc_magic = 0x50564c48;
constexpr std::uint32_t ipc_version = 1;
constexpr const wchar_t *default_channel = L"Local\\HorizonLockVRPreview-v1";

// All shared data is accessed under Channel's named mutex. No C++ atomics cross
// processes. GPU textures use key 0 for the writer and key 1 for the reader.
struct View {
    std::uint64_t texture_handle{};
    std::uint32_t width{}, height{}, format{};
    std::int32_t x{}, y{}, rect_width{}, rect_height{};
    float orientation[4]{0, 0, 0, 1};
    float fov[4]{-1, 1, 1, -1}; // left, right, up, down tangents
};
struct Frame {
    std::uint64_t generation{}, sequence{}, session{}, reference_epoch{};
    std::int64_t display_time{};
    std::uint64_t published_ms{};
    std::uint32_t producer_pid{}, view_count{};
    LUID adapter{};
    View views[2]{};
};
// ready: 0 = idle, 1 = pending GPU frame, 2 = consumer requests fresh textures.
struct Shared {
    std::uint32_t magic{}, version{}, size{}, ready{};
    std::uint32_t consumer_pid{}, paused{1}, eye{}, fps{60};
    std::uint64_t heartbeat_ms{};
    std::uint32_t producer_pid{}, status_code{};
    std::uint64_t producer_heartbeat_ms{}, published_frames{}, dropped_frames{};
    wchar_t application[128]{};
    wchar_t status[192]{};
    Frame frame{};
};

bool valid_view(const View &view);
bool valid_frame(const Frame &frame);
bool viewer_active(const Shared &shared, std::uint64_t now_ms);

// A phase-preserving capture schedule, independent of game frame cadence.
struct RateLimiter {
    std::uint64_t next_ns{};
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
    bool lock(); // zero timeout; WAIT_ABANDONED treated as acquired
    void unlock();
    Shared *data() const {
        return shared_;
    }

  private:
    HANDLE mapping_{}, mutex_{};
    Shared *shared_{};
    bool locked_{};
};
} // namespace vrst::preview
