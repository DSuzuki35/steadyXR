$ErrorActionPreference = 'Stop'
try {
    if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS before uninstalling VR Stabilizer.' }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $child = Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath)) -Verb RunAs -Wait -PassThru
        exit $child.ExitCode
    }
    $destination = Join-Path $env:ProgramData 'obs-studio\plugins\vr-stabilizer'
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }
    Write-Host 'VR Stabilizer removed. Your OBS scenes have not been modified.'
    Read-Host 'Press Enter to close'
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
