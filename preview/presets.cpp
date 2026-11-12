#define UNICODE
#define _UNICODE
#include "presets.hpp"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cwchar>
#include <string_view>

namespace vrst::preview {
namespace {
constexpr std::size_t max_snapshot_bytes = 256 * 1024;
wchar_t upper(wchar_t c) {
    return c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c;
}
bool same_name(const std::wstring &a, const std::wstring &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool fail(std::wstring &error, const wchar_t *message) {
    error = message;
    return false;
}
bool io_error(std::wstring &error, const wchar_t *action, DWORD code = GetLastError()) {
    wchar_t description[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, description,
                   512, nullptr);
    error = action;
    error += L" ";
    error += *description ? description : (L"Windows error " + std::to_wstring(code));
    while (!error.empty() && (error.back() == L'\r' || error.back() == L'\n'))
        error.pop_back();
    return false;
}
std::wstring_view trim(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos)
        return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}
bool regular_preset(const std::filesystem::path &path, std::wstring &error) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return io_error(error, L"Could not access the preset.");
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(error, L"The preset must be a regular INI file.");
    return true;
}
} // namespace

PresetStore::PresetStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
bool PresetStore::valid_name(const std::wstring &name, std::wstring &error) {
    error.clear();
    if (name.empty() || name.size() > 64)
        return fail(error, L"Enter a preset name containing 1 to 64 characters.");
    if (name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
        return fail(error, L"A preset name cannot end with a dot or a space.");
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = name[i];
        if (c < 32 || (c >= 127 && c <= 159) || std::wcschr(L"<>:\"/\\|?*", c))
            return fail(error,
                        L"Use a friendly name without path separators or special filename characters.");
        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 >= name.size() || name[i + 1] < 0xdc00 || name[i + 1] > 0xdfff)
                return fail(error, L"The preset name contains an invalid Unicode character.");
            ++i;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            return fail(error, L"The preset name contains an invalid Unicode character.");
        }
    }
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ')
        base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), upper);
    const bool numbered_device =
        base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
        ((base[3] >= L'1' && base[3] <= L'9') || base[3] == 0xb9 || base[3] == 0xb2 || base[3] == 0xb3);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" || base == L"CONIN$" ||
        base == L"CONOUT$" || numbered_device)
        return fail(error, L"That name is reserved by Windows. Choose another preset name.");
    return true;
}
bool PresetStore::valid_snapshot(const std::string &bytes, std::wstring &error) {
    error.clear();
    if (bytes.empty() || bytes.size() > max_snapshot_bytes)
        return fail(error, L"The preset is empty or exceeds the 256 KB size limit.");
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        if (bytes.size() % 2)
            return fail(error, L"The preset contains incomplete Unicode INI data.");
        for (std::size_t i = 2; i < bytes.size(); i += 2)
            text.push_back(wchar_t(static_cast<unsigned char>(bytes[i]) |
                                   (static_cast<unsigned char>(bytes[i + 1]) << 8)));
    } else {
        const std::size_t start = bytes.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
        for (std::size_t i = start; i < bytes.size(); ++i)
            text.push_back(static_cast<unsigned char>(bytes[i]));
    }
    for (const auto c : text)
        if (c < 32 && c != L'\r' && c != L'\n' && c != L'\t')
            return fail(error, L"The preset contains binary data instead of INI settings.");
    std::wstring_view remaining(text);
    while (!remaining.empty()) {
        const auto end = remaining.find_first_of(L"\r\n");
        const auto line = trim(remaining.substr(0, end));
        if (!line.empty() && line.front() == L'[') {
            const auto closing = line.find(L']');
            if (closing != std::wstring_view::npos) {
                auto section = std::wstring(trim(line.substr(1, closing - 1)));
                std::transform(section.begin(), section.end(), section.begin(), upper);
                const auto after = trim(line.substr(closing + 1));
                if (section == L"PREVIEW" && (after.empty() || after.front() == L';'))
                    return true;
            }
        }
        if (end == std::wstring_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    return fail(error, L"This file is not a SteadyXR preset: the Preview section is missing.");
}
std::vector<std::wstring> PresetStore::list(std::wstring &error) const {
    error.clear();
    std::vector<std::wstring> names;
    std::error_code ec;
    if (!std::filesystem::exists(directory_, ec)) {
        if (ec)
            io_error(error, L"Could not open the presets folder.", DWORD(ec.value()));
        return names;
    }
    auto entries = std::filesystem::directory_iterator(directory_, ec);
    for (; !ec && entries != std::filesystem::directory_iterator(); entries.increment(ec)) {
        const auto &path = entries->path();
        const auto attributes = GetFileAttributesW(path.c_str());
        std::wstring ignored;
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            same_name(path.extension().wstring(), L".ini") && valid_name(path.stem().wstring(), ignored))
            names.push_back(path.stem().wstring());
    }
    if (ec) {
        io_error(error, L"Could not list the presets folder.", DWORD(ec.value()));
        return {};
    }
    std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return names;
}
std::optional<std::string> PresetStore::read(const std::wstring &name, std::wstring &error) const {
    if (!valid_name(name, error))
        return {};
    const auto path = directory_ / (name + L".ini");
    if (!regular_preset(path, error))
        return {};
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        io_error(error, L"Could not read the preset.");
        return {};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const auto code = GetLastError();
        CloseHandle(file);
        io_error(error, L"Could not read the preset size.", code);
        return {};
    }
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(max_snapshot_bytes)) {
        CloseHandle(file);
        fail(error, L"The preset is empty or exceeds the 256 KB size limit.");
        return {};
    }
    std::string bytes(std::size_t(size.QuadPart), '\0');
    DWORD count{};
    const auto success = ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr);
    const auto code = GetLastError();
    CloseHandle(file);
    if (!success || count != bytes.size()) {
        io_error(error, L"Could not read the complete preset.", success ? ERROR_HANDLE_EOF : code);
        return {};
    }
    if (!valid_snapshot(bytes, error))
        return {};
    return bytes;
}
bool PresetStore::save(const std::wstring &name, const std::string &bytes, bool overwrite,
                       std::wstring &error) const {
    if (!valid_name(name, error) || !valid_snapshot(bytes, error))
        return false;
    const auto path = directory_ / (name + L".ini");
    if (overwrite) {
        if (!regular_preset(path, error))
            return false;
        if (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_READONLY)
            return io_error(error, L"Could not overwrite the read-only preset.", ERROR_ACCESS_DENIED);
    } else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return fail(error, L"A preset with this name already exists. Use Overwrite selected to replace it.");
    }
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec)
        return io_error(error, L"Could not create the presets folder.", DWORD(ec.value()));
    static std::atomic<unsigned long> sequence{};
    const auto temporary =
        directory_ / (L".horizon-preset-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp");
    const auto file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return io_error(error, L"Could not create the preset snapshot.");
    DWORD count{};
    bool written =
        WriteFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr) && count == bytes.size();
    if (written)
        written = FlushFileBuffers(file) != FALSE;
    const auto code = GetLastError();
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        return io_error(error, L"Could not write the complete preset.", code ? code : ERROR_WRITE_FAULT);
    }
    // The destination changes only after a complete, flushed snapshot exists beside it.
    const bool moved = MoveFileExW(temporary.c_str(), path.c_str(),
                                   MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0));
    if (!moved) {
        const auto move_error = GetLastError();
        DeleteFileW(temporary.c_str());
        return io_error(error, L"Could not save the preset.", move_error);
    }
    error.clear();
    return true;
}
bool PresetStore::remove(const std::wstring &name, std::wstring &error) const {
    if (!valid_name(name, error))
        return false;
    const auto path = directory_ / (name + L".ini");
    if (!regular_preset(path, error))
        return false;
    if (!DeleteFileW(path.c_str()))
        return io_error(error, L"Could not delete the preset.");
    error.clear();
    return true;
}

