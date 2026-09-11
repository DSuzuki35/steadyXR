#pragma once
#include "steadyxr_build_stamp.hpp"
#include <chrono>
#include <cstdint>
#ifdef VRST_PREVIEW_TEST_CLOCK
#include "build_expiry_test_clock.hpp"
#endif

namespace vrst::preview {
inline bool build_expired_at(std::uint64_t now) noexcept {
    return now >= expires_unix_seconds;
}
inline bool build_expired() noexcept {
#ifdef VRST_PREVIEW_TEST_CLOCK
    return build_expired_at(test_clock::unix_seconds);
#else
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return now >= 0 && build_expired_at(static_cast<std::uint64_t>(now));
#endif
}
inline const wchar_t *build_expiry_text() noexcept {
    return build_expiry_utc;
}
} // namespace vrst::preview
