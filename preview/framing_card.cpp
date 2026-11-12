#include "framing_card.hpp"
#include "crop_geometry.hpp"

namespace vrst::preview {
namespace {
constexpr COLORREF frame_blue = RGB(89, 157, 255);
constexpr COLORREF corner_blue = RGB(156, 201, 255);

void fill(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void outline(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FrameRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void grid(HDC dc, RECT image, RECT crop) {
    fill(dc, image, RGB(23, 29, 37));
    fill(dc, crop, RGB(30, 42, 58));
    const LONG width = image.right - image.left, height = image.bottom - image.top;
    const LONG step = std::max(10L, std::min(width, height) / 10);
    const LONG cx = image.left + width / 2, cy = image.top + height / 2;
    const auto saved = SaveDC(dc);
    IntersectClipRect(dc, image.left, image.top, image.right, image.bottom);
    for (LONG x = cx - width / 2 / step * step; x < image.right; x += step)
        fill(dc, {x, image.top, x + 1, image.bottom}, RGB(43, 52, 64));
    for (LONG y = cy - height / 2 / step * step; y < image.bottom; y += step)
        fill(dc, {image.left, y, image.right, y + 1}, RGB(43, 52, 64));
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, RGB(51, 61, 74));
    for (int percent : {25, 40}) {
        const int radius = MulDiv(std::min(width, height), percent, 100);
        Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    }
    fill(dc, {cx - 4, cy, cx + 5, cy + 1}, RGB(125, 151, 180));
    fill(dc, {cx, cy - 4, cx + 1, cy + 5}, RGB(125, 151, 180));
    RestoreDC(dc, saved);
    outline(dc, image, RGB(60, 69, 80));
}
void guides(HDC dc, RECT crop) {
    const LONG w = crop.right - crop.left, h = crop.bottom - crop.top;
    // Skip fine guides on highly zoomed or tiny selected regions.
    if (w >= 32 && h >= 20) {
        for (int index : {1, 2}) {
            const LONG x = crop.left + MulDiv(w, index, 3);
            const LONG y = crop.top + MulDiv(h, index, 3);
            SetDCPenColor(dc, RGB(72, 102, 139));
            SelectObject(dc, GetStockObject(DC_PEN));
            MoveToEx(dc, x, crop.top + 1, nullptr);
            LineTo(dc, x, crop.bottom - 1);
            MoveToEx(dc, crop.left + 1, y, nullptr);
            LineTo(dc, crop.right - 1, y);
        }
    }
    outline(dc, crop, frame_blue);
    for (bool right : {false, true})
        for (bool bottom : {false, true}) {
            const LONG x = right ? crop.right - 1 : crop.left;
            const LONG y = bottom ? crop.bottom - 1 : crop.top;
            RECT grip{x - 3, y - 3, x + 4, y + 4};
            fill(dc, grip, corner_blue);
            outline(dc, grip, RGB(31, 54, 86));
        }
}
struct Bitmap {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    ~Bitmap() {
        clear();
    }
    void clear() {
        if (previous && previous != HGDI_ERROR)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previous = nullptr;
    }
    bool create(HDC target, int width, int height) {
        clear();
        dc = CreateCompatibleDC(target);
        bitmap = CreateCompatibleBitmap(target, width, height);
        if (!dc || !bitmap)
            return false;
        previous = SelectObject(dc, bitmap);
        return previous && previous != HGDI_ERROR;
    }
};
} // namespace

RECT framing_image_rect(RECT area, double aspect) {
    const LONG width = area.right - area.left, height = area.bottom - area.top;
    if (width <= 0 || height <= 0)
        return {};
    aspect = crop_detail::source_aspect(aspect);
    const double fit = std::min(double(width), height * aspect);
    const LONG w = std::max(1L, std::lround(fit)), h = std::max(1L, std::lround(fit / aspect));
    const LONG x = area.left + (width - w) / 2, y = area.top + (height - h) / 2;
    return {x, y, x + w, y + h};
}
RECT framing_canvas_image(RECT bounds, double aspect, bool dashboard) {
    if (dashboard && bounds.right - bounds.left > 16 && bounds.bottom - bounds.top > 16)
        InflateRect(&bounds, -6, -6);
    return framing_image_rect(bounds, aspect);
}

RECT framing_crop_rect(RECT image, double aspect, const Options &options, unsigned eye) {
    const auto r = crop_from_options(aspect, options, eye);
    const LONG w = image.right - image.left, h = image.bottom - image.top;
    RECT crop{image.left + std::lround(r.left * w), image.top + std::lround(r.top * h),
              image.left + std::lround(r.right * w), image.top + std::lround(r.bottom * h)};
    crop.left = std::min(crop.left, image.right - 1);
    crop.top = std::min(crop.top, image.bottom - 1);
    crop.right = std::max(crop.left + 1, crop.right);
    crop.bottom = std::max(crop.top + 1, crop.bottom);
    return crop;
}

struct FramingCard::Cache {
    Bitmap full, dim;
    std::uint64_t revision{};
    LONG width{}, height{};
    bool prepare(HDC dc, const FramingStill &still, std::uint64_t generation, LONG w, LONG h) {
        if (revision == generation && width == w && height == h)
            return true;
        revision = 0;
        if (!full.create(dc, w, h) || !dim.create(dc, w, h))
            return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = still.width;
        info.bmiHeader.biHeight = -LONG(still.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(full.dc, HALFTONE);
        SetBrushOrgEx(full.dc, 0, 0, nullptr);
        if (StretchDIBits(full.dc, 0, 0, w, h, 0, 0, still.width, still.height, still.pixels.get(), &info,
                          DIB_RGB_COLORS, SRCCOPY) <= 0)
            return false;
        fill(dim.dc, {0, 0, w, h}, RGB(12, 16, 22));
        if (!AlphaBlend(dim.dc, 0, 0, w, h, full.dc, 0, 0, w, h, {AC_SRC_OVER, 0, 108, 0}))
            return false;
        width = w;
        height = h;
        revision = generation;
        return true;
    }
};
FramingCard::FramingCard() : cache_(new Cache) {}
FramingCard::~FramingCard() = default;
void FramingCard::paint(HDC dc, RECT bounds, const FramingStill &still, std::uint64_t revision,
                        const Options &options, double aspect, unsigned eye, bool dashboard) {
    if (!dc || IsRectEmpty(&bounds))
        return;
    const auto saved = SaveDC(dc);
    IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    fill(dc, bounds, RGB(15, 18, 23));
    if (still.pixels) {
        aspect = still.source_aspect;
        eye = still.eye;
    } else
        aspect = crop_detail::source_aspect(aspect);
    const RECT image = framing_canvas_image(bounds, aspect, dashboard);
    const RECT crop = framing_crop_rect(image, aspect, options, eye);
    const LONG w = image.right - image.left, h = image.bottom - image.top;
    POINT px[]{{image.left, image.top}, {image.right, image.bottom}};
    LPtoDP(dc, px, 2);
    const LONG pw = std::max(1L, std::abs(px[1].x - px[0].x));
    const LONG ph = std::max(1L, std::abs(px[1].y - px[0].y));
    if (still.pixels && cache_->prepare(dc, still, revision, pw, ph)) {
        // A second blit from the bright version fills the selected crop. Reuse
        // exactly the same image transform for both passes, including fractional DPI.
        StretchBlt(dc, image.left, image.top, w, h, cache_->dim.dc, 0, 0, pw, ph, SRCCOPY);
        const int cropped = SaveDC(dc);
        IntersectClipRect(dc, crop.left, crop.top, crop.right, crop.bottom);
        StretchBlt(dc, image.left, image.top, w, h, cache_->full.dc, 0, 0, pw, ph, SRCCOPY);
        RestoreDC(dc, cropped);
    } else
        grid(dc, image, crop);
    guides(dc, crop);
    if (dashboard)
        outline(dc, bounds, RGB(53, 60, 69));
    RestoreDC(dc, saved);
}
} // namespace vrst::preview
