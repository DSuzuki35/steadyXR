param(
    [string]$BuildDirectory = 'build-windows-msvc',
    [switch]$SoftwareGpu
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $configure = @('-S', '.', '-B', $BuildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64',
        '-DVRST_BUILD_PREVIEW=ON', '-DVRST_BUILD_TESTS=ON')
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
    & cmake --build $BuildDirectory --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    $testArguments = @('--test-dir', $BuildDirectory, '-C', 'Release', '--output-on-failure')
    if ($SoftwareGpu) {
        # The separate WARP test exercises D3D12 capture on hosted runners.
        $testArguments += @('-LE', 'physical-gpu')
    }
    & ctest @testArguments
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} finally { Pop-Location }
