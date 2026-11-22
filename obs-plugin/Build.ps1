# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$ObsRoot = 'C:/Program Files/obs-studio',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build-obs'),
    [string]$ObsSourceDirectory,
    [string]$ObsImportLibrary,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
function Check-Exit([string]$Step) { if ($LASTEXITCODE -ne 0) { throw "$Step failed ($LASTEXITCODE)" } }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$sdk = Join-Path $BuildDirectory 'sdk'
New-Item -ItemType Directory -Force -Path $sdk | Out-Null
if (!$ObsSourceDirectory) {
    $version = '32.2.1'
    $archive = Join-Path $sdk 'OBS-Studio-32.2.1-Sources.tar.gz'
    if (!(Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest "https://github.com/obsproject/obs-studio/releases/download/$version/OBS-Studio-$version-Sources.tar.gz" -OutFile $archive
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($archive))).Replace('-', '') }
    finally { $sha.Dispose() }
    if ($hash -ne '6A2532B1094BC51BC2FDEB1068D5C19CFE04216191A5B35C8707625401A80BF4') {
        throw 'Official OBS source archive SHA256 mismatch.'
    }
    & tar -xf $archive -C $sdk "obs-studio-$version-sources/libobs"
    Check-Exit 'Extract official OBS headers'
    $ObsSourceDirectory = Join-Path $sdk "obs-studio-$version-sources"
}
if (!$ObsImportLibrary) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio 2022 C++ Build Tools and Windows SDK are required.' }
    $toolset = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
    $tools = Join-Path $toolset.FullName 'bin/Hostx64/x64'
    $exports = & (Join-Path $tools 'dumpbin.exe') /nologo /exports (Join-Path $ObsRoot 'bin/64bit/obs.dll')
    Check-Exit 'Read installed libobs exports'
    $symbols = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)') { $Matches[1] } })
    if ($symbols.Count -lt 100) { throw 'No usable OBS DLL export table.' }
    @('LIBRARY obs.dll', 'EXPORTS') + $symbols | Set-Content -LiteralPath (Join-Path $sdk 'obs.def') -Encoding ascii
    $ObsImportLibrary = Join-Path $sdk 'obs.lib'
    & (Join-Path $tools 'lib.exe') /nologo /machine:x64 "/def:$sdk/obs.def" "/out:$ObsImportLibrary"
    Check-Exit 'Generate import library'
}
& cmake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 `
    "-DOBS_ROOT=$ObsRoot" "-DOBS_SOURCE_DIR=$ObsSourceDirectory" "-DOBS_IMPORT_LIB=$ObsImportLibrary"
Check-Exit 'Configure'
& cmake --build $BuildDirectory --config RelWithDebInfo
Check-Exit 'Build'
if (!$SkipTests) {
    & ctest --test-dir $BuildDirectory -C RelWithDebInfo --output-on-failure
    Check-Exit 'Tests'
}
& cmake --install $BuildDirectory --config RelWithDebInfo --prefix (Join-Path $BuildDirectory 'package/obs-plugin')
Check-Exit 'Stage plugin package'
