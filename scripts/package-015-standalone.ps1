param(
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$Signed014Directory,
    [Parameter(Mandatory=$true)][string]$CaptureSource,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$expected = '74a6943f014d85b81f301da852b61d5a8cb2251a199130978bf4cbdba097d91d'
$capture = Join-Path $Signed014Directory 'SteadyXR-OpenXR.dll'
if ((Get-FileHash -LiteralPath $capture -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Capture DLL hash mismatch. Packaging stopped.' }
if ((Get-AuthenticodeSignature -LiteralPath $capture).Status -ne 'Valid') { throw 'Capture DLL signature is not valid. Packaging stopped.' }
$exe = Join-Path $Build 'preview/Release/SteadyXR.exe'
if ((Get-Item -LiteralPath $exe).VersionInfo.FileVersion -ne '0.15.0') { throw 'Expected a built 0.15.0 executable.' }
if (!(Test-Path -LiteralPath $CaptureSource -PathType Leaf)) { throw 'Original capture source archive is missing.' }
$root = Split-Path $PSScriptRoot -Parent
$target = Join-Path $Destination 'Gaming-PC/SteadyXR-0.15.0'
$zip = Join-Path $Destination 'SteadyXR-0.15.0-Standalone-Windows-x64.zip'
if ((Test-Path -LiteralPath $target) -or (Test-Path -LiteralPath $zip)) { throw '0.15.0 delivery already exists. Preserve it and choose a new destination.' }
New-Item -ItemType Directory -Path $target -Force | Out-Null
foreach ($name in @('SteadyXR-OpenXR.dll','SteadyXR-OpenXR-build.json','XR_APILAYER_STEADYXR_preview.json','SteadyXR.ico','SteadyXR.png')) {
    Copy-Item -LiteralPath (Join-Path $Signed014Directory $name) -Destination $target
}
Copy-Item -LiteralPath $exe -Destination $target
Copy-Item -LiteralPath (Join-Path $Build 'preview/steadyxr-build.json') -Destination (Join-Path $target 'SteadyXR-build.json')
Copy-Item -LiteralPath (Join-Path $root 'preview/layers') -Destination $target -Recurse
foreach ($name in @('LICENSE','THIRD-PARTY-NOTICES.md')) { Copy-Item -LiteralPath (Join-Path $root $name) -Destination $target }
Copy-Item -LiteralPath (Join-Path $root 'preview/README.md') -Destination $target
Copy-Item -LiteralPath (Join-Path $root 'docs/workshop.md') -Destination (Join-Path $target 'Workshop-Guide.md')
Copy-Item -LiteralPath (Join-Path $root 'licenses') -Destination $target -Recurse
@{ app_version='0.15.0'; capture_version='0.14.0'; capture_sha256=$expected; capture_signature='Valid'; capture_preserved=$true } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $target 'Components.json') -Encoding UTF8
'App source: SteadyXR-0.15.0-Source.zip. The byte-identical signed capture component uses the original SteadyXR-0.14.0-Source.zip included there under prebuilt-capture.' | Set-Content -LiteralPath (Join-Path $target 'SOURCE.txt') -Encoding UTF8
$sourceStage = Join-Path $Build 'source-015'
if(Test-Path -LiteralPath $sourceStage){throw 'Source staging exists; inspect before packaging again.'}
New-Item -ItemType Directory -Path $sourceStage | Out-Null
$tracked = git -C $root ls-files
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate corresponding source.' }
foreach ($relative in $tracked) {
    $sourceFile=Join-Path $root $relative
    if (!(Test-Path -LiteralPath $sourceFile -PathType Leaf)) { continue }
    $destFile=Join-Path $sourceStage $relative
    New-Item -ItemType Directory -Path (Split-Path $destFile -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile -Destination $destFile
}
New-Item -ItemType Directory -Path (Join-Path $sourceStage 'prebuilt-capture') | Out-Null
Copy-Item -LiteralPath $CaptureSource -Destination (Join-Path $sourceStage 'prebuilt-capture')
$sourceZip = Join-Path $Destination 'SteadyXR-0.15.0-Source.zip'
Compress-Archive -LiteralPath $sourceStage -DestinationPath $sourceZip -CompressionLevel Optimal
$checks = Get-ChildItem -LiteralPath $target -File -Recurse | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($target.Length+1).Replace('\','/') }
$checks | Set-Content -LiteralPath (Join-Path $target 'SHA256SUMS.txt') -Encoding UTF8
Compress-Archive -LiteralPath $target -DestinationPath $zip -CompressionLevel Optimal
foreach ($archive in @($zip,$sourceZip)) { '{0}  {1}' -f (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant(),(Split-Path $archive -Leaf) | Set-Content -LiteralPath ($archive+'.sha256') -Encoding ASCII }
if ((Get-FileHash -LiteralPath (Join-Path $target 'SteadyXR-OpenXR.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Delivered capture hash mismatch.' }
Write-Output $zip
