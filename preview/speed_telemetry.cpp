#include "speed_telemetry.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <cmath>
#include <algorithm>

namespace vrst::preview {
namespace speed_detail {
struct Bytes {
    const unsigned char *data{};
    std::size_t size{};
    bool copy(std::size_t offset, void *out, std::size_t length) const {
        if (!data || offset > size || length > size - offset) return false;
        std::memcpy(out, data + offset, length);
        return true;
    }
};
struct Snapshot {
    bool valid{};
    double kph{}, time{};
    std::uint64_t session{};
    std::uint32_t tick{};
};
constexpr std::uint64_t poll_interval_ms = 50;
constexpr std::uint64_t reconnect_interval_ms = 1000;
constexpr std::uint64_t stale_ms = 500;
constexpr std::size_t max_mapping_bytes = 16 * 1024 * 1024;

bool contains(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
}
template <class T, class View> bool field(const View &view, std::size_t offset, T &out) {
    return view.copy(offset, &out, sizeof(out));
}
bool valid_speed(double metres_per_second) {
    return std::isfinite(metres_per_second) && std::abs(metres_per_second) <= 500.0;
}

// Independent byte reader for the published IRSDK v1/v2 ABI. No vendor code is
// included; layout/protocol references and LMU SDK provenance are in
// docs/telemetry.md. The SDK publishes fixed metadata and up to four data rows.
template <class View> Snapshot read_iracing(const View &view) {
    std::array<std::int32_t, 28> before{}, after{};
    if (!view.copy(0, before.data(), sizeof(before))) return {};
    if ((before[0] != 1 && before[0] != 2) || !(before[1] & 1) ||
        before[2] <= 0 || before[2] > 1000 || before[3] < 0 ||
        before[4] <= 0 || before[5] < 112 || before[6] <= 0 || before[6] > 4096 ||
        before[7] < 112 || before[8] < 1 || before[8] > 4 ||
        before[9] < 1 || before[9] > 1024 * 1024) return {};
    if (!contains(view.size, static_cast<std::size_t>(before[5]), static_cast<std::size_t>(before[4])) ||
        !contains(view.size, static_cast<std::size_t>(before[7]), static_cast<std::size_t>(before[6]) * 144)) return {};

    int newest = 0;
    for (int i = 0; i < before[8]; ++i) {
        const auto offset = before[13 + i * 4];
        if (offset < 112 || !contains(view.size, static_cast<std::size_t>(offset),
                                     static_cast<std::size_t>(before[9]))) return {};
        const auto candidate = static_cast<std::uint32_t>(before[12 + i * 4]);
        const auto current = static_cast<std::uint32_t>(before[12 + newest * 4]);
        if (static_cast<std::int32_t>(candidate - current) > 0) newest = i;
    }

    constexpr const char *names[]{"Speed", "IsOnTrack", "SessionState", "SessionNum", "SessionTime"};
    constexpr int widths[]{1, 1, 4, 4, 4, 8};
    std::array<int, 5> offsets{}, types{}, entries{};
    entries.fill(-1);
    std::array<std::array<unsigned char, 144>, 5> descriptors{};
    for (int i = 0; i < before[6]; ++i) {
        std::array<unsigned char, 144> descriptor{};
        if (!view.copy(static_cast<std::size_t>(before[7]) + static_cast<std::size_t>(i) * 144,
                       descriptor.data(), descriptor.size())) return {};
        const Bytes d{descriptor.data(), descriptor.size()};
        std::int32_t type{}, offset{}, count{};
        field(d, 0, type); field(d, 4, offset); field(d, 8, count);
        if (type < 0 || type > 5 || offset < 0 || count < 1 ||
            !contains(static_cast<std::size_t>(before[9]), static_cast<std::size_t>(offset),
                      static_cast<std::size_t>(count) * widths[type]) ||
            !std::memchr(descriptor.data() + 16, 0, 32)) return {};
        for (std::size_t j = 0; j < 5; ++j) {
            if (std::strcmp(reinterpret_cast<const char *>(descriptor.data() + 16), names[j]) != 0) continue;
            if (entries[j] >= 0 || count != 1) return {};
            entries[j] = i; offsets[j] = offset; types[j] = type; descriptors[j] = descriptor;
        }
    }
    for (int entry : entries) if (entry < 0) return {};
    if ((types[0] != 4 && types[0] != 5) || types[1] != 1 ||
        types[2] != 2 || types[3] != 2 || types[4] != 5) return {};
    const auto row = static_cast<std::size_t>(before[13 + newest * 4]);
    double speed{}, time{};
    unsigned char on_track{};
    std::int32_t state{}, session{};
    if (types[0] == 4) {
        float value{};
        if (!field(view, row + offsets[0], value)) return {};
        speed = value;
    } else if (!field(view, row + offsets[0], speed)) return {};
    if (!field(view, row + offsets[1], on_track) || !field(view, row + offsets[2], state) ||
        !field(view, row + offsets[3], session) || !field(view, row + offsets[4], time)) return {};

    // Read the selected row's commit tick after its fields; a producer can reuse
    // the row while we copy it. Recheck metadata too, including selected variables.
    MemoryBarrier();
    for (std::size_t j = 0; j < 5; ++j) {
        std::array<unsigned char, 144> descriptor{};
        if (!view.copy(static_cast<std::size_t>(before[7]) + static_cast<std::size_t>(entries[j]) * 144,
                       descriptor.data(), descriptor.size()) || descriptor != descriptors[j]) return {};
    }
    if (!view.copy(0, after.data(), sizeof(after)) ||
        std::memcmp(before.data(), after.data(), 40) != 0 ||
        before[12 + newest * 4] != after[12 + newest * 4] ||
        before[13 + newest * 4] != after[13 + newest * 4]) return {};
    if (on_track != 1 || state < 1 || state > 6 || session < 0 || !valid_speed(speed) ||
        !std::isfinite(time) || time < 0) return {};
    return {true, std::abs(speed) * 3.6, time, static_cast<std::uint64_t>(session),
            static_cast<std::uint32_t>(before[12 + newest * 4])};
}

// Measured with sizeof/offsetof against the installed 2026 Studio 397 SDK,
// Windows x64 (InternalsPlugin structs pack(4), outer structs default packing).
// This native interface has no self-describing versioned offsets. Read it only
// while owning its documented lock; player indices never become raw pointers.
constexpr std::size_t lmu_layout_bytes = 324824;
constexpr std::size_t lmu_scoring = 1632;
constexpr std::size_t lmu_telemetry = 128464;
constexpr std::size_t lmu_vehicle_stride = 1888;
Snapshot read_lmu(const Bytes &view) {
    if (view.size < lmu_layout_bytes) return {};
    unsigned char location{}, active{}, player{}, has_player{}, phase{}, realtime{};
    std::int32_t session{}, vehicles{};
    if (!field(view, 96, location) || !field(view, lmu_telemetry, active) ||
        !field(view, lmu_telemetry + 1, player) || !field(view, lmu_telemetry + 2, has_player) ||
        !field(view, lmu_scoring + 64, session) || !field(view, lmu_scoring + 104, vehicles) ||
        !field(view, lmu_scoring + 108, phase) || !field(view, lmu_scoring + 115, realtime)) return {};
    if (location != 3 || active < 1 || active > 104 || player >= active || has_player != 1 ||
        vehicles < 1 || vehicles > 104 || session < 0 || session > 13 ||
        phase < 1 || phase > 8 || realtime != 1) return {};
    const auto entry = lmu_telemetry + 4 + player * lmu_vehicle_stride;
    if (!contains(view.size, entry, lmu_vehicle_stride)) return {};
    std::int32_t id{};
    double time{}, velocity[3]{};
    if (!field(view, entry, id) || !field(view, entry + 12, time) ||
        !view.copy(entry + 184, velocity, sizeof(velocity))) return {};
    if (id < 0 || !std::isfinite(time) || time < 0) return {};
    for (double value : velocity) if (!valid_speed(value)) return {};
    const double speed = std::sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
    if (!valid_speed(speed)) return {};
    const auto identity = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id)) << 16) |
                          (static_cast<std::uint64_t>(session) << 8) | player;
    return {true, speed * 3.6, time, identity, 0};
}
struct MappingNames {
    const wchar_t *iracing{L"Local\\IRSDKMemMapFileName"};
    const wchar_t *lmu{L"LMU_Data"};
    const wchar_t *lock{L"LMU_SharedMemoryLockData"};
    const wchar_t *event{L"LMU_SharedMemoryLockEvent"};
};
class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    ~Mapping() { close(); }
    bool open(const wchar_t *name, DWORD access, std::size_t minimum) {
        close();
        handle_ = OpenFileMappingW(access, FALSE, name);
        if (handle_) base_ = MapViewOfFile(handle_, access, 0, 0, 0);
        MEMORY_BASIC_INFORMATION info{};
        if (!base_ || !VirtualQuery(base_, &info, sizeof(info)) ||
            info.State != MEM_COMMIT || info.Type != MEM_MAPPED ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.RegionSize < minimum || info.RegionSize > max_mapping_bytes) {
            close(); return false;
        }
        size_ = info.RegionSize;
        return true;
    }
    void close() {
        if (base_) UnmapViewOfFile(base_);
        if (handle_) CloseHandle(handle_);
        base_ = nullptr; handle_ = nullptr; size_ = 0;
    }
    Bytes bytes() const { return {static_cast<const unsigned char *>(base_), size_}; }
    void *base() const { return base_; }
