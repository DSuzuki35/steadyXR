#include "shortcut_bindings.hpp"
#include <iostream>
#include <stdexcept>

using namespace vrst::preview;
using A = ShortcutAction;
using Buttons = ShortcutBindings::Buttons;

void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}

int main() try {
    const auto file = std::filesystem::temp_directory_path() /
                      (L"SteadyXR-shortcuts-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    std::filesystem::remove(file);
    const GUID wheel{0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    const GUID stick{0x87654321, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    const auto held = Buttons{}.set(127);
    ShortcutBindings bindings(file);
    check(bindings.devices().empty(), "polling is idle by default");
    bindings.learn(A::center_camera, wheel);
    check(bindings.devices().size() == 1, "learning must watch the selected device");
    bindings.sample(wheel, held);
    bindings.sample(wheel, held);
    check(!bindings.assignment(A::center_camera), "ignore button held at learn start");
    bindings.sample(wheel, {});
    check(bindings.sample(wheel, held).none(), "learning must consume press");
    check(bindings.assignment(A::center_camera) && bindings.assignment(A::center_camera)->button == 127,
          "button 128 must be assignable");
    check(bindings.sample(wheel, held).none(), "held press cannot repeat");
    bindings.sample(wheel, {});
    check(bindings.sample(wheel, held).test(static_cast<size_t>(A::center_camera)), "fresh press fires");
    bindings.disconnect(wheel);
    check(bindings.sample(wheel, held).none(), "held reconnect must not fire");

    bindings.learn(A::compare, stick);
    bindings.sample(stick, {});
    bindings.sample(stick, held);
    check(bindings.assignment(A::compare).has_value(), "same button on another device is independent");
    bindings.learn(A::compare, wheel);
    bindings.sample(wheel, {});
    check(bindings.sample(wheel, held).none(), "duplicate learn must not dispatch old action");
    check(!bindings.learning() && bindings.message().find(L"Center camera") != std::wstring::npos,
          "duplicate reports the action already using the button");
    check(IsEqualGUID(bindings.assignment(A::compare)->device, stick), "conflict retains earlier binding");
    bindings.learn(A::compare, wheel);
    bindings.cancel();
    check(IsEqualGUID(bindings.assignment(A::compare)->device, stick), "cancel retains previous binding");

    ShortcutBindings restored(file);
    check(restored.devices().size() == 2 && restored.assignment(A::compare)->button == 127,
          "distinct device/button bindings must survive restart");
    restored.clear(A::center_camera);
    ShortcutBindings cleared(file);
    check(cleared.devices().size() == 1 && !cleared.assignment(A::center_camera),
          "clear the selected action persistently");
    check(cleared.assignment(A::compare).has_value(), "clearing one must preserve other actions");

    WritePrivateProfileStringW(L"Joystick", L"CenterCamera", L"{12345678-1234-1234-0102-030405060708},128",
                               file.c_str());
    WritePrivateProfileStringW(L"Joystick", L"NextPreset", L"{12345678-1234-1234-0102-030405060708},-1",
                               file.c_str());
    WritePrivateProfileStringW(L"Joystick", L"LearnChat", L"invalid,5", file.c_str());
    ShortcutBindings malformed(file);
    check(!malformed.assignment(A::center_camera) && !malformed.assignment(A::next_preset) &&
              !malformed.assignment(A::learn_chat),
          "skip malformed persisted records");

    ShortcutBindings unwritable(file / L"settings.ini");
    unwritable.learn(A::learn_chat, wheel);
    unwritable.sample(wheel, {});
    unwritable.sample(wheel, held);
    check(!unwritable.assignment(A::learn_chat) && !unwritable.message().empty(),
          "write failure cannot be reported as a saved assignment");
    std::filesystem::remove(file);
    std::cout << "Joystick shortcut tests passed\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
}
