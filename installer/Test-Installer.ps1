[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer,
      [Parameter(Mandatory=$true)][string]$PayloadDirectory,
      [Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$release = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release.json') -Raw | ConvertFrom-Json
$root = Join-Path $BuildDirectory "isolated install's folder"
$versionDirectory = Join-Path $root "SteadyXR-$($release.version)"
$launcher = Join-Path $root 'Start-Preview.cmd'
$testRegistry = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/SteadyXR-Installer-Isolated-Test_is1'
if (Test-Path -LiteralPath $testRegistry) { throw 'An earlier isolated installer test is still registered. Preserve and inspect it before retrying.' }
function Invoke-Setup([string]$Label, [bool]$Icons = $false) {
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', ('/DIR="' + $root + '"'),
                   ('/LOG="' + (Join-Path $BuildDirectory "$Label.log") + '"'))
    if ($Icons) { $arguments += '/TASKS=desktopicon' } else { $arguments += '/NOICONS' }
    $process = Start-Process -FilePath $Installer -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "$Label failed ($($process.ExitCode))." }
}
function Assert-Publisher([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $release.publisher_thumbprint -or
        !$signature.TimeStamperCertificate) { throw "Invalid signed executable/helper: $Path" }
}
function Launcher-Text([string]$Version) { return "@echo off`r`nstart `"`" `"%~dp0SteadyXR-$Version\SteadyXR.exe`"`r`n" }
function Write-Launcher([string]$Text) { [IO.File]::WriteAllText($launcher, $Text, [Text.Encoding]::ASCII) }
Assert-Publisher $Installer
# The test build uses this separate mutex, never the live application's mutex.
$mutex = New-Object Threading.Mutex($false, 'Local\SteadyXR-Installer-Isolated-Test-App')
try {
    $blocked = Start-Process -FilePath $Installer -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOICONS',
        ('/DIR="' + $root + '"'), ('/LOG="' + (Join-Path $BuildDirectory 'mutex-blocked.log') + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($blocked.ExitCode -eq 0) { throw 'Running application mutex did not block setup.' }
} finally { $mutex.Dispose() }
$settings = @('SteadyXR.ini', 'SteadyXR-ui.ini', 'SteadyXR-shortcuts.ini', 'preview.ini')
$oldDirectory = Join-Path $root 'SteadyXR-1.0.4'
New-Item -ItemType Directory -Force -Path $oldDirectory | Out-Null
foreach ($name in $settings) { [IO.File]::WriteAllText((Join-Path $oldDirectory $name), 'old version setting=' + $name) }
Write-Launcher (Launcher-Text '1.0.4')
Invoke-Setup 'install'
foreach ($file in Get-ChildItem -LiteralPath $PayloadDirectory -File -Recurse) {
    $relative = $file.FullName.Substring($PayloadDirectory.Length + 1)
    $installedFile = Join-Path $versionDirectory $relative
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $installedFile).Hash) {
        throw "Installed payload differs: $relative"
    }
}
foreach ($relative in @('SteadyXR.exe', 'SteadyXR-OpenXR.dll', 'SteadyXR-Update.ps1',
                       'obs-plugin/bin/64bit/steadyxr-obs.dll', 'obs-plugin/Install-OBS-Integration.ps1')) {
    Assert-Publisher (Join-Path $versionDirectory $relative)
}
$uninstaller = Join-Path $root 'uninstall/unins000.exe'
Assert-Publisher $uninstaller
if ([IO.File]::ReadAllText($launcher) -cne (Launcher-Text $release.version)) { throw 'Initial stable launcher differs from updater format.' }
foreach ($name in $settings) {
    if ([IO.File]::ReadAllText((Join-Path $versionDirectory $name)) -cne ('old version setting=' + $name)) {
        throw "Managed upgrade did not migrate settings: $name"
    }
}
foreach ($name in $settings) { [IO.File]::WriteAllText((Join-Path $versionDirectory $name), '[UserSettings] custom=' + $name) }
New-Item -ItemType Directory -Force -Path (Join-Path $root 'Presets'), (Join-Path $root 'SteadyXR-1.2.0') | Out-Null
$preset = Join-Path $root 'Presets/My settings.ini'
$future = Join-Path $root 'SteadyXR-1.2.0/user-created-version.txt'
[IO.File]::WriteAllText($preset, 'custom preset')
[IO.File]::WriteAllText($future, 'later portable update')
Write-Launcher (Launcher-Text '1.0.4')
Invoke-Setup 'reinstall-and-upgrade' $true
if ([IO.File]::ReadAllText($launcher) -cne (Launcher-Text $release.version)) { throw 'Old launcher did not upgrade.' }
$shell = New-Object -ComObject WScript.Shell
foreach ($name in @('SteadyXR.lnk', 'SteadyXR Desktop.lnk')) {
    $shortcutPath = Join-Path $root "test-shortcuts/$name"
    if (!(Test-Path -LiteralPath $shortcutPath)) { throw "Missing isolated shortcut $name" }
    $shortcut = $shell.CreateShortcut($shortcutPath)
    if ($shortcut.TargetPath -ine $launcher -or $shortcut.WorkingDirectory -ine $root -or
        $shortcut.IconLocation.Split(',')[0] -ine (Join-Path $root 'SteadyXR.ico')) { throw "Unstable shortcut $name" }
}
Write-Launcher (Launcher-Text '1.2.0')
Invoke-Setup 'preserve-newer-launcher'
if ([IO.File]::ReadAllText($launcher) -cne (Launcher-Text '1.2.0')) { throw 'Older installer reset the newer launcher.' }
$custom = "@echo off`r`necho Custom launcher`r`n"
Write-Launcher $custom
Invoke-Setup 'preserve-custom-launcher'
if ([IO.File]::ReadAllText($launcher) -cne $custom) { throw 'Custom launcher was overwritten.' }
foreach ($name in $settings) {
    if ([IO.File]::ReadAllText((Join-Path $versionDirectory $name)) -cne ('[UserSettings] custom=' + $name)) {
        throw "Reinstall changed user settings: $name"
    }
}
Assert-Publisher $uninstaller
$process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    ('/LOG="' + (Join-Path $BuildDirectory 'uninstall.log') + '"')) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Uninstall failed ($($process.ExitCode))." }
$deadline = [DateTime]::UtcNow.AddSeconds(10)
while ((Test-Path -LiteralPath $uninstaller) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
foreach ($name in $settings) {
    if ([IO.File]::ReadAllText((Join-Path $versionDirectory $name)) -cne ('[UserSettings] custom=' + $name)) {
        throw "Uninstall changed user settings: $name"
    }
}
if ([IO.File]::ReadAllText($preset) -cne 'custom preset' -or [IO.File]::ReadAllText($future) -cne 'later portable update') {
    throw 'Uninstall changed presets or future update contents.'
}
foreach ($removed in @((Join-Path $versionDirectory 'SteadyXR.exe'), $launcher, (Join-Path $root 'test-shortcuts/SteadyXR.lnk'),
                       (Join-Path $root 'test-shortcuts/SteadyXR Desktop.lnk'), $testRegistry)) {
    if (Test-Path -LiteralPath $removed) { throw "Installer-owned item remains after uninstall: $removed" }
}
Write-Output 'PASS: running-app mutex, signed installer/uninstaller, byte-identical signed payload, stable shortcuts, older/newer/custom launchers, reinstall settings, uninstall preservation'
