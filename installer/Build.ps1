[CmdletBinding()]
param(
    [string]$PayloadArchive,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build-installer-110'),
    [string]$Destination = "$env:USERPROFILE/OneDrive/Simracing/Utilities/Horizon Lock VR",
    [string]$Compiler,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules')
foreach ($module in @('Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security')) {
    Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', $module, "$module.psd1")) -ErrorAction Stop
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$release = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release.json') -Raw | ConvertFrom-Json
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
function Assert-Signed([string]$Path, [string]$Thumbprint = $release.publisher_thumbprint) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $Thumbprint -or !$signature.TimeStamperCertificate) {
        throw "Publisher/timestamp verification failed: $Path"
    }
}
function Check-Exit([string]$Step) { if ($LASTEXITCODE -ne 0) { throw "$Step failed ($LASTEXITCODE)." } }
if (!$PayloadArchive) {
    $PayloadArchive = Join-Path $BuildDirectory $release.archive
    if (!(Test-Path -LiteralPath $PayloadArchive)) { Invoke-WebRequest $release.archive_url -OutFile $PayloadArchive }
}
$PayloadArchive = (Get-Item -LiteralPath $PayloadArchive).FullName
if ((Get-FileHash -LiteralPath $PayloadArchive -Algorithm SHA256).Hash -ine $release.archive_sha256) {
    throw 'The input ZIP differs from the immutable signed SteadyXR 1.1.0 portable release.'
}
$run = Join-Path $BuildDirectory ('run-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
$prefix = "SteadyXR-$($release.version)/"
$zip = [IO.Compression.ZipFile]::OpenRead($PayloadArchive)
try {
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if (!$name.StartsWith($prefix, [StringComparison]::Ordinal) -or $name -match '(^|/)\.\.(/|$)|:|^/' -or
            $entry.Length -gt 256MB) { throw "Unsafe or unexpected ZIP entry: $name" }
        if ($name -match '(?i)(^|/)(SteadyXR(?:-shortcuts|-ui)?\.ini|preview\.ini|Presets)(/|$)') {
            throw "Release contains a reserved user-data path: $name"
        }
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($PayloadArchive, $run)
$payload = Join-Path $run "SteadyXR-$($release.version)"
foreach ($name in @('SteadyXR.exe', 'SteadyXR-OpenXR.dll', 'SteadyXR-Update.ps1',
                    'obs-plugin/bin/64bit/steadyxr-obs.dll', 'obs-plugin/Install-OBS-Integration.ps1')) {
    Assert-Signed (Join-Path $payload $name)
}
foreach ($name in @('SteadyXR.exe', 'SteadyXR-OpenXR.dll', 'obs-plugin/bin/64bit/steadyxr-obs.dll')) {
    if ((Get-Item -LiteralPath (Join-Path $payload $name)).VersionInfo.ProductVersion -ne $release.version) {
        throw "Unexpected product version in $name"
    }
}
if (!$Compiler) {
    $toolRoot = Join-Path $BuildDirectory 'tools/InnoSetup'
    $Compiler = Join-Path $toolRoot 'ISCC.exe'
    if (!(Test-Path -LiteralPath $Compiler)) {
        $toolInstaller = Join-Path $BuildDirectory 'tools/innosetup-7.1.1-x64.exe'
        New-Item -ItemType Directory -Force -Path (Split-Path $toolInstaller -Parent) | Out-Null
        if (!(Test-Path -LiteralPath $toolInstaller)) { Invoke-WebRequest $release.inno_url -OutFile $toolInstaller }
        $signature = Get-AuthenticodeSignature -LiteralPath $toolInstaller
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )CN=Pyrsys B\.V\.(,|$)') {
            throw 'Official Inno Setup compiler download publisher verification failed.'
        }
        $toolProcess = Start-Process -FilePath $toolInstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES',
            '/NORESTART', '/CURRENTUSER', '/NOICONS', '/TASKS=', ('/DIR="' + $toolRoot + '"')) -WindowStyle Hidden -Wait -PassThru
        if ($toolProcess.ExitCode -ne 0) { throw 'Installing the Inno Setup build tool failed.' }
    }
}
$compilerSignature = Get-AuthenticodeSignature -LiteralPath $Compiler
if ($compilerSignature.Status -ne 'Valid' -or $compilerSignature.SignerCertificate.Subject -notmatch '(^|, )CN=Pyrsys B\.V\.(,|$)') {
    throw 'The selected compiler is not signed by the official Inno Setup publisher.'
}
$signTool = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/*/x64/signtool.exe') |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$signTool) { throw 'Windows SDK signtool.exe is required.' }
$launcher = Join-Path $run 'Start-Preview.cmd'
[IO.File]::WriteAllText($launcher, "@echo off`r`nstart `"`" `"%~dp0SteadyXR-$($release.version)\SteadyXR.exe`"`r`n", [Text.Encoding]::ASCII)
$output = Join-Path $run 'output'
$signCommand = '$q' + $signTool + '$q sign /sha1 ' + $release.publisher_thumbprint +
    ' /fd SHA256 /tr http://time.certum.pl /td SHA256 $f'
$common = @('--no-ide-signtools', "--signtool=steadyxr=$signCommand", "--output-dir=$output",
    "--define=PayloadDir=$payload", "--define=LauncherFile=$launcher", "--define=AppVersion=$($release.version)")
& $Compiler @common (Join-Path $PSScriptRoot 'SteadyXR.iss')
Check-Exit 'Compile and sign installer/uninstaller'
$installer = Join-Path $output "SteadyXR-$($release.version)-Setup-Windows-x64.exe"
Assert-Signed $installer
if (!$SkipTests) {
    & $Compiler @common '--define=TestBuild=1' (Join-Path $PSScriptRoot 'SteadyXR.iss')
    Check-Exit 'Compile isolated test installer'
    $testInstaller = Join-Path $output "SteadyXR-$($release.version)-Setup-Test.exe"
    Assert-Signed $testInstaller
    & (Join-Path $PSScriptRoot 'Test-Installer.ps1') -Installer $testInstaller -PayloadDirectory $payload -BuildDirectory $run
    if (!$?) { throw 'Installer tests failed.' }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$final = Join-Path $Destination ([IO.Path]::GetFileName($installer))
if (Test-Path -LiteralPath $final) { throw "Preserving existing deliverable: $final" }
Copy-Item -LiteralPath $installer -Destination $final
$hash = (Get-FileHash -LiteralPath $final -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$final.sha256", "$hash  $([IO.Path]::GetFileName($final))`r`n", [Text.Encoding]::ASCII)
[pscustomobject]@{ Installer = $final; SHA256 = $hash; BuildDirectory = $run; Tests = !$SkipTests } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BuildDirectory 'latest-build.json') -Encoding UTF8
Write-Output "Signed installer: $final"
