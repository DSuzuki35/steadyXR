#include "vrst/receiver.hpp"
#include "vrst/net.hpp"
#include "vrst/protocol.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>

namespace vrst {
struct Receiver::Impl {
    std::mutex mutex;
    std::atomic<bool> stop{false};
    std::string host, error{"Enter the gaming PC's IPv4 address"};
    std::uint16_t port{wire::default_port};
    std::uint64_t generation{}, session{}, last_receive{}, last_sync{}, received{}, rejected{};
    PoseHistory history;
    ClockSync clock;
    std::thread worker;
    Impl() : worker([this] { run(); }) {}
    ~Impl() {
        stop = true;
        if (worker.joinable())
            worker.join();
    }
    void run() noexcept {
        try {
            net::Socket socket;
            std::optional<net::Endpoint> peer;
            std::uint64_t active_generation = ~0ULL, last_probe = 0, nonce = monotonic_ns();
            struct Probe {
                std::uint64_t nonce, time;
            };
            std::deque<Probe> pending;
            while (!stop) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (active_generation != generation) {
                        peer = net::Endpoint::parse(host, port);
                        active_generation = generation;
                        pending.clear();
                        last_probe = 0;
                        error = peer ? "Waiting for sender and clock sync"
                                     : (host.empty() ? "Enter the gaming PC's IPv4 address"
                                                     : "Invalid IPv4 address");
                    }
                }
                auto now = monotonic_ns();
                if (peer && now - last_probe >= 500000000ULL) {
                    wire::Message m;
                    m.type = wire::Type::sync_request;
                    m.nonce = ++nonce;
                    m.client_send = now;
                    auto b = wire::encode(m);
                    socket.send(*peer, b.data(), b.size());
                    pending.push_back({nonce, now});
                    while (pending.size() > 8)
                        pending.pop_front();
                    last_probe = now;
                }
                for (int batch = 0; batch < 64; ++batch) {
                    std::uint8_t bytes[256];
                    net::Endpoint from;
                    int length = socket.receive(bytes, sizeof(bytes), from);
                    if (length < 0)
                        break;
                    if (!peer || !(from == *peer))
                        continue;
                    auto at = monotonic_ns();
                    auto m = wire::decode(bytes, static_cast<std::size_t>(length));
                    std::lock_guard<std::mutex> lock(mutex);
                    if (active_generation != generation)
                        break;
                    if (!m) {
                        ++rejected;
                        continue;
                    }
                    if (m->type == wire::Type::sync_reply) {
                        auto it = std::find_if(pending.begin(), pending.end(), [&](auto p) {
                            return p.nonce == m->nonce && p.time == m->client_send;
                        });
                        if (it == pending.end() || at - it->time > 1000000000ULL) {
                            ++rejected;
                            continue;
                        }
                        if (session != m->session) {
                            session = m->session;
                            history.clear();
                            clock.clear();
                            last_receive = 0;
                        }
                        double old = clock.offset_ns();
                        bool was_ready = clock.ready();
                        if (clock.observe(m->client_send, m->server_receive, m->server_send, at)) {
                            if (was_ready && std::abs(old - clock.offset_ns()) > 20000000)
                                history.clear();
                            last_sync = at;
                            error.clear();
                        } else
                            ++rejected;
                        pending.erase(it);
                    } else if (m->type == wire::Type::pose) {
                        if (!clock.ready() || m->pose.session != session) {
                            ++rejected;
                            continue;
                        }
                        double remote_now = static_cast<double>(at) + clock.offset_ns();
                        double age = remote_now - static_cast<double>(m->pose.time_ns);
                        if (age < -100000000. || age > 2000000000. || !history.push(m->pose)) {
                            ++rejected;
                            continue;
                        }
                        last_receive = at;
                        ++received;
                    }
                }
                socket.wait(10);
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(mutex);
            error = e.what();
        }
    }
};
Receiver::Receiver() : impl_(new Impl) {}
Receiver::~Receiver() = default;
void Receiver::configure(const std::string &host, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (host == impl_->host && port == impl_->port)
        return;
    impl_->host = host;
    impl_->port = port ? port : wire::default_port;
    ++impl_->generation;
    impl_->session = impl_->last_receive = impl_->last_sync = impl_->received = impl_->rejected = 0;
    impl_->history.clear();
    impl_->clock.clear();
    impl_->error = "Connecting";
}
void Receiver::reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->history.clear();
}
TrackingSnapshot Receiver::snapshot(double delay_ms, double smoothing) {
    MotionSettings settings;
    settings.amount = smoothing;
    return snapshot(delay_ms, settings);
}
TrackingSnapshot Receiver::snapshot(double delay_ms, const MotionSettings &smoothing) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto now = monotonic_ns();
    TrackingSnapshot s;
    s.received = impl_->received;
    s.rejected = impl_->rejected;
    s.connected = impl_->last_receive && now - impl_->last_receive < 1000000000ULL;
    s.clock_ready = impl_->clock.ready() && now - impl_->last_sync < 3000000000ULL;
    s.rtt_ms = impl_->clock.rtt_ms();
    impl_->history.set_smoothing(smoothing);
    double remote_now = static_cast<double>(now) + impl_->clock.offset_ns();
    double lookback = std::isfinite(delay_ms) ? std::clamp(delay_ms, 0., 1000.) : 80.;
    double target = remote_now - lookback * 1000000.;
    if (s.connected && s.clock_ready && now - impl_->last_receive < 250000000ULL && target > 0 &&
        target < static_cast<double>(UINT64_MAX))
        s.pose = impl_->history.sample(static_cast<std::uint64_t>(target));
    auto pose_time = s.pose.valid ? s.pose.time_ns : impl_->history.latest_time();
    s.pose_age_ms = pose_time ? (remote_now - static_cast<double>(pose_time)) * 1e-6 : 0;
    const double local_pose_time = static_cast<double>(s.pose.time_ns) - impl_->clock.offset_ns();
    if (s.pose.valid && local_pose_time > 0 && local_pose_time < static_cast<double>(UINT64_MAX))
        s.local_pose_time_ns = static_cast<std::uint64_t>(local_pose_time);
    if (!impl_->error.empty())
        s.status = impl_->error;
    else if (!s.clock_ready)
        s.status = "Waiting for clock sync";
    else if (!s.connected)
        s.status = "Waiting for tracking / sender disconnected";
    else if (!s.pose.valid)
        s.status = "No valid pose at this delay; stabilization bypassed";
    else if (s.pose.flags & DemoPose)
        s.status = "DEMO tracking (synthetic)";
    else if (s.pose.flags & FramePose)
        s.status = "Connected: observed render-frame poses";
    else
        s.status = "Connected: live headset poses";
    return s;
}
} // namespace vrst
