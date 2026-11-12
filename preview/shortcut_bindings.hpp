#pragma once
#include "shortcuts.hpp"
#include <array>
#include <bitset>
#include <optional>
#include <string>
#include <vector>

namespace vrst::preview {
inline constexpr size_t shortcut_count = static_cast<size_t>(ShortcutAction::count);
const wchar_t *shortcut_name(ShortcutAction);

struct JoystickButton {
    GUID device{};
    unsigned button{};
};

class ShortcutBindings {
  public:
    using Buttons = std::bitset<128>;
    using Actions = std::bitset<shortcut_count>;
    explicit ShortcutBindings(std::filesystem::path file);
    const std::optional<JoystickButton> &assignment(ShortcutAction action) const;
    void learn(ShortcutAction, const GUID &);
    void cancel();
    void clear(ShortcutAction);
    bool learning() const;
    const std::wstring &message() const;
    std::vector<GUID> devices() const;
    void disconnect(const GUID &);
    Actions sample(const GUID &, const Buttons &down);

  private:
    struct DeviceState {
        GUID id;
        std::optional<Buttons> previous;
    };
    std::filesystem::path file_;
    std::array<std::optional<JoystickButton>, shortcut_count> assigned_{};
    std::optional<ShortcutAction> learning_;
    GUID learning_device_{};
    std::vector<DeviceState> states_;
    std::wstring message_;
    void update_devices();
    bool save(ShortcutAction, std::optional<JoystickButton>);
};
} // namespace vrst::preview
