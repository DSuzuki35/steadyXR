#include "build_expiry.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>

#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (0)

int main() {
    using namespace vrst::preview;
    // A deadline rounded to a calendar day or checked with > would violate these boundaries.
    CHECK(!build_expired_at(build_unix_seconds));
    CHECK(!build_expired_at(build_unix_seconds + 2591999));
    CHECK(build_expired_at(build_unix_seconds + 2592000));
    CHECK(build_expired_at(build_unix_seconds + 2592001));
    CHECK(expires_unix_seconds - build_unix_seconds == 2592000);
    CHECK(std::wcslen(build_expiry_text()) == 23);
    CHECK(std::wcscmp(build_expiry_text() + 19, L" UTC") == 0);
    const auto deadline = static_cast<std::time_t>(expires_unix_seconds);
    const auto *utc = std::gmtime(&deadline);
    CHECK(utc);
    wchar_t expected[32]{};
    CHECK(std::wcsftime(expected, 32, L"%Y-%m-%d %H:%M:%S UTC", utc));
    CHECK(std::wcscmp(build_expiry_text(), expected) == 0);
    test_clock::unix_seconds = expires_unix_seconds - 1;
    CHECK(!build_expired());
    test_clock::unix_seconds = expires_unix_seconds;
    CHECK(build_expired());
    std::puts("PASS: exact 30-day UTC boundary and current-clock expiry");
}
