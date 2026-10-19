#pragma once
#include "overlay_settings.hpp"
#include "theme.hpp"
#include <functional>
#include <vector>

namespace vrst::preview {
class OverlayPanel {
  public:
    HWND window{};
    Theme *theme{};
    Channel *channel{};
    OverlaySelection *selection{};
    std::function<void()> changed;
    OverlayCatalog shown{};
    std::vector<HWND> rows;
    void refresh() {
        for (auto h : rows)
            DestroyWindow(h);
        rows.clear();
        shown = channel->overlay_catalog();
        const bool live = overlay_catalog_live(shown);
        const bool scope = overlay_scope_matches(*selection, shown);
        std::wstring help = L"OpenXR capture only. Keep the source active, then Refresh.\nUpdate the OpenXR "
                            L"connection and restart the game for 0.10.0 (IPC 4).\nLayer order: game > "
                            L"overlay injector > SteadyXR > runtime.";
        if (selection->mode == 2 && !scope)
            help += L"\nSaved selection belongs to another game session. Reselect layers.";
        else if (!live || !shown.count)
            help += L"\nNo live layers. Check source activity and the layer order above.";
        else
            help += selection->mode == 1   ? L"\nAll supported layers, including newly appearing layers."
                    : selection->mode == 0 ? L"\nAdditional layers are disabled."
                                           : L"\nSelected layers apply only to this game session.";
        SetDlgItemTextW(window, 4, help.c_str());
        for (unsigned i = 0; i < std::min(shown.count, max_overlay_layers); ++i) {
            const auto &item = shown.layers[i];
            if (item.slot >= max_overlay_layers)
                continue;
            std::wstring label(item.label, wcsnlen(item.label, 128));
            if (!item.supported)
                label += L" (unsupported)";
            auto h = add(L"BUTTON", label.c_str(), BS_AUTOCHECKBOX, 18, 170 + int(rows.size()) * 25, 655, 24,
                         100 + item.slot);
            SendMessageW(h, BM_SETCHECK,
                         item.supported && (selection->mode == 1 || (scope && selection->mode == 2 &&
                                                                     (selection->mask & (1u << item.slot))))
                             ? BST_CHECKED
                             : BST_UNCHECKED,
                         0);
            EnableWindow(h, live && item.supported);
            rows.push_back(h);
        }
    }
    HWND add(const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
        auto child = CreateWindowExW(
            0, kind, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | theme->style(kind, style), x, y, w, h, window,
            reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        theme->control(child, kind);
        return child;
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto *p = reinterpret_cast<OverlayPanel *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = static_cast<OverlayPanel *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            p->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(p));
        }
        if (!p)
            return DefWindowProcW(h, m, w, l);
        if (m == WM_PAINT || m == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            auto dc = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(w);
            RECT r{};
            GetClientRect(h, &r);
            SetDCBrushColor(dc, RGB(24, 31, 42));
            FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (m == WM_PAINT)
                EndPaint(h, &ps);
            return 0;
        }
        LRESULT result{};
        if (p->theme->message(h, m, w, l, result))
            return result;
        if (m == WM_CLOSE) {
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        if (m == WM_COMMAND) {
            const auto id = LOWORD(w);
            if (id == 3) {
                p->refresh();
                return 0;
            }
            if (id == 1 || id == 2) {
                *p->selection = {};
                p->selection->mode = id == 1 ? 1 : 0;
                p->changed();
                p->refresh();
                return 0;
            }
            if (id >= 100 && id < 100 + max_overlay_layers) {
                const auto current = p->channel->overlay_catalog();
                if (!overlay_catalog_live(current) || current.producer_pid != p->shown.producer_pid ||
                    current.session != p->shown.session) {
                    p->refresh();
                    return 0;
                }
                const unsigned slot = id - 100;
                bool supported = false;
                for (unsigned i = 0; i < std::min(current.count, max_overlay_layers); ++i)
                    if (current.layers[i].slot == slot)
                        supported = current.layers[i].supported != 0;
                if (!supported) {
                    p->refresh();
                    return 0;
                }
                // Preserve session-scoped selections for temporarily absent layers.
                // An edit from All materializes the current supported inventory.
                std::uint32_t mask = p->selection->mode == 2 && overlay_scope_matches(*p->selection, current)
                                         ? p->selection->mask
                                         : 0;
                if (p->selection->mode == 1)
                    for (unsigned i = 0; i < std::min(current.count, max_overlay_layers); ++i)
                        if (current.layers[i].supported && current.layers[i].slot < max_overlay_layers)
                            mask |= 1u << current.layers[i].slot;
                if (SendDlgItemMessageW(h, id, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    mask |= 1u << slot;
                else
                    mask &= ~(1u << slot);
                *p->selection = {2, mask, current.producer_pid, current.session};
                p->changed();
                p->refresh();
                return 0;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    void open(HWND owner, Theme &t, Channel &c, OverlaySelection &s, std::function<void()> callback) {
        theme = &t;
        channel = &c;
        selection = &s;
        changed = std::move(callback);
        if (!IsWindow(window)) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"SteadyXROverlaySelection";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Additional OpenXR layers",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                     710, 640, owner, nullptr, wc.hInstance, this);
            theme->titlebar(window);
            add(L"BUTTON", L"All supported", BS_PUSHBUTTON, 18, 15, 150, 32, 1);
            add(L"BUTTON", L"None", BS_PUSHBUTTON, 180, 15, 110, 32, 2);
            add(L"BUTTON", L"Refresh layers", BS_PUSHBUTTON, 302, 15, 160, 32, 3);
            add(L"STATIC", L"", 0, 18, 60, 660, 104, 4);
        }
        refresh();
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
    bool route(MSG &msg) {
        return IsWindowVisible(window) && IsDialogMessageW(window, &msg);
    }
};
} // namespace vrst::preview
