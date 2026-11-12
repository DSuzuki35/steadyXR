#define DIRECTINPUT_VERSION 0x0800
#include "shortcut_bindings.hpp"
#include "theme.hpp"
#include <algorithm>
#include <dinput.h>
#include <objbase.h>
#include <wrl/client.h>

namespace vrst::preview {
namespace {
using Microsoft::WRL::ComPtr;
enum Control { actions_id = 6801, devices_id, refresh_id, learn_id, clear_id, close_id };

std::wstring guid_string(const GUID &id) {
    wchar_t text[40]{};
    StringFromGUID2(id, text, 40);
    return text;
}
struct DeviceInfo {
    GUID id{};
    std::wstring name;
};
struct InputDevice {
    GUID id{};
    ComPtr<IDirectInputDevice8W> handle;
    ULONGLONG retry{};
    bool connect(IDirectInput8W *input, HWND owner) {
        if (!input || FAILED(input->CreateDevice(id, &handle, nullptr)))
            return false;
        if (FAILED(handle->SetDataFormat(&c_dfDIJoystick2)) ||
            FAILED(handle->SetCooperativeLevel(owner, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)) ||
            FAILED(handle->Acquire())) {
            handle.Reset();
            return false;
        }
        return true;
    }
};
} // namespace

struct ShortcutController::Impl {
    HWND owner{}, window{}, actions{}, devices{}, assignment{}, feedback{};
    Theme *theme{};
    std::function<void(ShortcutAction)> trigger;
    std::unique_ptr<ShortcutBindings> bindings;
    ComPtr<IDirectInput8W> input;
    std::vector<InputDevice> watched;
    std::vector<DeviceInfo> available;
    ULONGLONG last_poll{};

