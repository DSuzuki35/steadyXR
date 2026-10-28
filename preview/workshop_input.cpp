#define DIRECTINPUT_VERSION 0x0800
#include "workshop_input.hpp"
#include <array>
#include <dinput.h>
#include <wrl/client.h>
namespace vrst::preview {
using Microsoft::WRL::ComPtr;
struct WorkshopInput::Impl {
    HWND window{}, owner{};
    std::filesystem::path ini;
    std::function<void()> trigger;
    ComPtr<IDirectInput8W> input;
    ComPtr<IDirectInputDevice8W> device;
    GUID selected{};
    int button{-1};
    bool learning{}, registered{};
    unsigned mods{}, vk{};
    ULONGLONG retry{};
    ButtonEdge edge;
    std::array<ButtonEdge, 128> learning_edges;
    std::wstring message{L"No wheel button assigned"}, hotkey_error;
    void save() {
        wchar_t guid[64]{};
        StringFromGUID2(selected, guid, 64);
        WritePrivateProfileStringW(L"Workshop", L"Device", guid, ini.c_str());
        WritePrivateProfileStringW(L"Workshop", L"Button", std::to_wstring(button).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Workshop", L"Modifiers", std::to_wstring(mods).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Workshop", L"Key", std::to_wstring(vk).c_str(), ini.c_str());
    }
    void connect() {
        device.Reset();
        edge.disconnect();
        for (auto &e : learning_edges)
            e.disconnect();
        if (!input || IsEqualGUID(selected, GUID_NULL))
            return;
        if (FAILED(input->CreateDevice(selected, &device, nullptr)))
            return;
        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(device->SetCooperativeLevel(owner, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            device.Reset();
            return;
        }
        device->Acquire();
    }
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto p = reinterpret_cast<Impl *>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(b)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (p && m == WM_HOTKEY && a == 1) {
            if (p->trigger)
                p->trigger();
            return 0;
        }
        return DefWindowProcW(w, m, a, b);
    }
};
WorkshopInput::WorkshopInput() : impl_(new Impl) {}
WorkshopInput::~WorkshopInput() {
    auto &p = *impl_;
    if (p.registered)
        UnregisterHotKey(p.window, 1);
    if (p.device)
        p.device->Unacquire();
    if (p.window)
        DestroyWindow(p.window);
}
void WorkshopInput::initialize(HWND owner, const std::filesystem::path &ini, std::function<void()> trigger) {
    auto &p = *impl_;
    p.owner = owner;
    p.ini = ini;
    p.trigger = std::move(trigger);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Impl::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SteadyXRWorkshopInput";
    RegisterClassW(&wc);
    p.window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &p);
    DirectInput8Create(wc.hInstance, DIRECTINPUT_VERSION, IID_IDirectInput8W,
                       reinterpret_cast<void **>(p.input.GetAddressOf()), nullptr);
    wchar_t guid[64]{};
    GetPrivateProfileStringW(L"Workshop", L"Device", L"", guid, 64, ini.c_str());
    CLSIDFromString(guid, &p.selected);
    p.button = static_cast<int>(GetPrivateProfileIntW(L"Workshop", L"Button", -1, ini.c_str()));
    if (p.button < 0 || p.button >= 128) {
        p.button = -1;
        p.selected = GUID_NULL;
    }
    hotkey(GetPrivateProfileIntW(L"Workshop", L"Modifiers", MOD_CONTROL | MOD_ALT, ini.c_str()),
           GetPrivateProfileIntW(L"Workshop", L"Key", 0, ini.c_str()));
    p.connect();
}
bool WorkshopInput::hotkey(unsigned modifiers, unsigned key) {
    auto &p = *impl_;
    modifiers &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if (p.registered)
        UnregisterHotKey(p.window, 1);
    p.registered = false;
    p.mods = modifiers;
    p.vk = key <= 255 ? key : 0;
    p.hotkey_error.clear();
    if (p.vk) {
        p.registered = RegisterHotKey(p.window, 1, p.mods | MOD_NOREPEAT, p.vk) != FALSE;
        if (!p.registered)
            p.hotkey_error =
                L"Shortcut unavailable: another application already uses it. Choose a different shortcut.";
    }
    p.save();
    return !p.vk || p.registered;
}
std::vector<WorkshopInput::Device> WorkshopInput::devices() {
    std::vector<Device> out;
    if (!impl_->input)
        return out;
    impl_->input->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        [](const DIDEVICEINSTANCEW *d, void *data) -> BOOL {
            static_cast<std::vector<Device> *>(data)->push_back({d->guidInstance, d->tszInstanceName});
            return DIENUM_CONTINUE;
        },
        &out, DIEDFL_ATTACHEDONLY);
    return out;
}
void WorkshopInput::learn(const GUID &id) {
    auto &p = *impl_;
    p.selected = id;
    p.button = -1;
    p.learning = true;
    p.message = L"Release buttons, then press the recording button on your wheel.";
    p.connect();
}
void WorkshopInput::clear_button() {
    auto &p = *impl_;
    p.learning = false;
    p.button = -1;
    p.selected = GUID_NULL;
    p.device.Reset();
    p.message = L"No wheel button assigned";
    p.save();
}
void WorkshopInput::poll() {
    auto &p = *impl_;
    if (!p.learning && p.button < 0)
        return;
    if (!p.device) {
        if (GetTickCount64() - p.retry < 1000)
            return;
        p.retry = GetTickCount64();
        p.connect();
        if (!p.device) {
            p.message = L"Assigned wheel disconnected; waiting to reconnect.";
            return;
        }
    }
    DIJOYSTATE2 state{};
    p.device->Poll();
    if (FAILED(p.device->GetDeviceState(sizeof state, &state))) {
        p.edge.disconnect();
        for (auto &e : p.learning_edges)
            e.disconnect();
        p.device.Reset();
        p.message = L"Wheel unavailable; waiting to reconnect.";
        return;
    }
    if (p.learning) {
        for (unsigned i = 0; i < 128; ++i)
            if (p.learning_edges[i].update((state.rgbButtons[i] & 0x80) != 0)) {
                p.button = i;
                p.learning = false;
                p.edge.disconnect();
                p.save();
                break;
            }
    }
    if (!p.learning) {
        p.message = L"Wheel button " + std::to_wstring(p.button + 1) + L" assigned";
        if (p.edge.update((state.rgbButtons[p.button] & 0x80) != 0) && p.trigger)
            p.trigger();
    }
}
std::wstring WorkshopInput::status() const {
    auto &p = *impl_;
    return p.hotkey_error.empty() ? p.message : p.hotkey_error;
}
unsigned WorkshopInput::modifiers() const {
    return impl_->mods;
}
unsigned WorkshopInput::key() const {
    return impl_->vk;
}
} // namespace vrst::preview
