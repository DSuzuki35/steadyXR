#include "transport.hpp"
#include <iostream>
#include <limits>

using namespace vrst::preview;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char *name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        failures += !ok;
    };
    Frame packet{};
    packet.producer_pid = packet.session = packet.sequence = packet.generation = packet.view_count = 1;
    auto &v = packet.views[0];
    v.texture_handle = 1;
    v.width = v.height = v.rect_width = v.rect_height = 64;
    v.format = 28;
    packet.overlay_count = 1;
    auto &overlay = packet.overlays[0];
    overlay.image = v;
    overlay.image.texture_handle = 2;
    overlay.type = 1;
    overlay.width = overlay.height = 1;
    check(valid_frame(packet), "valid quad metadata is accepted");
    overlay.image.texture_handle = 1;
    check(!valid_frame(packet), "an overlay cannot reuse a base keyed handle and deadlock acquisition");
    overlay.image.texture_handle = 2;
    packet.overlays[1] = overlay;
    packet.overlays[1].slot = 1;
    packet.overlay_count = 2;
    check(!valid_frame(packet), "two overlay slots cannot acquire the same keyed handle twice");
    packet.overlays[1].image.texture_handle = 3;
    check(valid_frame(packet), "distinct overlay slots and textures are accepted");
    packet.overlays[1].slot = 0;
    check(!valid_frame(packet), "duplicate overlay slots are rejected");
    packet.overlay_count = max_overlay_images + 1;
    check(!valid_frame(packet), "oversized overlay payload is rejected before indexing");
    packet.overlay_count = 1;
    overlay.position[0] = std::numeric_limits<float>::quiet_NaN();
    check(!valid_frame(packet), "nonfinite overlay geometry is rejected");
    overlay.position[0] = 0;
    overlay.type = 2;
    overlay.central_angle = 1;
    overlay.aspect_ratio = 2;
    for (float radius : {0.f, 1.f, std::numeric_limits<float>::infinity()}) {
        overlay.radius = radius;
        check(valid_frame(packet), "finite and infinite OpenXR cylinders are accepted");
    }
    overlay.radius = -1;
    check(!valid_frame(packet), "negative cylinder radius is rejected");
    const auto name = L"Local\\SteadyXR-consumer-test-" + std::to_wstring(GetCurrentProcessId());
    Channel first, second;
    check(first.open(true, name) && second.open(true, name), "isolated channel opens");
    check(first.heartbeat(1, 60) && first.owns_consumer(), "first consumer claims channel");
    const OverlaySelection selected{2, 1u << 3, 42, 7};
    check(overlay_selected(selected, 42, 7, 3) && !overlay_selected(selected, 42, 8, 3) &&
              !overlay_selected(selected, 43, 7, 3) && !overlay_selected(selected, 42, 7, 4),
          "overlay selection is scoped to the exact producer session and selected slot");
    check(!overlay_selected({}, 42, 7, 3) && overlay_selected({1}, 42, 7, 3) &&
              !overlay_selected({1}, 42, 7, max_overlay_layers),
          "overlay selection defaults off and rejects out-of-range slots");
    check(first.select_overlays(selected) && !second.select_overlays({1}),
          "only owning consumer may set overlay capture selection");
    if (first.lock()) {
        check(first.data()->overlay_selection.mask == (1u << 3), "non-owner cannot overwrite overlay choice");
        auto &catalog = first.data()->overlay_catalog;
        catalog.producer_pid = 42;
        catalog.session = 7;
        catalog.updated_ms = GetTickCount64();
        catalog.count = 1;
        catalog.layers[0].slot = 3;
        catalog.layers[0].type = 1;
        catalog.layers[0].supported = 1;
        first.unlock();
    }
    check(second.overlay_catalog().layers[0].slot == 3 && !second.owns_consumer(),
          "catalog can be inspected without taking capture ownership");
    check(first.request_gaze(true), "owner requests gaze");
    check(second.gaze_requested(), "producer observes permission without taking mutex");
    check(first.request_gaze(false) && !second.gaze_requested(), "disable immediately revokes permission");
    check(first.request_gaze(true), "owner restores permission");
    check(!second.request_gaze(false), "non-owner cannot disable gaze");
    if (first.lock()) { check(first.data()->gaze_requested == 1, "gaze request retained"); first.unlock(); }
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
    if (second.lock()) {
        check(second.data()->overlay_selection.mode == 0, "capture handoff clears the previous consumer's overlay choices");
        check(second.data()->gaze_requested == 0, "handoff clears previous gaze request");
        second.unlock();
    }
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
    if (first.lock()) { first.data()->version = 3; first.unlock(); }
    Channel incompatible;
    check(!incompatible.open(true, name), "v4 consumer rejects an incompatible v3 mapping");
    if (first.lock()) { first.data()->version = ipc_version; first.unlock(); }
    return failures ? 1 : 0;
}
