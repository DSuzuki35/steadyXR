# Windows PowerShell 5.1. Installed copies of this helper are Authenticode signed.
param([int]$ViewerPid)
$ErrorActionPreference = 'Stop'
# A launcher such as CTest can pass PowerShell 7's module search path into a
# Windows PowerShell 5.1 child. Use the modules belonging to this interpreter.
$env:PSModulePath = [IO.Path]::Combine($PSHOME, 'Modules')
foreach ($module in @('Microsoft.PowerShell.Utility','Microsoft.PowerShell.Security')) {
    Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', $module, "$module.psd1")) -ErrorAction Stop
}
$publisher = '89F7F00A4FD4BD66F5617F1202B8E7B9DD662247'
$repository = 'DSuzuki35/horizonlock-vr'

function Assert-Publisher([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $publisher) {
        throw "The publisher signature failed verification for $([IO.Path]::GetFileName($Path))."
    }
}

function Get-UpdateOffer($Release, [version]$Current) {
    if ($Release.draft -or $Release.prerelease) { return }
    # Windows version resources use 16-bit components. Only canonical stable tags are accepted.
    $part = '(0|[1-9][0-9]{0,4})'
    if ($Release.tag_name -cnotmatch "^v?(?<version>$part\.$part\.$part)\z") { return }
    $version = $Matches.version
    $next = [version]$version
    if ($next -le $Current -or [Math]::Max($next.Major,[Math]::Max($next.Minor,$next.Build)) -gt 65535) { return }
    $archive = "SteadyXR-$version-Standalone-Windows-x64.zip"
    $receipt = "SteadyXR-$version-Release.ps1"
    $prefix = "https://github.com/$repository/releases/download/$($Release.tag_name)"
    foreach ($name in @($archive,$receipt)) {
        $asset = @($Release.assets | Where-Object { $_.name -ceq $name })
        if ($asset.Count -ne 1 -or $asset[0].browser_download_url -cne "$prefix/$name") { return }
        $limit = if ($name -eq $receipt) { 128KB } else { 256MB }
        if ($asset[0].size -le 0 -or $asset[0].size -gt $limit) { return }
    }
    return [pscustomobject]@{ Version=$version; Tag=$Release.tag_name; Archive=$archive; Receipt=$receipt; Prefix=$prefix }
}

function Assert-ReleaseArchive($Offer, [string]$Receipt, [string]$Archive) {
    Assert-Publisher $Receipt
    # Only parse the signed data as text. Never execute a release receipt.
    $record = Get-Content -LiteralPath $Receipt -TotalCount 1
    $pattern = '^# SteadyXR SHA256 ([0-9A-Fa-f]{64})  ' + [regex]::Escape($Offer.Archive) + '\z'
    if ($record -cnotmatch $pattern) { throw 'The signed release receipt names a different package.' }
    $expected = $Matches[1]
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $expected) {
        throw 'The downloaded package does not match its signed SHA-256 receipt.'
    }
}

function Assert-Payload([string]$Directory, [string]$Version) {
    foreach ($name in @('SteadyXR.exe','SteadyXR-OpenXR.dll','SteadyXR-Update.ps1')) {
        $file = Join-Path $Directory $name
        Assert-Publisher $file
        if ($name -ne 'SteadyXR-Update.ps1' -and
            (Get-Item -LiteralPath $file).VersionInfo.ProductVersion -cne $Version) {
            throw "Unexpected product version in $name."
        }
    }
}

function Install-StagedUpdate([string]$Staged, [string]$Current, [string]$Target) {
    if ([IO.Directory]::Exists($Target) -or [IO.File]::Exists($Target)) {
        throw "This version folder already exists: $Target. Open the installed copy directly."
    }
    foreach ($name in @('SteadyXR.ini','SteadyXR-shortcuts.ini','preview.ini')) {
        $existing = Join-Path $Current $name
        if (Test-Path -LiteralPath $existing) {
            $to = Join-Path $Staged $name
            if (Test-Path -LiteralPath $to) { throw "The update includes a reserved user data path: $name" }
            Copy-Item -LiteralPath $existing -Destination $to -Recurse
        }
    }
    # A same-volume, non-replacing rename publishes the fully prepared folder.
    [IO.Directory]::Move($Staged, $Target)
}

function Update-PreviewLauncher([string]$Parent, [string]$Version, [string]$FromFolder) {
    $launcher = Join-Path $Parent 'Start-Preview.cmd'
    if (![IO.File]::Exists($launcher)) { return }
    $text = [IO.File]::ReadAllText($launcher)
    $format = '\A@echo off\r?\nstart "" "%~dp0SteadyXR-(?<old>[0-9]+\.[0-9]+\.[0-9]+)\\SteadyXR\.exe"\r?\n?\z'
    if ($text -cnotmatch $format) { return }
    $old = $Matches.old
    if (($FromFolder -and $FromFolder -ine "SteadyXR-$old") -or [version]$old -ge [version]$Version) { return }
    $temp = Join-Path $Parent ('.steadyxr-launcher-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $newText = $text.Replace("SteadyXR-$old\SteadyXR.exe", "SteadyXR-$Version\SteadyXR.exe")
        [IO.File]::WriteAllText($temp,$newText,[Text.Encoding]::ASCII)
        if ([IO.File]::ReadAllText($launcher) -ceq $text) {
            [IO.File]::Replace($temp,$launcher,[Management.Automation.Language.NullString]::Value)
        }
    } finally { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } }
}

