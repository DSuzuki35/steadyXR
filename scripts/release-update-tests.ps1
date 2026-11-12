$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../preview/SteadyXR-Update.ps1')
function Check($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Must-Fail([scriptblock]$Operation, [string]$Message) {
    try { & $Operation } catch { return }
    throw $Message
}
function Fixture([string]$Tag) {
    $v = $Tag -replace '^v',''
    $prefix = "https://github.com/DSuzuki35/horizonlock-vr/releases/download/$Tag"
    $assets = @("SteadyXR-$v-Standalone-Windows-x64.zip","SteadyXR-$v-Release.ps1") | ForEach-Object {
        [pscustomobject]@{ name=$_; browser_download_url="$prefix/$_"; size=20000 }
    }
    [pscustomobject]@{ tag_name=$Tag; draft=$false; prerelease=$false; assets=$assets }
}
$current = [version]'1.0.0'
Check (Get-UpdateOffer (Fixture 'v1.10.1') ([version]'1.9.3')) 'numeric comparison'
Check (Get-UpdateOffer (Fixture '2.0.0') $current) 'tag without v'
foreach ($tag in @('v0.99.1','v1.0.0','v1.1','v01.2.0','v1.0.1-beta3','v1.1.1+test','v65536.1.1','x1.1.1')) {
    Check (!(Get-UpdateOffer (Fixture $tag) $current)) "invalid or old tag accepted: $tag"
}
$release = Fixture 'v1.2.0'
$release.draft = $true
Check (!(Get-UpdateOffer $release $current)) 'draft accepted'
$release.draft = $false
$release.prerelease = $true
Check (!(Get-UpdateOffer $release $current)) 'prerelease accepted'
$release.prerelease = $false
$release.assets[0].browser_download_url = 'https://example.com/other.zip'
Check (!(Get-UpdateOffer $release $current)) 'unrelated asset URL accepted'
$release = Fixture 'v1.2.0'
$release.assets += $release.assets[0]
Check (!(Get-UpdateOffer $release $current)) 'duplicate asset accepted'
$release.assets = @($release.assets[0])
Check (!(Get-UpdateOffer $release $current)) 'missing receipt accepted'

$root = Join-Path ([IO.Path]::GetTempPath()) ('SteadyXR-test-' + [guid]::NewGuid().ToString('N'))
try {
    $old = Join-Path $root 'old'
    $staged = Join-Path $root 'stage'
    $target = Join-Path $root 'SteadyXR-1.2.0'
    [IO.Directory]::CreateDirectory((Join-Path $old 'Clips')) | Out-Null
    [IO.Directory]::CreateDirectory($staged) | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $old 'SteadyXR.ini'),[byte[]](0,254,235,32,42))
    [IO.File]::WriteAllText((Join-Path $old 'SteadyXR-shortcuts.ini'),'joystick bindings')
    [IO.File]::WriteAllBytes((Join-Path $old 'SteadyXR-ui.ini'),[byte[]](255,254,3,4,0))
    [IO.File]::WriteAllText((Join-Path $old 'Clips/clip.bin'),'recording')
    $reserved = Join-Path $staged 'SteadyXR.ini'
    [IO.File]::WriteAllText($reserved,'reserved file')
    Must-Fail { Install-StagedUpdate $staged $old $target } 'accepted package with reserved setting'
    Check (!(Test-Path -LiteralPath $target)) 'published a failed installation'
    [IO.File]::Delete($reserved)
    Install-StagedUpdate $staged $old $target
    Check ((Get-FileHash -LiteralPath (Join-Path $old 'SteadyXR.ini')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $target 'SteadyXR.ini')).Hash) 'settings changed'
    Check ([IO.File]::ReadAllText((Join-Path $target 'SteadyXR-shortcuts.ini')) -eq 'joystick bindings') 'shortcut copy'
    Check ((Get-FileHash -LiteralPath (Join-Path $old 'SteadyXR-ui.ini')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $target 'SteadyXR-ui.ini')).Hash) 'layout copy'
    $ready = Join-Path $root 'local-release'
    [IO.Directory]::CreateDirectory($ready) | Out-Null
    $namedMain = Join-Path $old 'my-preferred-profile.ini'
    [IO.File]::WriteAllText($namedMain,'user-selected profile')
    Copy-ViewerSettings $namedMain $ready
    Check ([IO.File]::ReadAllText((Join-Path $ready 'SteadyXR.ini')) -eq 'user-selected profile') 'selected main settings'
    foreach ($sidecar in @('SteadyXR-ui.ini','SteadyXR-shortcuts.ini')) {
        Check ((Get-FileHash -LiteralPath (Join-Path $old $sidecar)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $ready $sidecar)).Hash) 'local release sidecar copy'
    }
    Check ([IO.File]::Exists((Join-Path $old 'Clips/clip.bin'))) 'rollback copy changed'
    Must-Fail { Install-StagedUpdate $staged $old $target } 'overwrote installed version'
    $cmd = Join-Path $root 'Start-Preview.cmd'
    $template = '@echo off' + [Environment]::NewLine + 'start "" "%~dp0SteadyXR-1.0.0\SteadyXR.exe"' + [Environment]::NewLine
    [IO.File]::WriteAllText($cmd,$template)
    Update-PreviewLauncher $root '1.2.0' 'SteadyXR-0.9.0'
    Check ([IO.File]::ReadAllText($cmd) -ceq $template) 'changed launcher for another folder'
    Update-PreviewLauncher $root '1.2.0' 'SteadyXR-1.0.0'
    Check ([IO.File]::ReadAllText($cmd) -ceq $template.Replace('1.0.0','1.2.0')) 'generated launcher was not advanced'
    [IO.File]::WriteAllText($cmd,'@echo custom')
    Update-PreviewLauncher $root '2.0.0'
    Check ([IO.File]::ReadAllText($cmd) -ceq '@echo custom') 'changed custom launcher'

    $archive = Join-Path $root 'update.zip'
    $receipt = Join-Path $root 'receipt.ps1'
    [IO.File]::WriteAllText($archive,'signed package bytes')
    $hash = (Get-FileHash -LiteralPath $archive).Hash
    [IO.File]::WriteAllText($receipt,"# SteadyXR SHA256 $hash  update.zip")
    $offer = [pscustomobject]@{Archive='update.zip'}
    Must-Fail { Assert-ReleaseArchive $offer $receipt $archive } 'accepted unsigned receipt'
    $otherPublisher = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    Must-Fail { Assert-Publisher $otherPublisher } 'accepted a different publisher'
    # The real signature check above is independent of the digest/name checks below.
    function Assert-Publisher([string]$Path) {}
    Assert-ReleaseArchive $offer $receipt $archive
    [IO.File]::WriteAllText($archive,'altered package bytes')
    Must-Fail { Assert-ReleaseArchive $offer $receipt $archive } 'accepted altered ZIP'
    $offer.Archive = 'SteadyXR-9.9.9-Standalone-Windows-x64.zip'
    Must-Fail { Assert-ReleaseArchive $offer $receipt $archive } 'accepted replayed receipt'
} finally {
    if ([IO.Path]::GetDirectoryName($root) -eq ([IO.Path]::GetTempPath()).TrimEnd('\') -and [IO.Path]::GetFileName($root) -match '^SteadyXR-test-[a-f0-9]{32}$') {
        Remove-Item -LiteralPath $root -Force -Recurse
    }
}
'PASS: stable releases, authenticated assets, receipts, settings, layout, bindings and rollback'
