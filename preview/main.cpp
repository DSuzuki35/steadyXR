#define UNICODE
#define _UNICODE
#include "advanced.hpp"
#include "build_expiry.hpp"
#include "connection.hpp"
#include "overlays.hpp"
#include "presets.hpp"
#include "render_settings.hpp"
#include "renderer.hpp"
#include "settings_path.hpp"
#include "spectator_controls.hpp"
#include "spectator_settings.hpp"
#include "director_settings_io.hpp"
#include "theme.hpp"
#include "visual_settings_io.hpp"
#include "window_capture.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mmsystem.h>
#include <shellapi.h>
#include <sddl.h>
#include <string>

namespace {
using namespace vrst::preview;
constexpr int setup_id = 101, remove_id = 102, show_id = 103, pause_id = 104, reset_id = 105, smooth_id = 106,
              crop_id = 107, mirror_id = 108, eye_id = 109, fps_id = 110, bypass_id = 111, mode_id = 112,
              source_id = 113, refresh_id = 114, left_id = 115, right_id = 116, top_id = 117, bottom_id = 118,
              horizon_id = 119, mirror_limit_id = 120, aspect_id = 121, width_id = 122, height_id = 123,
              apply_id = 124, auto_fullscreen_id = 125, monitor_id = 126, adaptive_horizon_id = 127,
              mirror_softness_id = 128, comfort_id = 129, undo_comfort_id = 130, advanced_id = 131,
              presets_id = 132, layers_id = 133, compare_id = 134, next_preset_id = 135, hotkeys_id = 136,
              compact_status_id = 137, overlays_id = 138, spectator_id = 139, learn_chat_id = 140;
constexpr int hotkeys_default_id = 2100, hotkeys_alternate_id = 2101, hotkeys_off_id = 2102;
constexpr int navigation_id = 1500;
constexpr int comparison_original_id = 2110, comparison_split_id = 2111, comparison_side_by_side_id = 2112;
constexpr UINT_PTR save_timer = 4101, comparison_timer = 4102;
constexpr int hotkey_actions[]{reset_id, pause_id, next_preset_id, compare_id, learn_chat_id};
constexpr wchar_t hotkey_letters[]{L'R', L'P', L'N', L'B', L'C'};
std::wstring executable_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool okay = process && QueryFullProcessImageNameW(process, 0, path, &length);
    if (process)
        CloseHandle(process);
    return okay ? path : L"";
}
std::string read_bytes(const std::wstring &path) {
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
bool write_bytes(const std::wstring &path, const std::string &bytes) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), std::streamsize(bytes.size()));
    out.close();
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return !out.fail();
}
std::wstring decimal(double value) {
    wchar_t text[32]{};
    std::swprintf(text, 32, L"%.1f", value);
    return text;
}
std::wstring current_user_sid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    LPWSTR text{};
    const bool okay = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) &&
                      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &text);
    const std::wstring sid = okay ? text : L"";
    if (text)
        LocalFree(text);
    CloseHandle(token);
    return sid;
}
bool process_elevated() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size{};
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                          elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}
LONG launch_connection_helper(HWND owner, bool enable, const std::wstring &sid, HANDLE &process) {
    if (sid.empty())
        return ERROR_INVALID_SID;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768))
        return GetLastError();
    const auto arguments = std::wstring(enable ? L"--install-connection " : L"--remove-connection ") + sid;
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = executable;
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute))
        return GetLastError();
    process = execute.hProcess;
    return process ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}
