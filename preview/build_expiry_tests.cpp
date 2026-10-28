#include "build_expiry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <limits>

#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)

int main() {
    using namespace vrst::preview;
    // Neither the former 30-day deadline nor any later clock value may disable a build.
    const std::uint64_t timestamps[] = {
        0,
        build_unix_seconds,
        build_unix_seconds + 2591999,
        build_unix_seconds + 2592000,
        build_unix_seconds + 2592001,
        build_unix_seconds + 31536000,
        build_unix_seconds + 3155760000,
        std::numeric_limits<std::uint64_t>::max(),
    };
    for (const auto timestamp : timestamps) {
        CHECK(!build_expired_at(timestamp));
        test_clock::unix_seconds = timestamp;
        CHECK(!build_expired());
    }
    CHECK(expires_unix_seconds == 0);
    CHECK(std::wcscmp(build_expiry_text(), L"No expiry") == 0);
    std::puts("PASS: no expiry at 30 days, one year, 100 years, or the maximum clock value");
}
