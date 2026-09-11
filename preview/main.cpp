#define UNICODE
#define _UNICODE
#include "advanced.hpp"
#include "build_expiry.hpp"
#include "connection.hpp"
#include "presets.hpp"
#include "render_settings.hpp"
#include "renderer.hpp"
#include "settings_path.hpp"
#include "theme.hpp"
#include "window_capture.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mmsystem.h>
#include <shellapi.h>
#include <string>

namespace {
using namespace vrst::preview;
constexpr int setup_id = 101, remove_id = 102, show_id = 103, pause_id = 104, reset_id = 105, smooth_id = 106,
              crop_id = 107, mirror_id = 108, eye_id = 109, fps_id = 110, bypass_id = 111, mode_id = 112,
              source_id = 113, refresh_id = 114, left_id = 115, right_id = 116, top_id = 117, bottom_id = 118,
              horizon_id = 119, mirror_limit_id = 120, aspect_id = 121, width_id = 122, height_id = 123,
              apply_id = 124, auto_fullscreen_id = 125, monitor_id = 126, adaptive_horizon_id = 127,
              mirror_softness_id = 128, comfort_id = 129, undo_comfort_id = 130, advanced_id = 131,
              presets_id = 132, layers_id = 133;
constexpr UINT_PTR save_timer = 4101;
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
struct App {
    HWND controls{}, output{}, status{}, smooth_label{}, crop_label{}, smooth{}, crop{}, mirror{}, bypass{},
        eye{}, eye_label{}, fps{}, pause{}, mode{}, source{}, trims[4]{}, trim_labels[4]{}, horizon{},
        horizon_label{}, mirror_limit{}, mirror_limit_label{}, aspect{}, output_width{}, output_height{},
        auto_fullscreen{}, monitor{}, adaptive_horizon{}, mirror_softness{}, mirror_softness_label{};
    HFONT font{};
    Theme theme;
    AdvancedSettings advanced;
    PresetsDialog presets;
    HANDLE layer_editor_process{};
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
    unsigned aspect_numerator{16}, aspect_denominator{9};
    std::wstring monitor_name;
    std::vector<MONITORINFOEXW> monitors;
    unsigned selected_eye{}, rate{60};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    std::wstring directory, ini, render_ini, setup_message;
    std::uint64_t start_ms{}, last_received_ms{}, last_sequence{}, fps_frames{}, fps_start{};
    double measured_fps{};
    ~App() {
        if (layer_editor_process)
            CloseHandle(layer_editor_process);
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
        return expired || !renderer.has_frame() ||
               (!paused && last_received_ms && now >= last_received_ms && now - last_received_ms >= 2000);
    }
    const wchar_t *waiting_text() const {
        return expired       ? L"Build expired. Please install a newer SteadyXR build."
               : window_mode ? L"Waiting for window capture…"
                             : L"Waiting for OpenXR capture…";
    }
    void refresh_expiry() {
        if (!expired && build_expired()) {
            expired = true;
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
        if (enable && compatible_connection) {
            setup_message = L"Compatible OpenXR connection already installed.";
            return;
        }
        if (!diagnostic && machine_layer_installed(directory, true)) {
            setup_message = L"System-wide connection: use the layer editor to change its registration.";
            open_layer_editor();
            return;
        }
        const auto result = set_layer_connection(directory, enable);
        if (result == ERROR_SUCCESS) {
            resolve_connection();
            advanced.reload(render_ini);
        }
        setup_message = result == ERROR_SUCCESS
                            ? (enable ? L"OpenXR connection installed. Restart iRacing to use it."
                                      : L"Connection removed. This takes effect after restarting the game.")
                            : L"Could not change the connection. Windows error " + std::to_wstring(result);
    }
    void queue_save() {
        // Keep disk I/O out of mouse tracking, especially inside synced folders.
        SetTimer(controls, save_timer, 250, nullptr);
    }
    bool save() {
        if (controls)
            KillTimer(controls, save_timer);
        bool saved = write_edge_settings(ini, options);
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
                     reinterpret_cast<LPARAM>(L"Choose the iRacing preview window ..."));
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
        if (channel.lock()) {
            auto &s = *channel.data();
            s.consumer_pid = GetCurrentProcessId();
            s.heartbeat_ms = GetTickCount64();
            s.paused = active() && !window_mode && !demo ? 0 : 1;
            s.eye = selected_eye;
            s.fps = rate;
            channel.unlock();
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
        setup_message = L"Tuning preset applied. Source, eye and output settings retained.";
        update_status();
    }
    void labels() {
        SetWindowTextW(smooth_label, (L"Smoothing: " + decimal(options.smoothing) + L" %").c_str());
        const bool separate = options.edge_reserve[std::min(selected_eye, 1u)].enabled;
        SetWindowTextW(crop_label, separate ? L"Per-edge crop reserve"
                                            : (L"Crop reserve: " + decimal(options.crop) + L" %").c_str());
        EnableWindow(crop, !separate);
        SetWindowTextW(pause, paused ? L"Resume" : L"Pause / GPU comparison");
        EnableWindow(source, window_mode);
        EnableWindow(GetDlgItem(controls, refresh_id), window_mode);
        EnableWindow(eye, TRUE);
        SetWindowTextW(eye_label, window_mode ? L"Profile" : L"Eye");
        EnableWindow(GetDlgItem(controls, setup_id), !window_mode && !expired);
        SetWindowTextW(GetDlgItem(controls, setup_id),
                       registered(true) && !registered() ? L"Upgrade connection" : L"Install connection");
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
                       ? L"Older layer installed: click Upgrade connection, then restart iRacing.\r\n"
                       : L"Install the connection, then restart iRacing.\r\n";
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
                    text += L"Waiting for an OpenXR game using D3D11.\r\n";
                channel.unlock();
            }
        }
        if (!setup_message.empty())
            text += setup_message + L"\r\n";
        if (!active())
            text += L"PAUSED – no new frame copies requested.\r\n";
        else if (last_received_ms && GetTickCount64() - last_received_ms > 2000)
            text += L"No new frame for more than 2 seconds.\r\n";
        if (renderer.has_frame())
            text += L"Received: " + std::to_wstring(int(measured_fps + .5)) + L" FPS | " +
                    (window_mode && !demo    ? L"Image analysis"
                     : renderer.views() == 2 ? L"Context + foveal inset"
                                             : L"Stereo eye") +
                    L"\r\n";
        if (!renderer.error().empty())
            text += renderer.error() + L"\r\n";
        text += L"Render: " + std::to_wstring(options.output_width) + L" x " +
                std::to_wstring(options.output_height) + L" | Aspect: " + std::to_wstring(aspect_numerator) +
                L":" + std::to_wstring(aspect_denominator) +
                L"\r\nF11: fullscreen. No OBS or encoder required.";
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
    const bool heading = !std::wcscmp(kind, L"STATIC") && y == 18;
    style = app->theme.style(kind, style);
    auto result = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, x, y + 80, w, h, parent,
                                  reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    app->theme.control(result, kind, heading);
    return result;
}
LRESULT CALLBACK output_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CLOSE:
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
            app->renderer.draw(app->options, true, app->waiting_text());
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
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && app->status && app->output)
            app->refresh_editor_connection();
        break;
    case WM_TIMER:
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
        switch (LOWORD(w)) {
        case advanced_id:
            app->advanced.open(app->selected_eye);
            return 0;
        case presets_id:
            app->presets.open();
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
            ShowWindow(app->output, SW_RESTORE);
            app->paused = false;
            SetForegroundWindow(app->output);
            break;
        case pause_id:
            app->paused = !app->paused;
            if (!app->paused)
                app->renderer.reset();
            break;
        case reset_id:
            app->renderer.reset();
            break;
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
        app->quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int show) {
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
    const auto control_title = std::wstring(L"SteadyXR – Preview 0.7.0 | Expires ") + build_expiry_text();
    state.controls =
        CreateWindowExW(0, L"HorizonLockControls", control_title.c_str(),
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, 100, 100,
                        1050, 855, nullptr, nullptr, instance, nullptr);
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
    child(state.controls, L"STATIC", L"CAPTURE / STABILIZATION", 0, 22, 18, 610, 28);
    state.mode = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 22,
                       54, 595, 180, mode_id);
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Capture existing window (no restart)"));
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"OpenXR pose capture (optional connection)"));
    SendMessageW(state.mode, CB_SETCURSEL, state.window_mode ? 0 : 1, 0);
    state.source = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 22,
                         96, 450, 300, source_id);
    child(state.controls, L"BUTTON", L"Refresh", BS_PUSHBUTTON, 485, 95, 132, 29, refresh_id);
    child(state.controls, L"BUTTON", L"Install connection", BS_PUSHBUTTON, 22, 145, 210, 34, setup_id);
    child(state.controls, L"BUTTON", L"Remove connection", BS_PUSHBUTTON, 245, 145, 205, 34, remove_id);
    child(state.controls, L"BUTTON", L"Render / edges…", BS_PUSHBUTTON, 463, 145, 154, 34, advanced_id);
    child(state.controls, L"BUTTON", L"Open preview", BS_PUSHBUTTON, 22, 195, 190, 36, show_id);
    state.pause = child(state.controls, L"BUTTON", L"Pause / GPU comparison", BS_PUSHBUTTON, 225, 195, 215,
                        36, pause_id);
    child(state.controls, L"BUTTON", L"Reset preview", BS_PUSHBUTTON, 453, 195, 163, 36, reset_id);
    state.smooth_label = child(state.controls, L"STATIC", L"", 0, 22, 250, 210, 24);
    state.smooth =
        child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 230, 240, 385, 40, smooth_id);
    SendMessageW(state.smooth, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(state.smooth, TBM_SETPOS, TRUE, LPARAM(state.options.smoothing * 10));
    SendMessageW(state.smooth, TBM_SETTICFREQ, 100, 0);
    SendMessageW(state.smooth, TBM_SETPAGESIZE, 0, 10);
    state.crop_label = child(state.controls, L"STATIC", L"", 0, 22, 297, 210, 24);
    state.crop =
        child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 230, 287, 385, 40, crop_id);
    SendMessageW(state.crop, TBM_SETRANGE, TRUE, MAKELPARAM(0, 350));
    SendMessageW(state.crop, TBM_SETPOS, TRUE, LPARAM(state.options.crop * 10));
    SendMessageW(state.crop, TBM_SETTICFREQ, 50, 0);
    SendMessageW(state.crop, TBM_SETPAGESIZE, 0, 10);
    state.mirror = child(state.controls, L"BUTTON", L"Mirror edges beyond the crop reserve", BS_AUTOCHECKBOX,
                         22, 339, 592, 28, mirror_id);
    SendMessageW(state.mirror, BM_SETCHECK, state.options.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    state.bypass = child(state.controls, L"BUTTON", L"Disable stabilization (capture stays active)",
                         BS_AUTOCHECKBOX, 22, 375, 592, 28, bypass_id);
    state.eye_label = child(state.controls, L"STATIC", L"Eye", 0, 22, 425, 55, 25);
    state.eye =
        child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 85, 420, 160, 160, eye_id);
    SendMessageW(state.eye, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Left"));
    SendMessageW(state.eye, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Right"));
    SendMessageW(state.eye, CB_SETCURSEL, state.selected_eye, 0);
    child(state.controls, L"STATIC", L"FPS", 0, 280, 425, 75, 25);
    state.fps =
        child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 365, 420, 150, 160, fps_id);
    for (const wchar_t *value : {L"30 FPS", L"60 FPS", L"90 FPS"})
        SendMessageW(state.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(state.fps, CB_SETCURSEL, state.rate == 30 ? 0 : state.rate == 60 ? 1 : 2, 0);
    state.status = child(state.controls, L"STATIC", L"", 0, 22, 478, 596, 190);
    child(state.controls, L"BUTTON", L"Stream Comfort", BS_PUSHBUTTON, 22, 668, 145, 34, comfort_id);
    child(state.controls, L"BUTTON", L"Undo tuning", BS_PUSHBUTTON, 177, 668, 129, 34, undo_comfort_id);
    child(state.controls, L"BUTTON", L"Presets…", BS_PUSHBUTTON, 316, 668, 115, 34, presets_id);
    child(state.controls, L"BUTTON", L"OpenXR layers…", BS_PUSHBUTTON, 441, 668, 175, 34, layers_id);
    child(state.controls, L"STATIC", L"FRAMING / OUTPUT", 0, 655, 18, 360, 28);
    const double trim_values[] = {state.options.crop_left, state.options.crop_right, state.options.crop_top,
                                  state.options.crop_bottom};
    auto slider = [&](int id, int y, double value, int maximum, HWND &label) {
        label = child(state.controls, L"STATIC", L"", 0, 655, y, 360, 24);
        auto handle = child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ, 650, y + 25, 360, 32, id);
        SendMessageW(handle, TBM_SETRANGE, TRUE, MAKELPARAM(0, maximum));
        SendMessageW(handle, TBM_SETPOS, TRUE, LPARAM(value * 10 + .5));
        SendMessageW(handle, TBM_SETLINESIZE, 0, 1);
        SendMessageW(handle, TBM_SETPAGESIZE, 0, 10);
        return handle;
    };
    for (unsigned i = 0; i < 4; ++i)
        state.trims[i] = slider(left_id + i, 55 + i * 65, trim_values[i], 450, state.trim_labels[i]);
    state.horizon = slider(horizon_id, 320, state.options.horizon_lock, 1000, state.horizon_label);
    MoveWindow(state.horizon_label, 655, 400, 190, 24, FALSE);
    state.adaptive_horizon = child(state.controls, L"BUTTON", L"Adaptive curve", BS_AUTOCHECKBOX, 850, 318,
                                   157, 26, adaptive_horizon_id);
    SendMessageW(state.adaptive_horizon, BM_SETCHECK,
                 state.options.adaptive_horizon ? BST_CHECKED : BST_UNCHECKED, 0);
    state.mirror_limit =
        slider(mirror_limit_id, 385, state.options.mirror_limit, 500, state.mirror_limit_label);
    state.mirror_softness_label = child(state.controls, L"STATIC", L"", 0, 655, 448, 185, 24);
    state.mirror_softness =
        child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ, 845, 443, 162, 30, mirror_softness_id);
    SendMessageW(state.mirror_softness, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(state.mirror_softness, TBM_SETPOS, TRUE, LPARAM(state.options.mirror_softness * 10 + .5));
    SendMessageW(state.mirror_softness, TBM_SETLINESIZE, 0, 1);
    SendMessageW(state.mirror_softness, TBM_SETPAGESIZE, 0, 10);
    child(state.controls, L"STATIC", L"Aspect ratio", 0, 655, 479, 120, 24);
    state.aspect = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_HASSTRINGS | WS_VSCROLL, 790,
                         475, 216, 200, aspect_id);
    for (auto text : {L"16:9", L"21:9", L"32:9", L"4:3", L"1:1"})
        SendMessageW(state.aspect, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SetWindowTextW(
        state.aspect,
        (std::to_wstring(state.aspect_numerator) + L":" + std::to_wstring(state.aspect_denominator)).c_str());
    child(state.controls, L"STATIC", L"Render resolution", 0, 655, 518, 170, 24);
    state.output_width = child(state.controls, L"EDIT", std::to_wstring(state.options.output_width).c_str(),
                               WS_BORDER | ES_NUMBER, 820, 515, 80, 27, width_id);
    child(state.controls, L"STATIC", L"x", 0, 909, 518, 15, 24);
    state.output_height = child(state.controls, L"EDIT", std::to_wstring(state.options.output_height).c_str(),
                                WS_BORDER | ES_NUMBER, 932, 515, 75, 27, height_id);
    child(state.controls, L"BUTTON", L"Apply aspect / resolution", BS_PUSHBUTTON, 655, 552, 350, 32,
          apply_id);
    child(state.controls, L"STATIC", L"Output monitor", 0, 655, 601, 350, 24);
    state.monitor = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                          655, 628, 350, 150, monitor_id);
    state.auto_fullscreen = child(state.controls, L"BUTTON", L"Start preview in fullscreen", BS_AUTOCHECKBOX,
                                  655, 675, 350, 28, auto_fullscreen_id);
    SendMessageW(state.auto_fullscreen, BM_SETCHECK, state.start_fullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
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
    const auto presets_directory = self_test
                                       ? std::filesystem::path(state.ini + L"-presets")
                                       : std::filesystem::path(state.directory).parent_path() / L"Presets";
    state.presets.initialize(state.controls, state.theme, presets_directory,
                             {[&] { return state.snapshot(); },
                              [&](const std::string &bytes) { return state.apply_snapshot(bytes); },
                              [&](BuiltinPreset preset) { state.builtin(preset); }});
    state.set_tuning(state.options);
    state.list_monitors();
    if (!self_test)
        state.move_to_monitor();
    state.refresh_sources();
    state.labels();
    state.renderer.attach(state.output);
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
                    if (!state.presets.route(msg) && !state.advanced.route(msg) &&
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
        const auto before_preset = state.options;
        SendMessageW(state.controls, WM_COMMAND, comfort_id, 0);
        SendMessageW(state.controls, WM_COMMAND, comfort_id, 0); // Reapplying retains the original undo.
        okay = okay && state.can_undo_comfort && state.options.smoothing == 60 &&
               state.options.horizon_lock == 50 && state.options.mirror_softness == 70 &&
               SendMessageW(state.mirror_softness, TBM_GETPOS, 0, 0) == 700 &&
               state.options.output_width == before_preset.output_width &&
               state.options.crop_left == before_preset.crop_left;
        state.options.crop_left = 2; // A later framing adjustment is not undone by the tuning preset.
        SendMessageW(state.trims[0], TBM_SETPOS, TRUE, 20);
        SendMessageW(state.controls, WM_COMMAND, undo_comfort_id, 0);
        okay = okay && !state.can_undo_comfort && state.options.crop_left == 2 &&
               state.options.smoothing == before_preset.smoothing &&
               state.options.mirror_softness == before_preset.mirror_softness &&
               state.options.horizon_lock == before_preset.horizon_lock &&
               state.options.adaptive_horizon == before_preset.adaptive_horizon;
        state.advanced.open(1, false);
        const auto panel = state.advanced.handle();
        bool advanced_ok = panel && IsWindow(panel);
        // Per-eye margins are live and persist independently from render requests.
        if (panel) {
            SendMessageW(GetDlgItem(panel, AdvancedSettings::separate_id), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(panel, WM_COMMAND, AdvancedSettings::separate_id, 0);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::left_id), TBM_SETPOS, TRUE, 20);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::right_id), TBM_SETPOS, TRUE, 200);
            SendMessageW(GetDlgItem(panel, AdvancedSettings::vertical_id), TBM_SETPOS, TRUE, 675);
            SendMessageW(panel, WM_HSCROLL, 0,
                         reinterpret_cast<LPARAM>(GetDlgItem(panel, AdvancedSettings::left_id)));
            advanced_ok = state.options.edge_reserve[1].enabled && state.options.edge_reserve[1].left == 2 &&
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
                          reloaded.options.vertical_offset == 67.5;
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
        state.options.crop_left = 42;
        state.options.edge_reserve[0] = {};
        state.options.edge_reserve[1] = {};
        state.options.vertical_offset = 99;
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
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok =
            preset_ok && !state.window_mode && state.selected_eye == 1 && state.options.eye == 1 &&
            state.rate == 90 && !state.options.stabilize && !state.start_fullscreen &&
            state.options.output_width == 1920 && state.options.output_height == 1080 &&
            state.aspect_numerator == 21 && state.aspect_denominator == 9 && state.options.crop_left == 2 &&
            state.options.crop_right == 4.5 && state.options.crop_top == 6.7 &&
            state.options.crop_bottom == 8.9 && state.options.vertical_offset == -43.2 &&
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
        wchar_t retained[64]{};
        GetPrivateProfileStringW(L"FutureSettings", L"RetainMe", L"", retained, 64, state.ini.c_str());
        preset_ok = preset_ok && std::wstring(retained) == L"complete snapshot";
        const auto monitor_count = SendMessageW(state.monitor, CB_GETCOUNT, 0, 0);
        SendMessageW(preset_panel, WM_COMMAND, PresetsDialog::load_id, 0);
        preset_ok = preset_ok && SendMessageW(state.monitor, CB_GETCOUNT, 0, 0) == monitor_count;
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
            if (!state.presets.route(msg) && !state.advanced.route(msg) &&
                !IsDialogMessageW(state.controls, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (state.quit)
            break;
        const auto now = GetTickCount64();
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
                                                : state.renderer.consume(state.channel);
            if (received) {
                state.last_received_ms = now;
                ++state.fps_frames;
            }
            if ((received || state.dirty) && !state.waiting(now))
                state.renderer.draw(state.options);
            state.dirty = false;
            if (!submitted_feed)
                next_frame = std::max(next_frame + 1000. / state.rate, double(now));
        } else if (state.dirty && !state.waiting(now) && IsWindowVisible(state.output) &&
                   !IsIconic(state.output)) {
            state.renderer.draw(state.options);
            state.dirty = false;
        }
        const bool waiting = state.waiting(now);
        // Retry at a low cadence while idle: nonblocking Present may encounter
        // a full swapchain queue on the first transition away from live video.
        if (waiting && IsWindowVisible(state.output) && !IsIconic(state.output) &&
            (!was_waiting || now - last_waiting_paint >= 250)) {
            state.renderer.draw(state.options, true, state.waiting_text());
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
    if (state.channel.lock()) {
        auto &s = *state.channel.data();
        s.paused = 1;
        s.heartbeat_ms = 0;
        s.consumer_pid = 0;
        state.channel.unlock();
    }
    if (state.timer_resolution)
        timeEndPeriod(1);
    state.capture.stop();
    state.save();
    DestroyWindow(state.output);
    CloseHandle(singleton);
    app = nullptr;
    return 0;
}