    ShortcutAction action() const {
        return static_cast<ShortcutAction>(SendMessageW(actions, CB_GETCURSEL, 0, 0));
    }
    int selection() const {
        return int(SendMessageW(devices, CB_GETCURSEL, 0, 0));
    }
    void sync_devices() {
        const auto ids = bindings->devices();
        watched.erase(std::remove_if(watched.begin(), watched.end(),
                                     [&](const auto &device) {
                                         return std::none_of(ids.begin(), ids.end(), [&](const GUID &id) {
                                             return IsEqualGUID(device.id, id);
                                         });
                                     }),
                      watched.end());
        for (const auto &id : ids) {
            if (std::none_of(watched.begin(), watched.end(),
                             [&](const auto &d) { return IsEqualGUID(id, d.id); }))
                watched.push_back({id});
        }
    }
    void refresh_devices() {
        const auto old_selection = selection();
        GUID selected{};
        if (old_selection >= 0 && size_t(old_selection) < available.size())
            selected = available[old_selection].id;
        available.clear();
        SendMessageW(devices, CB_RESETCONTENT, 0, 0);
        if (input)
            input->EnumDevices(
                DI8DEVCLASS_GAMECTRL,
                [](const DIDEVICEINSTANCEW *d, void *out) -> BOOL {
                    static_cast<std::vector<DeviceInfo> *>(out)->push_back(
                        {d->guidInstance, d->tszInstanceName});
                    return DIENUM_CONTINUE;
                },
                &available, DIEDFL_ATTACHEDONLY);
        int index = available.empty() ? -1 : 0;
        for (size_t i = 0; i < available.size(); ++i) {
            const auto &d = available[i];
            auto label = d.name;
            if (std::count_if(available.begin(), available.end(),
                              [&](const auto &entry) { return entry.name == d.name; }) > 1)
                label += L" " + guid_string(d.id);
            SendMessageW(devices, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (IsEqualGUID(selected, d.id))
                index = int(i);
        }
        SendMessageW(devices, CB_SETCURSEL, index, 0);
    }
    void update_view() {
        const auto &bound = bindings->assignment(action());
        std::wstring text = L"Unassigned";
        if (bound) {
            const auto device = std::find_if(available.begin(), available.end(),
                                             [&](const auto &d) { return IsEqualGUID(bound->device, d.id); });
            text = L"Button " + std::to_wstring(bound->button + 1) + L"  /  " +
                   (device == available.end() ? guid_string(bound->device) : device->name);
            const auto connected = std::any_of(watched.begin(), watched.end(), [&](const auto &d) {
                return IsEqualGUID(bound->device, d.id) && d.handle;
            });
            if (!connected)
                text += L" (offline)";
        }
        SetWindowTextW(assignment, text.c_str());
        const auto status = bindings->message().empty() && available.empty()
                                ? L"Connect a joystick or wheel, then refresh the device list."
                                : bindings->message();
        SetWindowTextW(feedback, status.c_str());
        EnableWindow(GetDlgItem(window, clear_id), bound.has_value());
        EnableWindow(GetDlgItem(window, learn_id), selection() >= 0 || bindings->learning());
        SetWindowTextW(GetDlgItem(window, learn_id),
                       bindings->learning() ? L"Cancel learning" : L"Learn button");
    }
    void poll() {
        const auto now = GetTickCount64();
        if (!bindings || watched.empty() || now - last_poll < 20)
            return;
        last_poll = now;
        const bool was_learning = bindings->learning();
        bool devices_changed = false;
        ShortcutBindings::Actions triggered;
        for (auto &device : watched) {
            if (!device.handle) {
                if (now < device.retry)
                    continue;
                device.retry = now + 1000;
                if (!device.connect(input.Get(), owner))
                    continue;
                devices_changed = true;
            }
            DIJOYSTATE2 state{};
            if (FAILED(device.handle->Poll()) ||
                FAILED(device.handle->GetDeviceState(sizeof state, &state))) {
                device.handle.Reset();
                device.retry = now + 1000;
                bindings->disconnect(device.id);
                devices_changed = true;
                continue;
            }
            ShortcutBindings::Buttons down;
            for (size_t i = 0; i < down.size(); ++i)
                down.set(i, (state.rgbButtons[i] & 0x80) != 0);
            triggered |= bindings->sample(device.id, down);
        }
        const bool finished_learning = was_learning && !bindings->learning();
        if (finished_learning)
            sync_devices();
        if (window && IsWindowVisible(window) && (finished_learning || devices_changed))
            update_view();
        if (!was_learning && trigger)
            for (size_t i = 0; i < shortcut_count; ++i)
                if (triggered.test(i))
                    trigger(static_cast<ShortcutAction>(i));
    }
    void close() {
        bindings->cancel();
        sync_devices();
        ShowWindow(window, SW_HIDE);
    }
    void command(int id, int notification) {
        if ((id == actions_id || id == devices_id) && notification != CBN_SELCHANGE)
            return;
        switch (id) {
        case actions_id:
        case devices_id:
            bindings->cancel();
            break;
        case refresh_id:
            bindings->cancel();
            refresh_devices();
            break;
        case learn_id:
            if (bindings->learning())
                bindings->cancel();
            else if (const int i = selection(); i >= 0 && size_t(i) < available.size())
                bindings->learn(action(), available[i].id);
            break;
        case clear_id:
            bindings->clear(action());
            break;
        case close_id:
        case IDCANCEL:
            close();
            return;
        default:
            return;
        }
        sync_devices();
        update_view();
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        LRESULT themed{};
        if (self->theme->message(h, m, w, l, themed))
            return themed;
        switch (m) {
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            PAINTSTRUCT ps{};
            const auto dc = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            self->theme->surface(h, dc, {12, 12, 580, 360});
            if (m == WM_PAINT)
                EndPaint(h, &ps);
            return 0;
        }
        case WM_COMMAND:
            self->command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_CLOSE:
            self->close();
            return 0;
        case WM_NCDESTROY:
            self->window = nullptr;
            break;
        }
        return DefWindowProcW(h, m, w, l);
    }
    void create() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SteadyXRJoystickShortcuts";
        RegisterClassW(&wc);
        RECT rect{0, 0, 592, 372}, parent{};
        GetWindowRect(owner, &parent);
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&rect, style, FALSE, WS_EX_CONTROLPARENT);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"SteadyXR - Joystick shortcuts",
                                 style, parent.left + 40, parent.top + 40, rect.right - rect.left,
                                 rect.bottom - rect.top, owner, nullptr, wc.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        auto child = [&](const wchar_t *kind, const wchar_t *name, DWORD style, int id, int x, int y,
                         int width, int height) {
            auto h =
                CreateWindowExW(0, kind, name, WS_CHILD | WS_VISIBLE | theme->style(kind, style), x, y, width,
                                height, window, reinterpret_cast<HMENU>(INT_PTR(id)), wc.hInstance, nullptr);
            theme->control(h, kind);
            return h;
        };
        child(L"STATIC", L"Action", 0, 0, 32, 26, 520, 23);
        actions =
            child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, actions_id, 32, 52, 528, 220);
        for (size_t i = 0; i < shortcut_count; ++i)
            SendMessageW(actions, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(shortcut_name(static_cast<ShortcutAction>(i))));
        SendMessageW(actions, CB_SETCURSEL, 0, 0);
        assignment = child(L"STATIC", L"", SS_NOPREFIX, 0, 32, 90, 528, 43);
        child(L"STATIC", L"Joystick or wheel", 0, 0, 32, 139, 390, 23);
        devices = child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, devices_id, 32, 166,
                        408, 220);
        child(L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, refresh_id, 448, 164, 112, 34);
        child(L"BUTTON", L"Learn button", WS_TABSTOP | BS_PUSHBUTTON, learn_id, 32, 216, 196, 36);
        child(L"BUTTON", L"Clear assignment", WS_TABSTOP | BS_PUSHBUTTON, clear_id, 240, 216, 196, 36);
        feedback = child(L"STATIC", L"", SS_NOPREFIX, 0, 32, 264, 528, 44);
        child(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, close_id, 448, 312, 112, 34);
        theme->scale_window(window);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            theme->fit_window(window, monitor.rcWork);
    }
};

ShortcutController::ShortcutController() : impl_(std::make_unique<Impl>()) {}
ShortcutController::~ShortcutController() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void ShortcutController::initialize(HWND owner, Theme &theme, const std::filesystem::path &preview_ini,
                                    std::function<void(ShortcutAction)> trigger) {
    auto &p = *impl_;
    p.owner = owner;
    p.theme = &theme;
    p.trigger = std::move(trigger);
    p.bindings = std::make_unique<ShortcutBindings>(preview_ini.parent_path() / L"SteadyXR-shortcuts.ini");
    DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                       reinterpret_cast<void **>(p.input.ReleaseAndGetAddressOf()), nullptr);
    p.sync_devices();
}
void ShortcutController::open() {
    auto &p = *impl_;
    if (!p.bindings)
        return;
    if (!p.window)
        p.create();
    if (!p.window)
        return;
    if (!IsWindowVisible(p.window)) {
        p.refresh_devices();
        p.update_view();
        ShowWindow(p.window, SW_SHOW);
        SetFocus(p.actions);
    }
    SetForegroundWindow(p.window);
}
void ShortcutController::tick() {
    impl_->poll();
}
bool ShortcutController::route(MSG &msg) {
    return impl_->window && IsWindowVisible(impl_->window) && IsDialogMessageW(impl_->window, &msg);
}
} // namespace vrst::preview
