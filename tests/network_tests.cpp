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
    std::atomic<bool> stop{false}, emit{true};
    std::atomic<std::uint64_t> session{42};
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
                    m->server_receive = now + 5000000000ULL;
                    m->server_send = vrst::monotonic_ns() + 5000000000ULL;
                    m->session = session;
                    auto b = vrst::wire::encode(*m);
                    server.send(from, b.data(), b.size());
                }
            }
            if (peer && emit && now - last > 10000000) {
                last = now;
                vrst::wire::Message m;
                m.pose.session = session;
                m.pose.sequence = ++seq;
                m.pose.time_ns = now + 5000000000ULL;
                m.pose.flags = vrst::TrackingValid | vrst::FramePose;
                m.pose.orientation = vrst::axis_angle({0, 1, 0}, .02 * std::sin(now * 1e-9 * 20));
                auto b = vrst::wire::encode(m);
                server.send(*peer, b.data(), b.size());
            }
            server.wait(2);
        }
    });
    int failures = 0;
    {
        vrst::Receiver client;
        client.configure("127.0.0.1", server.port());
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
        emit = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        if (client.snapshot(80, 50).pose.valid) {
            std::cerr << "FAIL stale tracking did not bypass\n";
            ++failures;
        } else
            std::cout << "PASS tracking loss bypass\n";
        session = 43;
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
        client.configure("not-an-ip", 42424);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (client.snapshot(80, 50).pose.valid) {
            std::cerr << "FAIL reconfiguration retained tracking\n";
            ++failures;
        } else
            std::cout << "PASS invalid address fails closed\n";
    }
    stop = true;
    worker.join();
    return failures ? 1 : 0;
}
