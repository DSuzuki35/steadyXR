#include "pose_math.hpp"
#include "vrst/net.hpp" // Winsock must precede any Windows headers.
#include "vrst/protocol.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif
#ifdef VRST_WITH_OPENVR
#include <openvr.h>
#endif

namespace {
constexpr std::uint64_t subscriber_timeout = 3000000000ULL;
constexpr std::uint64_t pose_interval = 1000000000ULL / 90;
std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal handler requires lock-free stop flag");
void stop_signal(int) {
    stopped.store(true, std::memory_order_relaxed);
}
#ifdef _WIN32
BOOL WINAPI stop_console(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        stopped.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}
#endif

struct Options {
    std::uint16_t port{vrst::wire::default_port};
    bool right_eye{}, live{}, demo{}, help{};
};

Options parse_options(int argc, char **argv) {
    if (argc > 16)
        throw std::runtime_error("Too many arguments; use --help.");
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag.size() > 64)
            throw std::runtime_error("Argument is too long; use --help.");
        if (flag == "--help" || flag == "-h") {
            result.help = true;
            continue;
        }
        if (flag == "--demo") {
            result.demo = true;
            continue;
        }
        if (flag != "--port" && flag != "--eye" && flag != "--mode")
            throw std::runtime_error("Unknown option '" + std::string(flag) + "'; use --help.");
        if (++i == argc)
            throw std::runtime_error("Missing value for " + std::string(flag) + ".");
        const std::string_view value(argv[i]);
        if (flag == "--port") {
            unsigned port = 0;
            if (value.empty() || value.size() > 5)
                throw std::runtime_error("--port must be a number from 1 to 65535.");
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), port);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || port == 0 ||
                port > 65535)
                throw std::runtime_error("--port must be a number from 1 to 65535.");
            result.port = static_cast<std::uint16_t>(port);
        } else if (flag == "--eye") {
            if (value != "left" && value != "right")
                throw std::runtime_error("--eye must be left or right.");
            result.right_eye = value == "right";
        } else {
            if (value != "frame" && value != "live")
                throw std::runtime_error("--mode must be frame or live.");
            result.live = value == "live";
        }
    }
    return result;
}

void print_help() {
    std::cout
        << "VR Stabilizer pose sender 0.1\n\n"
        << "Usage: vrst-sender [--port 42424] [--eye left|right] [--mode frame|live] [--demo]\n"
        << "  --port N       UDP listening port, 1-65535 (default 42424)\n"
        << "  --eye EYE      Eye used by the captured view (default left)\n"
        << "  --mode MODE    Compositor frame pose, with live fallback (default frame), or live pose\n"
        << "  --demo         Synthetic 90 Hz poses; SteamVR and a headset are not needed\n"
        << "  --help, -h     Show this help\n\n"
        << "Frame timestamps record when this sender first observes each compositor frame.\n"
        << "This is observed-frame timing, not an exact HDMI/video match. Calibrate OBS Tracking delay.\n"
        << "The sender reads SteamVR as a background application and waits/reconnects automatically.\n";
}

std::set<std::string> local_ipv4() {
    std::set<std::string> result;
#ifdef _WIN32
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo *addresses = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &addresses) == 0) {
            for (auto *address = addresses; address; address = address->ai_next) {
                vrst::net::Endpoint endpoint;
                endpoint.address = *reinterpret_cast<const sockaddr_in *>(address->ai_addr);
                if ((ntohl(endpoint.address.sin_addr.s_addr) >> 24) != 127)
                    result.insert(endpoint.ip());
            }
            freeaddrinfo(addresses);
        }
    }
#else
    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
        for (auto *address = addresses; address; address = address->ifa_next) {
            if (!address->ifa_addr || address->ifa_addr->sa_family != AF_INET ||
                !(address->ifa_flags & IFF_UP) || (address->ifa_flags & IFF_LOOPBACK))
                continue;
            vrst::net::Endpoint endpoint;
            endpoint.address = *reinterpret_cast<const sockaddr_in *>(address->ifa_addr);
            result.insert(endpoint.ip());
        }
        freeifaddrs(addresses);
    }
#endif
    return result;
}

