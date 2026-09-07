param(
    [string]$ObsDirectory = 'C:\Program Files\obs-studio',
    [string]$PluginPath = 'build-windows/plugin/vr-stabilizer.dll',
    [string]$BuildDirectory = 'build-module-windows',
    [string]$DependenciesDirectory = '.deps',
    [string]$ObsSourceDirectory = $env:VRST_NATIVE_OBS_SOURCE_DIR,
    [string]$ObsGeneratedDirectory = $env:VRST_OBS_GENERATED_DIR,
    [string]$ObsLibrary = $env:VRST_OBS_LIBRARY,
    [string]$CoreLibrary = '',
    [string]$Compiler = $env:CXX
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
$previousPath = $env:PATH
try {
    if (-not $Compiler) {
        if (Get-Command cl.exe -ErrorAction SilentlyContinue) { $Compiler = 'cl.exe' }
        else { $Compiler = 'clang++' }
    }
    $compilerCommand = Get-Command $Compiler -ErrorAction Stop
    $Compiler = $compilerCommand.Source
    $msvc = [IO.Path]::GetFileNameWithoutExtension($Compiler) -in @('cl', 'clang-cl')
    if (-not $ObsSourceDirectory) { $ObsSourceDirectory = Join-Path $DependenciesDirectory 'obs-studio-32.2.2-sources' }
    if (-not $ObsGeneratedDirectory) { $ObsGeneratedDirectory = Join-Path $DependenciesDirectory 'obs-sdk/include' }
    if (-not $ObsLibrary) {
        $libraryName = if ($msvc) { 'obs.lib' } else { 'libobs.dll.a' }
        $ObsLibrary = Join-Path $DependenciesDirectory "obs-sdk/lib/$libraryName"
    }
    foreach ($required in @($ObsDirectory, $PluginPath, $ObsSourceDirectory, $ObsGeneratedDirectory, $ObsLibrary)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "Required native module test input does not exist: $required" }
    }
    $ObsDirectory = (Resolve-Path -LiteralPath $ObsDirectory).Path
    $PluginPath = (Resolve-Path -LiteralPath $PluginPath).Path
    if (-not $CoreLibrary) {
        $pluginDirectory = Split-Path -Parent $PluginPath
        foreach ($relative in @('../libvrst_core.a', '../vrst_core.lib', '../Release/vrst_core.lib', '../../Release/vrst_core.lib')) {
            $candidate = [IO.Path]::GetFullPath((Join-Path $pluginDirectory $relative))
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $CoreLibrary = $candidate; break }
        }
    }
    if (-not $CoreLibrary -or -not (Test-Path -LiteralPath $CoreLibrary -PathType Leaf)) {
        throw 'Built vrst_core library required for frame-pairing tests; set -CoreLibrary to its path.'
    }
    $CoreLibrary = (Resolve-Path -LiteralPath $CoreLibrary).Path
    $ObsSourceDirectory = (Resolve-Path -LiteralPath $ObsSourceDirectory).Path
    $ObsGeneratedDirectory = (Resolve-Path -LiteralPath $ObsGeneratedDirectory).Path
    $ObsLibrary = (Resolve-Path -LiteralPath $ObsLibrary).Path
    New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
    $BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $obsBin = Join-Path $ObsDirectory 'bin/64bit'
    $env:PATH = "$obsBin;$(Split-Path -Parent $Compiler);$previousPath"
    foreach ($suite in @('image', 'streamer', 'image_pairing')) {
        $source = Join-Path $taskRoot "tests/${suite}_module_tests.mm"
        $executable = Join-Path $BuildDirectory "vrst-${suite}-module-tests.exe"
        $config = Join-Path $BuildDirectory "obs-test-config-$suite"
        New-Item -ItemType Directory -Force -Path $config | Out-Null
        $pairingObjects = @()
        if ($suite -eq 'image_pairing') {
            $imageSource = Join-Path $taskRoot 'plugin/image_filter.cpp'
            if ($msvc) {
                $imageObject = Join-Path $BuildDirectory 'image_filter.obj'
                & $Compiler '/nologo' '/std:c++17' '/O2' '/EHsc' '/MT' '/c' '/wd4996' "/I$taskRoot/include" "/I$ObsSourceDirectory/libobs" "/I$ObsGeneratedDirectory" $imageSource "/Fo$imageObject"
                if ($LASTEXITCODE -ne 0) { throw 'Image pairing support compilation failed' }
                $pairingObjects = @($imageObject, $CoreLibrary)
            } else {
                $pairingObjects = @($imageSource, $CoreLibrary)
            }
        }
        # The harness deliberately stages and examines every output pixel twice.
        # Optimize its CPU work so the harness does not needlessly skip OBS ticks.
        if ($msvc) {
            $compileArgs = @('/nologo', '/std:c++17', '/O2', '/EHsc', '/MT', '/TP', '/wd4996',
                "/I$taskRoot/include", "/I$taskRoot/plugin", "/I$ObsSourceDirectory/libobs", "/I$ObsGeneratedDirectory",
                $source, "/Fo$BuildDirectory/$suite.obj", "/Fe$executable", '/link', $ObsLibrary) + $pairingObjects
        } else {
            $compileArgs = @('-std=c++17', '-O2', '-static', '-Wno-deprecated-declarations',
                "-I$taskRoot/include", "-I$taskRoot/plugin", "-I$ObsSourceDirectory/libobs", "-I$ObsGeneratedDirectory",
                '-x', 'c++', $source, '-x', 'none') + $pairingObjects + @($ObsLibrary, '-o', $executable)
        }
        & $Compiler @compileArgs
        if ($LASTEXITCODE -ne 0) { throw "$suite module test compilation failed" }
        $log = Join-Path $BuildDirectory "$suite-module-tests.log"
        & $executable $PluginPath (Join-Path $taskRoot 'plugin/data') $ObsDirectory $config 2>&1 |
            Tee-Object -FilePath $log
        if ($LASTEXITCODE -ne 0) { throw "$suite native OBS module checks failed; see $log" }
    }
} finally {
    $env:PATH = $previousPath
    Pop-Location
}
