// Include the reader implementation to exercise its internal parser and clocked
// polling boundary without exposing test controls in the public renderer API.
#include "speed_telemetry.cpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace vrst::preview;
using namespace vrst::preview::speed_detail;

namespace {
int failures = 0;
void check(bool ok, const char *name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    failures += !ok;
}
bool approximately(double a, double b) { return std::abs(a - b) < 0.00001; }

struct Fixture {
    std::vector<unsigned char> bytes;
    explicit Fixture(std::size_t n) : bytes(n) {}
    template <class T> void put(std::size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }
    Bytes view() const { return {bytes.data(), bytes.size()}; }
};

// Independent byte fixtures follow the published ABI, with deliberately shuffled
// iRacing descriptors/data offsets to catch fixed-position Speed readers.
Fixture iracing() {
    Fixture f(4096);
    f.put<int>(0, 2); f.put<int>(4, 1); f.put<int>(8, 60);
    f.put<int>(12, 1); f.put<int>(16, 32); f.put<int>(20, 128);
    f.put<int>(24, 5); f.put<int>(28, 256);
    f.put<int>(32, 2); f.put<int>(36, 128);
    f.put<unsigned>(48, 100); f.put<int>(52, 2048);
    f.put<unsigned>(64, 99); f.put<int>(68, 2304);
    const char *names[]{"SessionTime", "IsOnTrack", "SessionNum", "Speed", "SessionState"};
    const int types[]{5, 1, 2, 4, 2};
    const int offsets[]{64, 7, 96, 24, 32};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto entry = 256 + i * 144;
        f.put<int>(entry, types[i]); f.put<int>(entry + 4, offsets[i]);
        f.put<int>(entry + 8, 1);
        std::memcpy(f.bytes.data() + entry + 16, names[i], std::strlen(names[i]) + 1);
    }
    f.put<double>(2048 + 64, 20.0); f.put<unsigned char>(2048 + 7, 1);
    f.put<int>(2048 + 96, 2); f.put<float>(2048 + 24, 10.f);
    f.put<int>(2048 + 32, 4);
    f.put<double>(2304 + 64, 19.0); f.put<unsigned char>(2304 + 7, 1);
    f.put<int>(2304 + 96, 2); f.put<float>(2304 + 24, 20.f);
    f.put<int>(2304 + 32, 4);
    return f;
}
Fixture lmu() {
    Fixture f(324824);
    f.put<unsigned char>(96, 3);
    f.put<int>(1632 + 64, 10); f.put<int>(1632 + 104, 3);
    f.put<unsigned char>(1632 + 108, 5); f.put<unsigned char>(1632 + 115, 1);
    f.put<unsigned char>(128464, 3); f.put<unsigned char>(128465, 2);
    f.put<unsigned char>(128466, 1);
    const auto player = 128468 + 2 * 1888;
    f.put<int>(player, 42); f.put<double>(player + 12, 20.0);
    f.put<double>(player + 184, 3.0); f.put<double>(player + 192, 0.0);
    f.put<double>(player + 200, -4.0); // magnitude 5 m/s = 18 km/h
    f.put<double>(128468 + 184, 50.0); // an opponent must never supply speed
    return f;
}

// Mutates producer state between the first copy and validation copy. This
// catches missing metadata/tick rechecks, without timing-sensitive thread races.
struct TearingView {
    Fixture &fixture;
    std::size_t offset;
    mutable unsigned reads{};
    bool copy(std::size_t start, void *out, std::size_t length) const {
        if (++reads == 2) {
            unsigned value{};
            std::memcpy(&value, fixture.bytes.data() + offset, sizeof(value));
            fixture.put<unsigned>(offset, value + 1);
        }
        return fixture.view().copy(start, out, length);
    }
    std::size_t size;
};

struct TestMapping {
    HANDLE handle{};
    unsigned char *data{};
    TestMapping(const wchar_t *name, std::size_t size) {
        handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, static_cast<DWORD>(size), name);
        const auto creation_error = GetLastError();
        if (handle) data = static_cast<unsigned char *>(MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, size));
        check(data != nullptr && creation_error != ERROR_ALREADY_EXISTS, "isolated telemetry fixture mapping opens");
    }
    ~TestMapping() { if (data) UnmapViewOfFile(data); if (handle) CloseHandle(handle); }
    void set(const Fixture &f) { std::memcpy(data, f.bytes.data(), f.bytes.size()); }
    template <class T> void put(std::size_t offset, T value) { std::memcpy(data + offset, &value, sizeof(value)); }
    TestMapping(const TestMapping &) = delete;
    TestMapping &operator=(const TestMapping &) = delete;
};
struct TestNames {
    std::wstring ir, lm, lock, event;
    explicit TestNames(const wchar_t *suffix) {
        auto base = L"Local\\SteadyXR-speed-test-" + std::to_wstring(GetCurrentProcessId()) + suffix;
        ir = base + L"-ir"; lm = base + L"-lmu"; lock = base + L"-lock"; event = base + L"-event";
    }
    MappingNames names() const { return {ir.c_str(), lm.c_str(), lock.c_str(), event.c_str()}; }
};
void advance_ir(TestMapping &mapping, unsigned tick, double time, float speed = 10.f) {
    mapping.put<unsigned>(48, tick); mapping.put<double>(2112, time);
    mapping.put<float>(2072, speed);
}