private:
    HANDLE handle_{};
    void *base_{};
    std::size_t size_{};
};

// One nonblocking CAS attempt uses the SDK's existing lock. We never create or
// reset its coordination objects, increment waiters, wait, or spin. Data remains
// FILE_MAP_READ; only the dedicated busy synchronization word is written.
class LmuLock {
public:
    LmuLock(void *base, HANDLE wake_event) : words_(static_cast<volatile LONG *>(base)), event_(wake_event) {
        owned_ = words_ && event_ && InterlockedCompareExchange(words_ + 1, 1, 0) == 0;
    }
    ~LmuLock() {
        if (!owned_) return;
        InterlockedExchange(words_ + 1, 0);
        if (words_[0] > 0) SetEvent(event_);
    }
    bool owned() const { return owned_; }
    LmuLock(const LmuLock &) = delete;
    LmuLock &operator=(const LmuLock &) = delete;
private:
    volatile LONG *words_{};
    HANDLE event_{};
    bool owned_{};
};

class Source {
public:
    Source(SpeedGame game, MappingNames names) : game_(game), names_(names) {}
    ~Source() { close(); }
    void close() {
        data_.close(); lock_.close();
        if (event_) CloseHandle(event_);
        event_ = nullptr;
        observed_ = confirmed_ = false;
        last_ = {};
    }
    void reset() { close(); next_attempt_ = 0; }
    std::uint64_t fresh_at() const { return last_progress_; }
    SpeedReading sample(std::uint64_t now) {
        // A gap between capture calls must not revive an expired connection by
        // treating its next tick as progress from a previously valid session.
        if (data_.base() && now - last_progress_ >= stale_ms) close();
        if (!data_.base()) {
            if (now < next_attempt_) return {};
            next_attempt_ = now + reconnect_interval_ms;
            bool opened = false;
            if (game_ == SpeedGame::iracing) {
                opened = data_.open(names_.iracing, FILE_MAP_READ, 112);
            } else {
                opened = data_.open(names_.lmu, FILE_MAP_READ, lmu_layout_bytes) &&
                         lock_.open(names_.lock, FILE_MAP_READ | FILE_MAP_WRITE, 8);
                if (opened) event_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, names_.event);
                opened = opened && event_;
            }
            if (!opened) { close(); return {}; }
            last_progress_ = now;
        }
        Snapshot snapshot{};
        if (game_ == SpeedGame::iracing) {
            snapshot = read_iracing(data_.bytes());
        } else {
            // The guard ends before math/freshness processing or any other work.
            LmuLock guard(lock_.base(), event_);
            if (guard.owned()) snapshot = read_lmu(data_.bytes());
        }
        if (!snapshot.valid) {
            observed_ = confirmed_ = false;
            if (now - last_progress_ >= stale_ms) close();
            return {};
        }
        if (!observed_ || snapshot.session != last_.session || snapshot.time < last_.time) {
            observed_ = true;
            confirmed_ = false;
            // A new observation does not make an already frozen mapping fresh.
            // A real session change does begin a new 500 ms progress window.
            if (snapshot.session != last_.session || snapshot.time < last_.time) last_progress_ = now;
        } else if (snapshot.time > last_.time &&
                   (game_ == SpeedGame::lmu || snapshot.tick != last_.tick)) {
            confirmed_ = true;
            last_progress_ = now;
        }
        last_ = snapshot;
        if (now - last_progress_ >= stale_ms) { close(); return {}; }
        return confirmed_ ? SpeedReading{true, snapshot.kph, game_} : SpeedReading{};
    }
