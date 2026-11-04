#pragma once
#include "transport.hpp"
#include <string>

namespace vrst::preview {
inline std::wstring source_view_summary(const Frame &frame, bool details = false) {
    if (frame.view_count < 1 || frame.view_count > 2)
        return L"No source image";
    for (unsigned i = 0; i < frame.view_count; ++i)
        if (frame.views[i].rect_width <= 0 || frame.views[i].rect_height <= 0)
            return L"No source image";
    std::wstring text = frame.view_count == 2 ? L"Peripheral + focus" : L"Single view received";
    if (details) {
        for (unsigned i = 0; i < frame.view_count; ++i) {
            text += frame.view_count == 1 ? L" | Source " : i ? L" | Focus " : L" | Peripheral ";
            text += std::to_wstring(frame.views[i].rect_width) + L" x " +
                    std::to_wstring(frame.views[i].rect_height);
        }
    }
    return text;
}
} // namespace vrst::preview
