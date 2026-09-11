#include "transport.hpp"
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>
#include <limits>

namespace vrst::preview {
bool valid_view(const View &v) {
    if (!v.texture_handle || !v.width || !v.height || v.width > 16384 || v.height > 16384 || v.x < 0 ||
        v.y < 0 || v.rect_width <= 0 || v.rect_height <= 0 || std::uint64_t(v.x) + v.rect_width > v.width ||
        std::uint64_t(v.y) + v.rect_height > v.height)
        return false;
    switch (v.format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        break;
    default:
        return false;
    }
    float norm = 0;
    for (float q : v.orientation) {
        if (!std::isfinite(q))
            return false;
        norm += q * q;
    }
    if (!std::isfinite(norm) || norm < .25f || norm > 4.f)
        return false;
    for (float f : v.fov)
        if (!std::isfinite(f) || std::abs(f) > 10000.f)
            return false;
    return v.fov[1] - v.fov[0] > 1e-6f && v.fov[2] - v.fov[3] > 1e-6f;
}
bool valid_frame(const Frame &f) {
    if (!f.producer_pid || !f.generation || !f.sequence || !f.session || !f.view_count || f.view_count > 2 ||
        f.eye > 1)
        return false;
    for (std::uint32_t i = 0; i < f.view_count; ++i)
        if (!valid_view(f.views[i]))
            return false;
    return true;
}
bool viewer_active(const Shared &s, std::uint64_t now) {
    return s.consumer_pid && !s.paused && s.heartbeat_ms && now >= s.heartbeat_ms &&
           now - s.heartbeat_ms <= 2000;
}
bool RateLimiter::due(std::uint64_t now, unsigned requested) {
    const unsigned rate = std::clamp(requested, 1u, 240u);
    if (fps != rate) {
        fps = rate;
        next_ns = now;
    }
    return now >= next_ns;
}
void RateLimiter::published(std::uint64_t now, unsigned requested) {
    due(now, requested);
    const std::uint64_t period = 1000000000ull / fps;
    next_ns += period;
    // Preserve cadence, but discard missed periods instead of bursting to catch up.
    if (next_ns <= now)
        next_ns += ((now - next_ns) / period + 1) * period;
}
void RateLimiter::reset() {
    next_ns = 0;
    fps = 0;
}
Channel::~Channel() {
    close();
}
bool Channel::open(bool, const std::wstring &name) {
    close();
    std::wstring actual = name;
    if (name == default_channel) {
        wchar_t override_name[240]{};
        DWORD n = GetEnvironmentVariableW(L"HORIZONLOCK_PREVIEW_CHANNEL", override_name, 240);
        if (n && n < 240)
            actual = override_name;
    }
    mutex_ = CreateMutexW(nullptr, FALSE, (actual + L".mutex").c_str());
    if (!mutex_)
        return false;
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
                                  (actual + L".memory").c_str());
    if (mapping_)
        shared_ = static_cast<Shared *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared_ || !lock()) {
        close();
        return false;
    }
    if (shared_->magic == 0) {
        *shared_ = Shared{};
        shared_->magic = ipc_magic;
        shared_->version = ipc_version;
        shared_->size = sizeof(Shared);
    }
    const bool compatible =
        shared_->magic == ipc_magic && shared_->version == ipc_version && shared_->size == sizeof(Shared);
    unlock();
    if (!compatible)
        close();
    return compatible;
}
void Channel::close() {
    unlock();
    if (shared_)
        UnmapViewOfFile(shared_);
    if (mapping_)
        CloseHandle(mapping_);
    if (mutex_)
        CloseHandle(mutex_);
    shared_ = nullptr;
    mapping_ = mutex_ = nullptr;
}
bool Channel::lock() {
    if (!mutex_ || locked_)
        return false;
    DWORD result = WaitForSingleObject(mutex_, 0);
    locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return locked_;
}
void Channel::unlock() {
    if (locked_) {
        ReleaseMutex(mutex_);
        locked_ = false;
    }
}
} // namespace vrst::preview
