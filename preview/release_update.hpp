#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace vrst::preview {
// The update helper is shipped only in a signed distribution. The bootstrap
// pins its publisher before executing it. No download runs on the UI thread.
inline void check_for_release_update() {
    wchar_t module[32768]{}, system[MAX_PATH]{};
    const auto count = GetModuleFileNameW(nullptr, module, 32768);
    if (!count || count >= 32768 || !GetSystemDirectoryW(system, MAX_PATH))
        return;
    const auto directory = std::filesystem::path(module).parent_path();
    if (GetFileAttributesW((directory / L"SteadyXR-Update.ps1").c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    // Only a numeric process ID is interpolated. Paths are supplied via
    // CreateProcess's working-directory argument, never as PowerShell code.
    auto command = L"\"" + powershell.wstring() +
        L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
        L"$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules');"
        L"Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', 'Microsoft.PowerShell.Security', "
        L"'Microsoft.PowerShell.Security.psd1')) -ErrorAction Stop;"
        L"$p = Join-Path (Get-Location).Path 'SteadyXR-Update.ps1';"
        L"$lock = [IO.File]::Open($p, 'Open', 'Read', 'Read');"
        L"try { $s = Get-AuthenticodeSignature -LiteralPath $p;"
        L"if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Thumbprint -eq "
        L"'89F7F00A4FD4BD66F5617F1202B8E7B9DD662247') { & $p -ViewerPid " +
        std::to_wstring(GetCurrentProcessId()) + L" } } finally { $lock.Dispose() }\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, directory.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
} // namespace vrst::preview