class SessionIds {
  public:
    std::uint64_t next() {
        std::uint64_t result = 0;
        while (!result || result == previous_)
            result = random_();
        previous_ = result;
        return result;
    }

  private:
    std::mt19937_64 random_{std::random_device{}() ^ vrst::monotonic_ns()};
    std::uint64_t previous_{};
};

class Subscribers {
  public:
    void receive(vrst::net::Socket &socket, std::uint64_t session) {
        const auto now = vrst::monotonic_ns();
        peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                    [now](const Client &client) {
                                        return now - client.last_seen >= subscriber_timeout;
                                    }),
                     peers_.end());
        // Limit work per poll so a busy socket cannot starve pose acquisition.
        for (int request = 0; request < 64; ++request) {
            std::uint8_t buffer[256];
            vrst::net::Endpoint from;
            const int length = socket.receive(buffer, sizeof(buffer), from);
            const auto received_at = vrst::monotonic_ns();
            if (length < 0)
                break;
            auto message = vrst::wire::decode(buffer, static_cast<std::size_t>(length));
            if (!message || message->type != vrst::wire::Type::sync_request)
                continue;
            const auto address = ntohl(from.address.sin_addr.s_addr);
            if (address == 0 || address >= 0xe0000000U || from.port() == 0)
                continue;
            auto found = std::find_if(peers_.begin(), peers_.end(),
                                      [&from](const Client &client) { return client.endpoint == from; });
            if (found == peers_.end()) {
                if (peers_.size() == 8)
                    continue;
                peers_.push_back({from, received_at});
            } else
                found->last_seen = received_at;
            message->type = vrst::wire::Type::sync_reply;
            message->server_receive = received_at;
            message->server_send = vrst::monotonic_ns();
            message->session = session;
            const auto bytes = vrst::wire::encode(*message);
            socket.send(from, bytes.data(), bytes.size());
        }
        if (peers_.size() != reported_count_) {
            reported_count_ = peers_.size();
            std::cout << "OBS subscribers: " << reported_count_ << "/8\n";
        }
    }
    void publish(vrst::net::Socket &socket, const vrst::Pose &pose) const {
        vrst::wire::Message message;
        message.pose = pose;
        const auto bytes = vrst::wire::encode(message);
        for (const auto &client : peers_)
            socket.send(client.endpoint, bytes.data(), bytes.size());
    }

  private:
    struct Client {
        vrst::net::Endpoint endpoint;
        std::uint64_t last_seen;
    };
    std::vector<Client> peers_;
    std::size_t reported_count_{};
};

bool due(std::uint64_t now, std::uint64_t &next, std::uint64_t interval) {
    if (now < next)
        return false;
    next = now - next > interval * 4 ? now + interval : next + interval;
    return true;
}

