param([string]$BuildDirectory = 'build-windows-msvc')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'Run this script in the x64 Native Tools / Developer PowerShell for Visual Studio 2022 with Desktop development with C++ installed.' }
    & python scripts/prepare-deps.py
    if ($LASTEXITCODE -ne 0) { throw 'Dependency preparation failed' }
    $paths = Get-Content .deps/paths.json -Raw | ConvertFrom-Json
    $configure = @('-S', '.', '-B', $BuildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DVRST_BUILD_PLUGIN=ON', '-DVRST_BUILD_SENDER=ON', '-DVRST_BUILD_TESTS=ON', "-DVRST_OBS_SOURCE_DIR=$($paths.obs_source)", "-DVRST_OBS_LIBRARY=$($paths.obs_library)", "-DVRST_OBS_GENERATED_DIR=$($paths.obs_generated)", "-DVRST_OPENVR_DIR=$($paths.openvr)")
    $configure += @('-DVRST_BUILD_PREVIEW=ON', "-DVRST_OPENXR_DIR=$($paths.openxr)")
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    & cmake --build $BuildDirectory --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    & ctest --test-dir $BuildDirectory -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    & python scripts/package.py --build $BuildDirectory --openvr $paths.openvr --openxr $paths.openxr
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
} finally { Pop-Location }
