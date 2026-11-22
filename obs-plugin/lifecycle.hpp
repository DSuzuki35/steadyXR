// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <map>
#include <mutex>
#include <string>
#include <cwchar>

namespace steadyxr::obs_plugin {
inline std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!n) return {};
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), n);
    return result;
}

// One coordinator per loaded module, shared by all sources and scenes. It only
// owns processes it created itself. Closing means posting WM_CLOSE to the app's
// controls window; never terminate a process or close the capture/preview window.
class Lifecycle {
    std::mutex mutex_;
    std::map<void *, std::string> clients_;
    HANDLE owned_{};
    DWORD owned_pid_{};
    static bool running() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return true; // Cannot prove absent: do not launch a duplicate.
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Process32FirstW(snapshot, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"SteadyXR.exe") == 0) { found = true; break; }
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
        return found;
    }
    static BOOL CALLBACK close_window(HWND window, LPARAM param) {
        DWORD pid{};
        GetWindowThreadProcessId(window, &pid);
        if (pid == static_cast<DWORD>(param)) {
            wchar_t name[128]{};
            GetClassNameW(window, name, 128);
            if (std::wcscmp(name, L"HorizonLockControls") == 0) PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }
    void close_owned() {
        if (!owned_) return;
        if (WaitForSingleObject(owned_, 0) == WAIT_TIMEOUT) {
            // A source can be removed immediately after enabling it. Give the
            // newly launched GUI a bounded chance to create its controls window.
            WaitForInputIdle(owned_, 2000);
            EnumWindows(close_window, static_cast<LPARAM>(owned_pid_));
        }
        CloseHandle(owned_);
        owned_ = nullptr;
        owned_pid_ = 0;
    }
    bool launch(const std::string &path) {
        if (owned_ && WaitForSingleObject(owned_, 0) != WAIT_TIMEOUT) close_owned();
        if (owned_ || running()) return true;
        const auto executable = wide(path);
        const auto separator = executable.find_last_of(L"/\\");
        const auto leaf = executable.substr(separator == std::wstring::npos ? 0 : separator + 1);
        if (leaf.empty() || _wcsicmp(leaf.c_str(), L"SteadyXR.exe") != 0 ||
            GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        std::wstring command = L"\"" + executable + L"\" --obs-output";
        const auto folder = separator == std::wstring::npos ? std::wstring{} : executable.substr(0, separator);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            folder.empty() ? nullptr : folder.c_str(), &startup, &process)) return false;
        CloseHandle(process.hThread);
        owned_ = process.hProcess;
        owned_pid_ = process.dwProcessId;
        return true;
    }
  public:
    ~Lifecycle() { shutdown(); }
    bool update(void *client, bool enabled, const std::string &path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled) {
            const auto previous = clients_.find(client);
            const bool changed = previous == clients_.end() || previous->second != path;
            clients_[client] = path;
            return !changed || launch(path);
        }
        clients_.erase(client);
        if (clients_.empty()) close_owned();
        return true;
    }
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.clear();
        close_owned();
    }
    DWORD owned_pid() { std::lock_guard<std::mutex> lock(mutex_); return owned_pid_; }
};
} // namespace steadyxr::obs_plugin
