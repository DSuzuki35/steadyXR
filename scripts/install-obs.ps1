$ErrorActionPreference = 'Stop'
try {
    if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before installing VR Stabilizer.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $child = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath)) -Verb RunAs -Wait -PassThru
        exit $child.ExitCode
    }
    $package = Split-Path -Parent $PSScriptRoot
    $source = Join-Path $package 'obs-plugin'
    $destination = Join-Path $env:ProgramData 'obs-studio\plugins\vr-stabilizer'
    foreach ($relative in @('bin\64bit\vr-stabilizer.dll', 'data\stabilize.effect', 'data\image-stabilize.effect', 'data\locale\en-US.ini')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $relative) -PathType Leaf)) { throw "Package is incomplete: $relative. Extract the entire ZIP first." }
    }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $destination -Recurse -Force
    Write-Host "Installed VR Stabilizer to $destination" -ForegroundColor Green
    Write-Host 'Start OBS. Open Filters on the capture-card source and add the VR Stabilizer effect filter.'
    Read-Host 'Press Enter to close'
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
