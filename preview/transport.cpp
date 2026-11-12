#include "transport.hpp"
#include <algorithm>
#include <cmath>
#include <dxgiformat.h>
#include <limits>
#include <map>
#include <mutex>

namespace vrst::preview {
bool valid_overlay(const OverlayImage &o) {
    if (o.slot >= max_overlay_layers || o.type < 1 || o.type > 3 || (o.flags & ~3u) || !valid_view(o.image))
        return false;
    double norm = 0;
    for (float q : o.orientation) {
        if (!std::isfinite(q))
            return false;
        norm += double(q) * q;
    }
    if (norm < .25 || norm > 4)
        return false;
    for (float p : o.position)
        if (!std::isfinite(p))
            return false;
    const auto positive = [](float v) { return std::isfinite(v) && v > 0; };
    if (o.type == 1)
        return positive(o.width) && positive(o.height);
    if (o.type == 2)
        return !std::isnan(o.radius) && o.radius >= 0 && positive(o.central_angle) &&
               o.central_angle <= float(6.283185307179586) && positive(o.aspect_ratio);
    return true;
}
bool overlay_selected(const OverlaySelection &s, std::uint32_t pid, std::uint64_t session, unsigned slot) {
    if (slot >= max_overlay_layers)
        return false;
    return s.mode == 1 || (s.mode == 2 && pid && session && s.producer_pid == pid &&
                           s.session == session && (s.mask & (1u << slot)));
}
bool Channel::select_overlays(const OverlaySelection &selection) {
    if (!lock(true))
        return false;
    auto next = selection;
    if (next.mode > 2)
        next = {};
    next.mask &= (1u << max_overlay_layers) - 1;
    shared_->overlay_selection = next;
    unlock();
    return true;
}
OverlayCatalog Channel::overlay_catalog() {
    if (!lock())
        return {};
    auto result = shared_->overlay_catalog;
    unlock();
    if (result.count > max_overlay_layers)
        return {};
    std::uint32_t slots = 0;
    for (unsigned i = 0; i < result.count; ++i) {
        auto &info = result.layers[i];
        if (info.slot >= max_overlay_layers || (slots & (1u << info.slot)))
            return {};
        slots |= 1u << info.slot;
        info.label[std::size(info.label) - 1] = 0;
    }
    return result;
}
namespace {
std::mutex consumers_mutex;
std::map<std::wstring, const Channel *> consumers;
} // namespace
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
    if (f.overlay_count > max_overlay_images)
        return false;
    std::uint32_t slots = 0;
    for (unsigned i = 0; i < f.overlay_count; ++i) {
        const auto &overlay = f.overlays[i];
        if (!valid_overlay(overlay) || (slots & (1u << overlay.slot)))
            return false;
        for (unsigned j = 0; j < f.view_count; ++j)
            if (overlay.image.texture_handle == f.views[j].texture_handle)
                return false;
        for (unsigned j = 0; j < i; ++j)
            if (overlay.image.texture_handle == f.overlays[j].image.texture_handle)
                return false;
        slots |= 1u << overlay.slot;
    }
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
        // CPU submissions straddle the ideal headset tick. Admit a slightly early
        // snapshot while continuing to advance the original output cadence.
        early_ns = std::min<std::uint64_t>(4000000, 1000000000ull / fps / 3);
    }
    return now >= next_ns || next_ns - now <= early_ns;
}
void RateLimiter::published(std::uint64_t now, unsigned requested) {
    due(now, requested);
    const std::uint64_t period = 1000000000ull / fps;
    next_ns += period;
    // After a stall, discard missed periods. Leave a full new period if the
    // following slot is already eligible, so a repeated poll cannot publish twice.
    if (next_ns <= now || next_ns - now <= early_ns)
        next_ns = now + period;
}
void RateLimiter::reset() {
    next_ns = 0;
    fps = 0;
}
Channel::~Channel() {
    close();
}
bool Channel::request_gaze(bool enabled) {
    if (!lock(true)) return false;
    InterlockedExchange(&shared_->gaze_requested, enabled ? 1L : 0L);
    unlock();
    return true;
}
bool Channel::gaze_requested() const {
    return shared_ && InterlockedCompareExchange(&shared_->gaze_requested, 0, 0) != 0;
}
bool Channel::heartbeat(unsigned eye, unsigned fps) {
    if (!lock())
        return false;
    bool claimed = false;
    {
        std::lock_guard<std::mutex> guard(consumers_mutex);
        const auto owner = consumers.find(name_);
        const bool local_busy = owner != consumers.end() && owner->second != this;
        const auto now = GetTickCount64();
        const bool foreign_busy =
            shared_->consumer_pid != GetCurrentProcessId() && viewer_active(*shared_, now);
        if (!local_busy && !foreign_busy) {
            if (owner == consumers.end() || shared_->consumer_pid != GetCurrentProcessId() || shared_->paused) {
                shared_->ready = 2;
                shared_->overlay_selection = {};
                InterlockedExchange(&shared_->gaze_requested, 0);
            }
            consumers[name_] = this;
            shared_->consumer_pid = GetCurrentProcessId();
            shared_->heartbeat_ms = now;
            shared_->paused = 0;
            shared_->eye = std::min(eye, 1u);
            shared_->fps = std::clamp(fps, 1u, 240u);
            claimed = true;
        } else if (owner != consumers.end() && owner->second == this) {
            consumers.erase(owner);
        }
    }
    unlock();
    return claimed;
}
void Channel::release_consumer() {
    // Always forget the process-local owner even if the shared mutex is busy;
    // its last heartbeat then expires, and no dangling Channel pointer survives.
    const bool acquired = lock();
    {
        std::lock_guard<std::mutex> guard(consumers_mutex);
        const auto owner = consumers.find(name_);
        if (owner != consumers.end() && owner->second == this) {
            if (acquired && shared_->consumer_pid == GetCurrentProcessId()) {
                shared_->paused = 1;
                shared_->consumer_pid = 0;
                shared_->heartbeat_ms = 0;
                shared_->ready = 2;
                shared_->overlay_selection = {};
                InterlockedExchange(&shared_->gaze_requested, 0);
            }
            consumers.erase(owner);
        }
    }
    if (acquired)
        unlock();
}
bool Channel::owns_consumer() const {
    std::lock_guard<std::mutex> guard(consumers_mutex);
    const auto owner = consumers.find(name_);
    return owner != consumers.end() && owner->second == this;
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
    name_ = actual;
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
    release_consumer();
    if (shared_)
        UnmapViewOfFile(shared_);
    if (mapping_)
        CloseHandle(mapping_);
    if (mutex_)
        CloseHandle(mutex_);
    shared_ = nullptr;
    mapping_ = mutex_ = nullptr;
    name_.clear();
}
bool Channel::lock(bool require_consumer) {
    if (!mutex_ || locked_)
        return false;
    DWORD result = WaitForSingleObject(mutex_, 0);
    locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    if (locked_ && require_consumer &&
        (!shared_ || shared_->consumer_pid != GetCurrentProcessId() || shared_->paused || !owns_consumer()))
        unlock();
    return locked_;
}
void Channel::unlock() {
    if (locked_) {
        ReleaseMutex(mutex_);
        locked_ = false;
    }
}
} // namespace vrst::preview