void parser_tests() {
    auto f = iracing();
    auto s = read_iracing(f.view());
    check(s.valid && approximately(s.kph, 36) && approximately(s.time, 20), "iRacing locates dynamic Speed and newest buffer");
    f.put<unsigned>(64, 101);
    check(approximately(read_iracing(f.view()).kph, 72), "iRacing chooses a newer buffer even when it is not first");
    f = iracing(); f.put<unsigned>(48, 1); f.put<unsigned>(64, 0xfffffffeu);
    check(approximately(read_iracing(f.view()).kph, 36), "iRacing tick rollover preserves newest-buffer ordering");
    f = iracing(); f.put<int>(4, 0);
    check(!read_iracing(f.view()).valid, "iRacing disconnected status invalidates speed");
    f = iracing(); f.put<unsigned char>(2055, 0);
    check(!read_iracing(f.view()).valid, "iRacing off-track data is rejected");
    f = iracing(); f.put<int>(2080, 0);
    check(!read_iracing(f.view()).valid, "iRacing invalid session state is rejected");
    f = iracing(); f.put<int>(2144, -1);
    check(!read_iracing(f.view()).valid, "iRacing invalid session number is rejected");
    f = iracing(); f.put<float>(2072, std::numeric_limits<float>::quiet_NaN());
    check(!read_iracing(f.view()).valid, "iRacing nonfinite speed is rejected");
    f = iracing(); f.put<float>(2072, -3.f);
    check(read_iracing(f.view()).valid && approximately(read_iracing(f.view()).kph, 10.8), "iRacing reverse speed uses its magnitude");
    f = iracing(); f.put<float>(2072, 0.f);
    check(read_iracing(f.view()).valid && read_iracing(f.view()).kph == 0, "stationary iRacing vehicle is valid telemetry");
    for (auto offset : {24u, 28u, 32u, 36u, 52u, 68u, 16u, 20u, 256u + 3u * 144u + 4u}) {
        f = iracing(); f.put<int>(offset, 0x7fffffff);
        check(!read_iracing(f.view()).valid, "iRacing rejects overflowing counts and all referenced ranges");
    }
    f = iracing(); f.put<int>(256 + 3 * 144 + 8, 2);
    check(!read_iracing(f.view()).valid, "iRacing Speed must be scalar");
    f = iracing(); f.put<int>(256 + 3 * 144, 2);
    check(!read_iracing(f.view()).valid, "iRacing Speed must have a floating-point type");
    f = iracing(); std::memset(f.bytes.data() + 256 + 3 * 144 + 16, 'x', 32);
    check(!read_iracing(f.view()).valid, "iRacing unterminated variable names are rejected");
    f = iracing(); f.bytes.resize(100);
    check(!read_iracing(f.view()).valid, "truncated iRacing header is rejected");
    f = iracing(); TearingView tick_torn{f, 48, 0, f.bytes.size()};
    check(!read_iracing(tick_torn).valid, "iRacing rejects a row overwritten during its snapshot");
    f = iracing(); TearingView session_torn{f, 12, 0, f.bytes.size()};
    check(!read_iracing(session_torn).valid, "iRacing rejects metadata changed during its snapshot");
    f = iracing(); f.put<int>(4, 0); TearingView connection_torn{f, 4, 0, f.bytes.size()};
    check(!read_iracing(connection_torn).valid, "iRacing cannot connect halfway through a snapshot");

    auto lm = lmu();
    s = read_lmu(lm.view());
    check(s.valid && approximately(s.kph, 18), "LMU reads the selected player velocity magnitude");
    for (auto offset : {128464u, 128465u, 128466u, 96u, 1740u, 1747u}) {
        lm = lmu(); lm.put<unsigned char>(offset, 255);
        check(!read_lmu(lm.view()).valid, "LMU invalid count, index, flag or application state is rejected");
    }
    lm = lmu(); lm.put<int>(1736, 105);
    check(!read_lmu(lm.view()).valid, "LMU scoring count cannot exceed its vehicle array");
    lm = lmu(); lm.put<unsigned char>(1740, 9);
    check(!read_lmu(lm.view()).valid, "paused LMU telemetry is rejected");
    lm = lmu(); lm.put<unsigned char>(1747, 0);
    check(!read_lmu(lm.view()).valid, "LMU monitor mode is rejected");
    lm = lmu(); lm.put<double>(128468 + 2 * 1888 + 184, std::numeric_limits<double>::infinity());
    check(!read_lmu(lm.view()).valid, "LMU nonfinite local velocity is rejected");
    lm = lmu(); lm.bytes.resize(324823);
    check(!read_lmu(lm.view()).valid, "truncated LMU layout is rejected before indexing");
}

