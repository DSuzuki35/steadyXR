#pragma once
#include <cstdint>

namespace vrst::preview::detail {
// Completion on one device says nothing about work still queued on the other.
struct CaptureProgress {
    std::uint64_t capture_completed{}, read_completed{};
    bool capture_removed{}, reader_removed{};

    bool capture_finished(std::uint64_t required, bool untracked = false) const {
        return capture_removed || (!untracked &&
            (!required || (capture_completed != UINT64_MAX && required <= capture_completed)));
    }
    bool reads_finished(std::uint64_t required, bool untracked = false, bool pending = false) const {
        return reader_removed || (!untracked && !pending &&
            (!required || (read_completed != UINT64_MAX && required <= read_completed)));
    }
    bool finished(std::uint64_t capture, std::uint64_t read, bool untracked_capture = false,
                  bool untracked_read = false, bool pending_read = false) const {
        return capture_finished(capture, untracked_capture) &&
               reads_finished(read, untracked_read, pending_read);
    }
};
} // namespace vrst::preview::detail