#ifdef VRST_WITH_OPENVR
std::optional<vrst::Quat> rotation(const vr::HmdMatrix34_t &matrix) {
    vrst::sender::Rotation values{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            values[row][col] = matrix.m[row][col];
    return vrst::sender::rotation_quaternion(values);
}

std::optional<vrst::Quat> tracked_rotation(const vr::TrackedDevicePose_t &pose) {
    if (!pose.bDeviceIsConnected || !pose.bPoseIsValid ||
        (pose.eTrackingResult != vr::TrackingResult_Running_OK &&
         pose.eTrackingResult != vr::TrackingResult_Fallback_RotationOnly))
        return std::nullopt;
    return rotation(pose.mDeviceToAbsoluteTracking);
}

class SteamVrSource {
  public:
    SteamVrSource(const Options &options, SessionIds &ids, std::uint64_t &session)
        : eye_(options.right_eye ? vr::Eye_Right : vr::Eye_Left), live_(options.live), ids_(ids),
          session_(session) {}
    ~SteamVrSource() {
        if (system_)
            vr::VR_Shutdown();
    }

    std::optional<vrst::Pose> poll(std::uint64_t now) {
        if (!system_ && now >= retry_at_)
            connect(now);
        if (!system_)
            return invalid_pose(now);
        vr::VREvent_t event{};
        for (int count = 0; count < 64 && system_->PollNextEvent(&event, sizeof(event)); ++count) {
            if (event.eventType == vr::VREvent_Quit || event.eventType == vr::VREvent_DriverRequestedQuit) {
                system_->AcknowledgeQuit_Exiting();
                disconnect(now);
                report("SteamVR stopped. Waiting for it to restart...", now);
                return invalid_pose(now);
            }
            if (event.eventType == vr::VREvent_IpdChanged)
                metadata_at_ = 0;
        }
        if (now >= metadata_at_) {
            metadata_at_ = now + 1000000000ULL;
            // SteamVR can initialize the system before its compositor is ready.
            if (!compositor_ && !live_)
                compositor_ = vr::VRCompositor();
            float left = 0, right = 0, top = 0, bottom = 0;
            system_->GetProjectionRaw(eye_, &left, &right, &top, &bottom);
            const vrst::Projection projection{left, right, -top, -bottom};
            const auto eye_rotation = rotation(system_->GetEyeToHeadTransform(eye_));
            metadata_valid_ = vrst::valid(projection) && eye_rotation.has_value();
            if (metadata_valid_) {
                projection_ = projection;
                eye_rotation_ = *eye_rotation;
            }
        }
        vr::TrackedDevicePose_t current{};
        system_->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &current, 1);
        const auto current_rotation = tracked_rotation(current);
        vr::Compositor_FrameTiming timing{};
        timing.m_nSize = sizeof(timing);
        const bool got_frame = !live_ && compositor_ && compositor_->GetFrameTiming(&timing);
        const auto frame_rotation = got_frame ? tracked_rotation(timing.m_HmdPose) : std::nullopt;
        // Stamp the observation after querying SteamVR. Its system-time field is
        // deliberately unused: OpenVR does not promise our steady_clock epoch.
        const auto observed_at = vrst::monotonic_ns();
        const auto source =
            frames_.select(current_rotation.has_value() && metadata_valid_,
                           got_frame ? std::optional<std::uint32_t>(timing.m_nFrameIndex) : std::nullopt,
                           frame_rotation.has_value(), observed_at);
        if (source == vrst::sender::Source::invalid) {
            report(metadata_valid_ ? "Headset tracking unavailable. OBS will bypass motion correction."
                                   : "Waiting for valid headset projection and eye calibration...",
                   now);
            return invalid_pose(observed_at);
        }
        if (source == vrst::sender::Source::wait)
            return std::nullopt;
        const bool frame = source == vrst::sender::Source::frame;
        if (!frame && !due(observed_at, next_live_, pose_interval))
            return std::nullopt;
        report(frame   ? "Tracking: compositor frame poses. Calibrate OBS Tracking delay."
               : live_ ? "Tracking: live headset poses. Calibrate OBS Tracking delay."
                       : "Tracking: live fallback; compositor frame poses unavailable or stalled.",
               now);
        vrst::Pose pose;
        pose.time_ns = observed_at;
        pose.orientation =
            vrst::normalized(vrst::multiply(frame ? *frame_rotation : *current_rotation, eye_rotation_));
        pose.projection = projection_;
        pose.flags = vrst::TrackingValid | (frame ? vrst::FramePose : 0U);
        return pose;
    }

  private:
    vr::EVREye eye_;
    bool live_;
    SessionIds &ids_;
    std::uint64_t &session_;
    vr::IVRSystem *system_{};
    vr::IVRCompositor *compositor_{};
    vrst::sender::FrameSelector frames_;
    vrst::Projection projection_;
    vrst::Quat eye_rotation_;
    bool metadata_valid_{};
    std::uint64_t retry_at_{}, metadata_at_{}, next_live_{}, next_invalid_{}, reported_at_{};
    std::string reported_;

    void report(const std::string &status, std::uint64_t now) {
        if (status != reported_ && (reported_.empty() || now - reported_at_ >= 1000000000ULL)) {
            std::cout << status << '\n';
            reported_ = status;
            reported_at_ = now;
        }
    }
    void connect(std::uint64_t now) {
        retry_at_ = now + 2000000000ULL;
        vr::EVRInitError error = vr::VRInitError_None;
        system_ = vr::VR_Init(&error, vr::VRApplication_Background);
        if (error != vr::VRInitError_None || !system_) {
            system_ = nullptr;
            report(std::string("Waiting for SteamVR: ") + vr::VR_GetVRInitErrorAsEnglishDescription(error) +
                       ". Start SteamVR and your headset; retrying automatically.",
                   now);
            return;
        }
        compositor_ = vr::VRCompositor();
        frames_ = {};
        metadata_at_ = 0;
        metadata_valid_ = false;
        session_ = ids_.next();
        std::cout << "Connected to SteamVR as a background reader.\n";
    }
    void disconnect(std::uint64_t now) {
        vr::VR_Shutdown();
        system_ = nullptr;
        compositor_ = nullptr;
        frames_ = {};
        metadata_valid_ = false;
        session_ = ids_.next();
        retry_at_ = now + 2000000000ULL;
    }
    std::optional<vrst::Pose> invalid_pose(std::uint64_t now) {
        if (!due(now, next_invalid_, 20000000ULL))
            return std::nullopt;
        vrst::Pose pose;
        pose.time_ns = now;
        pose.projection = projection_;
        pose.flags = 0;
        return pose;
    }
};
#endif
} // namespace

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            print_help();
            return 0;
        }
