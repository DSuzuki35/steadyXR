[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Build,
    [string]$Destination = "$env:USERPROFILE/OneDrive/Simracing/Utilities/Horizon Lock VR",
    [string]$PreviousSettings,
    [string]$ValidationLog
)
$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath (Join-Path $PSScriptRoot '..')).FullName
$buildDir = (Get-Item -LiteralPath $Build).FullName
$delivery = (Get-Item -LiteralPath $Destination).FullName
. (Join-Path $root 'preview/SteadyXR-Update.ps1')
$stampPath = Join-Path $buildDir 'preview/steadyxr-build.json'
$stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
$version = $stamp.version
if ($version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z' -or $stamp.expires -ne $false) {
    throw 'Expected a stable build with no expiry.'
}
$binaryDir = Join-Path $buildDir 'preview/Release'
if (!(Test-Path -LiteralPath (Join-Path $binaryDir 'SteadyXR.exe'))) { $binaryDir = Join-Path $buildDir 'preview' }
foreach ($name in @('SteadyXR.exe','SteadyXR-OpenXR.dll')) {
    $binary = Get-Item -LiteralPath (Join-Path $binaryDir $name)
    if ($binary.VersionInfo.ProductVersion -cne $version) { throw "Incorrect build version in $name." }
}
$signTool = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/*/x64/signtool.exe') |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$signTool) { throw 'Install the Windows SDK signing tools.' }

function Sign-Artifact([string]$File) {
    & $signTool sign /sha1 $publisher /fd SHA256 /tr http://time.certum.pl /td SHA256 $File
    if ($LASTEXITCODE -ne 0) { throw "Signing failed for $File." }
    Assert-Publisher $File
    if (!(Get-AuthenticodeSignature -LiteralPath $File).TimeStamperCertificate) { throw "Missing timestamp in $File." }
}
function Write-Sums([string]$Archive) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$Archive.sha256", "$hash  $([IO.Path]::GetFileName($Archive))`r`n", [Text.Encoding]::ASCII)
}
function Archive-Folder([string]$Folder, [string]$Zip) {
    [IO.Compression.ZipFile]::CreateFromDirectory($Folder, $Zip, [IO.Compression.CompressionLevel]::Optimal, $true)
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$folderName = "SteadyXR-$version"
$names = @("$folderName-Standalone-Windows-x64.zip","$folderName-Source.zip","$folderName-Release.ps1")
$outputNames = @($names[0],$names[1],$names[2],($names[0]+'.sha256'),($names[1]+'.sha256'))
$gaming = Join-Path $delivery 'Gaming-PC'
$ready = Join-Path $gaming $folderName
foreach ($target in @($ready) + @($outputNames | ForEach-Object { Join-Path $delivery $_ })) {
    if (Test-Path -LiteralPath $target) { throw "Release output already exists: $target" }
}
if ($PreviousSettings) { $PreviousSettings = (Get-Item -LiteralPath $PreviousSettings).FullName }
if ($ValidationLog) { $ValidationLog = (Get-Item -LiteralPath $ValidationLog).FullName }
# Finish and verify everything in a unique sibling directory before publishing any output.
$work = Join-Path $delivery ('.steadyxr-package-' + [guid]::NewGuid().ToString('N'))
$payload = Join-Path $work $folderName
$source = Join-Path $work "$folderName-Source"
$archive = Join-Path $work $names[0]
$sourceArchive = Join-Path $work $names[1]
$receipt = Join-Path $work $names[2]
$moved = @()
try {
    [IO.Directory]::CreateDirectory($payload) | Out-Null
    [IO.Directory]::CreateDirectory($source) | Out-Null
    foreach ($name in @('SteadyXR.exe','SteadyXR-OpenXR.dll')) {
        Copy-Item -LiteralPath (Join-Path $binaryDir $name) -Destination $payload
    }
    Copy-Item -LiteralPath (Join-Path $root 'preview/SteadyXR-Update.ps1') -Destination $payload
    foreach ($name in @('SteadyXR.exe','SteadyXR-OpenXR.dll','SteadyXR-Update.ps1')) {
        Sign-Artifact (Join-Path $payload $name)
    }
    Copy-Item -LiteralPath $stampPath -Destination (Join-Path $payload 'SteadyXR-build.json')
    Copy-Item -LiteralPath $stampPath -Destination (Join-Path $payload 'SteadyXR-OpenXR-build.json')
    Copy-Item -LiteralPath (Join-Path $buildDir 'preview/XR_APILAYER_STEADYXR_preview.json') -Destination $payload
    foreach ($relative in @('preview/assets/SteadyXR.ico','preview/assets/SteadyXR.png','LICENSE','THIRD-PARTY-NOTICES.md','preview/layers','licenses')) {
        Copy-Item -LiteralPath (Join-Path $root $relative) -Destination $payload -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $root 'docs/standalone-updates.md') -Destination (Join-Path $payload 'Updates.md')
    Copy-Item -LiteralPath (Join-Path $root 'preview/README.md') -Destination $payload
    Copy-Item -LiteralPath (Join-Path $root 'docs/spectator-features.md') -Destination (Join-Path $payload 'Spectator-Guide.md')
    Copy-Item -LiteralPath (Join-Path $root 'docs/viewer-director.md') -Destination (Join-Path $payload 'Director-Guide.md')
    if ($ValidationLog) { Copy-Item -LiteralPath $ValidationLog -Destination (Join-Path $payload 'automated-tests.log') }
    $checks = Get-ChildItem -LiteralPath $payload -File -Recurse | Sort-Object FullName | ForEach-Object {
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.FullName.Substring($payload.Length+1).Replace('\','/')
    }
    $checks | Set-Content -LiteralPath (Join-Path $payload 'SHA256SUMS.txt') -Encoding UTF8
    Archive-Folder $payload $archive
    # Include current working sources used by the compiler, including new files in source directories.
    $tracked = @(git -c core.quotepath=false -C $root ls-files --cached)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate tracked source.' }
    $added = @(git -c core.quotepath=false -C $root ls-files --others --exclude-standard -- CMakeLists.txt cmake include plugin preview scripts sender src tests third_party docs/standalone-updates.md 'docs/*1.0.0*')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate new source inputs.' }
    foreach ($relative in $tracked + $added) {
        $from = Join-Path $root $relative
        if (!(Test-Path -LiteralPath $from -PathType Leaf)) { continue }
        $to = Join-Path $source $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to)) | Out-Null
        [IO.File]::Copy($from,$to)
    }
    Archive-Folder $source $sourceArchive
    Write-Sums $archive
    Write-Sums $sourceArchive
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($receipt, "# SteadyXR SHA256 $hash  $($names[0])`r`n", [Text.Encoding]::ASCII)
    Sign-Artifact $receipt
    Assert-ReleaseArchive ([pscustomobject]@{Archive=$names[0]}) $receipt $archive
    $roundTrip = Join-Path $work 'verified'
    [IO.Compression.ZipFile]::ExtractToDirectory($archive,$roundTrip)
    Assert-Payload (Join-Path $roundTrip $folderName) $version
    if ($PreviousSettings) {
        # Personal settings belong only in this user's ready-to-run folder.
        Copy-ViewerSettings $PreviousSettings $payload
    }
    [IO.Directory]::CreateDirectory($gaming) | Out-Null
    foreach ($name in $outputNames) {
        $to = Join-Path $delivery $name
        [IO.File]::Move((Join-Path $work $name),$to)
        $moved += $to
    }
    [IO.Directory]::Move($payload,$ready)
    try { Update-PreviewLauncher $gaming $version } catch {
        Write-Warning "Release installed. Start-Preview.cmd could not be updated: $($_.Exception.Message)"
    }
    Write-Output $ready
} catch {
    # Undo only files this invocation just moved. Earlier versions are never replaced.
    foreach ($path in $moved) {
        [IO.File]::Move($path,(Join-Path $work ([IO.Path]::GetFileName($path))))
    }
    throw
} finally {
    if ([IO.Path]::GetDirectoryName($work) -eq $delivery -and [IO.Path]::GetFileName($work) -match '^\.steadyxr-package-[a-f0-9]{32}$' -and [IO.Directory]::Exists($work)) {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
