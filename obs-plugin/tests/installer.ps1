# SPDX-License-Identifier: GPL-2.0-or-later
param([string]$SourceDirectory, [string]$PluginDll, [string]$FixtureExe, [string]$ObsRoot, [string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
function File-Hash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToBase64String($sha.ComputeHash([IO.File]::ReadAllBytes($Path))) }
    finally { $sha.Dispose() }
}
$root = Join-Path $BuildDirectory ('installer-test-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package/obs-plugin'
$target = Join-Path $root "OBS folder's portable copy"
New-Item -ItemType Directory -Force -Path (Join-Path $package 'bin/64bit'), (Join-Path $package 'data/locale'),
    (Join-Path $target 'bin/64bit'), (Join-Path $target 'data/obs-studio') | Out-Null
Copy-Item -LiteralPath $PluginDll -Destination (Join-Path $package 'bin/64bit/steadyxr-obs.dll')
foreach ($file in @('Install-OBS-Integration.ps1', 'LICENSE', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $SourceDirectory $file) -Destination (Join-Path $package $file)
}
foreach ($locale in @('en-US', 'de-DE')) {
    Copy-Item -LiteralPath (Join-Path $SourceDirectory "data/locale/$locale.ini") -Destination (Join-Path $package "data/locale/$locale.ini")
}
foreach ($name in @('obs64.exe', 'obs.dll')) {
    Copy-Item -LiteralPath (Join-Path $ObsRoot "bin/64bit/$name") -Destination (Join-Path $target "bin/64bit/$name")
}
$sentinel = Join-Path $target 'user-settings-sentinel.txt'
[IO.File]::WriteAllText($sentinel, 'untouched')
$shell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$installer = Join-Path $package 'Install-OBS-Integration.ps1'
& $shell -NoProfile -ExecutionPolicy Bypass -File $installer -Quiet -TargetDirectory $target -ApplicationPath $FixtureExe
if ($LASTEXITCODE -ne 0) { throw 'Portable install failed' }
$installed = Join-Path $target 'obs-plugins/64bit/steadyxr-obs.dll'
if ((File-Hash $installed) -ne (File-Hash $PluginDll)) { throw 'Installed DLL differs' }
$savedPath = [IO.File]::ReadAllText((Join-Path $target 'data/obs-plugins/steadyxr-obs/app-path.txt')).Trim()
if ($savedPath -ne [IO.Path]::GetFullPath($FixtureExe)) { throw 'Autostart path was not saved correctly' }
& $shell -NoProfile -ExecutionPolicy Bypass -File $installer -Quiet -TargetDirectory $target -ApplicationPath $FixtureExe
if ($LASTEXITCODE -ne 0) { throw 'Update install failed' }
if (@(Get-ChildItem -LiteralPath (Join-Path $target 'data/obs-plugins/steadyxr-obs') -Filter '*.backup').Count -ne 1) {
    throw 'Existing plugin was not preserved'
}
if ([IO.File]::ReadAllText($sentinel) -ne 'untouched') { throw 'Unrelated settings changed' }
$ErrorActionPreference = 'Continue'
& $shell -NoProfile -ExecutionPolicy Bypass -File $installer -Quiet -TargetDirectory $root -ApplicationPath $FixtureExe *> (Join-Path $root 'expected-invalid-folder.log')
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -eq 0) { throw 'Invalid OBS folder accepted' }
Write-Output 'PASS: portable installation, apostrophe/spaces in paths, app path, update backup, unrelated files preserved, invalid directory rejected'
