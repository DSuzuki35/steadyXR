#define UNICODE
#define _UNICODE
#include "renderer.hpp"
#include "window_capture.hpp"
#include <algorithm>
#include <commctrl.h>
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
              source_id = 113, refresh_id = 114;
constexpr wchar_t registry_key[] = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
struct App {
    HWND controls{}, output{}, status{}, smooth_label{}, crop_label{}, smooth{}, crop{}, mirror{}, bypass{},
        eye{}, fps{}, pause{}, mode{}, source{};
    HFONT font{};
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
    unsigned selected_eye{}, rate{60};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    std::wstring directory, ini, setup_message;
    std::uint64_t start_ms{}, last_received_ms{}, last_sequence{}, fps_frames{}, fps_start{};
    double measured_fps{};
    bool registered() const {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, registry_key, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        DWORD value = 1, size = sizeof(value), type = 0;
        const auto path = directory + L"\\XR_APILAYER_HORIZONLOCK_preview.json";
        auto result =
            RegQueryValueExW(key, path.c_str(), nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS && type == REG_DWORD && value == 0;
    }
    void connection(bool enable) {
        const auto path = directory + L"\\XR_APILAYER_HORIZONLOCK_preview.json";
        HKEY key{};
        LONG result;
        if (enable && (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
                       GetFileAttributesW((directory + L"\\horizonlock-openxr-layer.dll").c_str()) ==
                           INVALID_FILE_ATTRIBUTES)) {
            setup_message = L"App-Dateien fehlen. Bitte das komplette Preview-Paket verwenden.";
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
        setup_message =
            result == ERROR_SUCCESS
                ? (enable ? L"Verbindung eingerichtet. iRacing jetzt einmal neu starten."
                          : L"Verbindung entfernt. Die Aenderung gilt nach dem Spiel-Neustart.")
                : L"Verbindung konnte nicht geaendert werden. Windows-Fehler " + std::to_wstring(result);
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
    }
    void read() {
        auto value = [&](const wchar_t *key, int initial, int lo, int hi) {
            return std::clamp(int(GetPrivateProfileIntW(L"Preview", key, initial, ini.c_str())), lo, hi);
        };
        options.smoothing = value(L"Smoothing", 40, 0, 100);
        options.crop = value(L"Crop", 10, 0, 35);
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
                     reinterpret_cast<LPARAM>(L"iRacing-Vorschaufenster waehlen ..."));
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
        if (!fullscreen) {
            GetWindowPlacement(output, &placement);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(output, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowLongPtrW(output, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(output, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
        } else {
            SetWindowLongPtrW(output, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPlacement(output, &placement);
            SetWindowPos(output, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        fullscreen = !fullscreen;
        dirty = true;
    }
    void labels() {
        SetWindowTextW(smooth_label,
                       (L"Glaettung: " + std::to_wstring(int(options.smoothing)) + L" %").c_str());
        SetWindowTextW(crop_label, (L"Crop-Reserve: " + std::to_wstring(int(options.crop)) + L" %").c_str());
        SetWindowTextW(pause, paused ? L"Fortsetzen" : L"Pause / GPU-Vergleich");
        EnableWindow(source, window_mode);
        EnableWindow(GetDlgItem(controls, refresh_id), window_mode);
        EnableWindow(eye, !window_mode);
        EnableWindow(GetDlgItem(controls, setup_id), !window_mode);
    }
    void update_status() {
        std::wstring text;
        if (demo)
            text = L"DEMO – synthetisches Testbild, kein iRacing-Bild.\r\n";
        else if (window_mode) {
            text = L"Fensteraufnahme + Bildanalyse. Kein Spiel-Neustart noetig.\r\n";
            if (source_window && IsWindow(source_window)) {
                wchar_t title[512]{};
                GetWindowTextW(source_window, title, 512);
                text += std::wstring(title) + L"\r\n";
                if (IsIconic(source_window))
                    text += L"Das iRacing-Vorschaufenster bitte nicht minimieren.\r\n";
            } else
                text += L"Vorschaufenster oben waehlen; bei Bedarf Liste aktualisieren.\r\n";
            if (!capture.error().empty())
                text += capture.error() + L"\r\n";
        } else {
            text = registered() ? L"OpenXR-Verbindung eingerichtet.\r\n"
                                : L"1. Verbindung einrichten.  2. iRacing neu starten.\r\n";
            if (channel.lock()) {
                const auto &s = *channel.data();
                const auto now = GetTickCount64();
                if (s.producer_pid && now >= s.producer_heartbeat_ms &&
                    now - s.producer_heartbeat_ms < 2500) {
                    text += std::wstring(s.application, wcsnlen(s.application, 128)) + L"\r\n";
                    text += std::wstring(s.status, wcsnlen(s.status, 192)) + L"\r\n";
                } else
                    text += L"Warte auf ein OpenXR-Spiel mit D3D11.\r\n";
                channel.unlock();
            }
        }
        if (!setup_message.empty() && !window_mode)
            text += setup_message + L"\r\n";
        if (!active())
            text += L"PAUSE – keine neuen Bildkopien angefordert.\r\n";
        else if (last_received_ms && GetTickCount64() - last_received_ms > 2000)
            text += L"Kein neues Bild seit mehr als 2 Sekunden.\r\n";
        if (renderer.has_frame())
            text += L"Empfangen: " + std::to_wstring(int(measured_fps + .5)) + L" Bilder/s | " +
                    (window_mode && !demo    ? L"Bildanalyse"
                     : renderer.views() == 2 ? L"Context + fovealer Ausschnitt"
                                             : L"Stereo-Auge") +
                    L"\r\n";
        if (!renderer.error().empty())
            text += renderer.error() + L"\r\n";
        text += L"Ausgabe: 16:9, bis 1920 x 1080. F11 = Vollbild.\r\nBrillenbild bleibt unveraendert. Kein "
                L"OBS oder Encoder erforderlich.";
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
    auto result = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                                  reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
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
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(window, &ps);
        if (!app->renderer.has_frame()) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetTextColor(dc, RGB(225, 231, 240));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, app->font);
            rect.top = std::max<LONG>(20, (rect.bottom - 80) / 2);
            DrawTextW(
                dc,
                app->window_mode
                    ? L"Horizon Lock VR\nWarte auf das Vorschaufenster ...\nFenster im Bedienfeld waehlen."
                    : L"Horizon Lock VR\nWarte auf OpenXR ...\nVerbindung einrichten und iRacing neu "
                      L"starten.",
                -1, &rect, DT_CENTER | DT_WORDBREAK);
        }
        EndPaint(window, &ps);
        app->dirty = true;
        return 0;
    }
    }
    return DefWindowProcW(window, message, w, l);
}
LRESULT CALLBACK controls_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (!app)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_HSCROLL:
        app->options.smoothing = SendMessageW(app->smooth, TBM_GETPOS, 0, 0);
        app->options.crop = SendMessageW(app->crop, TBM_GETPOS, 0, 0);
        app->labels();
        app->save();
        app->dirty = true;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
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
    state.font =
        CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"HorizonLockControls";
    wc.lpfnWndProc = controls_proc;
    RegisterClassW(&wc);
    wc.lpszClassName = L"HorizonLockOutput";
    wc.lpfnWndProc = output_proc;
    wc.style = CS_DBLCLKS;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    state.controls = CreateWindowExW(0, L"HorizonLockControls", L"Horizon Lock VR – Vorschau 0.4.0",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 660,
                                     775, nullptr, nullptr, instance, nullptr);
    state.output =
        CreateWindowExW(0, L"HorizonLockOutput", L"Horizon Lock VR – Clean Preview", WS_OVERLAPPEDWINDOW, 800,
                        120, 976, 579, nullptr, nullptr, instance, nullptr);
    const auto channel_name =
        self_test ? L"Local\\HorizonLockPreview-UI-test-" + std::to_wstring(GetCurrentProcessId())
                  : std::wstring(default_channel);
    if (!state.controls || !state.output || !state.channel.open(true, channel_name)) {
        MessageBoxW(nullptr, L"Vorschau konnte nicht gestartet werden.", L"Horizon Lock VR", MB_ICONERROR);
        CloseHandle(singleton);
        return 1;
    }
    child(state.controls, L"STATIC", L"Stabilisiertes Vorschaufenster", 0, 22, 18, 610, 28);
    state.mode = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 22,
                       54, 595, 180, mode_id);
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Vorschaufenster aufnehmen (ohne Neustart)"));
    SendMessageW(state.mode, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"OpenXR-Pose direkt (optionale Verbindung)"));
    SendMessageW(state.mode, CB_SETCURSEL, state.window_mode ? 0 : 1, 0);
    state.source = child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL, 22,
                         96, 450, 300, source_id);
    child(state.controls, L"BUTTON", L"Aktualisieren", BS_PUSHBUTTON, 485, 95, 132, 29, refresh_id);
    child(state.controls, L"BUTTON", L"Verbindung einrichten", BS_PUSHBUTTON, 22, 145, 210, 34, setup_id);
    child(state.controls, L"BUTTON", L"Verbindung entfernen", BS_PUSHBUTTON, 245, 145, 205, 34, remove_id);
    child(state.controls, L"BUTTON", L"Vorschau oeffnen", BS_PUSHBUTTON, 22, 195, 190, 36, show_id);
    state.pause = child(state.controls, L"BUTTON", L"Pause / GPU-Vergleich", BS_PUSHBUTTON, 225, 195, 215, 36,
                        pause_id);
    child(state.controls, L"BUTTON", L"Reset Vorschau", BS_PUSHBUTTON, 453, 195, 163, 36, reset_id);
    state.smooth_label = child(state.controls, L"STATIC", L"", 0, 22, 250, 210, 24);
    state.smooth =
        child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 230, 240, 385, 40, smooth_id);
    SendMessageW(state.smooth, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(state.smooth, TBM_SETPOS, TRUE, LPARAM(state.options.smoothing));
    state.crop_label = child(state.controls, L"STATIC", L"", 0, 22, 297, 210, 24);
    state.crop =
        child(state.controls, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 230, 287, 385, 40, crop_id);
    SendMessageW(state.crop, TBM_SETRANGE, TRUE, MAKELPARAM(0, 35));
    SendMessageW(state.crop, TBM_SETPOS, TRUE, LPARAM(state.options.crop));
    state.mirror = child(state.controls, L"BUTTON", L"Raender spiegeln, wenn die Crop-Reserve nicht reicht",
                         BS_AUTOCHECKBOX, 22, 339, 592, 28, mirror_id);
    SendMessageW(state.mirror, BM_SETCHECK, state.options.mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    state.bypass =
        child(state.controls, L"BUTTON", L"Stabilisierung aus (Bildvergleich, Capture bleibt aktiv)",
              BS_AUTOCHECKBOX, 22, 375, 592, 28, bypass_id);
    child(state.controls, L"STATIC", L"Auge", 0, 22, 425, 55, 25);
    state.eye =
        child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 85, 420, 160, 160, eye_id);
    SendMessageW(state.eye, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Links"));
    SendMessageW(state.eye, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Rechts"));
    SendMessageW(state.eye, CB_SETCURSEL, state.selected_eye, 0);
    child(state.controls, L"STATIC", L"Bildrate", 0, 280, 425, 75, 25);
    state.fps =
        child(state.controls, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 365, 420, 150, 160, fps_id);
    for (const wchar_t *value : {L"30 FPS", L"60 FPS", L"90 FPS"})
        SendMessageW(state.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(state.fps, CB_SETCURSEL, state.rate == 30 ? 0 : state.rate == 60 ? 1 : 2, 0);
    state.status = child(state.controls, L"STATIC", L"", 0, 22, 478, 596, 245);
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
        DeleteObject(state.font);
        DeleteFileW(state.ini.c_str());
        CloseHandle(singleton);
        app = nullptr;
        return okay ? 0 : 2;
    }
    if (self_test) {
        state.source_window = state.output;
        state.source_pid = GetCurrentProcessId() + 1;
        const bool rejected_stale =
            !state.capture_frame(GetTickCount64()) && !state.source_window && !state.capture.running();
        SendMessageW(state.smooth, TBM_SETPOS, TRUE, 65);
        SendMessageW(state.controls, WM_HSCROLL, 0, reinterpret_cast<LPARAM>(state.smooth));
        SendMessageW(state.controls, WM_COMMAND, pause_id, 0);
        bool okay = rejected_stale && state.paused && state.options.smoothing == 65 &&
                    state.renderer.demo(0) && state.renderer.draw(state.options, false);
        if (state.channel.lock()) {
            okay = okay && state.channel.data()->paused == 1;
            state.channel.unlock();
        } else
            okay = false;
        state.update_status();
        okay = save_controls(state.controls, state.directory + L"\\preview-controls-test.bmp") && okay;
        DestroyWindow(state.output);
        DestroyWindow(state.controls);
        DeleteObject(state.font);
        DeleteFileW(state.ini.c_str());
        CloseHandle(singleton);
        app = nullptr;
        return okay ? 0 : 1;
    }
    ShowWindow(state.controls, show);
    ShowWindow(state.output, SW_SHOW);
    state.heartbeat();
    state.update_status();
    std::uint64_t last_status = 0, last_heartbeat = 0;
    double next_frame = double(GetTickCount64());
    MSG msg{};
    while (!state.quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state.quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
        if (state.active() && double(now) >= next_frame) {
            bool received = state.demo          ? state.renderer.demo((now - state.start_ms) / 1000.)
                            : state.window_mode ? state.capture_frame(now)
                                                : state.renderer.consume(state.channel);
            if (received) {
                state.last_received_ms = now;
                ++state.fps_frames;
            }
            if ((received || state.dirty) && state.renderer.has_frame())
                state.renderer.draw(state.options);
            state.dirty = false;
            next_frame = std::max(next_frame + 1000. / state.rate, double(now));
        } else if (state.dirty && state.renderer.has_frame() && IsWindowVisible(state.output) &&
                   !IsIconic(state.output)) {
            state.renderer.draw(state.options);
            state.dirty = false;
        }
        const DWORD delay =
            state.active() ? DWORD(std::clamp(next_frame - double(GetTickCount64()), 1., 16.)) : 50;
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
    DeleteObject(state.font);
    CloseHandle(singleton);
    app = nullptr;
    return 0;
}