void lifecycle_tests() {
    TestNames n(L"-lifecycle");
    TestMapping ir(n.ir.c_str(), 4096);
    ir.set(iracing());
    Poller poller(n.names());
    check(!poller.sample(SpeedGame::iracing, 1000).valid, "opening telemetry requires source progress before accepting speed");
    advance_ir(ir, 101, 20.05);
    check(!poller.sample(SpeedGame::iracing, 1049).valid, "telemetry polling is capped at 20 Hz");
    auto s = poller.sample(SpeedGame::iracing, 1050);
    check(s.valid && s.game == SpeedGame::iracing && approximately(s.speed_kph, 36), "advancing iRacing telemetry becomes valid");
    advance_ir(ir, 102, 20.10, 20.f);
    check(approximately(poller.sample(SpeedGame::iracing, 1099).speed_kph, 36), "repeated capture draws reuse the last speed sample");
    check(approximately(poller.sample(SpeedGame::iracing, 1100).speed_kph, 72), "next polling interval observes new speed");
    check(poller.sample(SpeedGame::iracing, 1599).valid, "unchanged telemetry remains valid within the stale timeout");
    check(!poller.sample(SpeedGame::iracing, 1600).valid, "cached speed expires at exactly 500 ms without source progress");
    advance_ir(ir, 103, 21.0);
    check(!poller.sample(SpeedGame::iracing, 2000).valid, "reopening after stale data again requires source progress");
    advance_ir(ir, 104, 21.05);
    check(poller.sample(SpeedGame::iracing, 2050).valid, "telemetry reconnects after stale data");
    ir.put<int>(2144, 3);
    advance_ir(ir, 105, 1.0);
    check(!poller.sample(SpeedGame::iracing, 2100).valid, "session changes cannot reuse a previous session's speed");
    advance_ir(ir, 106, 1.05);
    check(poller.sample(SpeedGame::iracing, 2150).valid, "a new session becomes valid after progress");
    ir.put<int>(4, 0);
    check(!poller.sample(SpeedGame::iracing, 2200).valid, "disconnect invalidates a recent cached reading immediately on poll");
    poller.reset();
    check(!poller.sample(SpeedGame::iracing, 2250).valid, "reset clears previous speed and freshness");
}

void retry_and_release_tests() {
    TestNames n(L"-retry");
    Poller poller(n.names());
    check(!poller.sample(SpeedGame::iracing, 1000).valid, "missing telemetry is nonfatal");
    {
        TestMapping ir(n.ir.c_str(), 4096);
        ir.set(iracing());
        check(!poller.sample(SpeedGame::iracing, 1900).valid, "missing mappings are retried no faster than once per second");
        advance_ir(ir, 101, 20.05);
        check(!poller.sample(SpeedGame::iracing, 2000).valid, "first successful mapping open starts a fresh baseline");
        advance_ir(ir, 102, 20.10);
        check(poller.sample(SpeedGame::iracing, 2050).valid, "later source progress recovers from unavailable mapping");
    }
    poller.reset();
    HANDLE lingering = OpenFileMappingW(FILE_MAP_READ, FALSE, n.ir.c_str());
    check(!lingering, "reset releases the mapping so a stopped producer's object disappears");
    if (lingering) CloseHandle(lingering);
}