#ifndef VRST_WITH_OPENVR
        if (!options.demo) {
            std::cerr << "This build has no Windows OpenVR support. Use the Windows sender for SteamVR,\n"
                         "or run with --demo to test the network and OBS filter.\n";
            return 1;
        }
#endif
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
#ifdef _WIN32
        SetConsoleCtrlHandler(stop_console, TRUE);
#endif
        vrst::net::Socket socket(options.port);
        std::cout << "VR Stabilizer pose sender 0.1\nUDP port: " << options.port
                  << " | Eye: " << (options.right_eye ? "right" : "left") << " | Mode: "
                  << (options.demo   ? "DEMO (synthetic poses)"
                      : options.live ? "live"
                                     : "frame")
                  << "\nGaming-PC IPv4 addresses:\n";
        const auto addresses = local_ipv4();
        if (addresses.empty())
            std::cout << "  No network IPv4 address found. Check Windows ipconfig.\n";
        for (const auto &address : addresses)
            std::cout << "  " << address << '\n';
        std::cout << "On the OBS PC: add the VR Stabilizer effect filter to your capture-card source,\n"
                  << "enter this gaming PC's LAN IPv4 address and UDP port " << options.port << ".\n"
                  << "Allow this sender through Windows Firewall on your private LAN if prompted.\n"
                  << "Capture the matching eye. Adjust Tracking delay to match the captured video.\n"
                  << "Frame mode uses observed-frame timing, not an exact HDMI/video match.\n"
                  << "Leave this window open. Press Ctrl+C to stop. Waiting for OBS subscribers...\n";
        SessionIds ids;
        std::uint64_t session = ids.next();
        std::uint64_t sequence = 0, next_demo = 0;
        const auto demo_start = vrst::monotonic_ns();
        Subscribers subscribers;
#ifdef VRST_WITH_OPENVR
        SteamVrSource steamvr(options, ids, session);
#endif
        while (!stopped.load(std::memory_order_relaxed)) {
            const auto now = vrst::monotonic_ns();
            std::optional<vrst::Pose> pose;
            if (options.demo) {
                if (due(now, next_demo, pose_interval)) {
                    const double seconds = static_cast<double>(now - demo_start) * 1e-9;
                    vrst::Pose demo;
                    demo.time_ns = now;
                    demo.flags = vrst::TrackingValid | vrst::FramePose | vrst::DemoPose;
                    demo.orientation =
                        vrst::multiply(vrst::axis_angle({0, 1, 0}, .18 * std::sin(seconds * 1.3)),
                                       vrst::axis_angle({1, 0, 0}, .07 * std::sin(seconds * 2.1)));
                    demo.projection = vrst::symmetric_projection(100, 90);
                    pose = demo;
                }
            }
#ifdef VRST_WITH_OPENVR
            else
                pose = steamvr.poll(now);
#endif
            subscribers.receive(socket, session);
            if (pose) {
                pose->session = session;
                pose->sequence = ++sequence;
                subscribers.publish(socket, *pose);
            }
            socket.wait(5);
        }
        std::cout << "Sender stopped.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "VR Stabilizer: " << error.what() << '\n';
        return 1;
    }
}
