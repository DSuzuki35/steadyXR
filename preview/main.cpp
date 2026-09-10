#define UNICODE
#define _UNICODE
#include "renderer.hpp"
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
              apply_id = 124, auto_fullscreen_id = 125, monitor_id = 126;
constexpr wchar_t registry_key[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
std::wstring decimal(double value) {
    wchar_t text[32]{};
    std::swprintf(text, 32, L"%.1f", value);
    return text;
}
struct App {
    HWND controls{}, output{}, status{}, smooth_label{}, crop_label{}, smooth{}, crop{}, mirror{}, bypass{},
        eye{}, fps{}, pause{}, mode{}, source{}, trims[4]{}, trim_labels[4]{}, horizon{}, horizon_label{},
        mirror_limit{}, mirror_limit_label{}, aspect{}, output_width{}, output_height{}, auto_fullscreen{},
        monitor{};
    HFONT font{};
    Theme theme;
    Channel channel;
    Renderer renderer;
    WindowCapture capture;
    std::vector<CaptureWindow> sources;
    HWND source_window{};
    DWORD source_pid{};
    bool window_mode{true};
    std::uint64_t last_capture_start{};
    Options options;
    bool paused{}, demo{}, fullscreen{}, dirty{true}, quit{}, timer_resolution{};
    bool start_fullscreen{true};
    unsigned aspect_numerator{16}, aspect_denominator{9};
    std::wstring monitor_name;
    std::vector<MONITORINFOEXW> monitors;
    unsigned selected_eye{}, rate{60};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    std::wstring directory, ini, setup_message;
    std::uint64_t start_ms{}, last_received_ms{}, last_sequence{}, fps_frames{}, fps_start{};
    double measured_fps{};
    bool waiting(std::uint64_t now) const {
        return !renderer.has_frame() ||
               (!paused && last_received_ms && now >= last_received_ms && now - last_received_ms >= 2000);
    }
    bool registered(bool any_version = false) const {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, registry_key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        DWORD value = 1, size = sizeof(value), type = 0;
        const auto path = directory + L"\\XR_APILAYER_HORIZONLOCK_preview.json";
        auto result =
            RegQueryValueExW(key, path.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
        bool found = result == ERROR_SUCCESS && type == REG_DWORD && value == 0;
        if (any_version && !found) {
            for (DWORD index = 0;; ++index) {
                wchar_t name[32768]{};
                DWORD length = 32768;
                size = sizeof(value);
                type = 0;
                value = 1;
                result = RegEnumValueW(key, index, name, &length, nullptr, &type,
                                       reinterpret_cast<BYTE *>(&value), &size);
                if (result == ERROR_NO_MORE_ITEMS)
                    break;
                if (result != ERROR_SUCCESS)
                    continue;
                if (type == REG_DWORD && value == 0 &&
                    std::filesystem::path(name).filename() == L"XR_APILAYER_HORIZONLOCK_preview.json" &&
                    GetFileAttributesW(name) != INVALID_FILE_ATTRIBUTES) {
                    found = true;
                    break;
                }
            }
        }
        RegCloseKey(key);
        return found;
    }
    void connection(bool enable) {
        if (enable && registered(true) && !registered()) {
            setup_message = L"An existing Horizon Lock layer is installed and works with this preview. No "
                            L"new connection is needed.";
            return;
        }
        const auto path = directory + L"\\XR_APILAYER_HORIZONLOCK_preview.json";
        HKEY key{};
        LONG result;
        if (enable && (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
                       GetFileAttributesW((directory + L"\\horizonlock-openxr-layer.dll").c_str()) ==
                           INVALID_FILE_ATTRIBUTES)) {
            setup_message = L"App files are missing. Please use the complete preview package.";
            return;
        }
        if (enable) {
            result = RegCreateKeyExW(HKEY_CURRENT_USER, registry_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                     &key, nullptr);
            DWORD zero = 0;
            if (result == ERROR_SUCCESS)
                result = RegSetValueExW(key, path.c_str(), 0, REG_DWORD,
                                        reinterpret_cast<const BYTE *>(&zero), sizeof(zero));
        } else {
            result = RegOpenKeyExW(HKEY_CURRENT_USER, registry_key, 0, KEY_SET_VALUE, &key);
            if (result == ERROR_SUCCESS)
                result = RegDeleteValueW(key, path.c_str());
            if (result == ERROR_FILE_NOT_FOUND)
                result = ERROR_SUCCESS;
        }
        if (key)
            RegCloseKey(key);
        setup_message = result == ERROR_SUCCESS
                            ? (enable ? L"Connection installed. Restart iRacing once to activate it."
                                      : L"Connection removed. This takes effect after restarting the game.")
                            : L"Could not change the connection. Windows error " + std::to_wstring(result);
    }
    void save() {
        auto write = [&](const wchar_t *key, int value) {
            WritePrivateProfileStringW(L"Preview", key, std::to_wstring(value).c_str(), ini.c_str());
        };
        write(L"Smoothing", int(options.smoothing));
        write(L"Crop", int(options.crop));
        write(L"Mirror", options.mirror);
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
        write(L"MirrorLimitTenths", int(options.mirror_limit * 10 + .5));
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
        WritePrivateProfileStringW(L"Preview", L"Monitor", monitor_name.c_str(), ini.c_str());
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
        options.mirror_limit = value(L"MirrorLimitTenths", 100, 0, 500) / 10.;
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
        options.mirror = value(L"Mirror", 1, 0, 1) != 0;
        selected_eye = value(L"Eye", 0, 0, 1);
        rate = value(L"FPS", 60, 30, 90);
        window_mode = value(L"Mode", 0, 0, 1) == 0;
        if (rate != 30 && rate != 60 && rate != 90)
            rate = 60;
    }
    bool active() const {
        return !paused && IsWindowVisible(output) && !IsIconic(output);
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
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            wchar_t name[32768]{};
            DWORD size = 32768;
            if (process && QueryFullProcessImageNameW(process, 0, name, &size)) {
                auto filename = std::filesystem::path(name).filename().wstring();
                std::transform(filename.begin(), filename.end(), filename.begin(), ::towlower);
                if (filename.find(L"iracingsim") == 0)
                    selected = int(i + 1);
            }
            if (process)
                CloseHandle(process);
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
    void labels() {
        SetWindowTextW(smooth_label, (L"Smoothing: " + decimal(options.smoothing) + L" %").c_str());
        SetWindowTextW(crop_label, (L"Crop reserve: " + decimal(options.crop) + L" %").c_str());
        SetWindowTextW(pause, paused ? L"Resume" : L"Pause / GPU comparison");
        EnableWindow(source, window_mode);
        EnableWindow(GetDlgItem(controls, refresh_id), window_mode);
        EnableWindow(eye, !window_mode);
        EnableWindow(GetDlgItem(controls, setup_id), !window_mode);
        const double values[] = {options.crop_left, options.crop_right, options.crop_top,
                                 options.crop_bottom};
        const wchar_t *names[] = {L"Crop left", L"Crop right", L"Crop top", L"Crop bottom"};
        for (unsigned i = 0; i < 4; ++i)
            SetWindowTextW(trim_labels[i],
                           (std::wstring(names[i]) + L": " + decimal(values[i]) + L" %").c_str());
        SetWindowTextW(horizon_label,
                       (L"Horizon lock (OpenXR): " + decimal(options.horizon_lock) + L" %").c_str());
        SetWindowTextW(mirror_limit_label,
                       (L"Mirror limit: " + decimal(options.mirror_limit) + L" %").c_str());
        EnableWindow(horizon, !window_mode);
        EnableWindow(mirror_limit, options.mirror);
    }
    void update_status() {
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
            text = registered(true) ? L"OpenXR connection installed.\r\n"
                                    : L"For a new connection: install the layer, then restart iRacing.\r\n";
            if (channel.lock()) {
                const auto &s = *channel.data();
                const auto now = GetTickCount64();
                if (s.producer_pid && now >= s.producer_heartbeat_ms &&
                    now - s.producer_heartbeat_ms < 2500) {
                    text = L"OpenXR feed connected.\r\n";
                    text += std::wstring(s.application, wcsnlen(s.application, 128)) + L"\r\n";
                    text += std::wstring(s.status, wcsnlen(s.status, 192)) + L"\r\n";
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
                L"\r\nF11: fullscreen. Headset image unchanged. No OBS or encoder required.";
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
            app->renderer.draw(app->options, true,
                               app->window_mode ? L"Waiting for window capture…"
                                                : L"Waiting for OpenXR capture…");
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
    case WM_HSCROLL:
        app->options.smoothing = SendMessageW(app->smooth, TBM_GETPOS, 0, 0) / 10.;
        app->options.crop = SendMessageW(app->crop, TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_left = SendMessageW(app->trims[0], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_right = SendMessageW(app->trims[1], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_top = SendMessageW(app->trims[2], TBM_GETPOS, 0, 0) / 10.;
        app->options.crop_bottom = SendMessageW(app->trims[3], TBM_GETPOS, 0, 0) / 10.;
        app->options.horizon_lock = SendMessageW(app->horizon, TBM_GETPOS, 0, 0) / 10.;
        app->options.mirror_limit = SendMessageW(app->mirror_limit, TBM_GETPOS, 0, 0) / 10.;
        app->labels();
        app->save();
        app->dirty = true;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
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
        case eye_id:
            if (HIWORD(w) == CBN_SELCHANGE) {
                app->selected_eye = unsigned(SendMessageW(app->eye, CB_GETCURSEL, 0, 0));
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
    state.demo = std::wstring(command).find(L"--demo") != std::wstring::npos;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    state.directory = std::filesystem::path(path).parent_path().wstring();
    state.ini = state.directory + L"\\preview.ini";
    if (self_test)
        state.ini =
            state.directory + L"\\preview-selftest-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    state.read();
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
    state.controls = CreateWindowExW(0, L"HorizonLockControls", L"Horizon Lock VR – Preview 0.5.0",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 1050,
                                     855, nullptr, nullptr, instance, nullptr);
    state.output =
        CreateWindowExW(0, L"HorizonLockOutput", L"Horizon Lock VR – Clean Preview", WS_OVERLAPPEDWINDOW, 800,
                        120, 976, 579, nullptr, nullptr, instance, nullptr);
    state.theme.titlebar(state.controls);
    state.theme.titlebar(state.output);
    const auto channel_name =
        self_test ? L"Local\\HorizonLockPreview-UI-test-" + std::to_wstring(GetCurrentProcessId())
                  : std::wstring(default_channel);
    if (!state.controls || !state.output || !state.channel.open(true, channel_name)) {
        MessageBoxW(nullptr, L"Could not start the preview.", L"Horizon Lock VR", MB_ICONERROR);
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
    child(state.controls, L"STATIC", L"Eye", 0, 22, 425, 55, 25);
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
    state.status = child(state.controls, L"STATIC", L"", 0, 22, 478, 596, 245);
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
    state.mirror_limit =
        slider(mirror_limit_id, 385, state.options.mirror_limit, 500, state.mirror_limit_label);
    child(state.controls, L"STATIC", L"Beyond mirror limit: black. Arrow keys: 0.1%.", 0, 655, 444, 370, 24);
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
                    if (!IsDialogMessageW(state.controls, &msg)) {
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
            SendDlgItemMessageW(state.controls, 125, BM_GETCHECK, 0, 0) == BST_CHECKED;
        state.source_window = state.output;
        state.source_pid = GetCurrentProcessId() + 1;
        const bool rejected_stale =
            !state.capture_frame(GetTickCount64()) && !state.source_window && !state.capture.running();
        SendMessageW(state.smooth, TBM_SETPOS, TRUE, 650);
        SendMessageW(state.controls, WM_HSCROLL, 0, reinterpret_cast<LPARAM>(state.smooth));
        SendMessageW(state.controls, WM_COMMAND, pause_id, 0);
        bool okay = display_controls && rejected_stale && state.paused && state.options.smoothing == 65 &&
                    state.renderer.demo(0) && state.renderer.draw(state.options, false);
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
                   restored.options.output_width == 1920 && restored.options.output_height == 1080 &&
                   restored.aspect_numerator == 21 && restored.aspect_denominator == 9 &&
                   !restored.start_fullscreen;
        }
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
            if (!IsDialogMessageW(state.controls, &msg)) {
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
            state.renderer.draw(state.options, true,
                                state.window_mode ? L"Waiting for window capture…"
                                                  : L"Waiting for OpenXR capture…");
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