void lmu_and_switch_tests() {
    TestNames n(L"-switch");
    TestMapping ir(n.ir.c_str(), 4096), lm(n.lm.c_str(), 324824), lock(n.lock.c_str(), 8);
    ir.set(iracing()); lm.set(lmu());
    lock.put<LONG>(0, 0); lock.put<LONG>(4, 0);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, n.event.c_str());
    check(event != nullptr, "LMU existing synchronization event fixture opens");
    Poller poller(n.names());
    const auto player_time = 128468 + 2 * 1888 + 12;
    check(!poller.sample(SpeedGame::lmu, 1000).valid, "LMU starts with a fresh baseline");
    lm.put<double>(player_time, 20.05);
    lock.put<LONG>(0, 1);
    auto s = poller.sample(SpeedGame::lmu, 1050);
    check(s.valid && s.game == SpeedGame::lmu && approximately(s.speed_kph, 18), "native LMU speed becomes valid after elapsed-time progress");
    check(*reinterpret_cast<LONG *>(lock.data + 4) == 0 && WaitForSingleObject(event, 0) == WAIT_OBJECT_0,
          "LMU releases its lock and wakes existing waiters");
    check(*reinterpret_cast<LONG *>(lock.data) == 1, "LMU polling does not change the producer's waiter count");
    lock.put<LONG>(0, 0); lock.put<LONG>(4, 1);
    lm.put<double>(player_time, 20.10);
    check(!poller.sample(SpeedGame::lmu, 1100).valid, "busy LMU writer is skipped without waiting or reading torn data");
    check(*reinterpret_cast<LONG *>(lock.data + 4) == 1, "failed LMU lock attempt never releases another owner's lock");
    lock.put<LONG>(4, 0);
    poller.sample(SpeedGame::lmu, 1150);
    lm.put<double>(player_time, 20.15);
    check(poller.sample(SpeedGame::lmu, 1200).valid, "LMU recovers after writer contention");
    check(!poller.sample(SpeedGame::iracing, 1250).valid, "switching game never returns the previous game's cached speed");
    advance_ir(ir, 101, 20.05);
    s = poller.sample(SpeedGame::iracing, 1300);
    check(s.valid && s.game == SpeedGame::iracing, "explicit game selection reads only the chosen source");
    poller.reset();
    check(!poller.sample(SpeedGame::automatic, 2000).valid, "automatic selection starts with fresh source baselines");
    advance_ir(ir, 102, 20.10); lm.put<double>(player_time, 20.20);
    check(poller.sample(SpeedGame::automatic, 2050).game == SpeedGame::iracing, "automatic selection prefers live iRacing when both games are active");
    ir.put<int>(4, 0);
    poller.sample(SpeedGame::automatic, 2100);
    lm.put<double>(player_time, 20.25);
    s = poller.sample(SpeedGame::automatic, 2150);
    check(s.valid && s.game == SpeedGame::lmu, "automatic selection falls back to advancing native LMU");
    auto expected_data = lmu();
    expected_data.put<double>(player_time, 20.25);
    check(std::memcmp(lm.data, expected_data.bytes.data(), expected_data.bytes.size()) == 0,
          "sampling never writes LMU telemetry or game state");
    lm.put<unsigned char>(128465, 255);
    check(!poller.sample(SpeedGame::automatic, 2200).valid && *reinterpret_cast<LONG *>(lock.data + 4) == 0,
          "invalid LMU data releases the acquired lock on every exit path");
    poller.reset();
    if (event) CloseHandle(event);
}

void unavailable_lmu_and_destruction_tests() {
    TestNames n(L"-no-lock");
    {
        TestMapping lm(n.lm.c_str(), 324824);
        lm.set(lmu());
        {
            Poller poller(n.names());
            check(!poller.sample(SpeedGame::lmu, 1000).valid, "LMU without native synchronization remains unavailable");
            HANDLE lock = OpenFileMappingW(FILE_MAP_READ, FALSE, n.lock.c_str());
            HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, n.event.c_str());
            check(!lock && !event, "LMU reader never creates missing game synchronization objects");
            if (lock) CloseHandle(lock);
            if (event) CloseHandle(event);
        }
    }
    HANDLE lingering = OpenFileMappingW(FILE_MAP_READ, FALSE, n.lm.c_str());
    check(!lingering, "failed LMU connection and destruction release the data mapping");
    if (lingering) CloseHandle(lingering);
    TestNames other(L"-destruction");
    auto reader = std::make_unique<Poller>(other.names());
    {
        TestMapping ir(other.ir.c_str(), 4096);
        ir.set(iracing());
        reader->sample(SpeedGame::iracing, 1000);
        advance_ir(ir, 101, 20.05);
        check(reader->sample(SpeedGame::iracing, 1050).valid, "destructor fixture establishes a live connection");
    }
    reader.reset();
    lingering = OpenFileMappingW(FILE_MAP_READ, FALSE, other.ir.c_str());
    check(!lingering, "destruction releases an active telemetry mapping");
    if (lingering) CloseHandle(lingering);
}
} // namespace

int main() {
    parser_tests();
    lifecycle_tests();
    retry_and_release_tests();
    lmu_and_switch_tests();
    unavailable_lmu_and_destruction_tests();
    return failures ? 1 : 0;
}
