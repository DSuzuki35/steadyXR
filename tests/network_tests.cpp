#include "vrst/net.hpp"
#include "vrst/protocol.hpp"
#include "vrst/receiver.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

int main() {
    vrst::net::Socket server;
    std::atomic<bool> stop{false}, emit{true}, repeat{false};
    std::atomic<std::uint64_t> session{42}, clock_offset{5000000000ULL};
    std::atomic<std::uint32_t> flags{vrst::TrackingValid | vrst::FramePose};
    std::atomic<int> sync_delay_ms{0};
    std::thread worker([&] {
        std::optional<vrst::net::Endpoint> peer;
        std::uint64_t seq = 0, last = 0;
        while (!stop) {
            std::uint8_t buf[256];
            vrst::net::Endpoint from;
            int n = server.receive(buf, sizeof(buf), from);
            auto now = vrst::monotonic_ns();
            if (n > 0) {
                auto m = vrst::wire::decode(buf, static_cast<std::size_t>(n));
                if (m && m->type == vrst::wire::Type::sync_request) {
                    peer = from;
                    m->type = vrst::wire::Type::sync_reply;
                    m->server_receive = now + clock_offset;
                    m->server_send = vrst::monotonic_ns() + clock_offset;
                    m->session = session;
                    auto b = vrst::wire::encode(*m);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sync_delay_ms.load()));
                    server.send(from, b.data(), b.size());
                }
            }
            if (peer && (emit || repeat) && now - last > 10000000) {
                last = now;
                vrst::wire::Message m;
                m.pose.session = session;
                m.pose.sequence = repeat ? seq : ++seq;
                m.pose.time_ns = now + clock_offset;
                m.pose.flags = flags;
                m.pose.orientation = vrst::axis_angle({0, 1, 0}, .02 * std::sin(now * 1e-9 * 20));
                auto b = vrst::wire::encode(m);
                server.send(*peer, b.data(), b.size());
            }
            server.wait(2);
        }
    });
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok)
            ++failures;
    };
    {
        vrst::Receiver client;
        const auto empty = client.snapshot(80, 50);
        check(!empty.latest_received && !empty.latest_pose.session && !empty.latest_receive_time_ns &&
                  empty.local_time_ns && !empty.remote_now_ns,
              "unconfigured receiver has a local clock and no sender timing");
        client.configure("127.0.0.1", server.port());
        auto wait_snapshot = [&](double delay, auto predicate) {
            const auto deadline = vrst::monotonic_ns() + 2000000000ULL;
            vrst::TrackingSnapshot result;
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                result = client.snapshot(delay, 50);
            } while (!predicate(result) && vrst::monotonic_ns() < deadline);
            return result;
        };
        auto until = vrst::monotonic_ns() + 2000000000ULL;
        vrst::TrackingSnapshot s;
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            s = client.snapshot(80, 50);
        } while (!s.pose.valid && vrst::monotonic_ns() < until);
        if (!s.pose.valid || !s.clock_ready || s.received < 3) {
            std::cerr << "FAIL loopback clock alignment and delayed pose delivery: " << s.status << '\n';
            ++failures;
        } else
            std::cout << "PASS actual UDP loopback with 5-second clock offset, pose age=" << s.pose_age_ms
                      << "ms\n";
        if (s.pose.valid && s.pose_age_ms < 65) {
            std::cerr << "FAIL 80ms lookback used current tracking\n";
            ++failures;
        }
        const auto local_now = vrst::monotonic_ns();
        if (!s.local_pose_time_ns || s.local_pose_time_ns > local_now ||
            local_now - s.local_pose_time_ns < 65000000ULL ||
            local_now - s.local_pose_time_ns > 200000000ULL) {
            std::cerr << "FAIL selected pose timestamp did not map back to local monotonic clock\n";
            ++failures;
        } else
            std::cout << "PASS mapped pose timestamp accounts for remote clock offset and delay\n";
        check(s.latest_received && s.latest_pose.session == 42 && s.pose.session == 42 &&
                  s.latest_pose.sequence > s.pose.sequence &&
                  s.latest_pose.time_ns > s.pose.time_ns + 50000000ULL,
              "latest received packet is independent of the 80ms selected pose");
        check(s.local_time_ns && s.latest_receive_time_ns <= s.local_time_ns &&
                  s.local_time_ns - s.latest_receive_time_ns < 100000000ULL &&
                  s.remote_now_ns > s.local_time_ns + 4950000000ULL &&
                  s.remote_now_ns < s.local_time_ns + 5050000000ULL &&
                  s.remote_now_ns > s.pose.time_ns + 65000000ULL &&
                  s.remote_now_ns < s.pose.time_ns + 200000000ULL,
              "snapshot names local receive time and sender now in their correct clock domains");
        check(s.pose.sample_time_ns == s.pose.time_ns && !s.pose.interpolated,
              "received frame pose timing stays on its raw packet timestamp");
        emit = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const auto frozen = client.snapshot(80, 50);
        const auto other_delay = client.snapshot(0, 50);
        check(frozen.latest_received && other_delay.latest_received &&
                  frozen.latest_pose.sequence == other_delay.latest_pose.sequence &&
                  frozen.latest_pose.time_ns == other_delay.latest_pose.time_ns &&
                  frozen.latest_receive_time_ns == other_delay.latest_receive_time_ns,
              "changing selected delay does not change latest received metadata");
        repeat = true;
        s = wait_snapshot(80, [&](const auto &value) { return value.rejected > frozen.rejected; });
        repeat = false;
        check(s.rejected > frozen.rejected && s.latest_pose.sequence == frozen.latest_pose.sequence &&
                  s.latest_pose.time_ns == frozen.latest_pose.time_ns &&
                  s.latest_receive_time_ns == frozen.latest_receive_time_ns,
              "rejected duplicate sequence cannot replace latest accepted packet timing");
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        s = client.snapshot(80, 50);
        if (s.pose.valid) {
            std::cerr << "FAIL stale tracking did not bypass\n";
            ++failures;
        } else
            std::cout << "PASS tracking loss bypass\n";
        check(s.latest_received && s.latest_pose.sequence == frozen.latest_pose.sequence &&
                  s.latest_receive_time_ns == frozen.latest_receive_time_ns &&
                  s.local_time_ns - s.latest_receive_time_ns >= 350000000ULL && !s.pose.session,
              "stale tracking retains truthful last reception while selected pose becomes unavailable");
        session = 43;
        s = wait_snapshot(80, [](const auto &value) { return !value.latest_received; });
        check(!s.latest_received && !s.latest_pose.session && !s.latest_pose.time_ns &&
                  !s.latest_receive_time_ns && !s.pose.valid,
              "new sender session clears previous packet before receiving a replacement");
        emit = true;
        until = vrst::monotonic_ns() + 2000000000ULL;
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            s = client.snapshot(80, 50);
        } while (!s.pose.valid && vrst::monotonic_ns() < until);
        if (!s.pose.valid) {
            std::cerr << "FAIL sender session restart recovery\n";
            ++failures;
        } else
            std::cout << "PASS sender session restart recovery\n";
        check(s.latest_received && s.latest_pose.session == 43 && s.pose.session == 43,
              "session recovery reports only the new sender session");
        flags = 0;
        s = wait_snapshot(0, [](const auto &value) {
            return value.latest_received && !(value.latest_pose.flags & vrst::TrackingValid);
        });
        check(s.latest_received && s.latest_pose.session == 43 && !s.latest_pose.flags && !s.pose.valid,
              "latest received invalid tracking packet keeps its original flags without claiming a pose");
        flags = vrst::TrackingValid;
        s = wait_snapshot(80, [](const auto &value) { return value.pose.valid && value.pose.interpolated; });
        check(s.pose.valid && s.pose.interpolated && s.pose.session == 43 &&
                  s.pose.sample_time_ns > s.pose.time_ns &&
                  s.pose.sample_time_ns < s.latest_pose.time_ns &&
                  s.remote_now_ns >= s.pose.sample_time_ns + 79999999ULL &&
                  s.remote_now_ns <= s.pose.sample_time_ns + 80000001ULL,
              "live UDP interpolation reports effective sender time separately from its earlier packet");
        emit = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        client.reset();
        s = client.snapshot(80, 50);
        check(!s.latest_received && !s.latest_pose.session && !s.latest_pose.time_ns &&
                  !s.latest_receive_time_ns && !s.pose.valid,
              "explicit reset clears selected and latest received packet timing");
        // Give the fresh session a slower route; the next fast sync must replace its clock estimate.
        sync_delay_ms = 60;
        session = 44;
        flags = vrst::TrackingValid | vrst::FramePose;
        emit = true;
        s = wait_snapshot(80, [](const auto &value) {
            return value.pose.valid && value.pose.session == 44;
        });
        check(s.latest_received && s.latest_pose.session == 44 && s.pose.valid,
              "fresh clock-jump test session receives tracking");
        emit = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        clock_offset = 6000000000ULL;
        sync_delay_ms = 0;
        s = wait_snapshot(80, [](const auto &value) {
            return value.remote_now_ns > value.local_time_ns + 5900000000ULL;
        });
        check(s.remote_now_ns > s.local_time_ns + 5900000000ULL && !s.latest_received &&
                  !s.latest_pose.session && !s.latest_pose.time_ns && !s.latest_receive_time_ns &&
                  !s.pose.valid,
              "clock discontinuity clears latest reception with the selected history");
        client.configure("not-an-ip", 42424);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (client.snapshot(80, 50).pose.valid) {
            std::cerr << "FAIL reconfiguration retained tracking\n";
            ++failures;
        } else
            std::cout << "PASS invalid address fails closed\n";
        s = client.snapshot(80, 50);
        check(!s.latest_received && !s.latest_pose.session && !s.latest_receive_time_ns &&
                  !s.remote_now_ns && !s.pose.session,
              "reconfiguration clears sender timing and clock domain estimate");
    }
    stop = true;
    worker.join();
    return failures ? 1 : 0;
}