private:
    SpeedGame game_;
    MappingNames names_;
    Mapping data_, lock_;
    HANDLE event_{};
    std::uint64_t next_attempt_{}, last_progress_{};
    bool observed_{}, confirmed_{};
    Snapshot last_{};
};

class Poller {
public:
    explicit Poller(MappingNames names = {})
        : iracing_(SpeedGame::iracing, names), lmu_(SpeedGame::lmu, names) {}
    SpeedReading sample(SpeedGame selection, std::uint64_t now) {
        if (selection != SpeedGame::automatic && selection != SpeedGame::iracing && selection != SpeedGame::lmu)
            selection = SpeedGame::automatic;
        if (selection != selection_) {
            iracing_.close(); lmu_.close(); cached_ = {}; selection_ = selection;
        }
        if (cached_.valid && now - fresh_at_ >= stale_ms) cached_ = {};
        if (now < next_poll_) return cached_;
        next_poll_ = now + poll_interval_ms;
        cached_ = {};
        if (selection != SpeedGame::lmu) {
            cached_ = iracing_.sample(now);
            if (cached_.valid) fresh_at_ = iracing_.fresh_at();
        }
        if (selection != SpeedGame::iracing) {
            // Poll both in automatic mode so a disconnected game can fall back
            // immediately to an already-confirmed, advancing second source.
            auto lmu = lmu_.sample(now);
            if (!cached_.valid && lmu.valid) { cached_ = lmu; fresh_at_ = lmu_.fresh_at(); }
        }
        return cached_;
    }
    void reset() {
        iracing_.reset(); lmu_.reset(); cached_ = {};
        next_poll_ = fresh_at_ = 0;
        selection_ = SpeedGame::automatic;
    }
private:
    Source iracing_, lmu_;
    SpeedGame selection_{SpeedGame::automatic};
    SpeedReading cached_{};
    std::uint64_t next_poll_{}, fresh_at_{};
};
} // namespace speed_detail
struct SpeedTelemetry::Impl { speed_detail::Poller poller; };
SpeedTelemetry::SpeedTelemetry() : impl_(std::make_unique<Impl>()) {}
SpeedTelemetry::~SpeedTelemetry() = default;
SpeedReading SpeedTelemetry::sample(SpeedGame selection) {
    return impl_->poller.sample(selection, GetTickCount64());
}
void SpeedTelemetry::reset() { impl_->poller.reset(); }
} // namespace vrst::preview
