#include "shortcut_bindings.hpp"
#include <algorithm>
#include <cwchar>
#include <objbase.h>

namespace vrst::preview {
namespace {
struct ActionInfo {
    const wchar_t *key, *name;
};
constexpr ActionInfo actions[]{
    {L"CenterCamera", L"Center camera"}, {L"EditFraming", L"Capture full eye"},
    {L"PauseResume", L"Pause / resume"}, {L"NextPreset", L"Next preset"},
    {L"Compare", L"Compare A/B"},        {L"LearnChat", L"Learn chat position"}};
static_assert(std::size(actions) == shortcut_count);

std::optional<JoystickButton> read_binding(const std::filesystem::path &file, const wchar_t *key) {
    wchar_t text[64]{};
    const auto length = GetPrivateProfileStringW(L"Joystick", key, L"", text, 64, file.c_str());
    // A braced GUID, a comma, and a zero-based index in [0, 127].
    if (length < 40 || length > 42 || text[0] != L'{' || text[37] != L'}' || text[38] != L',')
        return {};
    text[38] = 0;
    JoystickButton binding;
    if (FAILED(CLSIDFromString(text, &binding.device)) || IsEqualGUID(binding.device, GUID_NULL))
        return {};
    for (auto c = text + 39; *c; ++c) {
        if (*c < L'0' || *c > L'9')
            return {};
        binding.button = binding.button * 10 + unsigned(*c - L'0');
    }
    return binding.button < 128 ? std::optional<JoystickButton>(binding) : std::nullopt;
}
bool same_button(const std::optional<JoystickButton> &a, const JoystickButton &b) {
    return a && IsEqualGUID(a->device, b.device) && a->button == b.button;
}
} // namespace

const wchar_t *shortcut_name(ShortcutAction action) {
    return actions[static_cast<size_t>(action)].name;
}

ShortcutBindings::ShortcutBindings(std::filesystem::path file) : file_(std::move(file)) {
    for (size_t i = 0; i < shortcut_count; ++i) {
        const auto binding = read_binding(file_, actions[i].key);
        if (binding && std::none_of(assigned_.begin(), assigned_.end(),
                                    [&](const auto &entry) { return same_button(entry, *binding); }))
            assigned_[i] = binding;
    }
    update_devices();
}
const std::optional<JoystickButton> &ShortcutBindings::assignment(ShortcutAction action) const {
    return assigned_[static_cast<size_t>(action)];
}
void ShortcutBindings::learn(ShortcutAction action, const GUID &id) {
    learning_ = action;
    learning_device_ = id;
    message_ = L"Release the button, then press it to assign it.";
    update_devices();
    disconnect(id);
}
void ShortcutBindings::cancel() {
    learning_.reset();
    message_.clear();
    update_devices();
}
void ShortcutBindings::clear(ShortcutAction action) {
    cancel();
    if (save(action, {}))
        message_ = L"Assignment cleared.";
}
bool ShortcutBindings::learning() const {
    return learning_.has_value();
}
const std::wstring &ShortcutBindings::message() const {
    return message_;
}

std::vector<GUID> ShortcutBindings::devices() const {
    std::vector<GUID> ids;
    for (auto &state : states_)
        ids.push_back(state.id);
    return ids;
}
void ShortcutBindings::update_devices() {
    auto old = std::move(states_);
    states_.clear();
    auto keep = [&](const GUID &id) {
        auto matches = [&](const auto &state) { return IsEqualGUID(state.id, id); };
        if (std::any_of(states_.begin(), states_.end(), matches))
            return;
        const auto existing = std::find_if(old.begin(), old.end(), matches);
        states_.push_back(existing == old.end() ? DeviceState{id, {}} : *existing);
    };
    for (const auto &entry : assigned_)
        if (entry)
            keep(entry->device);
    if (learning_)
        keep(learning_device_);
}
void ShortcutBindings::disconnect(const GUID &id) {
    for (auto &state : states_)
        if (IsEqualGUID(state.id, id))
            state.previous.reset();
}
bool ShortcutBindings::save(ShortcutAction action, std::optional<JoystickButton> binding) {
    const auto index = static_cast<size_t>(action);
    std::wstring value;
    if (binding) {
        wchar_t guid[40]{};
        StringFromGUID2(binding->device, guid, 40);
        value = std::wstring(guid) + L"," + std::to_wstring(binding->button);
    }
    if (!WritePrivateProfileStringW(L"Joystick", actions[index].key, binding ? value.c_str() : nullptr,
                                    file_.c_str())) {
        message_ = L"Could not save the assignment. Check access to the settings folder.";
        return false;
    }
    assigned_[index] = binding;
    update_devices();
    return true;
}
ShortcutBindings::Actions ShortcutBindings::sample(const GUID &id, const Buttons &down) {
    Actions triggered;
    const auto state = std::find_if(states_.begin(), states_.end(),
                                    [&](const auto &entry) { return IsEqualGUID(entry.id, id); });
    if (state == states_.end())
        return triggered;
    const auto pressed = state->previous ? down & ~*state->previous : Buttons{};
    state->previous = down;
    if (pressed.none())
        return triggered;
    if (learning_) {
        if (!IsEqualGUID(learning_device_, id))
            return triggered;
        size_t button{};
        while (!pressed.test(button))
            ++button;
        const JoystickButton chosen{id, static_cast<unsigned>(button)};
        const auto action = *learning_;
        cancel();
        for (size_t i = 0; i < shortcut_count; ++i) {
            if (static_cast<ShortcutAction>(i) != action && same_button(assigned_[i], chosen)) {
                message_ = std::wstring(L"This button already controls ") + actions[i].name +
                           L". Clear that assignment first.";
                return triggered;
            }
        }
        if (save(action, chosen))
            message_ = L"Assignment saved.";
        return triggered;
    }
    for (size_t i = 0; i < shortcut_count; ++i)
        if (assigned_[i] && IsEqualGUID(assigned_[i]->device, id) && pressed.test(assigned_[i]->button))
            triggered.set(i);
    return triggered;
}
} // namespace vrst::preview
