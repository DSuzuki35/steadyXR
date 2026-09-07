#include <cstdlib>
#include <cstring>

// Child process for the real .cmd launchers: preserve Windows crash exit codes
// without crashing SteamVR or relying on Windows Error Reporting timing.
int main(int argc, char **argv) {
    const char *code = std::getenv("VRST_TEST_EXIT");
    const bool expect_overlay = std::getenv("VRST_TEST_OVERLAY") != nullptr;
    const bool got_overlay = argc == 2 && std::strcmp(argv[1], "--overlay") == 0;
    if (expect_overlay != got_overlay || (!expect_overlay && argc != 1))
        return 99;
    return code ? static_cast<int>(std::strtol(code, nullptr, 10)) : 0;
}
