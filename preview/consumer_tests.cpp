#include "transport.hpp"
#include <iostream>

using namespace vrst::preview;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        failures += !ok;
    };
    const auto name = L"Local\\SteadyXR-consumer-test-" + std::to_wstring(GetCurrentProcessId());
    Channel first, second;
    check(first.open(true, name) && second.open(true, name), "isolated channel opens");
    check(first.heartbeat(1, 60) && first.owns_consumer(), "first consumer claims channel");
    check(!second.heartbeat(0, 90) && !second.owns_consumer(), "second local consumer cannot steal capture");
    second.release_consumer();
    if (first.lock()) {
        check(first.data()->consumer_pid == GetCurrentProcessId() && first.data()->eye == 1 &&
                  first.data()->fps == 60 && !first.data()->paused,
              "non-owner cannot change or clear live settings");
        first.unlock();
    } else
        check(false, "inspect owner");
    first.release_consumer();
    check(!first.owns_consumer() && second.heartbeat(0, 90), "release permits source handoff");
    second.close();
    check(first.heartbeat(1, 60), "closing owner releases channel");
    first.release_consumer();
    check(second.open(true, name), "reopen observer");
    const auto foreign = GetCurrentProcessId() + 100000;
    if (first.lock()) {
        first.data()->consumer_pid = foreign;
        first.data()->paused = 0;
        first.data()->heartbeat_ms = GetTickCount64();
        first.unlock();
    }
    check(!first.heartbeat(0, 30), "live foreign consumer is respected");
    first.release_consumer();
    if (second.lock()) {
        check(second.data()->consumer_pid == foreign, "foreign consumer survives non-owner release");
        second.data()->heartbeat_ms = GetTickCount64() - 3000;
        second.unlock();
    }
    check(first.heartbeat(1, 60), "stale foreign consumer recovers without restart");
    if (second.lock()) {
        check(second.data()->ready == 2, "handoff requests fresh GPU textures");
        second.data()->consumer_pid = foreign;
        second.data()->heartbeat_ms = GetTickCount64();
        second.unlock();
    }
    const bool stale_claim_consumes = first.lock(true);
    check(!stale_claim_consumes, "foreign takeover blocks frame access before next heartbeat");
    if (stale_claim_consumes)
        first.unlock();
    check(!first.heartbeat(1, 60) && !first.owns_consumer(),
          "legacy consumer takeover relinquishes local ownership");
    first.release_consumer();
    if (second.lock()) {
        check(second.data()->consumer_pid == foreign, "yielding does not pause legacy consumer");
        second.data()->paused = 1;
        second.unlock();
    }
    check(first.heartbeat(4, 900), "paused consumer permits takeover");
    check(first.lock(true), "current owner can lock frame consumption");
    first.unlock();
    if (second.lock()) {
        check(second.data()->eye == 1 && second.data()->fps == 240, "capture request is clamped");
        second.unlock();
    }
    return failures ? 1 : 0;
}
