# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [string]$TargetDirectory,
    [string]$ApplicationPath,
    [switch]$Elevated,
    [switch]$Quiet
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-ObsStopped {
    if (Get-Process -Name obs64,obs32 -ErrorAction SilentlyContinue) {
        throw 'Close OBS Studio before installing the SteadyXR integration, then try again. No running application was stopped.'
    }
}
function Assert-OrdinaryPath([string]$Path) {
    # Reject junctions/symlinks anywhere in the selected installation path. File
    # replacement must stay inside the directory the user explicitly selected.
    $itemPath = [IO.Path]::GetFullPath($Path)
    while ($itemPath) {
        if (Test-Path -LiteralPath $itemPath) {
            $item = Get-Item -LiteralPath $itemPath -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Installation path contains a link or junction: $itemPath"
            }
        }
        $parent = Split-Path -Path $itemPath -Parent
        if ($parent -eq $itemPath) { break }
        $itemPath = $parent
    }
}
function Quote-PowerShellLiteral([string]$Value) { return "'" + $Value.Replace("'", "''") + "'" }

try {
    if (!$Quiet) { Add-Type -AssemblyName System.Windows.Forms }
    Assert-ObsStopped
    $payloadDll = Join-Path $PSScriptRoot 'bin/64bit/steadyxr-obs.dll'
    if (!(Test-Path -LiteralPath $payloadDll -PathType Leaf)) { throw 'The packaged OBS plugin DLL is missing. Extract the complete SteadyXR ZIP before installing.' }
    if (!$ApplicationPath) { $ApplicationPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'SteadyXR.exe' }
    $ApplicationPath = [IO.Path]::GetFullPath($ApplicationPath)
    if (!(Test-Path -LiteralPath $ApplicationPath -PathType Leaf) -or [IO.Path]::GetFileName($ApplicationPath) -ine 'SteadyXR.exe') {
        throw 'SteadyXR.exe was not found beside the obs-plugin folder. Extract the complete package first.'
    }
    if (!$TargetDirectory) {
        if ($Quiet) { throw '-Quiet requires -TargetDirectory.' }
        $picker = New-Object System.Windows.Forms.FolderBrowserDialog
        $picker.Description = 'Select your OBS Studio installation folder (the folder containing bin, data and obs-plugins).'
        $picker.ShowNewFolderButton = $false
        $standard = Join-Path $env:ProgramFiles 'obs-studio'
        $picker.SelectedPath = if (Test-Path -LiteralPath $standard) { $standard } else { $env:ProgramFiles }
        try {
            if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
            $TargetDirectory = $picker.SelectedPath
        } finally { $picker.Dispose() }
    }
    $TargetDirectory = [IO.Path]::GetFullPath($TargetDirectory).TrimEnd('\', '/')
    Assert-OrdinaryPath $TargetDirectory
    $obsExe = Join-Path $TargetDirectory 'bin/64bit/obs64.exe'
    $obsDll = Join-Path $TargetDirectory 'bin/64bit/obs.dll'
    if (!(Test-Path -LiteralPath $obsExe -PathType Leaf) -or !(Test-Path -LiteralPath $obsDll -PathType Leaf) -or
        !(Test-Path -LiteralPath (Join-Path $TargetDirectory 'data/obs-studio') -PathType Container)) {
        throw 'This is not an OBS Studio Windows x64 installation. Select the OBS root folder containing bin/64bit/obs64.exe and data/obs-studio.'
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($obsDll)
    if ($version.FileMajorPart -lt 32 -or ($version.FileMajorPart -eq 32 -and $version.FileMinorPart -lt 2)) {
        throw 'This plugin requires OBS Studio 32.2 or newer (Windows x64).'
    }
    $dllDirectory = Join-Path $TargetDirectory 'obs-plugins/64bit'
    $dataDirectory = Join-Path $TargetDirectory 'data/obs-plugins/steadyxr-obs'
    foreach ($destination in @($dllDirectory, $dataDirectory, (Join-Path $dataDirectory 'locale'))) { Assert-OrdinaryPath $destination }
    # Test access before requesting elevation. Portable installs need no UAC.
    $probe = Join-Path $TargetDirectory ('.steadyxr-write-check-' + [Guid]::NewGuid().ToString('N'))
    $canWrite = $false
    try {
        [IO.File]::WriteAllText($probe, '')
        Remove-Item -LiteralPath $probe
        $canWrite = $true
    } catch [UnauthorizedAccessException] { }
    if (!$canWrite) {
        if ($Elevated -or $Quiet) { throw 'The selected OBS installation is not writable. Run the installer with administrator permission.' }
        $command = '& ' + (Quote-PowerShellLiteral $PSCommandPath) +
            ' -TargetDirectory ' + (Quote-PowerShellLiteral $TargetDirectory) +
            ' -ApplicationPath ' + (Quote-PowerShellLiteral $ApplicationPath) + ' -Elevated'
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
        $child = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe') `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded) `
            -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        exit $child.ExitCode
    }
    Assert-ObsStopped
    $files = @(
        @{ From = $payloadDll; To = (Join-Path $dllDirectory 'steadyxr-obs.dll') },
        @{ From = (Join-Path $PSScriptRoot 'data/locale/en-US.ini'); To = (Join-Path $dataDirectory 'locale/en-US.ini') },
        @{ From = (Join-Path $PSScriptRoot 'data/locale/de-DE.ini'); To = (Join-Path $dataDirectory 'locale/de-DE.ini') },
        @{ From = (Join-Path $PSScriptRoot 'LICENSE'); To = (Join-Path $dataDirectory 'LICENSE') },
        @{ From = (Join-Path $PSScriptRoot 'README.md'); To = (Join-Path $dataDirectory 'README.md') }
    )
    foreach ($file in $files) {
        if (!(Test-Path -LiteralPath $file.From -PathType Leaf)) { throw "Incomplete plugin package: $($file.From)" }
        Assert-OrdinaryPath $file.To
    }
    Assert-OrdinaryPath (Join-Path $dataDirectory 'app-path.txt')
    New-Item -ItemType Directory -Force -Path $dllDirectory, (Join-Path $dataDirectory 'locale') | Out-Null
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fffffff')
    # Preserve the previous DLL for manual rollback; never delete other plugins.
    $installedDll = Join-Path $dllDirectory 'steadyxr-obs.dll'
    if (Test-Path -LiteralPath $installedDll) {
        Copy-Item -LiteralPath $installedDll -Destination (Join-Path $dataDirectory "steadyxr-obs-$stamp.dll.backup")
    }
    foreach ($file in $files) {
        $stage = $file.To + '.' + $stamp + '.tmp'
        Copy-Item -LiteralPath $file.From -Destination $stage
        Move-Item -LiteralPath $stage -Destination $file.To -Force
    }
    [IO.File]::WriteAllText((Join-Path $dataDirectory 'app-path.txt'), $ApplicationPath + [Environment]::NewLine,
                           (New-Object Text.UTF8Encoding($false)))
    $message = "SteadyXR OBS integration installed in:`n$TargetDirectory`n`nStart OBS, add a SteadyXR source, then select Direct OBS output plugin in SteadyXR. The source properties also offer automatic start/close."
    if ($Quiet) { Write-Output $message }
    else { [System.Windows.Forms.MessageBox]::Show($message, 'SteadyXR OBS integration', 'OK', 'Information') | Out-Null }
} catch {
    if ($Quiet) { Write-Error $_ -ErrorAction Continue }
    else { [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'SteadyXR OBS integration', 'OK', 'Error') | Out-Null }
    exit 1
}
