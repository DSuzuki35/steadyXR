#pragma once
#include <cstdint>

// Compiled only into the isolated expiry/fake-runtime tests. Never linked into shipped binaries.
namespace vrst::preview::test_clock {
inline std::uint64_t unix_seconds = 1;
}