struct PresetsDialog::Impl {
    HWND parent{}, window{}, names{}, name{}, status{};
    Theme *theme{};
    std::unique_ptr<PresetStore> store;
    Callbacks callbacks;
    bool refreshing{};

    HWND child(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int width, int height,
               int id = 0, bool heading = false) {
        if (std::wcscmp(kind, L"STATIC"))
            style |= WS_TABSTOP;
        auto h = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | theme->style(kind, style), x, y,
                                 width, height, window, reinterpret_cast<HMENU>(INT_PTR(id)),
                                 GetModuleHandleW(nullptr), nullptr);
        theme->control(h, kind, heading);
        return h;
    }
    void feedback(const std::wstring &text) {
        SetWindowTextW(status, text.c_str());
    }
    std::wstring entered_name() const {
        wchar_t text[128]{};
        GetWindowTextW(name, text, 128);
        return text;
    }
    std::wstring selected() const {
        const auto index = SendMessageW(names, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR)
            return {};
        const auto length = SendMessageW(names, CB_GETLBTEXTLEN, index, 0);
        if (length < 0 || length > 64)
            return {};
        std::wstring text(std::size_t(length) + 1, L'\0');
        SendMessageW(names, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(std::size_t(length));
        return text;
    }
    void enable_actions() {
        const bool has_selection = !selected().empty();
        for (const auto id : {load_id, overwrite_id, delete_id})
            EnableWindow(GetDlgItem(window, id), has_selection);
        std::wstring ignored;
        EnableWindow(GetDlgItem(window, save_id), PresetStore::valid_name(entered_name(), ignored));
    }
    void refresh(const std::wstring &selection = {}) {
        refreshing = true;
        SendMessageW(names, CB_RESETCONTENT, 0, 0);
        std::wstring error;
        const auto available = store->list(error);
        int selected_index = -1;
        for (const auto &value : available) {
            const auto index = SendMessageW(names, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
            if (same_name(value, selection))
                selected_index = int(index);
        }
        if (selected_index < 0 && !available.empty())
            selected_index = 0;
        SendMessageW(names, CB_SETCURSEL, selected_index, 0);
        if (selected_index >= 0)
            SetWindowTextW(name, selected().c_str());
        refreshing = false;
        enable_actions();
        if (!error.empty())
            feedback(error);
    }
    void save(bool overwrite) {
        const auto value = overwrite ? selected() : entered_name();
        std::wstring error;
        if (!PresetStore::valid_name(value, error)) {
            feedback(error);
            return;
        }
        if (!callbacks.capture) {
            feedback(L"The current configuration is not available to save.");
            return;
        }
        if (!store->save(value, callbacks.capture(), overwrite, error)) {
            feedback(error);
            return;
        }
        refresh(value);
        if (callbacks.list_changed)
            callbacks.list_changed();
        feedback((overwrite ? L"Updated \u201c" : L"Saved \u201c") + value +
                 L"\u201d with all settings, including render settings.");
    }
    void load() {
        const auto value = selected();
        std::wstring error;
        const auto bytes = store->read(value, error);
        if (!bytes) {
            feedback(error);
            return;
        }
        if (!callbacks.apply || !callbacks.apply(*bytes)) {
            feedback(L"Could not apply this preset. Check access to the preview settings folder.");
            return;
        }
        feedback(L"Loaded \u201c" + value +
                 L"\u201d. All settings restored. Changed render settings apply at the next game launch.");
    }
    void erase() {
        const auto value = selected();
        std::wstring error;
        if (!store->remove(value, error)) {
            feedback(error);
            return;
        }
        SetWindowTextW(name, L"");
        refresh();
        if (callbacks.list_changed)
            callbacks.list_changed();
        feedback(L"Deleted \u201c" + value + L"\u201d. Current preview settings are unchanged.");
    }
    void builtin(BuiltinPreset preset) {
        if (!callbacks.builtin)
            return;
        callbacks.builtin(preset);
        if (preset != BuiltinPreset::dan_suzuki)
            feedback(L"Camera tuning applied.");
    }
    void create() {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"HorizonLockPresets";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpfnWndProc = proc;
        RegisterClassW(&cls);
        RECT r{0, 0, 744, 582}, owner{};
        GetWindowRect(parent, &owner);
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
        window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"SteadyXR \u2014 Presets", style,
                                 owner.left + 40, owner.top + 40, r.right - r.left, r.bottom - r.top, parent,
                                 nullptr, cls.hInstance, this);
        if (!window)
            return;
        theme->titlebar(window);
        child(L"STATIC", L"Factory presets", 0, 24, 22, 696, 30, 0, true);
        child(L"BUTTON", L"Dan Suzuki", BS_PUSHBUTTON, 24, 73, 160, 39, dan_id);
        child(L"BUTTON", L"Responsive", BS_PUSHBUTTON, 202, 73, 160, 39, natural_id);
        child(L"BUTTON", L"Stream Comfort", BS_PUSHBUTTON, 380, 73, 160, 39, comfort_id);
        child(L"BUTTON", L"Strong leveling", BS_PUSHBUTTON, 558, 73, 162, 39, strong_id);
        child(L"STATIC", L"Dan Suzuki includes the saved camera, framing and 4K output settings.", 0, 24, 137, 696, 48);
        child(L"STATIC", L"YOUR COMPLETE PRESETS", 0, 24, 220, 696, 26, 0, true);
        child(L"STATIC",
              L"Save all settings, including source, output, eye profiles and extra render area.\r\n"
              L"Loaded render changes take effect at the next game launch.",
              0, 24, 255, 696, 48);
        child(L"STATIC", L"Saved preset", 0, 24, 311, 440, 24);
        names = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 24, 338, 456, 180, names_id);
        child(L"BUTTON", L"Load", BS_PUSHBUTTON, 504, 337, 96, 34, load_id);
        child(L"BUTTON", L"Delete", BS_PUSHBUTTON, 616, 337, 104, 34, delete_id);
        child(L"STATIC", L"Name for new preset", 0, 24, 389, 440, 24);
        name = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 24, 416, 312, 34, name_id);
        SendMessageW(name, EM_SETLIMITTEXT, 64, 0);
        child(L"BUTTON", L"Save new", BS_PUSHBUTTON, 352, 416, 128, 34, save_id);
        child(L"BUTTON", L"Overwrite selected", BS_PUSHBUTTON, 504, 416, 216, 34, overwrite_id);
        status = child(L"STATIC", L"Choose a saved preset, or enter a name to save your current settings.", 0,
                       24, 478, 696, 52, status_id);
        child(L"BUTTON", L"Close", BS_PUSHBUTTON, 616, 534, 104, 32, close_id);
        enable_actions();
    }
    static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, message, w, l);
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            const auto dc = message == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            RECT r{};
            GetClientRect(h, &r);
            SetDCBrushColor(dc, RGB(24, 31, 42));
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SetDCBrushColor(dc, RGB(53, 66, 82));
            RECT line{24, 203, 720, 204};
            FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (message == WM_PAINT)
                EndPaint(h, &ps);
            return 0;
        }
        LRESULT themed{};
        if (self->theme->message(h, message, w, l, themed))
            return themed;
        switch (message) {
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case names_id:
                if (HIWORD(w) == CBN_SELCHANGE) {
                    SetWindowTextW(self->name, self->selected().c_str());
                    self->enable_actions();
                }
                return 0;
            case name_id:
                if (HIWORD(w) == EN_CHANGE && !self->refreshing)
                    self->enable_actions();
                return 0;
            case save_id:
                self->save(false);
                return 0;
            case overwrite_id:
                self->save(true);
                return 0;
            case load_id:
                self->load();
                return 0;
            case delete_id:
                self->erase();
                return 0;
            case dan_id:
                self->builtin(BuiltinPreset::dan_suzuki);
                return 0;
            case natural_id:
                self->builtin(BuiltinPreset::natural);
                return 0;
            case comfort_id:
                self->builtin(BuiltinPreset::stream_comfort);
                return 0;
            case strong_id:
                self->builtin(BuiltinPreset::strong_leveling);
                return 0;
            case close_id:
            case IDCANCEL:
                ShowWindow(h, SW_HIDE);
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_NCDESTROY:
            self->window = nullptr;
            break;
        }
        return DefWindowProcW(h, message, w, l);
    }
};
PresetsDialog::PresetsDialog() : impl_(std::make_unique<Impl>()) {}
PresetsDialog::~PresetsDialog() {
    if (impl_->window)
        DestroyWindow(impl_->window);
}
void PresetsDialog::initialize(HWND parent, Theme &theme, const std::filesystem::path &directory,
                               Callbacks callbacks) {
    auto &s = *impl_;
    s.parent = parent;
    s.theme = &theme;
    s.store = std::make_unique<PresetStore>(directory);
    s.callbacks = std::move(callbacks);
}
void PresetsDialog::open(bool visible) {
    auto &s = *impl_;
    if (!s.theme || !s.store)
        return;
    if (!s.window)
        s.create();
    if (!s.window)
        return;
    s.refresh(s.selected());
    if (visible) {
        ShowWindow(s.window, SW_SHOW);
        SetForegroundWindow(s.window);
    }
}
HWND PresetsDialog::handle() const {
    return impl_->window;
}
bool PresetsDialog::route(MSG &msg) {
    return impl_->window && IsWindowVisible(impl_->window) && IsDialogMessageW(impl_->window, &msg);
}
} // namespace vrst::preview