function Show-UpdateMessage([string]$Text, [string]$Buttons, [string]$Icon) {
    Add-Type -AssemblyName System.Windows.Forms
    [Windows.Forms.MessageBox]::Show($Text, 'SteadyXR update', $Buttons, $Icon)
}

function Save-ReleaseAsset($Offer, [string]$Name, [string]$Work, [string]$GitHubCli, $Headers) {
    if ($GitHubCli) {
        & $GitHubCli release download $Offer.Tag --repo "github.com/$repository" --pattern $Name --dir $Work
        if ($LASTEXITCODE -ne 0) { throw "GitHub could not download $Name." }
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri "$($Offer.Prefix)/$Name" -OutFile (Join-Path $Work $Name) -Headers $Headers -TimeoutSec 180
    }
}

function Invoke-ViewerUpdate {
    if ($ViewerPid -le 0) { return }
    $currentDir = $PSScriptRoot
    $currentExe = Join-Path $currentDir 'SteadyXR.exe'
    $viewer = Get-Process -Id $ViewerPid -ErrorAction SilentlyContinue
    if (!$viewer -or $viewer.Path -ne $currentExe) { return }
    $current = [version](Get-Item -LiteralPath $currentExe).VersionInfo.ProductVersion
    $headers = @{ 'User-Agent'="SteadyXR/$current"; Accept='application/vnd.github+json' }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $gh = $null
    try {
        $latest = Invoke-RestMethod -Uri "https://api.github.com/repos/$repository/releases/latest" -Headers $headers -TimeoutSec 12
    } catch {
        # Private GitHub releases require an account with read access. Let an
        # installed GitHub CLI handle its own stored credentials and downloads.
        if (!$_.Exception.Response -or [int]$_.Exception.Response.StatusCode -notin @(403,404)) { return }
        $gh = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'GitHub CLI/gh.exe'
        if (!(Test-Path -LiteralPath $gh -PathType Leaf)) { return }
        $env:GH_PROMPT_DISABLED = '1'
        try {
            $response = & $gh api --hostname github.com "repos/$repository/releases/latest" 2>$null
            if ($LASTEXITCODE -ne 0) { return }
            $latest = $response | ConvertFrom-Json
        } catch { return }
    }
    $offer = Get-UpdateOffer $latest $current
    if (!$offer -or $viewer.HasExited) { return }
    $parent = [IO.Directory]::GetParent($currentDir).FullName
    $target = Join-Path $parent "SteadyXR-$($offer.Version)"
    if (Test-Path -LiteralPath $target) { return }
    $prompt = "Download SteadyXR $($offer.Version) and start it when you close the current app? Your settings and joystick bindings will be copied. Keep using the new folder for future launches:`r`n`r`n$target`r`n`r`nYour previous version stays available."
    if ((Show-UpdateMessage $prompt 'YesNo' 'Question') -ne 'Yes') { return }
    $work = Join-Path $parent ('.steadyxr-update-' + [guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($work) | Out-Null
        $receipt = Join-Path $work $offer.Receipt
        $archive = Join-Path $work $offer.Archive
        Save-ReleaseAsset $offer $offer.Receipt $work $gh $headers
        Assert-Publisher $receipt
        Save-ReleaseAsset $offer $offer.Archive $work $gh $headers
        $receiptLock = [IO.File]::OpenRead($receipt)
        try {
            $archiveLock = [IO.File]::OpenRead($archive)
            try {
                Assert-ReleaseArchive $offer $receipt $archive
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                $viewer.WaitForExit()
                [IO.Compression.ZipFile]::ExtractToDirectory($archive, (Join-Path $work 'payload'))
            } finally { $archiveLock.Dispose() }
        } finally { $receiptLock.Dispose() }
        $staged = Join-Path $work "payload/SteadyXR-$($offer.Version)"
        Assert-Payload $staged $offer.Version
        Install-StagedUpdate $staged $currentDir $target
        try { Update-PreviewLauncher $parent $offer.Version ([IO.Path]::GetFileName($currentDir)) } catch {
            [void](Show-UpdateMessage "The update is installed. Open $target\SteadyXR.exe for future sessions because Start-Preview.cmd could not be updated." 'OK' 'Information')
        }
        Start-Process -FilePath (Join-Path $target 'SteadyXR.exe') -WorkingDirectory $target
    } catch {
        [void](Show-UpdateMessage ("Update could not finish. Your previous app is available at $currentExe.`r`n`r`n" + $_.Exception.Message) 'OK' 'Warning')
    } finally {
        if ([IO.Path]::GetDirectoryName($work) -eq $parent -and [IO.Path]::GetFileName($work) -match '^\.steadyxr-update-[a-f0-9]{32}$' -and [IO.Directory]::Exists($work)) {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') { Invoke-ViewerUpdate }
