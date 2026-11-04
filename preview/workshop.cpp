#include "workshop.hpp"
#include "crop_geometry.hpp"
#include "speed_telemetry.hpp"
#include "source_status.hpp"
#include "workshop_clip.hpp"
#include "workshop_input.hpp"
#include "workshop_playback.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <map>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>
namespace vrst::preview {
namespace {
enum {
    record_id = 6100,
    open_id,
    play_id,
    source_id,
    center_id,
    reset_id,
    apply_id,
    resume_id,
    duration_id,
    quality_id,
    hotkey_id,
    bind_key_id,
    devices_id,
    refresh_id,
    learn_id,
    clear_id,
    timeline_id,
    loop_id,
    forward_id,
    crop_id,
    smooth_id,
    pitch_id,
    roll_id,
    x_id,
    y_id,
    compare_id
};
}
struct Workshop::Impl {
    HWND owner{}, window{}, canvas{}, overlay{}, status{}, input_status{}, time_label{}, bindings{};
    Theme *theme{};
    Renderer *live{};
    Renderer replay;
    Callbacks cb;
    std::filesystem::path ini, clips;
    std::filesystem::path pending_clip;
    ClipRecorder recorder;
    ClipReader reader;
    WorkshopInput input;
    SpeedTelemetry telemetry;
    std::vector<WorkshopInput::Device> devices;
    Options draft{}, initial{};
    bool editing{}, playing{}, source_view{true}, loop{true}, dragging{}, moving{}, refreshing{};
    bool warming{}, dirty{};
    std::size_t current{}, target{}, next{};
    bool seeking{};
    PlaybackClock clock;
    std::size_t forward_frame{SIZE_MAX};
    ULONGLONG next_play{}, last_poll{}, last_frame{}, last_status{};
    CaptureState previous_state{CaptureState::idle};
    POINT drag_start{};
    FramingRegion drag_rect{};
    CropHandle drag_handle{};
    Options before_drag{};
    std::map<int, HWND> controls, labels;
    Frame shown{};
    double source_aspect{1}, pixel_aspect{1};
    std::wstring clip_summary;
    HWND child(const wchar_t *kind, const wchar_t *label, DWORD style, int id) {
        HWND h = CreateWindowW(kind, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme->style(kind, style), 0,
                               0, 10, 10, window, reinterpret_cast<HMENU>(INT_PTR(id)),
                               GetModuleHandleW(nullptr), nullptr);
        theme->control(h, kind);
        controls[id] = h;
        return h;
    }
    void text(const std::wstring &s) {
        if (status)
            SetWindowTextW(status, s.c_str());
    }
    bool recording() {
        auto s = recorder.status().state;
        return s == CaptureState::preparing || s == CaptureState::recording || s == CaptureState::finishing;
    }
    RECT image_rect() const {
        RECT r{};
        GetClientRect(canvas, &r);
        const double w = r.right, h = r.bottom;
        if (w / h > pixel_aspect) {
            int width = int(h * pixel_aspect);
            r.left = (int(w) - width) / 2;
            r.right = r.left + width;
        } else {
            int height = int(w / pixel_aspect);
            r.top = (int(h) - height) / 2;
            r.bottom = r.top + height;
        }
        return r;
    }
    Options render_options() const {
        Options result = draft;
        RECT r{};
        GetClientRect(canvas, &r);
        result.output_width = std::max(1L, r.right);
        result.output_height = std::max(1L, r.bottom);
        result.workshop_source = source_view;
        return result;
    }
    RECT crop_rect() const {
        auto image = image_rect();
        const auto r = crop_from_options(source_aspect, draft, shown.eye);
        const double w = image.right - image.left, h = image.bottom - image.top;
        return {LONG(image.left + r.left * w), LONG(image.top + r.top * h), LONG(image.left + r.right * w),
                LONG(image.top + r.bottom * h)};
    }
    void sync() {
        if (!window)
            return;
        refreshing = true;
        const std::pair<int, double> values[]{{crop_id, draft.crop},           {smooth_id, draft.smoothing},
                                              {pitch_id, draft.pitch_lock},    {roll_id, draft.horizon_lock},
                                              {x_id, draft.horizontal_offset}, {y_id, draft.vertical_offset}};
        for (auto [id, v] : values) {
            SendMessageW(controls[id], TBM_SETPOS, TRUE, LONG(std::round(v)));
            wchar_t b[48]{};
            swprintf(b, 48, L"%.1f%%", v);
            SetWindowTextW(labels[id], b);
        }
        ShowWindow(overlay, editing && source_view ? SW_SHOW : SW_HIDE);
        InvalidateRect(overlay, nullptr, FALSE);
        SetWindowTextW(controls[source_id], source_view ? L"Show processed output" : L"Edit source framing");
        SetWindowTextW(controls[play_id], playing ? L"Pause" : L"Play");
        SendMessageW(controls[compare_id], BM_SETCHECK, draft.compare_original ? BST_CHECKED : BST_UNCHECKED,
                     0);
        for (int id : {play_id, source_id, compare_id, center_id, reset_id, apply_id, forward_id, timeline_id,
                       crop_id, smooth_id, pitch_id, roll_id, x_id, y_id})
            EnableWindow(controls[id], editing);
        refreshing = false;
    }
    void request_seek(std::size_t index) {
        if (!editing || !reader.count())
            return;
        target = std::min(index, reader.count() - 1);
        current = target;
        next = 0;
        warming = seeking = true;
        dirty = true;
        replay.reset();
        if (!reader.seek(0)) {
            text(reader.error());
            warming = false;
            playing = false;
        } else
            text(L"Rebuilding preview from recorded poses...");
    }
    void changed() {
        sync();
        request_seek(current);
    }
    void finish_edit() {
        playing = warming = editing = false;
        reader.close();
        replay.source_changed();
        shown = {};
        EnableWindow(owner, TRUE);
        if (cb.pause_live)
            cb.pause_live(false);
        sync();
    }
    void close() {
        if (recording())
            recorder.cancel();
        finish_edit();
        ShowWindow(window, SW_HIDE);
    }
    void start_stop() {
        if (recording()) {
            recorder.stop(L"Stopped by recording shortcut");
            return;
        }
        if (editing) {
            text(L"Return to live before recording another clip.");
            return;
        }
        if (!cb.source_ready || !cb.source_ready()) {
            text(L"Start a live OpenXR preview before recording. Window capture is not a pose recording "
                 L"source.");
            return;
        }
        Frame frame{};
        ID3D11Texture2D *textures[2]{};
        if (!live->source_snapshot(frame, textures)) {
            text(L"Waiting for a complete OpenXR source frame.");
            return;
        }
        CaptureOptions options;
        int selection = window ? int(SendMessageW(controls[duration_id], CB_GETCURSEL, 0, 0)) : 1;
        options.duration_seconds = selection == 0 ? 15 : selection == 2 ? 60 : 30;
        options.fps = window && SendMessageW(controls[quality_id], CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
        std::error_code ec;
        std::filesystem::create_directories(clips, ec);
        if (ec) {
            text(L"Cannot create the local clips folder.");
            return;
        }
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t name[100]{};
        swprintf(name, 100, L"Drive-%04u%02u%02u-%02u%02u%02u-%03u.sxclip", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        recorder.start(live->device(), clips / name, frame, options);
        last_frame = GetTickCount64();
        previous_state = CaptureState::idle;
        text(recorder.status().message);
    }
    void open_clip(const std::filesystem::path &path) {
        if (recording()) {
            text(L"Finish or cancel recording before opening a clip.");
            return;
        }
        if (!replay.device()) {
            text(L"Workshop graphics device is unavailable.");
            return;
        }
        if (!reader.open(replay.device(), path)) {
            auto error = reader.error();
            finish_edit();
            text(error);
            return;
        }
        draft = cb.options();
        draft.spectator.natural_enabled = false;
        draft.workshop_source = false;
        draft.compare_original = draft.compare_effects = false;
        draft.compare_mode = 0;
        initial = draft;
        const auto &info = reader.info();
        const auto *last = reader.metadata(reader.count() - 1);
        double seconds = last ? last->media_time / 1e7 : 0;
        unsigned gaps = 0;
        for (std::size_t i = 1; i < reader.count(); ++i)
            if (reader.metadata(i)->media_time - reader.metadata(i - 1)->media_time >
                15000000LL / info.options.fps)
                ++gaps;
        wchar_t summary[240]{};
        swprintf(summary, 240, L"%zu frames | %.1f fps average | %u x %u proxy | %u dropped | %u gaps",
                 reader.count(), seconds > 0 ? (reader.count() - 1) / seconds : 0, info.width[0],
                 info.height[0], info.dropped_frames, gaps);
        clip_summary = summary;
        if (info.partial)
            clip_summary += L" | Partial: " + info.stop_reason;
        editing = true;
        playing = false;
        if (cb.pause_live)
            cb.pause_live(true);
        EnableWindow(owner, FALSE);
        current = 0;
        forward_frame = SIZE_MAX;
        SendMessageW(controls[timeline_id], TBM_SETRANGEMAX, FALSE, reader.count() - 1);
        text(L"Live output paused. Drag the source frame or its handles; changes stay in this draft until "
             L"Apply.");
        sync();
        request_seek(0);
    }
    void browse() {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Open a SteadyXR .sxclip folder");
        if (FAILED(dialog->Show(window)))
            return;
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item)))
            return;
        PWSTR path{};
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            open_clip(path);
            CoTaskMemFree(path);
        }
    }
    void refresh_devices() {
        devices = input.devices();
        SendMessageW(controls[devices_id], CB_RESETCONTENT, 0, 0);
        for (auto &d : devices)
            SendMessageW(controls[devices_id], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
        if (!devices.empty())
            SendMessageW(controls[devices_id], CB_SETCURSEL, 0, 0);
    }
    void command(int id) {
        if (id == record_id)
            start_stop();
        else if (id == open_id)
            browse();
        else if (id == play_id) {
            playing = !playing;
            const auto *f = reader.metadata(current);
            if (f)
                clock.start(GetTickCount64(), f->media_time);
            sync();
        } else if (id == source_id) {
            source_view = !source_view;
            changed();
        } else if (id == compare_id) {
            source_view = false;
            draft.compare_original = SendMessageW(controls[compare_id], BM_GETCHECK, 0, 0) == BST_CHECKED;
            draft.compare_effects = false;
            draft.compare_mode = 0;
            changed();
        } else if (id == center_id) {
            draft.horizontal_offset = draft.vertical_offset = 0;
            changed();
        } else if (id == reset_id) {
            draft = initial;
            changed();
        } else if (id == forward_id) {
            forward_frame = current;
            request_seek(current);
        } else if (id == apply_id) {
            auto out = draft;
            out.workshop_source = false;
            out.compare_original = false;
            cb.apply(out);
            text(L"Settings applied to the active profile. Return to live when ready.");
            initial = out;
        } else if (id == resume_id) {
            finish_edit();
            ShowWindow(window, SW_HIDE);
        } else if (id == loop_id)
            loop = SendMessageW(controls[loop_id], BM_GETCHECK, 0, 0) == BST_CHECKED;
        else if (id == refresh_id)
            refresh_devices();
        else if (id == clear_id)
            input.clear_button();
        else if (id == learn_id) {
            int i = int(SendMessageW(controls[devices_id], CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < int(devices.size()))
                input.learn(devices[i].id);
        } else if (id == bind_key_id) {
            WORD key = WORD(SendMessageW(controls[hotkey_id], HKM_GETHOTKEY, 0, 0));
            unsigned flags = HIBYTE(key), mods = 0;
            if (flags & HOTKEYF_CONTROL)
                mods |= MOD_CONTROL;
            if (flags & HOTKEYF_SHIFT)
                mods |= MOD_SHIFT;
            if (flags & HOTKEYF_ALT)
                mods |= MOD_ALT;
            input.hotkey(mods, LOBYTE(key));
            SetWindowTextW(input_status, input.status().c_str());
        }
    }
    void layout() {
        if (!window)
            return;
        RECT r{};
        GetClientRect(window, &r);
        auto px = [](int n) { return n; };
        const int m = px(20), side = px(280), gap = px(20), left = r.right - side - 3 * m;
        auto place = [&](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, x, y, w, hgt, TRUE); };
        place(status, m, m, r.right - 2 * m, px(68));
        place(canvas, m, px(104), left, r.bottom - px(322));
        MoveWindow(overlay, 0, 0, left, r.bottom - px(322), TRUE);
        place(controls[timeline_id], m, r.bottom - px(136), left, px(28));
        place(time_label, m, r.bottom - px(106), left, px(22));
        int x = m;
        for (int id : {play_id, source_id, loop_id, compare_id}) {
            int width = px(id == source_id ? 195 : id == compare_id ? 110 : 80);
            place(controls[id], x, r.bottom - px(74), width, px(34));
            x += width + px(8);
        }
        const int sx = r.right - side - m;
        int y = px(104);
        for (int id : {crop_id, x_id, y_id, smooth_id, pitch_id, roll_id}) {
            place(controls[id + 100], sx, y, side - px(58), px(20));
            place(labels[id], sx + side - px(58), y, px(58), px(20));
            place(controls[id], sx, y + px(24), side, px(22));
            y += px(62);
        }
        for (int id : {center_id, forward_id, reset_id, apply_id, resume_id}) {
            place(controls[id], sx, y, side, px(30));
            y += px(36);
        }
        // Capture controls sit in a separate compact strip, above the timeline.
        int cy = r.bottom - px(194);
        place(controls[record_id], m, cy, px(135), px(32));
        place(controls[open_id], m + px(143), cy, px(115), px(32));
        place(controls[duration_id], m + px(266), cy, px(82), px(200));
        place(controls[quality_id], m + px(356), cy, px(100), px(200));
        place(controls[6200], m + px(464), cy, px(150), px(32));
        InvalidateRect(overlay, nullptr, FALSE);
    }
    void open() {
        if (window) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            if (!pending_clip.empty()) {
                auto path = pending_clip;
                pending_clip.clear();
                open_clip(path);
            }
            return;
        }
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"SteadyXRWorkshop";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"SteadyXR - Capture Workshop",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 120, 80, 1180, 900, owner, nullptr,
                                 wc.hInstance, this);
        theme->titlebar(window);
        status = child(
            L"STATIC",
            L"Record the full OpenXR source, then tune your spectator view. Live pauses only while editing.",
            0, 6000);
        wc.lpszClassName = L"SteadyXRWorkshopCanvas";
        wc.lpfnWndProc = canvas_proc;
        RegisterClassW(&wc);
        canvas = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1,
                                 1, window, nullptr, wc.hInstance, this);
        wc.lpszClassName = L"SteadyXRWorkshopCrop";
        wc.lpfnWndProc = overlay_proc;
        RegisterClassW(&wc);
        overlay = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName,
                                  L"Drag crop rectangle; arrows move; Escape cancels", WS_CHILD | WS_TABSTOP,
                                  0, 0, 1, 1, canvas, nullptr, wc.hInstance, this);
        if (!overlay || !SetLayeredWindowAttributes(overlay, RGB(1, 2, 3), 255, LWA_COLORKEY))
            text(L"Crop overlay unavailable. Use the framing sliders.");
        for (auto item : std::vector<std::pair<int, const wchar_t *>>{{record_id, L"Record clip"},
                                                                      {open_id, L"Open clip..."},
                                                                      {play_id, L"Play"},
                                                                      {source_id, L"Show processed output"},
                                                                      {center_id, L"Center crop"},
                                                                      {forward_id, L"Set forward view"},
                                                                      {reset_id, L"Reset draft"},
                                                                      {apply_id, L"Apply to active profile"},
                                                                      {resume_id, L"Return to live"}})
            child(L"BUTTON", item.second, BS_PUSHBUTTON, item.first);
        child(L"BUTTON", L"Loop", BS_AUTOCHECKBOX, loop_id);
        SendMessageW(controls[loop_id], BM_SETCHECK, BST_CHECKED, 0);
        child(L"BUTTON", L"Original", BS_AUTOCHECKBOX, compare_id);
        for (auto item :
             std::vector<std::pair<int, const wchar_t *>>{{crop_id, L"Crop reserve"},
                                                          {x_id, L"Horizontal position"},
                                                          {y_id, L"Vertical position"},
                                                          {smooth_id, L"Smoothing"},
                                                          {pitch_id, L"Pitch steadiness (optional)"},
                                                          {roll_id, L"Level horizon"}}) {
            child(L"STATIC", item.second, 0, item.first + 100);
            labels[item.first] = child(L"STATIC", L"", 0, item.first + 200);
            child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, item.first);
            SendMessageW(controls[item.first], TBM_SETRANGE, FALSE,
                         MAKELPARAM(item.first == x_id || item.first == y_id ? -100 : 0,
                                    item.first == crop_id ? 40 : 100));
        }
        child(TRACKBAR_CLASSW, L"Clip position", TBS_HORZ | TBS_NOTICKS, timeline_id);
        time_label = child(L"STATIC", L"No recording loaded", 0, 6001);
        child(L"COMBOBOX", L"Duration", CBS_DROPDOWNLIST, duration_id);
        for (auto t : {L"15 sec", L"30 sec", L"60 sec"})
            SendMessageW(controls[duration_id], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
        SendMessageW(controls[duration_id], CB_SETCURSEL, 1, 0);
        child(L"COMBOBOX", L"Capture quality", CBS_DROPDOWNLIST, quality_id);
        for (auto t : {L"30 fps", L"60 fps"})
            SendMessageW(controls[quality_id], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
        SendMessageW(controls[quality_id], CB_SETCURSEL, 0, 0);
        // Input setup is a native owned dialog, opened from the workshop menu.
        child(L"BUTTON", L"Recording controls...", BS_PUSHBUTTON, 6200);
        replay.attach(canvas);
        layout();
        theme->scale_window(window);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            theme->fit_window(window, monitor.rcWork);
        sync();
        ShowWindow(window, SW_SHOW);
        if (!pending_clip.empty()) {
            auto path = pending_clip;
            pending_clip.clear();
            open_clip(path);
        }
    }
    void configure_input();
    static Impl *get(HWND w, UINT m, LPARAM l) {
        auto p = reinterpret_cast<Impl *>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        return p;
    }
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto p = get(w, m, b);
        if (!p)
            return DefWindowProcW(w, m, a, b);
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(w, &ps);
            auto brush = CreateSolidBrush(RGB(15, 15, 18));
            FillRect(dc, &ps.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(w, &ps);
            return 0;
        }
        LRESULT result{};
        if (p->theme->message(w, m, a, b, result))
            return result;
        if (m == WM_COMMAND) {
            if (LOWORD(a) == 6200)
                p->configure_input();
            else
                p->command(LOWORD(a));
            return 0;
        }
        if (m == WM_SIZE) {
            p->dirty = true;
            return 0;
        }
        if (m == WM_GETMINMAXINFO) {
            auto i = reinterpret_cast<MINMAXINFO *>(b);
            i->ptMinTrackSize = {600, 400};
            return 0;
        }
        if (m == WM_CLOSE) {
            p->close();
            return 0;
        }
        if (m == WM_HSCROLL && !p->refreshing) {
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(b));
            double v = double(SendMessageW(reinterpret_cast<HWND>(b), TBM_GETPOS, 0, 0));
            if (id == timeline_id) {
                p->playing = false;
                p->request_seek(std::size_t(v));
                return 0;
            }
            if (!p->editing)
                return 0;
            if (id == crop_id || id == x_id || id == y_id)
                crop_to_options(crop_from_options(p->source_aspect, p->draft, p->shown.eye), p->source_aspect,
                                p->draft, p->shown.eye);
            switch (id) {
            case crop_id:
                p->draft.crop = v;
                break;
            case smooth_id:
                p->draft.smoothing = v;
                break;
            case pitch_id:
                p->draft.pitch_lock = v;
                break;
            case roll_id:
                p->draft.horizon_lock = v;
                break;
            case x_id:
                p->draft.horizontal_offset = v;
                break;
            case y_id:
                p->draft.vertical_offset = v;
                break;
            }
            p->changed();
            return 0;
        }
        return DefWindowProcW(w, m, a, b);
    }
    static LRESULT CALLBACK canvas_proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto p = get(w, m, b);
        if (p && m == WM_SIZE && p->overlay) {
            RECT r{};
            GetClientRect(w, &r);
            MoveWindow(p->overlay, 0, 0, r.right, r.bottom, TRUE);
            p->dirty = true;
            return 0;
        }
        // Color-keyed overlay pixels pass input to the canvas. Use the same
        // handler for interior drags and opaque edge handles.
        if (p && p->editing && p->source_view &&
            (m == WM_LBUTTONDOWN || m == WM_MOUSEMOVE || m == WM_LBUTTONUP || m == WM_SETCURSOR))
            return overlay_proc(p->overlay, m, a, b);
        if (p && m == WM_PAINT) {
            PAINTSTRUCT ps{};
            BeginPaint(w, &ps);
            EndPaint(w, &ps);
            p->dirty = true;
            return 0;
        }
        return DefWindowProcW(w, m, a, b);
    }
    static LRESULT CALLBACK overlay_proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        auto p = get(w, m, b);
        if (!p)
            return DefWindowProcW(w, m, a, b);
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(w, &ps);
            RECT full{};
            GetClientRect(w, &full);
            auto brush = CreateSolidBrush(RGB(1, 2, 3));
            FillRect(dc, &full, brush);
            DeleteObject(brush);
            auto r = p->crop_rect();
            auto pen = CreatePen(PS_SOLID, 2, RGB(167, 155, 240));
            auto old = SelectObject(dc, pen);
            auto oldb = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, oldb);
            SelectObject(dc, old);
            DeleteObject(pen);
            auto handle = CreateSolidBrush(RGB(112, 89, 221));
            for (auto x : {r.left, (r.left + r.right) / 2, r.right})
                for (auto y : {r.top, (r.top + r.bottom) / 2, r.bottom}) {
                    if (x == (r.left + r.right) / 2 && y == (r.top + r.bottom) / 2)
                        continue;
                    RECT box{x - 5, y - 5, x + 5, y + 5};
                    FillRect(dc, &box, handle);
                }
            DeleteObject(handle);
            EndPaint(w, &ps);
            return 0;
        }
        if (m == WM_GETDLGCODE) {
            auto msg = reinterpret_cast<MSG *>(b);
            return DLGC_WANTARROWS | DLGC_WANTCHARS |
                   ((p->dragging && msg && msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE)
                        ? DLGC_WANTMESSAGE
                        : 0);
        }
        if (m == WM_LBUTTONDOWN && p->editing) {
            POINT q{GET_X_LPARAM(b), GET_Y_LPARAM(b)};
            auto r = p->crop_rect();
            bool l = std::abs(q.x - r.left) < 12, rr = std::abs(q.x - r.right) < 12,
                 t = std::abs(q.y - r.top) < 12, bt = std::abs(q.y - r.bottom) < 12;
            RECT hit{r.left - 12, r.top - 12, r.right + 12, r.bottom + 12};
            if (!PtInRect(&hit, q))
                return 0;
            p->moving = !(l || rr || t || bt);
            p->drag_handle = l    ? (t    ? CropHandle::top_left
                                     : bt ? CropHandle::bottom_left
                                          : CropHandle::left)
                             : rr ? (t    ? CropHandle::top_right
                                     : bt ? CropHandle::bottom_right
                                          : CropHandle::right)
                             : t  ? CropHandle::top
                                  : CropHandle::bottom;
            p->drag_start = q;
            p->drag_rect = crop_from_options(p->source_aspect, p->draft, p->shown.eye);
            p->before_drag = p->draft;
            p->dragging = true;
            p->playing = false;
            SetCapture(w);
            SetFocus(w);
            return 0;
        }
        if (m == WM_MOUSEMOVE && p->dragging) {
            auto r = p->image_rect();
            double dx = double(GET_X_LPARAM(b) - p->drag_start.x) / std::max(1L, r.right - r.left),
                   dy = double(GET_Y_LPARAM(b) - p->drag_start.y) / std::max(1L, r.bottom - r.top);
            auto crop = p->moving ? move_crop(p->drag_rect, dx, dy)
                                  : resize_crop(p->drag_rect, p->drag_handle, dx, dy, p->source_aspect,
                                                p->draft.aspect);
            crop_to_options(crop, p->source_aspect, p->draft, p->shown.eye);
            p->sync();
            return 0;
        }
        if (m == WM_LBUTTONUP && p->dragging) {
            p->dragging = false;
            ReleaseCapture();
            p->changed();
            return 0;
        }
        if (m == WM_CAPTURECHANGED) {
            p->dragging = false;
            return 0;
        }
        if (m == WM_KEYDOWN) {
            if (a == VK_ESCAPE && p->dragging) {
                p->draft = p->before_drag;
                p->dragging = false;
                ReleaseCapture();
                p->changed();
                return 0;
            }
            double d = GetKeyState(VK_SHIFT) < 0 ? .01 : .001;
            double x = a == VK_LEFT ? -d : a == VK_RIGHT ? d : 0, y = a == VK_UP ? -d : a == VK_DOWN ? d : 0;
            if (x || y) {
                crop_to_options(move_crop(crop_from_options(p->source_aspect, p->draft, p->shown.eye), x, y),
                                p->source_aspect, p->draft, p->shown.eye);
                p->changed();
                return 0;
            }
        }
        if (m == WM_SETCURSOR) {
            SetCursor(LoadCursor(nullptr, p->dragging && !p->moving ? IDC_SIZENWSE : IDC_SIZEALL));
            return TRUE;
        }
        return DefWindowProcW(w, m, a, b);
    }
};
// The shortcut dialog uses the system hotkey control and real DirectInput buttons.
void Workshop::Impl::configure_input() {
    if (bindings && IsWindow(bindings)) {
        SetForegroundWindow(bindings);
        return;
    }
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SteadyXRWorkshopBindings";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = [](HWND w, UINT m, WPARAM a, LPARAM b) -> LRESULT {
        auto p = get(w, m, b);
        if (!p)
            return DefWindowProcW(w, m, a, b);
        if (m == WM_PAINT) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(w, &ps);
            auto brush = CreateSolidBrush(RGB(15, 15, 18));
            FillRect(dc, &ps.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(w, &ps);
            return 0;
        }
        if (m == WM_NCDESTROY) {
            p->input_status = p->bindings = nullptr;
        }
        LRESULT result{};
        if (p->theme->message(w, m, a, b, result))
            return result;
        if (m == WM_COMMAND) {
            p->command(LOWORD(a));
            return 0;
        }
        if (m == WM_CLOSE) {
            DestroyWindow(w);
            return 0;
        }
        return DefWindowProcW(w, m, a, b);
    };
    RegisterClassW(&wc);
    auto dialog = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Recording controls",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 180, 160, 570, 320,
                                  window, nullptr, wc.hInstance, this);
    theme->titlebar(dialog);
    bindings = dialog;
    auto add = [&](const wchar_t *kind, const wchar_t *label, DWORD style, int id, int y, int width) {
        auto h =
            CreateWindowW(kind, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme->style(kind, style), 20, y,
                          width, 30, dialog, reinterpret_cast<HMENU>(INT_PTR(id)), wc.hInstance, nullptr);
        theme->control(h, kind);
        controls[id] = h;
        return h;
    };
    add(HOTKEY_CLASSW, L"Record shortcut", 0, hotkey_id, 20, 330);
    unsigned flags = 0;
    if (input.modifiers() & MOD_CONTROL)
        flags |= HOTKEYF_CONTROL;
    if (input.modifiers() & MOD_SHIFT)
        flags |= HOTKEYF_SHIFT;
    if (input.modifiers() & MOD_ALT)
        flags |= HOTKEYF_ALT;
    SendMessageW(controls[hotkey_id], HKM_SETHOTKEY, MAKEWORD(input.key(), flags), 0);
    auto button = add(L"BUTTON", L"Assign shortcut", BS_PUSHBUTTON, bind_key_id, 20, 150);
    SetWindowPos(button, nullptr, 370, 20, 150, 30, SWP_NOZORDER);
    auto combo = add(L"COMBOBOX", L"Wheel device", CBS_DROPDOWNLIST, devices_id, 70, 330);
    SetWindowPos(combo, nullptr, 20, 70, 330, 220, SWP_NOZORDER);
    button = add(L"BUTTON", L"Refresh devices", BS_PUSHBUTTON, refresh_id, 70, 150);
    SetWindowPos(button, nullptr, 370, 70, 150, 30, SWP_NOZORDER);
    add(L"BUTTON", L"Learn wheel button", BS_PUSHBUTTON, learn_id, 115, 330);
    button = add(L"BUTTON", L"Clear button", BS_PUSHBUTTON, clear_id, 115, 150);
    SetWindowPos(button, nullptr, 370, 115, 150, 30, SWP_NOZORDER);
    input_status = add(L"STATIC", input.status().c_str(), 0, 6002, 165, 500);
    SetWindowPos(input_status, nullptr, 20, 165, 500, 80, SWP_NOZORDER);
    refresh_devices();
    theme->scale_window(dialog);
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor))
        theme->fit_window(dialog, monitor.rcWork);
}
Workshop::Workshop() : impl_(new Impl) {}
Workshop::~Workshop() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void Workshop::initialize(HWND owner, Theme &theme, Renderer &live, const std::filesystem::path &ini,
                          Callbacks cb) {
    auto &p = *impl_;
    p.owner = owner;
    p.theme = &theme;
    p.live = &live;
    p.ini = ini;
    p.clips = ini.parent_path() / L"Clips";
    p.cb = std::move(cb);
    p.input.initialize(owner, ini, [&p] { p.start_stop(); });
}
void Workshop::open() {
    impl_->open();
}
void Workshop::open_clip(const std::filesystem::path &path) {
    impl_->open();
    impl_->open_clip(path);
}
bool Workshop::editing() const {
    return impl_->editing;
}
bool Workshop::route(MSG &msg) {
    auto &p = *impl_;
    if (p.bindings && IsWindow(p.bindings) && IsDialogMessageW(p.bindings, &msg))
        return true;
    return p.window && IsWindowVisible(p.window) && IsDialogMessageW(p.window, &msg);
}
void Workshop::on_frame() {
    auto &p = *impl_;
    if (!p.recording())
        return;
    Frame frame{};
    ID3D11Texture2D *images[2]{};
    if (p.live->source_snapshot(frame, images)) {
        double speed = -1;
        const auto options = p.cb.options();
        if (options.director.speed_fov_enabled) {
            auto reading = p.telemetry.sample(static_cast<SpeedGame>(options.director.telemetry_game));
            if (reading.valid)
                speed = reading.speed_kph;
        } else
            p.telemetry.reset();
        p.recorder.submit(frame, images, speed);
        p.last_frame = GetTickCount64();
    }
}
void Workshop::tick() {
    auto &p = *impl_;
    const auto now = GetTickCount64();
    if (now - p.last_poll >= 20) {
        p.input.poll();
        p.last_poll = now;
        if (p.input_status)
            SetWindowTextW(p.input_status, p.input.status().c_str());
    }
    auto state = p.recorder.status();
    if (p.recording() && now - p.last_frame > 2000)
        p.recorder.stop(L"OpenXR frames stopped; saved available frames");
    if (state.state != p.previous_state) {
        p.previous_state = state.state;
        if (p.window) {
            p.text(state.message);
            SetWindowTextW(p.controls[record_id], p.recording() ? L"Stop recording" : L"Record clip");
        }
        if (state.state == CaptureState::ready) {
            p.pending_clip = state.path;
            p.text(L"Clip saved. Open Workshop to edit; live output continues until then.");
            if (p.window && GetForegroundWindow() == p.window) {
                p.pending_clip.clear();
                p.open_clip(state.path);
            }
        }
    }
    if (p.recording() && p.window && now - p.last_status >= 250) {
        p.last_status = now;
        p.text(L"Recording full OpenXR source - " + std::to_wstring(state.recorded_frames) + L" frames, " +
               std::to_wstring(state.dropped_frames) + L" dropped. " + state.message);
    }
    if (!p.window || !IsWindowVisible(p.window))
        return;
    if (p.editing) {
        if (!p.warming && p.playing) {
            const auto *following = p.reader.metadata(p.current + 1);
            if (following && p.clock.due(now, following->media_time)) {
                p.target = p.current + 1;
                p.warming = true;
                p.seeking = false;
            } else if (!following) {
                if (p.loop)
                    p.request_seek(0);
                else {
                    p.playing = false;
                    p.sync();
                }
            }
        }
        if (p.warming) {
            for (unsigned n = 0; n < 4 && p.warming; ++n) {
                DecodedFrame decoded;
                if (!p.reader.next(decoded)) {
                    p.text(p.reader.error());
                    p.warming = p.playing = p.dirty = false;
                    break;
                }
                ID3D11Texture2D *textures[]{decoded.textures[0].Get(), decoded.textures[1].Get()};
                if (!p.replay.consume_replay(decoded.frame, textures, decoded.speed_kph)) {
                    p.text(L"Decoded image does not match its pose metadata.");
                    p.warming = p.playing = p.dirty = false;
                    break;
                }
                const bool present = p.next == p.target;
                if (present) {
                    p.shown = decoded.frame;
                    const auto &v = p.shown.views[0];
                    p.source_aspect = (v.fov[1] - v.fov[0]) / (v.fov[2] - v.fov[3]);
                    p.pixel_aspect = double(v.rect_width) / v.rect_height;
                }
                auto render_options = p.render_options();
                if (p.next == p.forward_frame)
                    p.replay.reset_horizon();
                if (!p.replay.draw(render_options, present)) {
                    p.text(p.replay.error());
                    p.warming = p.playing = false;
                    p.dirty = false;
                    break;
                }
                ++p.next;
                if (present) {
                    p.current = p.target;
                    p.warming = false;
                    p.dirty = true;
                    if (p.seeking) {
                        const auto *f = p.reader.metadata(p.current);
                        if (f)
                            p.clock.start(GetTickCount64(), f->media_time);
                        p.seeking = false;
                    }
                }
            }
        }
        if (p.dirty && !p.warming && p.reader.count() && p.reader.metadata(p.current)) {
            auto render_options = p.render_options();
            p.replay.draw(render_options);
            SendMessageW(p.controls[timeline_id], TBM_SETPOS, TRUE, p.current);
            const auto *f = p.reader.metadata(p.current);
            wchar_t t[160]{};
            swprintf(t, 160, L"%.1f s / %.1f s  |  %s", f ? f->media_time / 1e7 : 0,
                     p.reader.metadata(p.reader.count() - 1)->media_time / 1e7,
                     p.source_view ? L"Full source + crop guide" : L"Processed output");
            SetWindowTextW(p.time_label, t);
            p.sync();
            p.text(std::wstring(p.replay.correction_fraction() < .999
                                    ? L"Live paused - source margins limit correction. Add crop or reduce "
                                      L"stabilization.\r\n"
                                    : L"Live paused - draft changes apply only when you choose Apply.\r\n") +
                   (f ? source_view_summary(f->frame, true) + L"\r\n" : L"") + p.clip_summary);
            p.dirty = false;
        }
    } else if (p.dirty) {
        Options idle;
        p.replay.draw(idle, true, L"Record or open a clip to begin");
        p.dirty = false;
    }
}
} // namespace vrst::preview