struct App {
    HWND controls{}, output{}, status{}, smooth_label{}, crop_label{}, smooth{}, crop{}, mirror{}, bypass{},
        eye{}, eye_label{}, fps{}, pause{}, mode{}, source{}, trims[4]{}, trim_labels[4]{}, horizon{},
        horizon_label{}, mirror_limit{}, mirror_limit_label{}, aspect{}, output_width{}, output_height{},
        auto_fullscreen{}, monitor{}, adaptive_horizon{}, mirror_softness{}, mirror_softness_label{},
        compact_status{}, comparison{}, shortcut_hint{};
    HFONT font{};
    Theme theme;
    std::vector<HWND> control_pages[4];
    void show_page(unsigned selected) {
        const auto focus = GetFocus();
        for (unsigned p = 0; p < std::size(control_pages); ++p)
            if (p != selected && focus &&
                std::find(control_pages[p].begin(), control_pages[p].end(), focus) != control_pages[p].end())
                SetFocus(GetDlgItem(controls, navigation_id + selected));
        for (unsigned p = 0; p < std::size(control_pages); ++p) {
            for (auto h : control_pages[p])
                ShowWindow(h, p == selected ? SW_SHOW : SW_HIDE);
            const auto tab = GetDlgItem(controls, navigation_id + p);
            SendMessageW(tab, BM_SETCHECK, p == selected ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(tab, nullptr, FALSE);
        }
    }
    AdvancedSettings advanced;
    PresetsDialog presets;
    OverlayPanel overlays;
    SpectatorControls spectator_controls;
    HANDLE layer_editor_process{};
    HANDLE connection_process{};
    bool connection_enable{};
    Channel channel;
    Renderer renderer;
    WindowCapture capture;
    std::vector<CaptureWindow> sources;
    HWND source_window{};
    DWORD source_pid{};
    std::wstring source_executable, source_title;
    bool window_mode{true};
    std::uint64_t last_capture_start{};
    Options options;
    Options before_comfort;
    bool can_undo_comfort{};
    bool paused{}, demo{}, fullscreen{}, dirty{true}, quit{}, timer_resolution{}, expired{};
    bool start_fullscreen{true}, diagnostic{}, compatible_connection{};
    bool capture_conflict{};
    unsigned aspect_numerator{16}, aspect_denominator{9};
    std::wstring monitor_name;
    std::vector<MONITORINFOEXW> monitors;
    unsigned selected_eye{}, rate{60};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    std::wstring directory, ini, render_ini, setup_message;
    std::filesystem::path presets_directory;
    std::wstring preset_cursor, hotkey_error;
    unsigned hotkey_profile{1}; // 0 off, 1 Ctrl+Alt, 2 Ctrl+Alt+Shift.
    bool hotkey_registered[5]{};
    std::uint64_t compare_until{};
    std::uint64_t start_ms{}, last_received_ms{}, last_sequence{}, fps_frames{}, fps_start{};
    double measured_fps{};
    ~App() {
        unregister_hotkeys();
        if (layer_editor_process)
            CloseHandle(layer_editor_process);
        if (connection_process)
            CloseHandle(connection_process);
    }
    void unregister_hotkeys() {
        for (unsigned i = 0; i < std::size(hotkey_actions); ++i) {
            if (hotkey_registered[i])
                UnregisterHotKey(controls, hotkey_actions[i]);
            hotkey_registered[i] = false;
        }
    }
    void register_hotkeys() {
        unregister_hotkeys();
        hotkey_error.clear();
        if (!hotkey_profile || diagnostic)
            return;
        const UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT | (hotkey_profile == 2 ? MOD_SHIFT : 0);
        for (unsigned i = 0; i < std::size(hotkey_actions); ++i) {
            hotkey_registered[i] =
                RegisterHotKey(controls, hotkey_actions[i], modifiers, hotkey_letters[i]) != FALSE;
            if (!hotkey_registered[i]) {
                if (!hotkey_error.empty())
                    hotkey_error += L", ";
                hotkey_error += hotkey_letters[i];
            }
        }
        if (!hotkey_error.empty())
            hotkey_error =
                L"Hotkeys unavailable: " + hotkey_error + L". In Hotkeys, choose the alternate keys or Off.";
    }
    std::wstring hotkey_hint() const {
        if (!hotkey_profile)
            return L"Global hotkeys are off. F11: fullscreen.";
        return std::wstring(hotkey_profile == 2 ? L"Ctrl+Alt+Shift: " : L"Ctrl+Alt: ") +
               L"R reset | P pause | N next preset | B compare | C learn chat";
    }
    void configure_hotkeys(unsigned profile) {
        hotkey_profile = profile;
        register_hotkeys();
        save();
        update_status();
    }
    void hotkeys_menu() {
        const auto menu = CreatePopupMenu();
        if (!menu)
            return;
        AppendMenuW(menu, MF_STRING | (hotkey_profile == 1 ? MF_CHECKED : 0), hotkeys_default_id,
                    L"Ctrl+Alt + R / P / N / B / C (learn chat)");
        AppendMenuW(menu, MF_STRING | (hotkey_profile == 2 ? MF_CHECKED : 0), hotkeys_alternate_id,
                    L"Ctrl+Alt+Shift + R / P / N / B / C (alternate)");
        AppendMenuW(menu, MF_STRING | (!hotkey_profile ? MF_CHECKED : 0), hotkeys_off_id,
                    L"Off — disable global hotkeys");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const wchar_t *comparison_labels[]{
            options.compare_effects ? L"A/B: Visual effects bypass (20s)" : L"A/B: Original (20s)",
            options.compare_effects ? L"A/B: 50/50 split - effects / bypass"
                                    : L"A/B: 50/50 split — left stabilized, right original",
            options.compare_effects ? L"A/B: Side by side - effects / bypass"
                                    : L"A/B: Side by side — left stabilized, right original"};
        for (unsigned i = 0; i < 3; ++i)
            AppendMenuW(menu, MF_STRING | (options.compare_mode == i ? MF_CHECKED : 0),
                        comparison_original_id + i, comparison_labels[i]);
        RECT bounds{};
        GetWindowRect(GetDlgItem(controls, hotkeys_id), &bounds);
        const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, bounds.left, bounds.bottom,
                                             0, controls, nullptr);
        DestroyMenu(menu);
        if (selected)
            SendMessageW(controls, WM_COMMAND, selected, 0);
    }
    bool comparing(std::uint64_t now) const {
        return compare_until && now < compare_until && !paused && !expired;
    }
    Options draw_options(std::uint64_t now) const {
        auto result = options;
        result.compare_original = comparing(now);
        return result;
    }
    void cancel_comparison() {
        if (compare_until) {
            KillTimer(controls, comparison_timer);
            compare_until = 0;
            dirty = true;
        }
    }
    void refresh_comparison(std::uint64_t now) {
        if (compare_until && (!comparing(now) || waiting(now))) {
            cancel_comparison();
            update_status();
        }
    }
    void toggle_comparison() {
        const auto now = GetTickCount64();
        if (comparing(now))
            cancel_comparison();
        else if (!paused && !expired && !waiting(now)) {
            compare_until = now + 20000;
            SetTimer(controls, comparison_timer, 100, nullptr);
            dirty = true;
        } else {
            cancel_comparison();
            setup_message = L"Start or resume a live preview for a 20-second A/B comparison.";
        }
        update_status();
    }
    void reset_horizon() {
        cancel_comparison();
        renderer.reset_horizon();
        dirty = true;
        setup_message =
            window_mode && !demo
                ? L"Horizon reset is ready for OpenXR pose capture. Hold the headset approximately upright."
                : L"Horizon reset. Hold the headset approximately upright when resetting.";
        update_status();
    }
    void learn_chat() {
        if (window_mode || demo || !active() || options.spectator.bypass) {
            setup_message = L"Chat learning needs an active OpenXR preview and spectator bypass off.";
        } else {
            renderer.learn_chat();
            heartbeat();
            setup_message = L"Look at chat and hold your gaze steady during the countdown and learning.";
        }
        update_status();
    }
    void next_preset() {
        PresetStore store(presets_directory);
        std::wstring error;
        const auto saved = store.list(error);
        if (!error.empty()) {
            setup_message = error;
            update_status();
            return;
        }
        std::vector<std::wstring> entries{L"builtin:natural", L"builtin:comfort", L"builtin:strong"};
        for (const auto &name : saved)
            entries.push_back(L"saved:" + name);
        const auto previous = std::find(entries.begin(), entries.end(), preset_cursor);
        const auto next = previous == entries.end()
                              ? 0u
                              : unsigned((std::distance(entries.begin(), previous) + 1) % entries.size());
        if (next < 3) {
            const BuiltinPreset builtins[]{BuiltinPreset::natural, BuiltinPreset::stream_comfort,
                                           BuiltinPreset::strong_leveling};
            builtin(builtins[next]);
        } else {
            const auto &name = saved[next - 3];
            const auto bytes = store.read(name, error);
            if (!bytes || !apply_snapshot(*bytes)) {
                setup_message = error.empty()
                                    ? L"Could not apply the next preset. Check access to the settings folder."
                                    : error;
                update_status();
                return;
            }
            preset_cursor = entries[next];
            setup_message = L"Preset: " + name + L". Changed render settings apply at the next game launch.";
            update_status();
        }
    }
    void refresh_editor_connection() {
        const auto old_ini = render_ini;
        resolve_connection();
        if (old_ini != render_ini)
            advanced.reload(render_ini);
        labels();
        update_status();
    }
    void open_layer_editor() {
        if (diagnostic)
            return;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const auto executable = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        const auto script = directory + L"\\layers\\LayerEditor.ps1";
        const auto arguments =
            L"-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File \"" + script + L"\"";
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        execute.hwnd = controls;
        execute.lpFile = executable.c_str();
        execute.lpParameters = arguments.c_str();
        execute.nShow = SW_HIDE;
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(script, file_error) || !ShellExecuteExW(&execute)) {
            setup_message =
                L"Could not open the layer editor. Keep the layers folder beside the preview app.";
            update_status();
            return;
        }
        if (layer_editor_process)
            CloseHandle(layer_editor_process);
        layer_editor_process = execute.hProcess;
    }
    bool waiting(std::uint64_t now) const {
        return expired || capture_conflict || !renderer.has_frame() ||
               (!paused && last_received_ms && now >= last_received_ms && now - last_received_ms >= 2000);
    }
    const wchar_t *waiting_text() const {
        return expired            ? L"Build expired. Please install a newer SteadyXR build."
               : capture_conflict ? L"OpenXR capture is in use by OBS or another preview."
               : window_mode      ? L"Waiting for window capture…"
                                  : L"Waiting for OpenXR capture…";
    }
    void refresh_expiry() {
        if (!expired && build_expired()) {
            expired = true;
            cancel_comparison();
            dirty = true;
            heartbeat();
            labels();
            InvalidateRect(output, nullptr, FALSE);
        }
    }
    bool registered(bool any_version = false) const {
        return compatible_connection || layer_installed(directory, any_version);
    }
    void resolve_connection() {
        const auto installed =
            diagnostic ? std::optional<std::wstring>{} : compatible_layer_settings_path(directory);
        compatible_connection = installed.has_value();
        render_ini = installed ? *installed : ini;
    }
    void connection(bool enable) {
        if (enable && expired) {
            setup_message = L"This build expired. Install a newer SteadyXR build to enable capture.";
            return;
        }
        if (diagnostic || connection_process)
            return;
        // Preserve the active capture settings when migrating an identical DLL from
        // another release directory; the viewer's own preferences remain in this INI.
        const auto previous_settings = installed_layer_settings_path();
        if (enable && previous_settings && *previous_settings != ini &&
            GetFileAttributesW(previous_settings->c_str()) != INVALID_FILE_ATTRIBUTES &&
            !write_render_settings(ini, read_render_settings(*previous_settings))) {
            setup_message = L"Could not preserve existing render settings. The connection was not changed.";
            return;
        }
        auto result = set_layer_connection(directory, enable);
        if ((result == ERROR_ACCESS_DENIED || result == ERROR_PRIVILEGE_NOT_HELD) && !process_elevated()) {
            result = launch_connection_helper(controls, enable, current_user_sid(), connection_process);
            if (result == ERROR_SUCCESS) {
                connection_enable = enable;
                setup_message = L"Applying the system-wide OpenXR connection...";
                labels();
                return;
            }
        }
        connection_finished(enable, result);
    }
    void connection_finished(bool enable, LONG result) {
        if (result == ERROR_SUCCESS) {
            resolve_connection();
            advanced.reload(render_ini);
        }
        setup_message = result == ERROR_SUCCESS
                            ? (enable ? L"System-wide OpenXR connection installed. Restart the game to use it."
                                      : L"System-wide connection disabled. Restart the game to apply it.")
                        : result == ERROR_CANCELLED
                            ? L"Administrator approval was cancelled. The connection was not changed."
                            : L"Could not change the system-wide connection. Windows error " + std::to_wstring(result);
        labels();
        update_status();
    }
    void queue_save() {
        // Keep disk I/O out of mouse tracking, especially inside synced folders.
        SetTimer(controls, save_timer, 250, nullptr);
    }
    bool save() {
        if (controls)
            KillTimer(controls, save_timer);
        bool saved = write_edge_settings(ini, options);
        saved = write_spectator_settings(ini, options.spectator) && saved;
        saved = write_director_settings(ini, options.director) && saved;
        saved = write_visual_settings(ini, options.visual) && saved;
        saved = write_overlay_settings(ini, options.overlays) && saved;
        auto write = [&](const wchar_t *key, int value) {
            saved =
                WritePrivateProfileStringW(L"Preview", key, std::to_wstring(value).c_str(), ini.c_str()) &&
                saved;
        };
        write(L"Smoothing", int(options.smoothing));
        write(L"Crop", int(options.crop));
        write(L"Mirror", options.mirror);
        write(L"Stabilize", options.stabilize);
        write(L"Eye", selected_eye);
        write(L"FPS", rate);
        write(L"Mode", window_mode ? 0 : 1);
        write(L"SmoothingTenths", int(options.smoothing * 10 + .5));
        write(L"CropTenths", int(options.crop * 10 + .5));
        write(L"CropLeftTenths", int(options.crop_left * 10 + .5));
        write(L"CropRightTenths", int(options.crop_right * 10 + .5));
        write(L"CropTopTenths", int(options.crop_top * 10 + .5));
        write(L"CropBottomTenths", int(options.crop_bottom * 10 + .5));
        write(L"HorizonTenths", int(options.horizon_lock * 10 + .5));
        write(L"AdaptiveHorizon", options.adaptive_horizon);
        write(L"MirrorLimitTenths", int(options.mirror_limit * 10 + .5));
        write(L"MirrorSoftnessTenths", int(options.mirror_softness * 10 + .5));
        write(L"OutputWidth", options.output_width);
        write(L"OutputHeight", options.output_height);
        write(L"AspectNumerator", aspect_numerator);
        write(L"AspectDenominator", aspect_denominator);
        write(L"AutoFullscreen", start_fullscreen);
        write(L"HotkeyProfile", hotkey_profile);
        write(L"CompareMode", options.compare_mode);
        write(L"CompareEffects", options.compare_effects);
        if (output) {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(MonitorFromWindow(output, MONITOR_DEFAULTTONEAREST), &info))
                monitor_name = info.szDevice;
        }
        DWORD current_pid{};
        GetWindowThreadProcessId(source_window, &current_pid);
        if (source_window && source_pid && source_pid == current_pid) {
            source_executable = executable_path(source_pid);
            wchar_t title[32768]{};
            GetWindowTextW(source_window, title, 32768);
            source_title = title;
        }
        saved =
            WritePrivateProfileStringW(L"Preview", L"Monitor", monitor_name.c_str(), ini.c_str()) && saved;
        saved = WritePrivateProfileStringW(L"Preview", L"SourceExecutable", source_executable.c_str(),
                                           ini.c_str()) &&
                saved;
        saved = WritePrivateProfileStringW(L"Preview", L"SourceTitle", source_title.c_str(), ini.c_str()) &&
                saved;
        return saved;
    }
    void read() {
        options.spectator = read_spectator_settings(ini);
        options.director = read_director_settings(ini);
        options.visual = read_visual_settings(ini);
        options.overlays = read_overlay_settings(ini);
        auto value = [&](const wchar_t *key, int initial, int lo, int hi) {
            return std::clamp(int(GetPrivateProfileIntW(L"Preview", key, initial, ini.c_str())), lo, hi);
        };
        options.smoothing = value(L"Smoothing", 40, 0, 100);
        options.crop = value(L"Crop", 10, 0, 35);
        options.smoothing = value(L"SmoothingTenths", int(options.smoothing * 10), 0, 1000) / 10.;
        options.crop = value(L"CropTenths", int(options.crop * 10), 0, 350) / 10.;
        options.crop_left = value(L"CropLeftTenths", 0, 0, 450) / 10.;
        options.crop_right = value(L"CropRightTenths", 0, 0, 450) / 10.;
        options.crop_top = value(L"CropTopTenths", 0, 0, 450) / 10.;
        options.crop_bottom = value(L"CropBottomTenths", 0, 0, 450) / 10.;
        options.horizon_lock = value(L"HorizonTenths", 0, 0, 1000) / 10.;
        options.adaptive_horizon = value(L"AdaptiveHorizon", 1, 0, 1) != 0;
        options.mirror_limit = value(L"MirrorLimitTenths", 100, 0, 500) / 10.;
        options.mirror_softness = value(L"MirrorSoftnessTenths", 600, 0, 1000) / 10.;
        options.output_width = value(L"OutputWidth", 3840, 320, 7680);
        options.output_height = value(L"OutputHeight", 2160, 180, 4320);
        aspect_numerator = value(L"AspectNumerator", 16, 1, 10000);
        aspect_denominator = value(L"AspectDenominator", 9, 1, 10000);
        options.aspect = double(aspect_numerator) / aspect_denominator;
        if (options.aspect < .25 || options.aspect > 4) {
            aspect_numerator = 16;
            aspect_denominator = 9;
            options.aspect = 16. / 9.;
        }
        start_fullscreen = value(L"AutoFullscreen", 1, 0, 1) != 0;
        hotkey_profile = value(L"HotkeyProfile", 1, 0, 2);
        options.compare_mode = value(L"CompareMode", 0, 0, 2);
        options.compare_effects = value(L"CompareEffects", 0, 0, 1) != 0;
        wchar_t saved_monitor[64]{};
        GetPrivateProfileStringW(L"Preview", L"Monitor", L"", saved_monitor, 64, ini.c_str());
        monitor_name = saved_monitor;
        wchar_t source_text[32768]{};
        GetPrivateProfileStringW(L"Preview", L"SourceExecutable", L"", source_text, 32768, ini.c_str());
        source_executable = source_text;
        GetPrivateProfileStringW(L"Preview", L"SourceTitle", L"", source_text, 32768, ini.c_str());
        source_title = source_text;
        options.mirror = value(L"Mirror", 1, 0, 1) != 0;
        options.stabilize = value(L"Stabilize", 1, 0, 1) != 0;
        selected_eye = value(L"Eye", 0, 0, 1);
        options.eye = selected_eye;
        read_edge_settings(ini, options);
        rate = value(L"FPS", 60, 30, 90);
        window_mode = value(L"Mode", 0, 0, 1) == 0;
        if (rate != 30 && rate != 60 && rate != 90)
            rate = 60;
    }
    bool active() const {
        return !expired && !paused && IsWindowVisible(output) && !IsIconic(output);
    }
    void restart_source() {
        cancel_comparison();
        capture.stop();
        renderer.source_changed();
        last_capture_start = last_received_ms = 0;
        dirty = true;
        InvalidateRect(output, nullptr, FALSE);
    }
    void refresh_sources() {
        DWORD current_pid{};
        GetWindowThreadProcessId(source_window, &current_pid);
        const HWND old = current_pid == source_pid ? source_window : nullptr;
        sources = WindowCapture::windows();
        SendMessageW(source, CB_RESETCONTENT, 0, 0);
        SendMessageW(source, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Choose a game preview window ..."));
        int selected = 0;
        for (unsigned i = 0; i < sources.size(); ++i) {
            SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sources[i].title.c_str()));
            if (sources[i].handle == old) {
                selected = int(i + 1);
                continue;
            }
            if (old || selected)
                continue;
            DWORD pid{};
            GetWindowThreadProcessId(sources[i].handle, &pid);
            const auto name = executable_path(pid);
            if (!source_executable.empty()) {
                if (!_wcsicmp(name.c_str(), source_executable.c_str()) &&
                    (source_title.empty() || sources[i].title == source_title))
                    selected = int(i + 1);
            } else if (!name.empty()) {
                auto filename = std::filesystem::path(name).filename().wstring();
                std::transform(filename.begin(), filename.end(), filename.begin(), ::towlower);
                if (filename.find(L"iracingsim") == 0)
                    selected = int(i + 1);
            }
        }
        SendMessageW(source, CB_SETCURSEL, selected, 0);
        source_window = selected ? sources[std::size_t(selected - 1)].handle : nullptr;
        source_pid = selected ? sources[std::size_t(selected - 1)].pid : 0;
        if (source_window != old)
            restart_source();
    }
    bool capture_frame(std::uint64_t now) {
        DWORD current_pid{};
        if (source_window)
            GetWindowThreadProcessId(source_window, &current_pid);
        if (source_window && (!IsWindow(source_window) || !source_pid || current_pid != source_pid)) {
            source_window = nullptr;
            source_pid = 0;
            SendMessageW(source, CB_SETCURSEL, 0, 0);
            restart_source();
            return false;
        }
        if (!renderer.device()) {
            capture.stop();
            if (!renderer.attach(output))
                return false;
        }
        if (!capture.running() && source_window && now - last_capture_start > 1000) {
            last_capture_start = now;
            capture.start(renderer.device(), source_window);
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        std::uint64_t time{};
        capture.poll(texture, time);
        return renderer.consume_window(texture.Get(), time, options);
    }
    void heartbeat() {
        const bool previous_conflict = capture_conflict;
        if (active() && !window_mode && !demo) {
            const bool claimed = channel.heartbeat(selected_eye, rate);
            if (claimed) {
                channel.select_overlays(options.overlays);
                channel.request_gaze(!options.spectator.bypass &&
                                     (options.spectator.gaze_enabled || renderer.calibration_active()));
            }
            capture_conflict = !claimed && !channel.owns_consumer();
        } else {
            channel.release_consumer();
            capture_conflict = false;
        }
        if (previous_conflict != capture_conflict) {
            renderer.source_changed();
            last_received_ms = 0;
            dirty = true;
        }
        if (!active() || !window_mode || demo)
            capture.stop();
        const bool want_resolution = active();
        if (want_resolution != timer_resolution) {
            if (want_resolution)
                timeBeginPeriod(1);
            else
                timeEndPeriod(1);
            timer_resolution = want_resolution;
        }
    }
    void toggle_fullscreen() {
        const auto visible = IsWindowVisible(output) ? WS_VISIBLE : 0;
        if (!fullscreen) {
            GetWindowPlacement(output, &placement);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(output, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowLongPtrW(output, GWL_STYLE, WS_POPUP | visible);
            SetWindowPos(output, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
        } else {
            SetWindowLongPtrW(output, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
            auto restored = placement;
            if (!visible)
                restored.showCmd = SW_HIDE;
            SetWindowPlacement(output, &restored);
            SetWindowPos(output, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        fullscreen = !fullscreen;
        dirty = true;
    }
    void list_monitors() {
        monitors.clear();
        SendMessageW(monitor, CB_RESETCONTENT, 0, 0);
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR handle, HDC, LPRECT, LPARAM data) -> BOOL {
                auto &items = *reinterpret_cast<std::vector<MONITORINFOEXW> *>(data);
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(handle, &info))
                    items.push_back(info);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&monitors));
        MONITORINFOEXW current{};
        current.cbSize = sizeof(current);
        GetMonitorInfoW(MonitorFromWindow(output, MONITOR_DEFAULTTONEAREST), &current);
        int selected = 0;
        const auto wanted = monitor_name.empty() ? std::wstring(current.szDevice) : monitor_name;
        for (unsigned i = 0; i < monitors.size(); ++i) {
            const auto &m = monitors[i];
            const auto label = std::to_wstring(i + 1) + L": " + m.szDevice + L" (" +
                               std::to_wstring(m.rcMonitor.right - m.rcMonitor.left) + L" x " +
                               std::to_wstring(m.rcMonitor.bottom - m.rcMonitor.top) + L")";
            SendMessageW(monitor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (wanted == m.szDevice)
                selected = int(i);
        }
        SendMessageW(monitor, CB_SETCURSEL, selected, 0);
    }
    void move_to_monitor() {
        const auto index = SendMessageW(monitor, CB_GETCURSEL, 0, 0);
        if (index < 0 || std::size_t(index) >= monitors.size())
            return;
        const auto &m = monitors[std::size_t(index)];
        monitor_name = m.szDevice;
        RECT before{};
        GetWindowRect(output, &before);
        if (fullscreen) {
            const LONG dx = m.rcMonitor.left - before.left, dy = m.rcMonitor.top - before.top;
            OffsetRect(&placement.rcNormalPosition, dx, dy);
            SetWindowPos(output, nullptr, m.rcMonitor.left, m.rcMonitor.top,
                         m.rcMonitor.right - m.rcMonitor.left, m.rcMonitor.bottom - m.rcMonitor.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(output, nullptr, m.rcWork.left + 20, m.rcWork.top + 20,
                         std::min<LONG>(before.right - before.left, m.rcWork.right - m.rcWork.left - 40),
                         std::min<LONG>(before.bottom - before.top, m.rcWork.bottom - m.rcWork.top - 40),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        dirty = true;
    }
    bool apply_output() {
        BOOL width_ok{}, height_ok{};
        const auto width = GetDlgItemInt(controls, width_id, &width_ok, FALSE);
        const auto height = GetDlgItemInt(controls, height_id, &height_ok, FALSE);
        wchar_t text[64]{}, extra{};
        GetWindowTextW(aspect, text, 64);
        unsigned numerator{}, denominator{};
        const int parsed = std::swscanf(text, L" %u : %u %lc", &numerator, &denominator, &extra);
        const double ratio = denominator ? double(numerator) / denominator : 0;
        if (!width_ok || !height_ok || width < 320 || width > 7680 || height < 180 || height > 4320 ||
            parsed != 2 || numerator > 10000 || denominator > 10000 || ratio < .25 || ratio > 4) {
            setup_message =
                L"Use 320–7680 x 180–4320 pixels and an aspect ratio such as 16:9 (range 1:4 to 4:1).";
            return false;
        }
        options.output_width = width;
        options.output_height = height;
        options.aspect = ratio;
        aspect_numerator = numerator;
        aspect_denominator = denominator;
        setup_message.clear();
        dirty = true;
        return true;
    }
    void set_tuning(const Options &tuning) {
        options.smoothing = tuning.smoothing;
        options.crop = tuning.crop;
        options.horizon_lock = tuning.horizon_lock;
        options.adaptive_horizon = tuning.adaptive_horizon;
        options.mirror = tuning.mirror;
        options.mirror_limit = tuning.mirror_limit;
        options.mirror_softness = tuning.mirror_softness;
        options.mirror_stretch = tuning.mirror_stretch;
        advanced.refresh_live_controls();
        options.stabilize = tuning.stabilize;
        SendMessageW(smooth, TBM_SETPOS, TRUE, LPARAM(options.smoothing * 10 + .5));
        SendMessageW(crop, TBM_SETPOS, TRUE, LPARAM(options.crop * 10 + .5));
        SendMessageW(horizon, TBM_SETPOS, TRUE, LPARAM(options.horizon_lock * 10 + .5));
        SendMessageW(mirror_limit, TBM_SETPOS, TRUE, LPARAM(options.mirror_limit * 10 + .5));
        SendMessageW(mirror_softness, TBM_SETPOS, TRUE, LPARAM(options.mirror_softness * 10 + .5));
        SendMessageW(mirror, BM_SETCHECK, options.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(adaptive_horizon, BM_SETCHECK, options.adaptive_horizon ? BST_CHECKED : BST_UNCHECKED,
                     0);
        SendMessageW(bypass, BM_SETCHECK, options.stabilize ? BST_UNCHECKED : BST_CHECKED, 0);
    }
    std::string snapshot() {
        // Include valid output edits and the unsaved render draft visible in Advanced.
        if (!apply_output()) {
            update_status();
            return {};
        }
        if (!save()) {
            setup_message = L"Could not save current settings. Check access to SteadyXR.ini, then retry.";
            update_status();
            return {};
        }
        const auto staging = ini + L".snapshot-" + std::to_wstring(GetCurrentProcessId());
        std::string bytes;
        if (write_bytes(staging, read_bytes(ini)) && write_render_settings(staging, advanced.draft()))
            bytes = read_bytes(staging);
        DeleteFileW(staging.c_str());
        return bytes;
    }
    bool apply_snapshot(const std::string &bytes) {
        std::wstring error;
        if (!PresetStore::valid_snapshot(bytes, error))
            return false;
        const auto staging = ini + L".restore-" + std::to_wstring(GetCurrentProcessId());
        if (!write_bytes(staging, bytes)) {
            DeleteFileW(staging.c_str());
            return false;
        }
        const auto previous_render = read_render_settings(render_ini);
        const auto next_render = read_render_settings(staging);
        // A compatible older registered DLL still reads its own adjacent INI on
        // game launch. Keep that actual destination in sync with explicit loads.
        const bool separate_render_file = _wcsicmp(render_ini.c_str(), ini.c_str()) != 0;
        if (separate_render_file && !write_render_settings(render_ini, next_render)) {
            write_render_settings(render_ini, previous_render);
            DeleteFileW(staging.c_str());
            return false;
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
        if (!MoveFileExW(staging.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (separate_render_file)
                write_render_settings(render_ini, previous_render);
            DeleteFileW(staging.c_str());
            return false;
        }
        KillTimer(controls, save_timer);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
        read();
        spectator_controls.refresh();
        if (IsWindow(overlays.window))
            overlays.refresh();
        register_hotkeys();
        preset_cursor.clear();
        set_tuning(options);
        const double trim_values[]{options.crop_left, options.crop_right, options.crop_top,
                                   options.crop_bottom};
        for (unsigned i = 0; i < 4; ++i)
            SendMessageW(trims[i], TBM_SETPOS, TRUE, LPARAM(std::lround(trim_values[i] * 10)));
        SendMessageW(eye, CB_SETCURSEL, selected_eye, 0);
        SendMessageW(mode, CB_SETCURSEL, window_mode ? 0 : 1, 0);
        SendMessageW(fps, CB_SETCURSEL, rate == 30 ? 0 : rate == 60 ? 1 : 2, 0);
        SetWindowTextW(output_width, std::to_wstring(options.output_width).c_str());
        SetWindowTextW(output_height, std::to_wstring(options.output_height).c_str());
        SetWindowTextW(
            aspect, (std::to_wstring(aspect_numerator) + L":" + std::to_wstring(aspect_denominator)).c_str());
        SendMessageW(auto_fullscreen, BM_SETCHECK, start_fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
        list_monitors();
        if (!diagnostic)
            move_to_monitor();
        source_window = nullptr;
        source_pid = 0;
        refresh_sources();
        restart_source();
        advanced.reload(render_ini);
        can_undo_comfort = false;
        heartbeat();
        labels();
        setup_message = L"Complete preset loaded. Changed render expansion applies at the next game launch.";
        update_status();
        return true;
    }
    void builtin(BuiltinPreset preset) {
        cancel_comparison();
        const wchar_t *name = preset == BuiltinPreset::natural          ? L"Natural"
                              : preset == BuiltinPreset::stream_comfort ? L"Stream Comfort"
                                                                        : L"Strong leveling";
        preset_cursor = preset == BuiltinPreset::natural          ? L"builtin:natural"
                        : preset == BuiltinPreset::stream_comfort ? L"builtin:comfort"
                                                                  : L"builtin:strong";
        if (!can_undo_comfort)
            before_comfort = options;
        auto tuning = options;
        if (preset == BuiltinPreset::stream_comfort)
            apply_stream_comfort(tuning);
        else {
            const bool strong = preset == BuiltinPreset::strong_leveling;
            tuning.smoothing = strong ? 70 : 25;
            tuning.horizon_lock = strong ? 85 : 25;
            tuning.adaptive_horizon = true;
            tuning.crop = strong ? 15 : 8;
            tuning.mirror = tuning.stabilize = true;
            tuning.mirror_limit = strong ? 12 : 10;
            tuning.mirror_softness = strong ? 75 : 60;
        }
        set_tuning(tuning);
        can_undo_comfort = true;
        labels();
        save();
        dirty = true;
        setup_message = std::wstring(L"Preset: ") + name + L". Source, eye and output settings retained.";
        update_status();
    }
    void labels() {
        SetWindowTextW(smooth_label, (L"Smoothing: " + decimal(options.smoothing) + L" %").c_str());
        const bool separate = options.edge_reserve[std::min(selected_eye, 1u)].enabled;
        SetWindowTextW(crop_label, separate ? L"Per-edge crop reserve"
                                            : (L"Crop reserve: " + decimal(options.crop) + L" %").c_str());
        EnableWindow(crop, !separate);
        SetWindowTextW(pause, paused ? L"Resume preview" : L"Pause preview");
        EnableWindow(source, window_mode);
        EnableWindow(GetDlgItem(controls, refresh_id), window_mode);
        EnableWindow(eye, TRUE);
        SetWindowTextW(eye_label, window_mode ? L"Profile" : L"Eye");
        EnableWindow(GetDlgItem(controls, setup_id), !window_mode && !expired && !connection_process);
        EnableWindow(GetDlgItem(controls, remove_id), !connection_process);
        SetWindowTextW(GetDlgItem(controls, setup_id),
                       registered(true) ? L"Upgrade / repair globally" : L"Install for all users");
        const double values[] = {options.crop_left, options.crop_right, options.crop_top,
                                 options.crop_bottom};
        const wchar_t *names[] = {L"Crop left", L"Crop right", L"Crop top", L"Crop bottom"};
        for (unsigned i = 0; i < 4; ++i)
            SetWindowTextW(trim_labels[i],
                           (std::wstring(names[i]) + L": " + decimal(values[i]) + L" %").c_str());
        SetWindowTextW(horizon_label, (L"Horizon lock: " + decimal(options.horizon_lock) + L" %").c_str());
        SetWindowTextW(mirror_limit_label,
                       (L"Mirror limit: " + decimal(options.mirror_limit) + L" %").c_str());
        SetWindowTextW(mirror_softness_label,
                       (L"Mirror softness: " + decimal(options.mirror_softness) + L" %").c_str());
        EnableWindow(horizon, !window_mode);
        EnableWindow(adaptive_horizon, !window_mode);
        EnableWindow(mirror_limit, options.mirror);
        EnableWindow(mirror_softness, options.mirror);
        EnableWindow(GetDlgItem(controls, undo_comfort_id), can_undo_comfort);
    }
    void update_status() {
        const auto now = GetTickCount64();
        const bool comparison_active = comparing(now);
        const bool stale = last_received_ms && now >= last_received_ms && now - last_received_ms >= 2000;
        const auto fps_value = !active() || stale || capture_conflict ? 0 : int(measured_fps + .5);
        const wchar_t *feed = demo ? L"Demo" : window_mode ? L"Window" : L"OpenXR";
        const wchar_t *connection = expired                                        ? L"expired"
                                    : capture_conflict                             ? L"in use elsewhere"
                                    : paused                                       ? L"paused"
                                    : !IsWindowVisible(output) || IsIconic(output) ? L"hidden"
                                    : !renderer.has_frame() || stale               ? L"waiting"
                                                                                   : L"connected";
        const wchar_t *comparison_name = options.compare_effects     ? L"Effects A/B"
                                         : options.compare_mode == 1 ? L"50/50 comparison"
                                         : options.compare_mode == 2 ? L"Side-by-side comparison"
                                                                     : L"Original";
        const auto view = comparison_active ? std::wstring(comparison_name) + L" (" +
                                                  std::to_wstring((compare_until - now + 999) / 1000) + L"s)"
                          : expired           ? std::wstring(L"Preview stopped")
                          : options.stabilize ? std::wstring(L"Stabilized")
                                              : std::wstring(L"Stabilization off");
        const auto summary =
            std::to_wstring(fps_value) + L" FPS | " + feed + L" " + connection + L" | " + view;
        SetWindowTextW(compact_status, summary.c_str());
        const wchar_t *comparison_button = options.compare_effects     ? L"Compare effects (20s)"
                                           : options.compare_mode == 1 ? L"Compare 50/50 (20s)"
                                           : options.compare_mode == 2 ? L"Compare side by side (20s)"
                                                                       : L"Compare original (20s)";
        SetWindowTextW(comparison, comparison_active ? L"End A/B comparison" : comparison_button);
        EnableWindow(comparison, comparison_active || (!paused && !expired && !waiting(now)));
        SetWindowTextW(
            shortcut_hint,
            (hotkey_hint() + L"\r\nReset horizon: hold the headset approximately upright.").c_str());
        if (expired) {
            const auto message = std::wstring(L"This SteadyXR build has expired.\r\nValid until: ") +
                                 build_expiry_text() +
                                 L"\r\nPlease install a newer build to resume the preview.\r\nYour settings "
                                 L"are retained. The OpenXR layer editor remains available.";
            SetWindowTextW(status, message.c_str());
            return;
        }
        std::wstring text;
        if (demo)
            text = L"DEMO – synthetic test image, not an iRacing image.\r\n";
        else if (window_mode) {
            text = L"Window capture + image analysis. No game restart needed.\r\n";
            if (source_window && IsWindow(source_window)) {
                wchar_t title[512]{};
                GetWindowTextW(source_window, title, 512);
                text += std::wstring(title) + L"\r\n";
                if (IsIconic(source_window))
                    text += L"Keep the original iRacing preview window unminimized.\r\n";
            } else
                text += L"Select a source window above; refresh the list if needed.\r\n";
            if (!capture.error().empty())
                text += capture.error() + L"\r\n";
        } else {
            text = registered() ? L"Compatible OpenXR connection installed.\r\n"
                   : registered(true)
                       ? L"Older layer installed: click Upgrade connection, then restart the OpenXR game.\r\n"
                       : L"Install the connection, then restart the OpenXR game.\r\n";
            if (channel.lock()) {
                const auto &s = *channel.data();
                const auto now = GetTickCount64();
                if (s.producer_pid && now >= s.producer_heartbeat_ms &&
                    now - s.producer_heartbeat_ms < 2500) {
                    text = L"OpenXR feed connected.\r\n";
                    text += std::wstring(s.application, wcsnlen(s.application, 128)) + L"\r\n";
                    text += std::wstring(s.status, wcsnlen(s.status, 192)) + L"\r\n";
                    const auto configured = read_render_settings(render_ini);
                    bool pending = configured.enabled != bool(s.render_enabled);
                    if (configured.enabled)
                        for (unsigned eye = 0; eye < 2; ++eye)
                            pending |= std::abs(configured.eyes[eye].left - s.render_left[eye]) > .05 ||
                                       std::abs(configured.eyes[eye].right - s.render_right[eye]) > .05;
                    if (pending)
                        text += L"New render settings pending: restart the OpenXR game.\r\n";
                    else if (setup_message.rfind(L"Render settings saved.", 0) == 0)
                        setup_message.clear();
                    if (s.render_enabled) {
                        const auto eye = std::min(selected_eye, 1u);
                        text += L"Render extension this session: L " + decimal(s.render_left[eye]) +
                                L" %, R " + decimal(s.render_right[eye]) + L" %.\r\n";
                    }
                } else
                    text += L"Waiting for an OpenXR game using D3D11 or D3D12.\r\n";
                channel.unlock();
            }
        }
        if (!setup_message.empty())
            text = setup_message + L"\r\n" + text;
        if (!hotkey_error.empty())
            text = hotkey_error + L"\r\n" + text;
        if (capture_conflict)
            text += L"OpenXR capture is in use by OBS or another preview. Hide or pause that source to use "
                    L"this preview.\r\n";
        if (paused)
            text += L"PAUSED – no new frame copies requested.\r\n";
        else if (!IsWindowVisible(output) || IsIconic(output))
            text += L"Preview hidden – no new frame copies requested.\r\n";
        else if (last_received_ms && GetTickCount64() - last_received_ms > 2000)
            text += L"No new frame for more than 2 seconds.\r\n";
        if (renderer.has_frame())
            text += std::wstring(window_mode && !demo    ? L"Image analysis"
                                 : renderer.views() == 2 ? L"Context + foveal inset"
                                                         : L"Stereo eye") +
                    L"\r\n";
        if (!renderer.error().empty())
            text += renderer.error() + L"\r\n";
        text += L"Render: " + std::to_wstring(options.output_width) + L" x " +
                std::to_wstring(options.output_height) + L" | Aspect: " + std::to_wstring(aspect_numerator) +
                L":" + std::to_wstring(aspect_denominator) +
                L"\r\nF11: fullscreen. No OBS or encoder required.";
        // Preserve selection/scrolling in the details field while only the FPS
        // summary changes. Errors and connection changes remain inspectable.
        std::wstring previous(std::size_t(GetWindowTextLengthW(status)) + 1, L'\0');
        GetWindowTextW(status, previous.data(), int(previous.size()));
        previous.resize(std::wcslen(previous.c_str()));
        if (previous != text)
            SetWindowTextW(status, text.c_str());
    }
};
App *app{};
bool save_controls(HWND window, const std::wstring &path) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    HDC dc = GetDC(window), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !memory) {
        if (bitmap)
            DeleteObject(bitmap);
        if (memory)
            DeleteDC(memory);
        ReleaseDC(window, dc);
        return false;
    }
    auto previous = SelectObject(memory, bitmap);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                 PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    // Finish batched text/shape drawing before the CPU reads the DIB pixels.
    GdiFlush();
    unsigned nonblack = 0;
    for (int i = 0; i < width * height; ++i)
        if ((static_cast<const std::uint32_t *>(bits)[i] & 0xffffff) != 0)
            ++nonblack;
    const bool painted = nonblack > unsigned(width * height / 4);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + width * height * 4;
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    out.write(reinterpret_cast<const char *>(&file), sizeof(file));
    out.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(static_cast<const char *>(bits), width * height * 4);
    const bool saved = out.good();
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, dc);
    return painted && saved;
}
HWND child(HWND parent, const wchar_t *kind, const wchar_t *label, DWORD style, int x, int y, int w, int h,
           int id = 0) {
    if (std::wcscmp(kind, L"STATIC") != 0)
        style |= WS_TABSTOP;
    style = app->theme.style(kind, style);
    auto result = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                  reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    app->theme.control(result, kind);
    return result;
}
LRESULT CALLBACK output_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CLOSE:
        app->cancel_comparison();
        ShowWindow(window, SW_HIDE);
        app->heartbeat();
        return 0;
    case WM_SIZE:
        app->dirty = true;
        app->heartbeat();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F11) {
            app->toggle_fullscreen();
            return 0;
        }
        if (w == VK_ESCAPE && app->fullscreen) {
            app->toggle_fullscreen();
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        app->toggle_fullscreen();
        return 0;
    case WM_EXITSIZEMOVE:
        app->save();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window, &ps);
        EndPaint(window, &ps);
        if (app->waiting(GetTickCount64()))
            app->renderer.draw(app->draw_options(GetTickCount64()), true, app->waiting_text());
        app->dirty = true;
        return 0;
    }
    }
    return DefWindowProcW(window, message, w, l);
}
LRESULT CALLBACK controls_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (!app)
        return DefWindowProcW(window, message, w, l);
    LRESULT themed{};
    if (app->theme.message(window, message, w, l, themed))
        return themed;
    switch (message) {
    case WM_HOTKEY:
        if (app->hotkey_profile && std::find(std::begin(hotkey_actions), std::end(hotkey_actions), int(w)) !=
                                       std::end(hotkey_actions))
            SendMessageW(window, WM_COMMAND, w, 0);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && app->status && app->output)
            app->refresh_editor_connection();
        break;
    case WM_TIMER:
        if (w == comparison_timer) {
            const bool had_comparison = app->compare_until != 0;
            const auto now = GetTickCount64();
            app->refresh_comparison(now);
            // Popup menus and window dragging run a nested message loop. End
            // the displayed comparison there as well as in the capture loop.
            if (had_comparison && !app->compare_until && IsWindowVisible(app->output) &&
                !IsIconic(app->output))
                app->renderer.draw(app->draw_options(now), true,
                                   app->waiting(now) ? app->waiting_text() : nullptr);
            return 0;
        }
        if (w == save_timer) {
            app->save();
            return 0;
        }
        break;
    case WM_HSCROLL:
        app->options.smoothing = SendMessageW(app->smooth, TBM_GETPOS, 0, 0) / 10.;
        app->options.crop = SendMessageW(app->crop, TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_left = SendMessageW(app->trims[0], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_right = SendMessageW(app->trims[1], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_top = SendMessageW(app->trims[2], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_bottom = SendMessageW(app->trims[3], TBM_GETPOS, 0, 0) / 10.;
        app->options.horizon_lock = SendMessageW(app->horizon, TBM_GETPOS, 0, 0) / 10.;
        app->options.mirror_limit = SendMessageW(app->mirror_limit, TBM_GETPOS, 0, 0) / 10.;
        app->options.mirror_softness = SendMessageW(app->mirror_softness, TBM_GETPOS, 0, 0) / 10.;
        app->labels();
        app->queue_save();
        app->dirty = true;
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= navigation_id && LOWORD(w) < navigation_id + 4) {
            app->show_page(LOWORD(w) - navigation_id);
            return 0;
        }
        switch (LOWORD(w)) {
        case spectator_id:
            app->spectator_controls.open();
            return 0;
        case learn_chat_id:
            app->learn_chat();
            return 0;
        case compare_id:
            app->toggle_comparison();
            return 0;
        case next_preset_id:
            app->next_preset();
            return 0;
        case hotkeys_id:
            app->hotkeys_menu();
            return 0;
        case hotkeys_default_id:
        case hotkeys_alternate_id:
        case hotkeys_off_id:
            app->configure_hotkeys(LOWORD(w) == hotkeys_default_id     ? 1
                                   : LOWORD(w) == hotkeys_alternate_id ? 2
                                                                       : 0);
            return 0;
        case comparison_original_id:
        case comparison_split_id:
        case comparison_side_by_side_id:
            app->options.compare_mode = LOWORD(w) - comparison_original_id;
            app->save();
            app->dirty = true;
            app->update_status();
            return 0;
        case advanced_id:
            app->advanced.open(app->selected_eye);
            return 0;
        case presets_id:
            app->presets.open();
            return 0;
        case overlays_id:
            app->overlays.open(app->controls, app->theme, app->channel, app->options.overlays, [] {
                app->queue_save();
                app->dirty = true;
            });
            return 0;
        case layers_id:
            app->open_layer_editor();
            return 0;
        case comfort_id: {
            app->builtin(BuiltinPreset::stream_comfort);
            break;
        }
        case undo_comfort_id:
            if (app->can_undo_comfort) {
                app->set_tuning(app->before_comfort);
                app->can_undo_comfort = false;
                app->setup_message = L"Previous stabilization settings restored.";
            }
            break;
        case apply_id:
            app->apply_output();
            break;
        case aspect_id:
        case width_id:
        case height_id:
            // Partial edits are applied together by the Apply button.
            return 0;
        case auto_fullscreen_id:
            app->start_fullscreen = SendMessageW(app->auto_fullscreen, BM_GETCHECK, 0, 0) == BST_CHECKED;
            break;
        case monitor_id:
            if (HIWORD(w) == CBN_SELCHANGE)
                app->move_to_monitor();
            break;
        case mode_id:
            if (HIWORD(w) == CBN_SELCHANGE) {
                app->window_mode = SendMessageW(app->mode, CB_GETCURSEL, 0, 0) == 0;
                app->restart_source();
            }
            break;
        case source_id:
            if (HIWORD(w) == CBN_SELCHANGE) {
                const int index = int(SendMessageW(app->source, CB_GETCURSEL, 0, 0));
                app->source_window = index > 0 && std::size_t(index) <= app->sources.size()
                                         ? app->sources[std::size_t(index - 1)].handle
                                         : nullptr;
                app->source_pid = app->source_window ? app->sources[std::size_t(index - 1)].pid : 0;
                app->source_executable = executable_path(app->source_pid);
                app->source_title = app->source_window ? app->sources[std::size_t(index - 1)].title : L"";
                app->restart_source();
            }
            break;
        case refresh_id:
            app->refresh_sources();
            break;
        case setup_id:
            if (!app->window_mode)
                app->connection(true);
            break;
        case remove_id:
            app->connection(false);
            break;
        case show_id:
            app->cancel_comparison();
            ShowWindow(app->output, SW_RESTORE);
            app->paused = false;
            SetForegroundWindow(app->output);
            break;
        case pause_id:
            app->cancel_comparison();
            app->paused = !app->paused;
            if (!app->paused)
                app->renderer.reset();
            break;
        case reset_id:
            app->reset_horizon();
            return 0;
        case mirror_id:
            app->options.mirror = SendMessageW(app->mirror, BM_GETCHECK, 0, 0) == BST_CHECKED;
            break;
        case bypass_id:
            app->options.stabilize = SendMessageW(app->bypass, BM_GETCHECK, 0, 0) != BST_CHECKED;
            break;
        case adaptive_horizon_id:
            app->options.adaptive_horizon =
                SendMessageW(app->adaptive_horizon, BM_GETCHECK, 0, 0) == BST_CHECKED;
            break;
        case eye_id:
            if (HIWORD(w) == CBN_SELCHANGE) {
                app->cancel_comparison();
                app->selected_eye = unsigned(SendMessageW(app->eye, CB_GETCURSEL, 0, 0));
                app->options.eye = app->selected_eye;
                app->renderer.reset();
            }
            break;
        case fps_id:
            if (HIWORD(w) == CBN_SELCHANGE) {
                int selected = int(SendMessageW(app->fps, CB_GETCURSEL, 0, 0));
                app->rate = selected == 0 ? 30 : selected == 1 ? 60 : 90;
            }
            break;
        default:
            return DefWindowProcW(window, message, w, l);
        }
        app->heartbeat();
        app->labels();
        app->save();
        app->update_status();
        app->dirty = true;
        return 0;
    case WM_CLOSE:
        app->save();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        app->unregister_hotkeys();
        app->quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int show) {
    int argument_count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    const bool install = arguments && argument_count > 1 && wcscmp(arguments[1], L"--install-connection") == 0;
    const bool remove = arguments && argument_count > 1 && wcscmp(arguments[1], L"--remove-connection") == 0;
    if (install || remove) {
        // Handle elevated registration before the viewer singleton and any graphics/UI initialization.
        if (argument_count != 3) {
            LocalFree(arguments);
            return ERROR_INVALID_PARAMETER;
        }
        const std::wstring sid(arguments[2]);
        LocalFree(arguments);
        PSID binary_sid{};
        LPWSTR canonical_sid{};
        const bool valid_sid = ConvertStringSidToSidW(sid.c_str(), &binary_sid) &&
                               ConvertSidToStringSidW(binary_sid, &canonical_sid) && sid == canonical_sid;
        if (binary_sid) LocalFree(binary_sid);
        if (canonical_sid) LocalFree(canonical_sid);
        if (!valid_sid)
            return ERROR_INVALID_SID;
        HKEY original_user{};
        auto result = RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &original_user);
        if (result != ERROR_SUCCESS)
            return result;
        RegCloseKey(original_user);
        const auto user_key = sid + L"\\" + layer_registry_key;
        const LayerRegistryRoots roots{HKEY_LOCAL_MACHINE, layer_registry_key, HKEY_USERS, user_key.c_str()};
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        const auto directory = std::filesystem::path(executable).parent_path().wstring();
        const auto previous_settings = installed_layer_settings_path(roots);
        const auto settings = directory + L"\\SteadyXR.ini";
        if (install && previous_settings && *previous_settings != settings &&
            GetFileAttributesW(previous_settings->c_str()) != INVALID_FILE_ATTRIBUTES &&
            !write_render_settings(settings, read_render_settings(*previous_settings)))
            result = ERROR_ACCESS_DENIED;
        else
            result = set_layer_connection(directory, install, roots);
        if ((result == ERROR_ACCESS_DENIED || result == ERROR_PRIVILEGE_NOT_HELD) && !process_elevated()) {
            HANDLE process{};
            result = launch_connection_helper(nullptr, install, sid, process);
            if (result == ERROR_SUCCESS) {
                const auto wait = WaitForSingleObject(process, INFINITE);
                DWORD exit_code = ERROR_PROCESS_ABORTED;
                if (wait == WAIT_OBJECT_0)
                    GetExitCodeProcess(process, &exit_code);
                result = static_cast<LONG>(exit_code);
                CloseHandle(process);
            }
        }
        return result;
    }
    if (arguments)
        LocalFree(arguments);
    const bool verify_window = std::wstring(command).find(L"--verify-window") != std::wstring::npos;
    const bool self_test = verify_window || std::wstring(command).find(L"--self-test") != std::wstring::npos;
    auto dpi = reinterpret_cast<BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (dpi)
        dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE singleton = CreateMutexW(nullptr, TRUE,
                                    self_test ? L"Local\\HorizonLockVRPreview-Viewer-self-test"
                                              : L"Local\\HorizonLockVRPreview-Viewer-v1");
    if (!singleton)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto existing = FindWindowW(L"HorizonLockControls", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleton);
        return 0;
    }
    App state;
    app = &state;
    state.diagnostic = self_test;
    state.expired = build_expired();
    state.demo = std::wstring(command).find(L"--demo") != std::wstring::npos;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    state.directory = std::filesystem::path(path).parent_path().wstring();
    state.ini = state.directory + L"\\SteadyXR.ini";
    DWORD migration_error{};
    if (!self_test && !migrate_preview_settings(state.directory, migration_error)) {
        const auto message =
            L"Could not migrate settings to SteadyXR.ini. Existing settings were kept. Windows error " +
            std::to_wstring(migration_error);
        MessageBoxW(nullptr, message.c_str(), L"SteadyXR", MB_ICONERROR);
        CloseHandle(singleton);
        return 1;
    }
    if (self_test)
        state.ini =
            state.directory + L"\\preview-selftest-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    state.read();
    state.resolve_connection();
    state.start_ms = state.fps_start = GetTickCount64();
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    state.theme.initialize();
    state.font = state.theme.body;
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = state.theme.icon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"HorizonLockControls";
    wc.lpfnWndProc = controls_proc;
    RegisterClassW(&wc);
    wc.lpszClassName = L"HorizonLockOutput";
    wc.lpfnWndProc = output_proc;
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    const auto control_title = std::wstring(L"SteadyXR – Preview 0.14.0 | No expiry");
    RECT controls_rect{0, 0, 1024, 800};
    const DWORD controls_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&controls_rect, controls_style, FALSE, 0);
    state.controls =
        CreateWindowExW(0, L"HorizonLockControls", control_title.c_str(), controls_style, 100, 100,
                        controls_rect.right - controls_rect.left, controls_rect.bottom - controls_rect.top,
                        nullptr, nullptr, instance, nullptr);
    state.output = CreateWindowExW(0, L"HorizonLockOutput", L"SteadyXR – Clean Preview", WS_OVERLAPPEDWINDOW,
                                   800, 120, 976, 579, nullptr, nullptr, instance, nullptr);
    state.theme.titlebar(state.controls);
    state.theme.titlebar(state.output);
    const auto channel_name =
        self_test ? L"Local\\HorizonLockPreview-UI-test-" + std::to_wstring(GetCurrentProcessId())
                  : std::wstring(default_channel);
    if (!state.controls || !state.output || !state.channel.open(true, channel_name)) {
        MessageBoxW(nullptr, L"Could not start the preview.", L"SteadyXR", MB_ICONERROR);
        CloseHandle(singleton);
        return 1;
    }
    auto add = [&](int page, const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int width,
                   int height, int id = 0) {
        const auto h = child(state.controls, kind, text, style, x, y, width, height, id);
        if (page >= 0)
            state.control_pages[page].push_back(h);
        else
            SetPropW(h, L"SteadyXRSurface", reinterpret_cast<HANDLE>(1));
        return h;
    };
    auto note = [&](int page, const wchar_t *text, int x, int y, int width, int height) {
        const auto h = add(page, L"STATIC", text, 0, x, y, width, height);
        SetPropW(h, L"SteadyXRMuted", reinterpret_cast<HANDLE>(1));
        return h;
    };
    const wchar_t *tabs[]{L"Capture", L"Camera", L"Framing", L"Output"};
    const wchar_t *titles[]{L"Choose your capture", L"Find your camera feel", L"Frame the action",
                            L"Set up your output"};
    const wchar_t *descriptions[]{
        L"Capture a game window, or connect OpenXR for pose tracking and gaze data.",
        L"Smooth small movements while keeping deliberate head turns responsive.",
        L"Balance the visible scene, stabilization reserve and edge treatment.",
        L"Choose the size and display for your clean spectator preview."};
    for (int page = 0; page < 4; ++page) {
        const auto tab =
            add(-1, L"BUTTON", tabs[page], BS_AUTORADIOBUTTON | BS_PUSHLIKE | (page == 0 ? WS_GROUP : 0),
                24 + page * 160, 102, 150, 42, navigation_id + page);
        SetPropW(tab, L"SteadyXRTab", reinterpret_cast<HANDLE>(1));
        const auto title = add(page, L"STATIC", titles[page], 0, 48, 186, 912, 28);
        state.theme.control(title, L"STATIC", true);
        note(page, descriptions[page], 48, 220, 912, 26);
    }
    add(-1, L"BUTTON", L"Open preview", BS_PUSHBUTTON | WS_GROUP, 810, 24, 190, 38, show_id);
    state.pause = add(-1, L"BUTTON", L"Pause preview", BS_PUSHBUTTON, 616, 24, 180, 38, pause_id);
    add(-1, L"BUTTON", L"Spectator & effects", BS_PUSHBUTTON, 710, 102, 290, 42, spectator_id);

    add(0, L"STATIC", L"Capture mode", 0, 48, 262, 552, 24);
    state.mode = add(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 48, 290, 552, 180, mode_id);
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Capture existing window (no restart)"));
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"OpenXR pose capture (optional connection)"));
    SendMessageW(state.mode, CB_SETCURSEL, state.window_mode ? 0 : 1, 0);
    add(0, L"STATIC", L"Game window", 0, 48, 342, 552, 24);
    state.source = add(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 48, 370, 420, 300, source_id);
    add(0, L"BUTTON", L"Refresh", BS_PUSHBUTTON, 480, 369, 120, 30, refresh_id);
    add(0, L"BUTTON", L"Install connection", BS_PUSHBUTTON, 48, 432, 230, 34, setup_id);
    add(0, L"BUTTON", L"Remove connection", BS_PUSHBUTTON, 294, 432, 215, 34, remove_id);
    add(0, L"BUTTON", L"Layer order…", BS_PUSHBUTTON, 48, 490, 150, 34, layers_id);
    add(0, L"BUTTON", L"Overlay layers…", BS_PUSHBUTTON, 214, 490, 180, 34, overlays_id);
    state.eye_label = add(0, L"STATIC", L"Eye", 0, 650, 262, 132, 24);
    state.eye = add(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 650, 290, 132, 160, eye_id);
    for (auto text : {L"Left", L"Right"})
        SendMessageW(state.eye, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(state.eye, CB_SETCURSEL, state.selected_eye, 0);
    add(0, L"STATIC", L"Frame rate", 0, 806, 262, 150, 24);
    state.fps = add(0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 806, 290, 150, 160, fps_id);
    for (auto text : {L"30 FPS", L"60 FPS", L"90 FPS"})
        SendMessageW(state.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(state.fps, CB_SETCURSEL, state.rate == 30 ? 0 : state.rate == 60 ? 1 : 2, 0);
    note(0, L"Connection details", 650, 342, 326, 24);
    state.status =
        add(0, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 650, 374, 326, 150);
    SetPropW(state.status, L"SteadyXRMuted", reinterpret_cast<HANDLE>(1));

    auto slider = [&](int page, int id, int x, int y, int width, double value, int maximum, HWND &label) {
        label = add(page, L"STATIC", L"", 0, x, y, width, 24);
        const auto h = add(page, TRACKBAR_CLASSW, L"", TBS_HORZ, x, y + 28, width, 32, id);
        SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(0, maximum));
        SendMessageW(h, TBM_SETPOS, TRUE, LPARAM(value * 10 + .5));
        SendMessageW(h, TBM_SETLINESIZE, 0, 1);
        SendMessageW(h, TBM_SETPAGESIZE, 0, 10);
        return h;
    };
    state.bypass = add(1, L"BUTTON", L"Disable stabilization (capture stays active)", BS_AUTOCHECKBOX, 48,
                       258, 800, 28, bypass_id);
    SendMessageW(state.bypass, BM_SETCHECK, state.options.stabilize ? BST_UNCHECKED : BST_CHECKED, 0);
    state.smooth = slider(1, smooth_id, 48, 320, 424, state.options.smoothing, 1000, state.smooth_label);
    state.horizon =
        slider(1, horizon_id, 536, 320, 424, state.options.horizon_lock, 1000, state.horizon_label);
    state.adaptive_horizon =
        add(1, L"BUTTON", L"Adaptive horizon curve", BS_AUTOCHECKBOX, 536, 400, 424, 28, adaptive_horizon_id);
    SendMessageW(state.adaptive_horizon, BM_SETCHECK,
                 state.options.adaptive_horizon ? BST_CHECKED : BST_UNCHECKED, 0);
    note(1, L"Start with a comfortable preset, then fine-tune.", 48, 400, 424, 42);
    add(1, L"BUTTON", L"Stream Comfort", BS_PUSHBUTTON, 48, 452, 192, 36, comfort_id);
    add(1, L"BUTTON", L"Undo tuning", BS_PUSHBUTTON, 256, 452, 192, 36, undo_comfort_id);
    add(1, L"BUTTON", L"Reset horizon", BS_PUSHBUTTON, 536, 452, 200, 36, reset_id);
    note(1, L"For gaze framing and natural head motion, open Spectator & effects above.", 48, 508, 912, 26);

    state.crop = slider(2, crop_id, 48, 262, 424, state.options.crop, 350, state.crop_label);
    const double trim_values[]{state.options.crop_left, state.options.crop_right, state.options.crop_top,
                               state.options.crop_bottom};
    for (unsigned i = 0; i < 4; ++i)
        state.trims[i] = slider(2, left_id + i, 48 + (i % 2) * 224, 358 + (i / 2) * 68, 200, trim_values[i],
                                450, state.trim_labels[i]);
    state.mirror = add(2, L"BUTTON", L"Mirror edges beyond the crop reserve", BS_AUTOCHECKBOX, 536, 262, 424,
                       28, mirror_id);
    SendMessageW(state.mirror, BM_SETCHECK, state.options.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    state.mirror_limit =
        slider(2, mirror_limit_id, 536, 312, 424, state.options.mirror_limit, 500, state.mirror_limit_label);
    state.mirror_softness = slider(2, mirror_softness_id, 536, 390, 424, state.options.mirror_softness, 1000,
                                   state.mirror_softness_label);
    add(2, L"BUTTON", L"Render & edge settings…", BS_PUSHBUTTON, 48, 500, 248, 34, advanced_id);
    note(2,
         L"More crop gives stabilization room to move.\r\nEdge mirroring fills gaps near the frame boundary.",
         536, 478, 424, 58);

    add(3, L"STATIC", L"Aspect ratio", 0, 48, 262, 200, 24);
    state.aspect =
        add(3, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_HASSTRINGS | WS_VSCROLL, 48, 290, 200, 200, aspect_id);
    for (auto text : {L"16:9", L"21:9", L"32:9", L"4:3", L"1:1"})
        SendMessageW(state.aspect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SetWindowTextW(
        state.aspect,
        (std::to_wstring(state.aspect_numerator) + L":" + std::to_wstring(state.aspect_denominator)).c_str());
    add(3, L"STATIC", L"Render resolution", 0, 296, 262, 288, 24);
    state.output_width = add(3, L"EDIT", std::to_wstring(state.options.output_width).c_str(),
                             WS_BORDER | ES_NUMBER, 296, 290, 120, 29, width_id);
    add(3, L"STATIC", L"×", 0, 432, 294, 20, 24);
    state.output_height = add(3, L"EDIT", std::to_wstring(state.options.output_height).c_str(),
                              WS_BORDER | ES_NUMBER, 464, 290, 120, 29, height_id);
    add(3, L"BUTTON", L"Apply aspect & resolution", BS_PUSHBUTTON, 48, 346, 536, 36, apply_id);
    add(3, L"STATIC", L"Output monitor", 0, 48, 410, 536, 24);
    state.monitor = add(3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 48, 438, 536, 150, monitor_id);
    state.auto_fullscreen = add(3, L"BUTTON", L"Start preview in fullscreen", BS_AUTOCHECKBOX, 48, 492, 536,
                                28, auto_fullscreen_id);
    SendMessageW(state.auto_fullscreen, BM_SETCHECK, state.start_fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto output_title = add(3, L"STATIC", L"A clean view for your audience", 0, 650, 266, 326, 50);
    state.theme.control(output_title, L"STATIC", true);
    note(
        3,
        L"Open preview shows only the captured scene.\r\n\r\nOptional exposure, bloom and color are in "
        L"Spectator & effects.\r\n\r\nFor a two-PC setup, apply visual effects on the stream PC to keep that "
        L"work off the gaming PC.",
        650, 334, 326, 190);

    state.compact_status = add(-1, L"STATIC", L"", SS_LEFTNOWORDWRAP, 24, 574, 976, 24, compact_status_id);
    state.comparison =
        add(-1, L"BUTTON", L"Compare original (20s)", BS_PUSHBUTTON, 24, 612, 252, 36, compare_id);
    add(-1, L"BUTTON", L"Next preset", BS_PUSHBUTTON, 292, 612, 160, 36, next_preset_id);
    add(-1, L"BUTTON", L"Presets…", BS_PUSHBUTTON, 468, 612, 168, 36, presets_id);
    add(-1, L"BUTTON", L"Hotkeys / A/B…", BS_PUSHBUTTON, 652, 612, 180, 36, hotkeys_id);
    state.shortcut_hint = note(-1, L"", 24, 666, 976, 40);
    const auto beta_text = std::wstring(L"Free community test version · No expiry");
    const auto beta_notice = add(-1, L"STATIC", beta_text.c_str(), 0, 24, 716, 976, 26);
    state.theme.control(beta_notice, L"STATIC", true);
    note(-1,
         L"Future releases are planned to be paid: channel subscriber/supporter access or a one-time "
         L"license purchase.",
         24, 752, 976, 32);
    state.show_page(0);
    state.spectator_controls.initialize(
        state.controls, state.theme, state.options, state.renderer,
        [&] {
            state.queue_save();
            state.dirty = true;
            state.heartbeat();
            state.update_status();
        },
        [&] { state.learn_chat(); });
    state.advanced.initialize(
        state.controls, state.theme, state.options, state.render_ini,
        [&] {
            state.labels();
            state.queue_save();
            state.dirty = true;
        },
        [&](std::wstring message) {
            if (message.rfind(L"Render settings saved.", 0) == 0) {
                if (_wcsicmp(state.ini.c_str(), state.render_ini.c_str()))
                    write_render_settings(state.ini, read_render_settings(state.render_ini));
                state.save();
            }
            state.setup_message = std::move(message);
            state.update_status();
        });
    const auto presets_directory = state.presets_directory =
        self_test ? std::filesystem::path(state.ini + L"-presets")
                  : std::filesystem::path(state.directory).parent_path() / L"Presets";
    state.presets.initialize(state.controls, state.theme, presets_directory,
                             {[&] { return state.snapshot(); },
                              [&](const std::string &bytes) {
                                  if (!state.apply_snapshot(bytes))
                                      return false;
                                  wchar_t name[65]{};
                                  GetDlgItemTextW(state.presets.handle(), PresetsDialog::names_id, name, 65);
                                  state.preset_cursor = L"saved:" + std::wstring(name);
                                  return true;
                              },
                              [&](BuiltinPreset preset) { state.builtin(preset); }});
    state.set_tuning(state.options);
    state.list_monitors();
    if (!self_test)
        state.move_to_monitor();
    state.refresh_sources();
    state.labels();
    state.renderer.attach(state.output);
    state.register_hotkeys();
    if (verify_window) {
        // Explicit diagnostic mode: capture only the auto-selected iRacing
        // simulator process, without opening UI, installing a layer or editing settings.
        unsigned frames = 0;
        const auto begin = GetTickCount64();
        while (state.source_window && GetTickCount64() - begin < 4000 && frames < 15) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (state.capture_frame(GetTickCount64())) {
                ++frames;
                state.renderer.draw(state.options, false);
            }
            Sleep(8);
        }
        std::vector<std::uint32_t> pixels;
        unsigned width{}, height{};
        const bool okay = frames > 0 && state.renderer.readback(pixels, width, height);
        if (okay) {
            std::ofstream out(std::filesystem::path(state.directory + L"\\preview-live-check.ppm"),
                              std::ios::binary);
            out << "P6\n" << width << ' ' << height << "\n255\n";
            for (auto pixel : pixels) {
                const char rgb[3] = {char(pixel), char(pixel >> 8), char(pixel >> 16)};
                out.write(rgb, 3);
            }
        }
        std::wofstream report(std::filesystem::path(state.directory + L"\\preview-live-check.txt"));
        report << L"frames=" << frames << L" size=" << width << L"x" << height << L" source_pid="
               << state.source_pid << L"\n";
        report << state.capture.error() << L"\n" << state.renderer.error() << L"\n";
        state.capture.stop();
        DestroyWindow(state.output);
        DestroyWindow(state.controls);
        DeleteFileW(state.ini.c_str());
        CloseHandle(singleton);
        app = nullptr;
        return okay ? 0 : 2;
    }
    if (self_test) {
        if (std::wstring(command).find(L"--show-controls") != std::wstring::npos) {
            ShowWindow(state.controls, SW_SHOW);
            const auto until = GetTickCount64() + 120000;
            MSG msg{};
            while (!state.quit && GetTickCount64() < until) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (!state.spectator_controls.route(msg) && !state.overlays.route(msg) &&
                        !state.presets.route(msg) && !state.advanced.route(msg) &&
                        !IsDialogMessageW(state.controls, &msg)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
                MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            ShowWindow(state.controls, SW_HIDE);
        }
        std::ofstream trace(std::filesystem::path(state.directory + L"\\preview-ui-check.txt"));
        const auto visible_control = [](HWND h) {
            return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE);
        };
        const auto controls_fit = [](HWND panel) {
            RECT client{};
            GetClientRect(panel, &client);
            for (auto h = GetWindow(panel, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
                if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE))
                    continue;
                RECT r{};
                GetWindowRect(h, &r);
                MapWindowPoints(nullptr, panel, reinterpret_cast<POINT *>(&r), 2);
                if (r.left < 0 || r.top < 0 || r.right > client.right || r.bottom > client.bottom)
                    return false;
            }
            return true;
        };
        const auto before_navigation = state.snapshot();
        const auto selected_navigation = [](HWND panel, int first, unsigned count, unsigned selected) {
            for (unsigned p = 0; p < count; ++p) {
                const auto button = GetDlgItem(panel, first + p);
                if ((GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK) != BS_AUTORADIOBUTTON ||
                    SendMessageW(button, BM_GETCHECK, 0, 0) != (p == selected ? BST_CHECKED : BST_UNCHECKED))
                    return false;
            }
            return true;
        };
        bool navigation_ok = IsWindow(GetDlgItem(state.controls, 1500)) && visible_control(state.mode) &&
                             !visible_control(state.smooth) && !visible_control(state.monitor);
        SendMessageW(state.controls, WM_COMMAND, 1501, 0);
        navigation_ok = navigation_ok && !visible_control(state.mode) && visible_control(state.smooth);
        SendMessageW(state.controls, WM_COMMAND, 1503, 0);
        navigation_ok = navigation_ok && visible_control(state.monitor) && !visible_control(state.smooth) &&
                        before_navigation == state.snapshot() &&
                        selected_navigation(state.controls, navigation_id, 4, 3);
        SendMessageW(state.controls, WM_COMMAND, 1500, 0);
        trace << "navigation-preserves-settings=" << navigation_ok << '\n';
        if (!navigation_ok) {
            DestroyWindow(state.output);
            DestroyWindow(state.controls);
            DeleteFileW(state.ini.c_str());
            CloseHandle(singleton);
            app = nullptr;
            return 1;
        }
        const bool display_controls =
            IsWindow(GetDlgItem(state.controls, 115)) &&
            GetDlgItemInt(state.controls, 122, nullptr, FALSE) == 3840 &&
            GetDlgItemInt(state.controls, 123, nullptr, FALSE) == 2160 &&
            SendDlgItemMessageW(state.controls, 125, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            SendDlgItemMessageW(state.controls, adaptive_horizon_id, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state.source_window = state.output;
        state.source_pid = GetCurrentProcessId() + 1;
        const bool rejected_stale =
            !state.capture_frame(GetTickCount64()) && !state.source_window && !state.capture.running();
        SendMessageW(state.smooth, TBM_SETPOS, TRUE, 650);
        SendMessageW(state.controls, WM_HSCROLL, 0, reinterpret_cast<LPARAM>(state.smooth));
        SendMessageW(state.controls, WM_COMMAND, pause_id, 0);
        bool okay = display_controls && IsWindow(GetDlgItem(state.controls, layers_id)) && rejected_stale &&
                    state.paused && state.options.smoothing == 65 && state.renderer.demo(0) &&
                    state.renderer.draw(state.options, false);
        okay = okay && state.theme.icon && state.theme.small_icon;
        // Global shortcuts must reach the same actions even while a child
        // control owns focus. Diagnostic runs send messages without reserving keys.
        SendMessageW(state.controls, WM_HOTKEY, pause_id, 0);
        bool interactions_ok = !state.paused && IsWindow(state.comparison) &&
                               IsWindow(GetDlgItem(state.controls, next_preset_id)) &&
                               IsWindow(GetDlgItem(state.controls, hotkeys_id));
        const bool spectator_entry_ok = IsWindow(GetDlgItem(state.controls, spectator_id));
        trace << "spectator-controls-entry=" << spectator_entry_ok << '\n';
        interactions_ok = interactions_ok && spectator_entry_ok;
        const auto before_spectator_test = state.options;
        state.spectator_controls.open(false);
        const auto spectator_panel = state.spectator_controls.handle();
        bool spectator_ok = IsWindow(spectator_panel) && !IsWindowVisible(spectator_panel) &&
                            !state.options.spectator.gaze_enabled && !state.options.spectator.chat_enabled &&
                            !state.options.spectator.natural_enabled && !state.options.visual.active() &&
                            !state.options.director.lookahead_enabled &&
                            !state.options.director.auto_crop_enabled &&
                            !state.options.director.speed_fov_enabled &&
                            std::size(hotkey_actions) == 5 && hotkey_letters[4] == L'C';
        ShowWindow(spectator_panel, SW_SHOWNOACTIVATE);
        const auto before_spectator_navigation = state.snapshot();
        const wchar_t *spectator_pages[]{L"gaze", L"motion", L"exposure", L"bloom", L"color", L"director"};
        const int spectator_markers[]{
            SpectatorControls::camera_bool_id + 1, SpectatorControls::camera_bool_id + 3,
            SpectatorControls::visual_bool_id + 2, SpectatorControls::visual_bool_id + 3,
            SpectatorControls::visual_bool_id + 4, SpectatorControls::director_bool_id};
        for (unsigned p = 0; p < std::size(spectator_pages); ++p) {
            SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id + p, 0);
            spectator_ok =
                selected_navigation(spectator_panel, SpectatorControls::page_id,
                                    unsigned(std::size(spectator_pages)), p) && spectator_ok;
            for (unsigned other = 0; other < std::size(spectator_pages); ++other)
                spectator_ok = spectator_ok &&
                               bool(visible_control(GetDlgItem(spectator_panel, spectator_markers[other]))) ==
                                   (p == other);
            spectator_ok = controls_fit(spectator_panel) && spectator_ok;
            RedrawWindow(spectator_panel, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            spectator_ok = save_controls(spectator_panel, state.directory + L"\\preview-" +
                                                              spectator_pages[p] + L"-test.bmp") &&
                           spectator_ok;
        }
        spectator_ok = spectator_ok && before_spectator_navigation == state.snapshot();
        trace << "spectator-navigation=" << spectator_ok << '\n';
        // Director edits use native Tab commitment and persist when navigating to another page.
        const auto director_input = GetDlgItem(spectator_panel, SpectatorControls::director_number_id);
        SetFocus(director_input);
        SetWindowTextW(director_input, L"4");
        MSG director_tab{};
        director_tab.hwnd = director_input;
        director_tab.message = WM_KEYDOWN;
        director_tab.wParam = VK_TAB;
        spectator_ok = state.spectator_controls.route(director_tab) && spectator_ok;
        spectator_ok = spectator_ok && state.options.director.lookahead_frames == 4 &&
                       GetFocus() != director_input;
        SetFocus(director_input);
        SetWindowTextW(director_input, L"2");
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id, 0);
        spectator_ok = spectator_ok && state.options.director.lookahead_frames == 2 &&
                       GetFocus() == GetDlgItem(spectator_panel, SpectatorControls::page_id);
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id + 5, 0);
        for (unsigned i = 0; i < std::size(director_bool_fields); ++i) {
            CheckDlgButton(spectator_panel, SpectatorControls::director_bool_id + i, BST_CHECKED);
            SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::director_bool_id + i, 0);
        }
        SetDlgItemTextW(spectator_panel, SpectatorControls::director_number_id + 1, L"999");
        SendMessageW(spectator_panel, WM_COMMAND,
                     MAKEWPARAM(SpectatorControls::director_number_id + 1, EN_KILLFOCUS), 0);
        SetDlgItemTextW(spectator_panel, SpectatorControls::director_number_id + 2, L"nan");
        SendMessageW(spectator_panel, WM_COMMAND,
                     MAKEWPARAM(SpectatorControls::director_number_id + 2, EN_KILLFOCUS), 0);
        SendMessageW(GetDlgItem(spectator_panel, SpectatorControls::telemetry_id), CB_SETCURSEL, 2, 0);
        SendMessageW(spectator_panel, WM_COMMAND,
                     MAKEWPARAM(SpectatorControls::telemetry_id, CBN_SELCHANGE), 0);
        spectator_ok = spectator_ok && state.options.director.lookahead_enabled &&
                       state.options.director.auto_crop_enabled && state.options.director.speed_fov_enabled &&
                       state.options.director.auto_crop_max_percent == 30 &&
                       state.options.director.auto_crop_release_ms == 1200 &&
                       state.options.director.telemetry_game == 2;
        const auto director_snapshot = state.snapshot();
        state.spectator_controls.refresh();
        spectator_ok = spectator_ok && selected_navigation(spectator_panel, SpectatorControls::page_id, 6, 5) &&
                       director_snapshot == state.snapshot();
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id, 0);
        trace << "director-controls-edit-navigation=" << spectator_ok << '\n';
        // Native Tab navigation commits a numeric edit and moves focus to the next field.
        const auto gaze_input = GetDlgItem(spectator_panel, SpectatorControls::camera_number_id);
        SetFocus(gaze_input);
        SetWindowTextW(gaze_input, L"37");
        MSG tab_key{};
        tab_key.hwnd = gaze_input;
        tab_key.message = WM_KEYDOWN;
        tab_key.wParam = VK_TAB;
        spectator_ok = state.spectator_controls.route(tab_key) && spectator_ok;
        spectator_ok =
            spectator_ok && state.options.spectator.gaze_strength == 37 && GetFocus() != gaze_input;
        // Switching away from a focused edit commits it, then leaves focus on the visible navigation.
        SetFocus(gaze_input);
        SetWindowTextW(gaze_input, L"42");
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id + 1, 0);
        spectator_ok = spectator_ok && state.options.spectator.gaze_strength == 42 &&
                       GetFocus() == GetDlgItem(spectator_panel, SpectatorControls::page_id + 1);
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id, 0);
        trace << "spectator-keyboard-commit=" << spectator_ok << '\n';
        RedrawWindow(spectator_panel, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        spectator_ok =
            save_controls(spectator_panel, state.directory + L"\\preview-spectator-test.bmp") && spectator_ok;
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id + 2, 0);
        RedrawWindow(spectator_panel, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        spectator_ok =
            save_controls(spectator_panel, state.directory + L"\\preview-visual-test.bmp") && spectator_ok;
        ShowWindow(spectator_panel, SW_HIDE);
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::page_id, 0);
        SetDlgItemTextW(spectator_panel, SpectatorControls::camera_number_id, L"999");
        SendMessageW(spectator_panel, WM_COMMAND,
                     MAKEWPARAM(SpectatorControls::camera_number_id, EN_KILLFOCUS), 0);
        spectator_ok = spectator_ok && state.options.spectator.gaze_strength == 100;
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::cinematic_id, 0);
        spectator_ok = spectator_ok && state.options.spectator.natural_enabled &&
                       state.options.spectator.natural_settling_ms == 300 && !state.options.visual.active();
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::natural_id, 0);
        spectator_ok = spectator_ok && state.options.spectator.natural_settling_ms == 180;
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::filmic_id, 0);
        spectator_ok = spectator_ok && state.options.visual.color && state.options.visual.bloom;
        CheckDlgButton(spectator_panel, SpectatorControls::compare_scope_id, BST_CHECKED);
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::compare_scope_id, 0);
        wchar_t effects_comparison_label[64]{};
        GetWindowTextW(state.comparison, effects_comparison_label, 64);
        spectator_ok =
            spectator_ok && std::wstring(effects_comparison_label).find(L"effects") != std::wstring::npos;
        spectator_ok = state.save() && spectator_ok;
        App spectator_restored;
        spectator_restored.ini = state.ini;
        spectator_restored.read();
        spectator_ok = spectator_ok && spectator_restored.options.spectator.natural_enabled &&
                       spectator_restored.options.visual.color && spectator_restored.options.visual.bloom &&
                       spectator_restored.options.director.lookahead_enabled &&
                       spectator_restored.options.director.auto_crop_enabled &&
                       spectator_restored.options.director.speed_fov_enabled &&
                       spectator_restored.options.director.lookahead_frames == 2 &&
                       spectator_restored.options.director.auto_crop_max_percent == 30 &&
                       spectator_restored.options.director.telemetry_game == 2 &&
                       spectator_restored.options.compare_effects;
        // Explicit calibration requests gaze even with framing off, then bypass and pause release it.
        const bool old_window_mode = state.window_mode, old_demo = state.demo;
        state.window_mode = state.demo = false;
        state.options.spectator = {};
        ShowWindow(state.output, SW_SHOWNOACTIVATE);
        SendMessageW(state.controls, WM_HOTKEY, learn_chat_id, 0);
        spectator_ok = spectator_ok && state.renderer.calibration_active();
        if (state.channel.lock()) {
            spectator_ok = spectator_ok && state.channel.data()->gaze_requested == 1;
            state.channel.unlock();
        } else
            spectator_ok = false;
        state.options.spectator.bypass = true;
        state.heartbeat();
        if (state.channel.lock()) {
            spectator_ok = spectator_ok && state.channel.data()->gaze_requested == 0;
            state.channel.unlock();
        } else
            spectator_ok = false;
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::cancel_id, 0);
        spectator_ok = spectator_ok && !state.renderer.calibration_active();
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::clear_id, 0);
        SendMessageW(spectator_panel, WM_COMMAND, SpectatorControls::neutral_id, 0);
        spectator_ok = spectator_ok && !state.options.visual.active();
        ShowWindow(state.output, SW_HIDE);
        state.window_mode = old_window_mode;
        state.demo = old_demo;
        state.options = before_spectator_test;
        state.spectator_controls.refresh();
        state.heartbeat();
        state.save();
        trace << "spectator-controls-settings-learning=" << spectator_ok << '\n';
        interactions_ok = interactions_ok && spectator_ok;
        RECT overlay_button_rect{}, controls_rect{};
        GetWindowRect(GetDlgItem(state.controls, overlays_id), &overlay_button_rect);
        MapWindowPoints(nullptr, state.controls, reinterpret_cast<POINT *>(&overlay_button_rect), 2);
        GetClientRect(state.controls, &controls_rect);
        interactions_ok = interactions_ok && overlay_button_rect.left >= 0 && overlay_button_rect.top >= 0 &&
                          overlay_button_rect.right <= controls_rect.right &&
                          overlay_button_rect.bottom <= controls_rect.bottom;
        RECT spectator_button_rect{};
        GetWindowRect(GetDlgItem(state.controls, spectator_id), &spectator_button_rect);
        MapWindowPoints(nullptr, state.controls, reinterpret_cast<POINT *>(&spectator_button_rect), 2);
        interactions_ok = interactions_ok && spectator_button_rect.left >= 0 &&
                          spectator_button_rect.top >= 0 &&
                          spectator_button_rect.right <= controls_rect.right &&
                          spectator_button_rect.bottom <= controls_rect.bottom;
        if (state.channel.lock()) {
            auto &catalog = state.channel.data()->overlay_catalog;
            catalog = {};
            catalog.producer_pid = 4242;
            catalog.session = 701;
            catalog.updated_ms = GetTickCount64();
            catalog.count = 2;
            catalog.layers[0] = {0, 1, 1, 512, 256};
            wcscpy_s(catalog.layers[0].label, L"Quad 512 x 256 - slot 1");
            catalog.layers[1] = {1, 999, 0, 0, 0};
            wcscpy_s(catalog.layers[1].label, L"Unsupported type 999 - slot 2");
            state.channel.unlock();
        } else
            interactions_ok = false;
        SendMessageW(state.controls, WM_COMMAND, overlays_id, 0);
        const auto overlay_window = state.overlays.window;
        interactions_ok =
            interactions_ok && IsWindowVisible(overlay_window) && IsWindow(GetDlgItem(overlay_window, 3));
        SendMessageW(overlay_window, WM_COMMAND, 1, 0);
        interactions_ok = interactions_ok && state.options.overlays.mode == 1;
        SendMessageW(overlay_window, WM_COMMAND, 3, 0);
        SendMessageW(overlay_window, WM_COMMAND, 2, 0);
        interactions_ok = interactions_ok && state.options.overlays.mode == 0 &&
                          IsWindowEnabled(GetDlgItem(overlay_window, 100)) &&
                          !IsWindowEnabled(GetDlgItem(overlay_window, 101));
        SendDlgItemMessageW(overlay_window, 100, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(overlay_window, WM_COMMAND, 100, 0);
        interactions_ok = interactions_ok && state.options.overlays.mode == 2 &&
                          state.options.overlays.mask == 1 && state.options.overlays.producer_pid == 4242 &&
                          state.options.overlays.session == 701;
        interactions_ok = save_controls(overlay_window, state.directory + L"\\preview-overlays-test.bmp") &&
                          interactions_ok;
        if (state.channel.lock()) {
            state.channel.data()->overlay_catalog.session = 702;
            state.channel.unlock();
        } else
            interactions_ok = false;
        SendDlgItemMessageW(overlay_window, 100, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(overlay_window, WM_COMMAND, 100, 0);
        interactions_ok =
            interactions_ok && state.options.overlays.session == 701 && state.options.overlays.mask == 1;
        SendMessageW(overlay_window, WM_COMMAND, 2, 0);
        if (state.channel.lock()) {
            state.channel.data()->overlay_catalog = {};
            state.channel.unlock();
        }
        SendMessageW(overlay_window, WM_CLOSE, 0, 0);
        // A/B changes only the draw copy, returns at its deadline, and preserves
        // the complete serialized configuration (including framing and presets).
        state.last_received_ms = GetTickCount64();
        const auto before_comparison = state.snapshot();
        const auto comparison_started = GetTickCount64();
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        const auto comparison_deadline = state.compare_until;
        const auto compared_options = state.draw_options(comparison_deadline - 1);
        wchar_t interaction_text[512]{};
        GetWindowTextW(state.compact_status, interaction_text, 512);
        interactions_ok = interactions_ok && comparison_deadline >= comparison_started + 20000 &&
                          comparison_deadline <= GetTickCount64() + 20000 &&
                          compared_options.compare_original && !state.options.compare_original &&
                          compared_options.smoothing == 65 && compared_options.output_width == 3840 &&
                          compared_options.output_height == 2160 && compared_options.crop == 10 &&
                          std::wstring(interaction_text).find(L"Original") != std::wstring::npos &&
                          !before_comparison.empty() && state.snapshot() == before_comparison;
        // Keep the source fresh so this specifically tests the 20-second timeout.
        state.last_received_ms = comparison_deadline - 1;
        state.refresh_comparison(comparison_deadline - 1);
        interactions_ok = interactions_ok && state.comparing(comparison_deadline - 1);
        state.refresh_comparison(comparison_deadline);
        interactions_ok = interactions_ok && !state.compare_until &&
                          !state.draw_options(comparison_deadline).compare_original &&
                          state.snapshot() == before_comparison;
        SendMessageW(state.controls, WM_COMMAND, compare_id, 0);
        interactions_ok = interactions_ok && state.compare_until;
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        interactions_ok = interactions_ok && !state.compare_until;
        SendMessageW(state.controls, WM_COMMAND, compare_id, 0);
        SendMessageW(state.controls, WM_HOTKEY, reset_id, 0);
        interactions_ok = interactions_ok && !state.compare_until &&
                          state.setup_message.find(L"approximately upright") != std::wstring::npos &&
                          state.snapshot() == before_comparison;
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        SendMessageW(state.controls, WM_HOTKEY, pause_id, 0);
        interactions_ok = interactions_ok && state.paused && !state.compare_until;
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        interactions_ok = interactions_ok && !state.compare_until;
        state.measured_fps = 89;
        state.update_status();
        GetWindowTextW(state.compact_status, interaction_text, 512);
        interactions_ok = interactions_ok && std::wstring(interaction_text).find(L"0 FPS") == 0 &&
                          std::wstring(interaction_text).find(L"paused") != std::wstring::npos &&
                          std::wstring(interaction_text).find(L"Original") == std::wstring::npos;
        // The selected layout persists, while changing it during A/B preserves
        // the same deadline and never persists the temporary comparison itself.
        SendMessageW(state.controls, WM_COMMAND, comparison_split_id, 0);
        state.paused = false;
        state.last_received_ms = GetTickCount64();
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        const auto mode_deadline = state.compare_until;
        GetWindowTextW(state.compact_status, interaction_text, 512);
        interactions_ok = interactions_ok && mode_deadline &&
                          state.draw_options(mode_deadline - 1).compare_mode == 1 &&
                          std::wstring(interaction_text).find(L"50/50 comparison") != std::wstring::npos;
        SendMessageW(state.controls, WM_COMMAND, comparison_side_by_side_id, 0);
        interactions_ok = interactions_ok && state.compare_until == mode_deadline &&
                          state.draw_options(mode_deadline - 1).compare_mode == 2;
        {
            App restored;
            restored.ini = state.ini;
            restored.read();
            interactions_ok =
                interactions_ok && restored.options.compare_mode == 2 && !restored.options.compare_original;
        }
        SendMessageW(state.controls, WM_COMMAND, comparison_original_id, 0);
        state.cancel_comparison();
        state.paused = true;
        SendMessageW(state.controls, WM_COMMAND, hotkeys_off_id, 0);
        SendMessageW(state.controls, WM_HOTKEY, pause_id, 0);
        interactions_ok = interactions_ok && state.paused;
        SendMessageW(state.controls, WM_COMMAND, hotkeys_alternate_id, 0);
        {
            App restored;
            restored.ini = state.ini;
            restored.read();
            interactions_ok =
                interactions_ok && restored.hotkey_profile == 2 && !restored.options.compare_original;
        }
        SendMessageW(state.controls, WM_COMMAND, hotkeys_default_id, 0);
        SendMessageW(state.controls, WM_HOTKEY, 9999, 0);
        interactions_ok = interactions_ok && state.paused;
        state.paused = false;
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        state.expired = true;
        state.refresh_comparison(GetTickCount64());
        interactions_ok = interactions_ok && !state.compare_until;
        trace << "comparison-modes=" << interactions_ok << '\n';
        state.expired = false;
        state.paused = false;
        state.last_received_ms = GetTickCount64();
        SendMessageW(state.controls, WM_COMMAND, compare_id, 0);
        // A popup has its own message loop: its timer must end A/B without
        // waiting for the outer capture loop to resume.
        state.compare_until = GetTickCount64() - 1;
        SendMessageW(state.controls, WM_TIMER, comparison_timer, 0);
        interactions_ok = interactions_ok && !state.compare_until;
        state.paused = true;
        state.setup_message.clear();
        trace << "preview-interactions=" << interactions_ok << '\n';
        okay = interactions_ok && okay;
        // A stalled source clears the output after two seconds and recovers on
        // the very next frame; a deliberate pause retains the current image.
        state.last_received_ms = 10000;
        state.paused = false;
        okay = okay && !state.waiting(11999) && state.waiting(12000);
        state.last_received_ms = 12001;
        okay = okay && !state.waiting(12001);
        state.paused = true;
        okay = okay && !state.waiting(15000);
        state.expired = true;
        okay = okay && state.waiting(15000) && !state.active() &&
               std::wstring(state.waiting_text()).find(L"Build expired") != std::wstring::npos;
        Channel obs_consumer;
        bool cooperation_ok = obs_consumer.open(true, channel_name) && obs_consumer.heartbeat(1, 90);
        state.heartbeat();
        if (obs_consumer.lock()) {
            cooperation_ok = cooperation_ok && !obs_consumer.data()->paused &&
                             obs_consumer.data()->eye == 1 && obs_consumer.data()->fps == 90;
            obs_consumer.unlock();
        } else
            cooperation_ok = false;
        obs_consumer.close();
        trace << "consumer-cooperation=" << cooperation_ok << '\n';
        okay = cooperation_ok && okay;
        state.heartbeat();
        if (state.channel.lock()) {
            okay = okay && state.channel.data()->paused == 1;
            state.channel.unlock();
        } else
            okay = false;
        state.update_status();
        wchar_t expired_status[512]{};
        GetWindowTextW(state.status, expired_status, 512);
        okay = okay && std::wstring(expired_status).find(L"has expired") != std::wstring::npos;
        okay = okay && state.renderer.draw(state.options, true, state.waiting_text());
        state.expired = false;
        state.paused = false;
        state.window_mode = false;
        state.last_received_ms = 1;
        state.paused = true;
        state.window_mode = true;
        state.last_received_ms = 0;
        state.labels();
        trace << "initial=" << okay << '\n';
        // Fine-grained controls persist exactly; invalid edits cannot partially resize output.
        const int trims[] = {123, 45, 67, 89};
        for (unsigned i = 0; i < 4; ++i)
            SendMessageW(state.trims[i], TBM_SETPOS, TRUE, trims[i]);
        SendMessageW(state.horizon, TBM_SETPOS, TRUE, 425);
        SendMessageW(state.mirror_limit, TBM_SETPOS, TRUE, 78);
        SendMessageW(state.mirror_softness, TBM_SETPOS, TRUE, 635);
        SendMessageW(state.controls, WM_HSCROLL, 0, reinterpret_cast<LPARAM>(state.trims[0]));
        SetWindowTextW(state.output_width, L"1920");
        SetWindowTextW(state.output_height, L"1080");
        SetWindowTextW(state.aspect, L"21:9");
        okay = state.apply_output() && okay;
        SetWindowTextW(state.output_height, L"0");
        okay = !state.apply_output() && state.options.output_width == 1920 &&
               state.options.output_height == 1080 && okay;
        SetWindowTextW(state.output_height, L"1080");
        SetWindowTextW(state.aspect, L"16:9garbage");
        okay = !state.apply_output() && state.aspect_numerator == 21 && okay;
        SetWindowTextW(state.aspect, L"21:9");
        state.setup_message.clear();
        state.start_fullscreen = false;
        SendMessageW(state.adaptive_horizon, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(state.controls, WM_COMMAND, adaptive_horizon_id, 0);
        state.save();
        {
            App restored;
            restored.ini = state.ini;
            restored.read();
            okay = okay && std::abs(restored.options.crop_left - 12.3) < 1e-8 &&
                   std::abs(restored.options.crop_right - 4.5) < 1e-8 &&
                   std::abs(restored.options.crop_top - 6.7) < 1e-8 &&
                   std::abs(restored.options.crop_bottom - 8.9) < 1e-8 &&
                   std::abs(restored.options.horizon_lock - 42.5) < 1e-8 &&
                   std::abs(restored.options.mirror_limit - 7.8) < 1e-8 &&
                   std::abs(restored.options.mirror_softness - 63.5) < 1e-8 &&
                   restored.options.output_width == 1920 && restored.options.output_height == 1080 &&
                   restored.aspect_numerator == 21 && restored.aspect_denominator == 9 &&
                   !restored.start_fullscreen && !restored.options.adaptive_horizon;
        }
        state.options.mirror_stretch = 18.6;
        const auto before_preset = state.options;
        SendMessageW(state.controls, WM_COMMAND, comfort_id, 0);
        SendMessageW(state.controls, WM_COMMAND, comfort_id, 0); // Reapplying retains the original undo.
        okay = okay && state.can_undo_comfort && state.options.smoothing == 60 &&
               state.options.horizon_lock == 50 && state.options.mirror_softness == 70 &&
               SendMessageW(state.mirror_softness, TBM_GETPOS, 0, 0) == 700 &&
               state.options.output_width == before_preset.output_width &&
               state.options.crop_left == before_preset.crop_left;
        state.options.mirror_stretch = 77.7;
        state.options.crop_left = 2; // A later framing adjustment is not undone by the tuning preset.
        SendMessageW(state.trims[0], TBM_SETPOS, TRUE, 20);
        SendMessageW(state.controls, WM_COMMAND, undo_comfort_id, 0);
        okay = okay && !state.can_undo_comfort && state.options.crop_left == 2 &&
               state.options.smoothing == before_preset.smoothing &&
               state.options.mirror_softness == before_preset.mirror_softness &&
               state.options.mirror_stretch == before_preset.mirror_stretch &&
               state.options.horizon_lock == before_preset.horizon_lock &&
               state.options.adaptive_horizon == before_preset.adaptive_horizon;
        state.advanced.open(1, true);
        const auto panel = state.advanced.handle();
        bool advanced_ok = panel && IsWindow(panel);
        const auto stretch_slider = GetDlgItem(panel, AdvancedSettings::stretch_id);
        advanced_ok = advanced_ok && IsWindowVisible(stretch_slider);
        RECT panel_bounds{};
        GetClientRect(panel, &panel_bounds);
        for (auto control = GetWindow(panel, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
            if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE))
                continue;
            RECT bounds{};
            GetWindowRect(control, &bounds);
            MapWindowPoints(nullptr, panel, reinterpret_cast<POINT *>(&bounds), 2);
            advanced_ok = advanced_ok && bounds.left >= 0 && bounds.top >= 0 &&
                          bounds.right <= panel_bounds.right && bounds.bottom <= panel_bounds.bottom;
        }
        SendMessageW(stretch_slider, TBM_SETPOS, TRUE, 642);
        SendMessageW(panel, WM_HSCROLL, 0, reinterpret_cast<LPARAM>(stretch_slider));
        advanced_ok = advanced_ok && state.options.mirror_stretch == 64.2;

        // Per-eye margins are live and persist independently from render requests.
        if (panel) {
            SendMessageW(GetDlgItem(panel, AdvancedSettings::separate_id), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(panel, WM_COMMAND, AdvancedSettings::separate_id, 0);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::left_id), TBM_SETPOS, TRUE, 20);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::right_id), TBM_SETPOS, TRUE, 200);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::vertical_id), TBM_SETPOS, TRUE, 675);
            SendMessageW(panel, WM_HSCROLL, 0,
                         reinterpret_cast<LPARAM>(GetDlgItem(panel, AdvancedSettings::left_id)));
            advanced_ok = advanced_ok && state.options.edge_reserve[1].enabled &&
                          state.options.edge_reserve[1].left == 2 &&
                          state.options.edge_reserve[1].right == 20 &&
                          state.options.vertical_offset == 67.5 && !state.options.edge_reserve[0].enabled;
            SendMessageW(panel, WM_COMMAND, AdvancedSettings::inward_id, 0);
            advanced_ok = advanced_ok && !read_render_settings(state.ini).enabled;
            SendMessageW(panel, WM_COMMAND, AdvancedSettings::save_render_id, 0);
            const auto render = read_render_settings(state.ini);
            advanced_ok = advanced_ok && render.enabled && render.eyes[0].right == 10 &&
                          render.eyes[1].left == 10 && render.eyes[0].left == 0 && render.eyes[1].right == 0;
            App reloaded;
            reloaded.ini = state.ini;
            reloaded.read();
            advanced_ok = advanced_ok && reloaded.options.edge_reserve[1].left == 2 &&
                          reloaded.options.edge_reserve[1].right == 20 &&
                          reloaded.options.vertical_offset == 67.5 && reloaded.options.mirror_stretch == 64.2;
            advanced_ok =
                save_controls(panel, state.directory + L"\\preview-advanced-test.bmp") && advanced_ok;
        }
        trace << "advanced=" << advanced_ok << '\n';
        okay = okay && advanced_ok;
        state.window_mode = true;
        state.labels();
        bool profile_ok = IsWindowEnabled(state.eye);
        SendMessageW(state.eye, CB_SETCURSEL, 1, 0);
        SendMessageW(state.controls, WM_COMMAND, MAKEWPARAM(eye_id, CBN_SELCHANGE), 0);
        profile_ok =
            profile_ok && state.selected_eye == 1 && state.options.eye == 1 && !IsWindowEnabled(state.crop);
        SendMessageW(state.eye, CB_SETCURSEL, 0, 0);
        SendMessageW(state.controls, WM_COMMAND, MAKEWPARAM(eye_id, CBN_SELCHANGE), 0);
        profile_ok = profile_ok && state.options.eye == 0 && IsWindowEnabled(state.crop);
        trace << "window-profile=" << profile_ok << '\n';
        okay = okay && profile_ok;
        // Exercise the real dialog callbacks: unsaved render drafts, bypass,
        // both eyes, signed Y, source identity and future INI keys must survive.
        state.window_mode = false;
        state.selected_eye = state.options.eye = 1;
        state.rate = 90;
        state.options.stabilize = false;
        state.options.compare_mode = 2;
        state.options.compare_effects = true;
        state.options.spectator.gaze_enabled = state.options.spectator.chat_enabled = true;
        state.options.spectator.natural_enabled = true;
        state.options.spectator.chat_width_degrees = 27.5;
        for (const auto &f : director_bool_fields)
            state.options.director.*f.member = true;
        for (const auto &f : director_number_fields)
            state.options.director.*f.member = f.maximum;
        state.options.director.telemetry_game = 1;
        state.options.visual.exposure = state.options.visual.adaptation = true;
        state.options.visual.bloom = state.options.visual.color = true;
        state.options.visual.exposure_ev = -.75f;
        state.options.visual.metering_region = {.1f, .2f, .8f, .9f};
        state.options.overlays = {2, 0x8021, 4242, 18446744073709551000ull};
        state.options.vertical_offset = -43.2;
        state.options.edge_reserve[0] = {true, 3.4, 8.7, 5.6, 12.3};
        state.source_executable = L"C:\\Preset test\\simulator.exe";
        state.source_title = L"Simulator preview";
        state.save();
        WritePrivateProfileStringW(L"FutureSettings", L"RetainMe", L"complete snapshot", state.ini.c_str());
        SendMessageW(GetDlgItem(panel, AdvancedSettings::render_left_id), TBM_SETPOS, TRUE, 237);
        SendMessageW(panel, WM_HSCROLL, 0,
                     reinterpret_cast<LPARAM>(GetDlgItem(panel, AdvancedSettings::render_left_id)));
        // Advanced's live controls still hold the earlier Y value; set its signed value explicitly.
        SendMessageW(GetDlgItem(panel, AdvancedSettings::vertical_id), TBM_SETPOS, TRUE, -432);
        SendMessageW(panel, WM_HSCROLL, 0,
                     reinterpret_cast<LPARAM>(GetDlgItem(panel, AdvancedSettings::vertical_id)));
        state.presets.open(false);
        const auto preset_panel = state.presets.handle();
        SetDlgItemTextW(preset_panel, PresetsDialog::name_id, L"Complete round trip");
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::save_id, 0);
        PresetStore test_store(presets_directory);
        std::wstring preset_error;
        const auto stored_snapshot = test_store.read(L"Complete round trip", preset_error);
        bool preset_ok = stored_snapshot.has_value() && read_render_settings(state.ini).eyes[1].left == 10;
        state.builtin(BuiltinPreset::natural);
        state.options.compare_mode = 0;
        state.options.compare_effects = false;
        state.options.spectator = {};
        state.options.director = {};
        state.options.visual = {};
        state.options.overlays = {};
        state.options.crop_left = 42;
        state.options.edge_reserve[0] = {};
        state.options.edge_reserve[1] = {};
        state.options.vertical_offset = 99;
        state.options.mirror_stretch = 0;
        state.selected_eye = state.options.eye = 0;
        state.window_mode = true;
        state.rate = 30;
        state.start_fullscreen = true;
        state.source_executable.clear();
        state.source_title.clear();
        SetWindowTextW(state.output_width, L"1280");
        SetWindowTextW(state.output_height, L"720");
        SetWindowTextW(state.aspect, L"4:3");
        state.apply_output();
        state.save();
        if (state.channel.lock()) {
            auto &catalog = state.channel.data()->overlay_catalog;
            catalog = {};
            catalog.producer_pid = 4242;
            catalog.session = 18446744073709551000ull;
            catalog.updated_ms = GetTickCount64();
            catalog.count = 3;
            catalog.layers[0] = {0, 1, 1, 512, 256};
            catalog.layers[1] = {1, 1, 1, 512, 256};
            catalog.layers[2] = {5, 1, 1, 512, 256};
            for (auto &entry : catalog.layers)
                wcscpy_s(entry.label, L"Preset test quad");
            state.channel.unlock();
        } else
            preset_ok = false;
        SendMessageW(state.controls, WM_COMMAND, overlays_id, 0);
        const auto preset_overlay_panel = state.overlays.window;
        preset_ok =
            preset_ok && SendDlgItemMessageW(preset_overlay_panel, 100, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok =
            preset_ok && !state.window_mode && state.selected_eye == 1 && state.options.eye == 1 &&
            state.rate == 90 && !state.options.stabilize && !state.start_fullscreen &&
            state.options.compare_mode == 2 && !state.options.compare_original &&
            state.options.overlays.mode == 2 && state.options.overlays.mask == 0x8021 &&
            state.options.overlays.producer_pid == 4242 &&
            state.options.overlays.session == 18446744073709551000ull && state.options.output_width == 1920 &&
            state.options.output_height == 1080 && state.aspect_numerator == 21 &&
            state.aspect_denominator == 9 && state.options.crop_left == 2 &&
            state.options.crop_right == 4.5 && state.options.crop_top == 6.7 &&
            state.options.crop_bottom == 8.9 && state.options.vertical_offset == -43.2 &&
            state.options.mirror_stretch == 64.2 && SendMessageW(stretch_slider, TBM_GETPOS, 0, 0) == 642 &&
            state.options.edge_reserve[0].enabled && state.options.edge_reserve[0].left == 3.4 &&
            state.options.edge_reserve[0].right == 8.7 && state.options.edge_reserve[0].top == 5.6 &&
            state.options.edge_reserve[0].bottom == 12.3 && state.options.edge_reserve[1].enabled &&
            state.options.edge_reserve[1].right == 20 &&
            state.source_executable == L"C:\\Preset test\\simulator.exe" && !state.source_window &&
            read_render_settings(state.ini).eyes[1].left == 23.7 &&
            state.advanced.draft().eyes[1].left == 23.7 && SendMessageW(state.fps, CB_GETCURSEL, 0, 0) == 2 &&
            SendMessageW(state.eye, CB_GETCURSEL, 0, 0) == 1 &&
            SendMessageW(state.bypass, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            SendMessageW(GetDlgItem(panel, AdvancedSettings::vertical_id), TBM_GETPOS, 0, 0) == -432;
        wchar_t restored_chat_width[32]{};
        GetDlgItemTextW(spectator_panel, SpectatorControls::camera_number_id + 6, restored_chat_width, 32);
        preset_ok =
            preset_ok && state.options.compare_effects && state.options.spectator.gaze_enabled &&
            state.options.spectator.chat_enabled && state.options.spectator.natural_enabled &&
            state.options.spectator.chat_width_degrees == 27.5 && state.options.visual.exposure &&
            state.options.visual.adaptation && state.options.visual.bloom && state.options.visual.color &&
            state.options.visual.exposure_ev == -.75f && state.options.visual.metering_region.bottom == .9f &&
            std::wstring(restored_chat_width) == L"27.5" &&
            IsDlgButtonChecked(spectator_panel, SpectatorControls::visual_bool_id + 1) == BST_CHECKED &&
            !state.renderer.spectator_status().zone_valid;
        for (const auto &f : director_bool_fields)
            preset_ok = preset_ok && state.options.director.*f.member;
        for (const auto &f : director_number_fields)
            preset_ok = preset_ok && state.options.director.*f.member == f.maximum;
        preset_ok = preset_ok && state.options.director.telemetry_game == 1 &&
                    IsDlgButtonChecked(spectator_panel, SpectatorControls::director_bool_id) == BST_CHECKED &&
                    SendDlgItemMessageW(spectator_panel, SpectatorControls::telemetry_id,
                                        CB_GETCURSEL, 0, 0) == 1;
        preset_ok = preset_ok &&
                    SendDlgItemMessageW(preset_overlay_panel, 100, BM_GETCHECK, 0, 0) == BST_CHECKED &&
                    SendDlgItemMessageW(preset_overlay_panel, 101, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
                    SendDlgItemMessageW(preset_overlay_panel, 105, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendDlgItemMessageW(preset_overlay_panel, 101, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(preset_overlay_panel, WM_COMMAND, 101, 0);
        preset_ok = preset_ok && state.options.overlays.mask == 0x8023;
        // A temporarily absent selected layer must survive an unrelated edit.
        if (state.channel.lock()) {
            auto &catalog = state.channel.data()->overlay_catalog;
            catalog.layers[0] = catalog.layers[1];
            catalog.layers[1] = catalog.layers[2];
            catalog.count = 2;
            catalog.updated_ms = GetTickCount64();
            state.channel.unlock();
        } else
            preset_ok = false;
        SendMessageW(preset_overlay_panel, WM_COMMAND, 3, 0);
        preset_ok = preset_ok && !IsWindow(GetDlgItem(preset_overlay_panel, 100));
        SendDlgItemMessageW(preset_overlay_panel, 101, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(preset_overlay_panel, WM_COMMAND, 101, 0);
        preset_ok = preset_ok && state.options.overlays.mask == 0x8021;
        if (state.channel.lock()) {
            auto &catalog = state.channel.data()->overlay_catalog;
            catalog.layers[2] = {0, 1, 1, 512, 256};
            wcscpy_s(catalog.layers[2].label, L"Returning preset test quad");
            catalog.count = 3;
            catalog.updated_ms = GetTickCount64();
            state.channel.unlock();
        } else
            preset_ok = false;
        SendMessageW(preset_overlay_panel, WM_COMMAND, 3, 0);
        preset_ok =
            preset_ok && SendDlgItemMessageW(preset_overlay_panel, 100, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(preset_overlay_panel, WM_CLOSE, 0, 0);
        if (state.channel.lock()) {
            state.channel.data()->overlay_catalog = {};
            state.channel.unlock();
        }
        wchar_t retained[64]{};
        GetPrivateProfileStringW(L"FutureSettings", L"RetainMe", L"", retained, 64, state.ini.c_str());
        preset_ok = preset_ok && std::wstring(retained) == L"complete snapshot";
        const auto monitor_count = SendMessageW(state.monitor, CB_GETCOUNT, 0, 0);
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok = preset_ok && SendMessageW(state.monitor, CB_GETCOUNT, 0, 0) == monitor_count;
        // Next preset wraps through the built-ins and the current saved list,
        // including a complete snapshot loaded from the dialog.
        state.paused = false;
        state.renderer.demo(0);
        state.last_received_ms = GetTickCount64();
        SendMessageW(state.controls, WM_HOTKEY, compare_id, 0);
        bool preset_shortcuts_ok = state.compare_until != 0;
        SendMessageW(state.controls, WM_HOTKEY, next_preset_id, 0);
        preset_shortcuts_ok = preset_shortcuts_ok && !state.compare_until && state.options.smoothing == 25 &&
                              state.options.horizon_lock == 25 && state.options.vertical_offset == -43.2;
        SendMessageW(state.controls, WM_HOTKEY, next_preset_id, 0);
        preset_shortcuts_ok =
            preset_shortcuts_ok && state.options.smoothing == 60 && state.options.horizon_lock == 50;
        SendMessageW(state.controls, WM_COMMAND, next_preset_id, 0);
        preset_shortcuts_ok =
            preset_shortcuts_ok && state.options.smoothing == 70 && state.options.horizon_lock == 85;
        SendMessageW(state.controls, WM_HOTKEY, next_preset_id, 0);
        preset_shortcuts_ok = preset_shortcuts_ok && !state.options.stabilize &&
                              state.options.vertical_offset == -43.2 && state.options.crop_left == 2 &&
                              state.preset_cursor == L"saved:Complete round trip" &&
                              read_render_settings(state.ini).eyes[1].left == 23.7;
        state.paused = true;
        state.heartbeat();
        trace << "preset-shortcuts=" << preset_shortcuts_ok << '\n';
        preset_ok = preset_shortcuts_ok && preset_ok;
        // An unchanged registered 0.6.0 layer still reads its own settings file.
        const auto installed_ini = state.ini + L"-installed.ini";
        RenderSettings old_render{};
        preset_ok = write_render_settings(installed_ini, old_render) && preset_ok;
        state.render_ini = installed_ini;
        state.advanced.reload(installed_ini);
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok = preset_ok && read_render_settings(installed_ini).enabled &&
                    read_render_settings(installed_ini).eyes[1].left == 23.7 &&
                    state.advanced.draft().eyes[1].left == 23.7;
        state.render_ini = state.ini;
        state.advanced.reload(state.ini);
        DeleteFileW(installed_ini.c_str());
        // Never report a successful complete snapshot after a settings write fails.
        const auto attributes = GetFileAttributesW(state.ini.c_str());
        preset_ok = SetFileAttributesW(state.ini.c_str(), attributes | FILE_ATTRIBUTE_READONLY) && preset_ok;
        state.options.smoothing = 14.9;
        SetDlgItemTextW(preset_panel, PresetsDialog::name_id, L"Must not save stale values");
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::save_id, 0);
        preset_ok = test_store.list(preset_error).size() == 1 && preset_ok;
        SetFileAttributesW(state.ini.c_str(), attributes);
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok = save_controls(preset_panel, state.directory + L"\\preview-presets-test.bmp") && preset_ok;
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::delete_id, 0);
        preset_ok = test_store.list(preset_error).empty() && preset_ok;
        RemoveDirectoryW(presets_directory.c_str());
        trace << "complete-presets=" << preset_ok << '\n';
        okay = okay && preset_ok;
        RECT initial{}, expanded{}, restored{};
        trace << "settings=" << okay << '\n';
        GetWindowRect(state.output, &initial);
        state.toggle_fullscreen();
        GetWindowRect(state.output, &expanded);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(state.output, MONITOR_DEFAULTTONEAREST), &monitor);
        okay = okay && state.fullscreen && EqualRect(&expanded, &monitor.rcMonitor) &&
               !IsWindowVisible(state.output);
        trace << "fullscreen=" << okay << " visible=" << IsWindowVisible(state.output) << '\n';
        state.toggle_fullscreen();
        GetWindowRect(state.output, &restored);
        okay = okay && !state.fullscreen && EqualRect(&initial, &restored) && !IsWindowVisible(state.output);
        trace << "restore=" << okay << " rect=" << EqualRect(&initial, &restored)
              << " visible=" << IsWindowVisible(state.output) << '\n';
        if (state.channel.lock()) {
            okay = okay && state.channel.data()->paused == 1;
            state.channel.unlock();
        } else
            okay = false;
        state.update_status();
        const auto before_final_navigation = state.snapshot();
        const wchar_t *main_pages[]{L"capture", L"camera", L"framing", L"output"};
        ShowWindow(state.controls, SW_SHOWNOACTIVATE);
        for (unsigned p = 0; p < 4; ++p) {
            SendMessageW(state.controls, WM_COMMAND, navigation_id + p, 0);
            okay = controls_fit(state.controls) && okay;
            RedrawWindow(state.controls, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            okay = save_controls(state.controls,
                                 state.directory + L"\\preview-" + main_pages[p] + L"-test.bmp") &&
                   okay;
        }
        okay = okay && before_final_navigation == state.snapshot();
        trace << "page-layouts=" << okay << '\n';
        state.show_page(0);
        okay = save_controls(state.controls, state.directory + L"\\preview-controls-test.bmp") && okay;
        DestroyWindow(state.output);
        DestroyWindow(state.controls);
        DeleteFileW(state.ini.c_str());
        CloseHandle(singleton);
        app = nullptr;
        return okay ? 0 : 1;
    }
    ShowWindow(state.controls, show);
    ShowWindow(state.output, SW_SHOW);
    if (state.start_fullscreen && !state.fullscreen)
        state.toggle_fullscreen();
    state.heartbeat();
    state.update_status();
    std::uint64_t last_status = 0, last_heartbeat = 0;
    double next_frame = double(GetTickCount64());
    bool was_waiting = false;
    std::uint64_t last_waiting_paint{};
    MSG msg{};
    while (!state.quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state.quit = true;
                break;
            }
            if (!state.spectator_controls.route(msg) && !state.overlays.route(msg) &&
                !state.presets.route(msg) && !state.advanced.route(msg) &&
                !IsDialogMessageW(state.controls, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (state.quit)
            break;
        const auto now = GetTickCount64();
        state.refresh_comparison(now);
        if (now - last_heartbeat >= 200) {
            state.heartbeat();
            last_heartbeat = now;
        }
        if (now - last_status >= 500) {
            state.refresh_expiry();
            if (state.layer_editor_process &&
                WaitForSingleObject(state.layer_editor_process, 0) == WAIT_OBJECT_0) {
                CloseHandle(state.layer_editor_process);
                state.layer_editor_process = nullptr;
                state.refresh_editor_connection();
            }
            if (state.connection_process && WaitForSingleObject(state.connection_process, 0) == WAIT_OBJECT_0) {
                DWORD result = ERROR_PROCESS_ABORTED;
                GetExitCodeProcess(state.connection_process, &result);
                CloseHandle(state.connection_process);
                state.connection_process = nullptr;
                state.connection_finished(state.connection_enable, static_cast<LONG>(result));
            }
            if (now - state.fps_start >= 1000) {
                state.measured_fps = state.fps_frames * 1000. / double(now - state.fps_start);
                state.fps_start = now;
                state.fps_frames = 0;
            }
            state.update_status();
            last_status = now;
        }
        // OpenXR is already rate-limited by the producer. A second 60 Hz
        // polling gate misses frames whose GPU key becomes ready just after
        // the poll, leaving the producer's single slot occupied another frame.
        const bool submitted_feed = !state.demo && !state.window_mode;
        if (state.active() && (submitted_feed || double(now) >= next_frame)) {
            bool received = state.demo          ? state.renderer.demo((now - state.start_ms) / 1000.)
                            : state.window_mode ? state.capture_frame(now)
                                                : state.channel.owns_consumer() &&
                                                      state.renderer.consume(state.channel, true);
            if (received) {
                state.last_received_ms = now;
                ++state.fps_frames;
            }
            if ((received || state.dirty) && !state.waiting(now))
                state.renderer.draw(state.draw_options(now));
            state.dirty = false;
            if (!submitted_feed)
                next_frame = std::max(next_frame + 1000. / state.rate, double(now));
        } else if (state.dirty && !state.waiting(now) && IsWindowVisible(state.output) &&
                   !IsIconic(state.output)) {
            state.renderer.draw(state.draw_options(now));
            state.dirty = false;
        }
        const bool waiting = state.waiting(now);
        // Retry at a low cadence while idle: nonblocking Present may encounter
        // a full swapchain queue on the first transition away from live video.
        if (waiting && IsWindowVisible(state.output) && !IsIconic(state.output) &&
            (!was_waiting || now - last_waiting_paint >= 250)) {
            state.renderer.draw(state.draw_options(now), true, state.waiting_text());
            last_waiting_paint = now;
        }
        if (waiting != was_waiting) {
            InvalidateRect(state.output, nullptr, FALSE);
            was_waiting = waiting;
        }
        const DWORD delay = !state.active() ? 50
                            : submitted_feed
                                ? 1
                                : DWORD(std::clamp(next_frame - double(GetTickCount64()), 1., 16.));
        MsgWaitForMultipleObjectsEx(0, nullptr, delay, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    state.channel.release_consumer();
    if (state.timer_resolution)
        timeEndPeriod(1);
    state.capture.stop();
    state.save();
    DestroyWindow(state.output);
    CloseHandle(singleton);
    app = nullptr;
    return 0;
}
