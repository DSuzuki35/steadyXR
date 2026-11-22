// SPDX-License-Identifier: GPL-2.0-or-later
#include "../lifecycle.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool okay, const char *what) { if (!okay) throw std::runtime_error(what); }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        using steadyxr::obs_plugin::Lifecycle;
        int first{}, second{};
        Lifecycle manager;
        require(manager.update(&first, true, argv[1]), "start fixture");
        const DWORD pid = manager.owned_pid();
        require(pid != 0, "isolated fixture ownership (close SteadyXR before testing)");
        HANDLE owned = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        require(owned != nullptr, "owned fixture handle");
        require(manager.update(&second, true, argv[1]) && manager.owned_pid() == pid, "sources share one app");
        manager.update(&first, false, "");
        require(WaitForSingleObject(owned, 30) == WAIT_TIMEOUT, "first source cannot close second source's app");
        manager.update(&second, false, "");
        require(WaitForSingleObject(owned, 2500) == WAIT_OBJECT_0, "last source closes app gracefully");
        DWORD code{};
        require(GetExitCodeProcess(owned, &code) && code == 0, "fixture received --obs-output and normal WM_CLOSE");
        CloseHandle(owned);

        // Test the startup race: disable immediately after CreateProcess.
        require(manager.update(&first, true, argv[1]), "immediate fixture start");
        owned = OpenProcess(SYNCHRONIZE, FALSE, manager.owned_pid());
        require(owned != nullptr, "immediate fixture process");
        manager.shutdown();
        require(WaitForSingleObject(owned, 2500) == WAIT_OBJECT_0, "immediate shutdown closes owned GUI");
        CloseHandle(owned);

        const auto executable = steadyxr::obs_plugin::wide(argv[1]);
        std::wstring command = L"\"" + executable + L"\" --obs-output";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION manual{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &startup, &manual) != FALSE, "manually started fixture");
        CloseHandle(manual.hThread);
        WaitForInputIdle(manual.hProcess, 2000);
        require(manager.update(&first, true, argv[1]) && manager.owned_pid() == 0, "do not claim existing app");
        manager.shutdown();
        require(WaitForSingleObject(manual.hProcess, 50) == WAIT_TIMEOUT, "existing user's app remains running");
        EnumWindows([](HWND window, LPARAM param) -> BOOL {
            DWORD window_pid{}; GetWindowThreadProcessId(window, &window_pid);
            if (window_pid == static_cast<DWORD>(param)) PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }, static_cast<LPARAM>(manual.dwProcessId));
        require(WaitForSingleObject(manual.hProcess, 2500) == WAIT_OBJECT_0, "test fixture cleanup");
        CloseHandle(manual.hProcess);
        require(!manager.update(&first, true, "C:/nonexistent/SteadyXR.exe"), "missing app rejected");
        manager.shutdown();
        std::cout << "PASS: shared lifecycle, graceful last-source close, immediate shutdown, external app preserved, missing path\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
