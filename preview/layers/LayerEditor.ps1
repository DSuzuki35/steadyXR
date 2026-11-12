param([switch]$SelfTest, [switch]$Report, [string]$TestOutput, [string]$OriginUserSid)
$ErrorActionPreference = 'Stop'
trap {
    if (-not $SelfTest -and -not $Report) {
        Add-Type -AssemblyName System.Windows.Forms
        [Windows.Forms.MessageBox]::Show(('The layer editor could not continue: ' + $_.Exception.Message), 'SteadyXR', 'OK', 'Error') | Out-Null
    }
    throw $_
}
Add-Type -Path (Join-Path $PSScriptRoot 'RegistryStore.cs') -ReferencedAssemblies System.Runtime.Serialization,System.Xml

function Resolve-OriginUserSid([string]$value) {
    if ([string]::IsNullOrEmpty($value)) { return [Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
    $parsed = New-Object Security.Principal.SecurityIdentifier($value)
    if ($parsed.Value -cne $value) { throw 'OriginUserSid must be a canonical Windows SID.' }
    return $parsed.Value
}
$script:originUserSid = Resolve-OriginUserSid $OriginUserSid

function Get-LayerDetails($value, $group) {
    $displayName = $value.Name
    try { $displayName = [IO.Path]::GetFileName($value.Name) } catch { }
    if ([string]::IsNullOrWhiteSpace($displayName)) { $displayName = '(unnamed value)' }
    $result = [ordered]@{ Name = $displayName; TechnicalName = ''; Status = 'Invalid registry value';
        Manifest = $value.Name; Library = ''; Description = ''; Environment = ''; IsDword = $false; Enabled = $false }
    if ($value.Kind -eq 4 -and $value.Data.Length -eq 4) {
        $result.IsDword = $true
        $result.Enabled = [BitConverter]::ToUInt32($value.Data, 0) -eq 0
        $result.Status = if ($result.Enabled) { 'Enabled' } else { 'Disabled' }
    }
    if ([string]::IsNullOrWhiteSpace($value.Name) -or -not (Test-Path -LiteralPath $value.Name -PathType Leaf -ErrorAction SilentlyContinue)) {
        $result.Status += ' / missing manifest'
        return [pscustomobject]$result
    }
    try {
        if ((Get-Item -LiteralPath $value.Name).Length -gt 1MB) { throw 'Manifest is larger than 1 MB.' }
        $json = Get-Content -LiteralPath $value.Name -Raw -Encoding UTF8 | ConvertFrom-Json
        $layer = $json.api_layer
        if (-not ($layer.name -is [string]) -or -not ($layer.library_path -is [string]) -or
            [string]::IsNullOrWhiteSpace($layer.name) -or [string]::IsNullOrWhiteSpace($layer.library_path)) {
            throw 'Manifest has no valid api_layer name or library_path.'
        }
        $result.Name = $layer.name -replace '^XR_APILAYER_', ''
        $result.TechnicalName = $layer.name
        if ($layer.name -eq 'XR_APILAYER_HORIZONLOCK_preview') { $result.Name = 'SteadyXR' }
        $result.Description = [string]$layer.description
        $result.Library = if ([IO.Path]::IsPathRooted($layer.library_path)) { $layer.library_path } else {
            [IO.Path]::GetFullPath((Join-Path (Split-Path $value.Name -Parent) $layer.library_path))
        }
        if (-not (Test-Path -LiteralPath $result.Library -PathType Leaf)) { $result.Status += ' / missing DLL' }
        if ($group.SubKey.EndsWith('\Explicit')) {
            if ($result.Enabled) { $result.Status = $result.Status.Replace('Enabled', 'Explicit: app enables') }
        } else {
            foreach ($field in @('disable_environment', 'enable_environment')) {
                $variable = $layer.$field
                if ($variable -is [string] -and $variable.Length) {
                    $present = $false
                    foreach ($scope in @('Process', 'User', 'Machine')) {
                        if ($null -ne [Environment]::GetEnvironmentVariable($variable, $scope)) { $present = $true }
                    }
                    $result.Environment += "$field`: $variable (present: $present)`r`n"
                    if ($result.Enabled -and $field -eq 'disable_environment' -and $present) {
                        $result.Status += ' / environment blocked'
                    }
                    if ($result.Enabled -and $field -eq 'enable_environment' -and -not $present) {
                        $result.Status += ' / enable flag absent'
                    }
                }
            }
        }
    } catch { $result.Status += ' / invalid manifest'; $result.Description = $_.Exception.Message }
    return [pscustomobject]$result
}

$script:fixturePath = ''
$script:fixtureDirectory = ''
if ($SelfTest) {
    if ([string]::IsNullOrWhiteSpace($TestOutput)) { throw 'SelfTest requires a private TestOutput directory.' }
    $TestOutput = [IO.Path]::GetFullPath($TestOutput)
    [IO.Directory]::CreateDirectory($TestOutput) | Out-Null
    $script:fixturePath = 'SOFTWARE\SteadyXR\Tests\Editor-' + [Guid]::NewGuid().ToString('N')
    $script:fixtureDirectory = Join-Path $TestOutput ('fixture-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($script:fixtureDirectory) | Out-Null
    $script:store = New-Object SteadyXR.RegistryStore($script:fixturePath, $true)
    $fixtureKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($script:fixturePath + '\Implicit')
    foreach ($entryName in @('Motion Compensation', 'Example Overlay', 'SteadyXR')) {
        $filename = if ($entryName -eq 'SteadyXR') { 'XR_APILAYER_STEADYXR_preview.json' } else { $entryName + '.json' }
        $manifest = Join-Path $script:fixtureDirectory $filename
        $technical = if ($entryName -eq 'SteadyXR') { 'XR_APILAYER_HORIZONLOCK_preview' } else { 'XR_APILAYER_TEST_' + $entryName.Replace(' ', '_') }
        @{ file_format_version = '1.0.0'; api_layer = @{ name = $technical; library_path = '.\fixture.dll'; description = 'Private editor test fixture'; disable_environment = 'STEADYXR_PRIVATE_TEST_DISABLE' } } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding UTF8
        $fixtureKey.SetValue($manifest, 0, [Microsoft.Win32.RegistryValueKind]::DWord)
    }
    $fixtureKey.Close()
    [IO.File]::WriteAllBytes((Join-Path $script:fixtureDirectory 'fixture.dll'), [byte[]]@(0))
} else { $script:store = New-Object SteadyXR.RegistryStore }
$script:is64Bit = $true
$script:before = $script:store.Read($script:is64Bit)
$script:draft = $script:store.Clone($script:before)
$versionDirectory = Split-Path $PSScriptRoot -Parent
$script:backupDirectory = if ($SelfTest) { Join-Path $script:fixtureDirectory 'backups' } else {
    Join-Path (Split-Path $versionDirectory -Parent) 'Layer-Backups'
}
$script:ownManifest = Join-Path $versionDirectory 'XR_APILAYER_STEADYXR_preview.json'
if ($SelfTest) { $script:ownManifest = Join-Path $script:fixtureDirectory 'XR_APILAYER_STEADYXR_preview.json' }
$script:undoBefore = $null
$script:undoExpected = $null
$script:lastBackup = ''
$script:pending = $false
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$script:isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if ($Report) {
    $rows = foreach ($group in $script:before) {
        foreach ($value in $group.Values) {
            $details = Get-LayerDetails $value $group
            [pscustomobject]@{ Scope = $group.Hive; Is64Bit = $group.Is64Bit; Group = $group.SubKey; Name = $details.Name;
                Status = $details.Status; Manifest = $details.Manifest; Library = $details.Library; Error = $group.Error }
        }
    }
    $rows | ConvertTo-Json -Depth 5
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot 'LayerEditorTheme.cs') -ReferencedAssemblies System.Windows.Forms,System.Drawing
[void][SteadyXR.LayerEditorTheme]::SetProcessDPIAware()
[Windows.Forms.Application]::EnableVisualStyles()
$display = [Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
try { $dpiScale = $display.DpiX / 96.0 } finally { $display.Dispose() }
function Scale-Unit([int]$value) { return [int][Math]::Round($value * $dpiScale) }
$highContrast = [Windows.Forms.SystemInformation]::HighContrast
function Get-Color([string]$hex, [Drawing.Color]$accessible) {
    if ($highContrast) { return $accessible }
    return [Drawing.ColorTranslator]::FromHtml($hex)
}
$chrome = Get-Color '#151517' ([Drawing.SystemColors]::Control)
$canvas = Get-Color '#1c1c1f' ([Drawing.SystemColors]::Window)
$rail = Get-Color '#19191c' ([Drawing.SystemColors]::Control)
$card = Get-Color '#242428' ([Drawing.SystemColors]::Window)
$field = Get-Color '#2a2d32' ([Drawing.SystemColors]::Control)
$line = Get-Color '#35353c' ([Drawing.SystemColors]::WindowText)
$textColor = Get-Color '#f4f6f8' ([Drawing.SystemColors]::WindowText)
$muted = Get-Color '#aab0b9' ([Drawing.SystemColors]::WindowText)
$blue = Get-Color '#5e9eff' ([Drawing.SystemColors]::Highlight)
$script:form = New-Object Windows.Forms.Form
$script:form.SuspendLayout()
$script:form.Text = 'SteadyXR - OpenXR layers'
$script:form.BackColor = $canvas
$script:form.ForeColor = $textColor
$script:form.Font = New-Object Drawing.Font('Segoe UI', 10)
$script:form.AutoScaleMode = 'None'
$script:form.ClientSize = New-Object Drawing.Size((Scale-Unit 1100), (Scale-Unit 692))
$script:form.MinimumSize = New-Object Drawing.Size((Scale-Unit 440), (Scale-Unit 330))
$script:form.StartPosition = 'CenterScreen'
$icon = Join-Path $versionDirectory 'SteadyXR.ico'
if (-not (Test-Path -LiteralPath $icon)) { $icon = Join-Path $versionDirectory 'assets\SteadyXR.ico' }
if (Test-Path -LiteralPath $icon) { $script:form.Icon = New-Object Drawing.Icon($icon) }

function Place-Control($control, $parent, [int]$x, [int]$y, [int]$width, [int]$height, [string]$anchor) {
    $control.SetBounds((Scale-Unit $x), (Scale-Unit $y), (Scale-Unit $width), (Scale-Unit $height))
    $parent.Controls.Add($control)
    if ($anchor) { $control.Anchor = $anchor }
}
function Add-Panel($parent, $color, $x, $y, $width, $height, $anchor) {
    $surface = New-Object Windows.Forms.Panel
    $surface.BackColor = $color
    Place-Control $surface $parent $x $y $width $height $anchor
    return $surface
}
function Add-Label($parent, [string]$caption, $x, $y, $width, $height, $fontSize, $color, [switch]$Strong) {
    $label = New-Object Windows.Forms.Label
    $label.Text = $caption
    $label.TextAlign = 'MiddleLeft'
    $label.ForeColor = $color
    $family = if ($Strong) { 'Segoe UI Semibold' } else { 'Segoe UI' }
    $label.Font = New-Object Drawing.Font($family, $fontSize)
    $label.AutoEllipsis = $true
    Place-Control $label $parent $x $y $width $height
    return $label
}
function Add-Button($parent, [string]$caption, $x, $y, $width, [scriptblock]$action, [switch]$Primary) {
    $button = New-Object Windows.Forms.Button
    $button.Text = $caption
    Place-Control $button $parent $x $y $width 36
    [SteadyXR.LayerEditorTheme]::StyleButton($button, $Primary.IsPresent)
    $button.Add_Click($action)
    return $button
}

# The full surface remains reachable by scrolling on small displays or with larger system fonts.
$viewport = New-Object Windows.Forms.Panel
$viewport.Dock = 'Fill'
$viewport.AutoScroll = $true
$viewport.AutoScrollMinSize = $script:form.ClientSize
$script:form.Controls.Add($viewport)
$shell = Add-Panel $viewport $canvas 0 0 1100 692
$shell.Dock = 'Fill'
$header = Add-Panel $shell $chrome 0 0 1100 60 'Top, Left, Right'
$logoPath = Join-Path (Split-Path $icon -Parent) 'SteadyXR.png'
if (Test-Path -LiteralPath $logoPath) {
    $logo = New-Object Windows.Forms.PictureBox
    $logo.Image = [Drawing.Image]::FromFile($logoPath)
    $logo.SizeMode = 'Zoom'
    Place-Control $logo $header 22 14 32 32
}
$null = Add-Label $header 'SteadyXR' 62 0 126 58 16 $textColor -Strong
$null = Add-Panel $header $line 207 17 1 26
$null = Add-Label $header ([string][char]0x25cf) 230 0 18 58 10 $blue
$null = Add-Label $header 'Layer editor' 250 0 220 58 12 $textColor -Strong
$application = Join-Path $versionDirectory 'SteadyXR.exe'
if (Test-Path -LiteralPath $application -PathType Leaf) {
    $productVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($application).ProductVersion
    if ($productVersion) {
        $versionLabel = Add-Label $header ('v' + $productVersion) 924 0 152 58 10 $muted
        $versionLabel.TextAlign = 'MiddleRight'
        $versionLabel.Anchor = 'Top, Right'
    }
}
$headerDivider = Add-Panel $header $line 0 0 1100 1
$headerDivider.Dock = 'Bottom'

$sidebar = Add-Panel $shell $rail 0 60 208 632 'Top, Bottom, Left'
$null = Add-Panel $sidebar $line 207 0 1 632 'Top, Bottom, Right'
$null = Add-Label $sidebar 'ARCHITECTURE' 22 17 166 30 9 $muted -Strong
$architecture = Add-Panel $sidebar $rail 14 54 180 84
function Add-Architecture([string]$caption, [int]$y) {
    $choice = New-Object Windows.Forms.RadioButton
    $choice.Appearance = 'Button'
    $choice.Text = $caption
    Place-Control $choice $architecture 0 $y 180 38
    [SteadyXR.LayerEditorTheme]::StyleButton($choice, $false)
    return $choice
}
$view64 = Add-Architecture '64-bit (iRacing)' 0
$view32 = Add-Architecture '32-bit applications' 44
$view64.Checked = $true
$null = Add-Label $sidebar 'REGISTRATION' 22 158 166 30 9 $muted -Strong
$script:register = Add-Button $sidebar 'Install / repair SteadyXR' 14 197 180 { Register-OwnLayer }
$script:admin = Add-Button $sidebar 'Administrator editor...' 14 241 180 { Open-AdminEditor }
$null = Add-Label $sidebar 'RECOVERY' 22 303 166 30 9 $muted -Strong
$script:undo = Add-Button $sidebar 'Undo last apply' 14 342 180 { Undo-Plan }
$script:restore = Add-Button $sidebar 'Restore backup...' 14 386 180 { Restore-Backup }

$null = Add-Label $shell 'OpenXR layers' 232 76 540 46 23 $textColor -Strong
$script:runtime = Add-Label $shell '' 234 125 834 25 10 $muted
$script:runtime.Anchor = 'Top, Left, Right'

$registration = Add-Panel $shell $card 232 166 844 272 'Top, Bottom, Left, Right'
[SteadyXR.LayerEditorTheme]::StyleCard($registration)
$null = Add-Label $registration 'Registered layers' 18 10 330 36 14 $textColor -Strong
$script:refresh = Add-Button $registration 'Refresh' 736 11 90 { Read-Layers }
$script:refresh.Anchor = 'Top, Right'
$null = Add-Label $registration 'Implicit layers load in order within each scope, system first.' 18 48 780 24 10 $muted

$script:grid = New-Object Windows.Forms.DataGridView
Place-Control $script:grid $registration 18 83 808 126 'Top, Bottom, Left, Right'
$script:grid.AccessibleName = 'Registered OpenXR layers'
$script:grid.BackgroundColor = $canvas
$script:grid.BorderStyle = 'None'
$script:grid.AllowUserToAddRows = $false; $script:grid.AllowUserToDeleteRows = $false
$script:grid.AllowUserToResizeRows = $false; $script:grid.RowHeadersVisible = $false
$script:grid.ReadOnly = $true; $script:grid.MultiSelect = $false; $script:grid.SelectionMode = 'FullRowSelect'
$script:grid.AutoGenerateColumns = $false; $script:grid.EnableHeadersVisualStyles = $false
$script:grid.ColumnHeadersBorderStyle = 'None'
$script:grid.ColumnHeadersHeightSizeMode = 'DisableResizing'
$script:grid.ColumnHeadersHeight = Scale-Unit 30
$script:grid.ColumnHeadersDefaultCellStyle.BackColor = $field
$script:grid.ColumnHeadersDefaultCellStyle.ForeColor = $muted
$script:grid.DefaultCellStyle.BackColor = $canvas; $script:grid.DefaultCellStyle.ForeColor = $textColor
$script:grid.DefaultCellStyle.SelectionBackColor = Get-Color '#223147' ([Drawing.SystemColors]::Highlight)
$script:grid.DefaultCellStyle.SelectionForeColor = Get-Color '#dbeaff' ([Drawing.SystemColors]::HighlightText)
$script:grid.DefaultCellStyle.Padding = New-Object Windows.Forms.Padding((Scale-Unit 6), 0, (Scale-Unit 4), 0)
$script:grid.ColumnHeadersDefaultCellStyle.Padding = $script:grid.DefaultCellStyle.Padding
$script:grid.CellBorderStyle = 'SingleHorizontal'
$script:grid.GridColor = $line; $script:grid.RowTemplate.Height = Scale-Unit 32
foreach ($spec in @(@('Order', 70), @('Layer', 208), @('Status', 226), @('Scope', 149), @('Type', 109))) {
    $column = New-Object Windows.Forms.DataGridViewTextBoxColumn
    $column.HeaderText = $spec[0]; $column.FillWeight = $spec[1]
    $column.AutoSizeMode = 'Fill'; $column.SortMode = 'NotSortable'
    $script:grid.Columns.Add($column) | Out-Null
}
$script:up = Add-Button $registration 'Move up' 18 222 100 { Move-Layer -1 }
$script:down = Add-Button $registration 'Move down' 126 222 110 { Move-Layer 1 }
$script:toggle = Add-Button $registration 'Disable' 249 222 100 { Toggle-Layer }
$script:remove = Add-Button $registration 'Remove registration' 357 222 165 { Remove-Layer }
foreach ($button in @($script:up, $script:down, $script:toggle, $script:remove)) { $button.Anchor = 'Bottom, Left' }

$details = Add-Panel $shell $card 232 450 844 162 'Bottom, Left, Right'
[SteadyXR.LayerEditorTheme]::StyleCard($details)
$detailTitle = Add-Label $details 'Layer details' 18 7 808 31 14 $textColor -Strong
$detailTitle.Anchor = 'Top, Left, Right'
function Add-Readout($parent, $name, $x, $y, $width, $height, $anchor) {
    $readout = New-Object Windows.Forms.TextBox
    $readout.AccessibleName = $name
    $readout.ReadOnly = $true; $readout.Multiline = $true
    $readout.BorderStyle = 'None'; $readout.ScrollBars = 'Vertical'
    $readout.BackColor = $parent.BackColor; $readout.ForeColor = $muted
    Place-Control $readout $parent $x $y $width $height $anchor
    return $readout
}
$script:detailBox = Add-Readout $details 'Layer details' 18 44 808 109 'Top, Bottom, Left, Right'
$footer = Add-Panel $shell $chrome 208 624 892 68 'Bottom, Left, Right'
$null = Add-Panel $footer $line 0 0 892 1 'Top, Left, Right'
$script:status = Add-Readout $footer 'Editor status' 24 13 456 46 'Top, Left, Right'
$script:apply = Add-Button $footer 'Apply changes' 499 16 128 { Apply-Plan } -Primary
$script:discard = Add-Button $footer 'Discard changes' 635 16 137 { Discard-Plan }
$script:close = Add-Button $footer 'Close' 780 16 88 { $script:form.Close() }
foreach ($button in @($script:apply, $script:discard, $script:close)) { $button.Anchor = 'Top, Right' }

$script:form.ResumeLayout($true)
foreach ($control in @($script:form, $viewport, $script:grid, $script:detailBox, $script:status)) {
    $control.Add_HandleCreated({ [SteadyXR.LayerEditorTheme]::ThemeNative($this) })
    if ($control.IsHandleCreated) { [SteadyXR.LayerEditorTheme]::ThemeNative($control) }
}
$workArea = [Windows.Forms.Screen]::FromControl($script:form).WorkingArea
$script:form.Size = New-Object Drawing.Size([Math]::Min($script:form.Width, $workArea.Width), [Math]::Min($script:form.Height, $workArea.Height))

function Set-Status([string]$message) { $script:status.Text = $message }
function Get-Selection {
    if ($script:grid.SelectedRows.Count -eq 0) { return $null }
    return $script:grid.SelectedRows[0].Tag
}
function Update-Actions {
    if ($script:renderingGrid) { return }
    $selection = Get-Selection
    $valid = $null -ne $selection
    $editable = $valid -and ($selection.Group.Hive -eq 'CurrentUser' -or $script:isAdmin)
    $script:toggle.Enabled = $editable -and $selection.Details.IsDword
    if (-not $SelfTest -and $valid -and $selection.Group.Hive -eq 'CurrentUser' -and
        -not $selection.Details.Enabled -and
        $selection.Details.Manifest -match '(?i)[\\/]XR_APILAYER_(STEADYXR|HORIZONLOCK)_preview\.json$') {
        $script:toggle.Enabled = $false
    }
    $script:remove.Enabled = $editable
    $script:up.Enabled = $editable -and $selection.Implicit -and $selection.ValueIndex -gt 0
    $script:down.Enabled = $editable -and $selection.Implicit -and ($selection.ValueIndex + 1) -lt $selection.Group.Values.Count
    $script:toggle.Text = if ($valid -and $selection.Details.Enabled) { 'Disable' } else { 'Enable' }
    if ($valid) {
        $note = if (-not $editable) { 'Open the administrator editor to modify system-wide registrations.' }
                elseif (-not $selection.Implicit) { 'Explicit layer order is controlled by the game or XR_ENABLE_API_LAYERS, not this list.' }
                else { 'Move within this scope only. Removing a registration keeps its manifest and DLL files.' }
        $script:detailBox.Text = $selection.Details.TechnicalName + "`r`nManifest: " + $selection.Details.Manifest +
            "`r`nLibrary: " + $selection.Details.Library + "`r`n" + $selection.Details.Description + "`r`n" +
            $selection.Details.Environment + $note
    } else { $script:detailBox.Text = 'Select a registration to see the full manifest and DLL paths.' }
    $script:pending = -not $script:store.Same($script:before, $script:draft)
    $script:apply.Enabled = $script:pending
    $script:discard.Enabled = $script:pending
    $script:refresh.Enabled = -not $script:pending
    $architecture.Enabled = -not $script:pending
    $script:undo.Enabled = -not $script:pending -and $null -ne $script:undoBefore -and $script:undoBefore[0].Is64Bit -eq $script:is64Bit
    $script:restore.Enabled = -not $script:pending
    $script:admin.Enabled = -not $script:isAdmin -and -not $script:pending -and -not $SelfTest
    $ownExists = $false
    foreach ($group in $script:draft) { foreach ($value in $group.Values) {
        foreach ($ownFilename in @('XR_APILAYER_STEADYXR_preview.json', 'XR_APILAYER_HORIZONLOCK_preview.json')) {
            if ($value.Name.Replace('/', '\').EndsWith(('\' + $ownFilename), [StringComparison]::OrdinalIgnoreCase)) { $ownExists = $true }
        }
    } }
    $script:register.Enabled = $script:is64Bit -and -not $ownExists -and (Test-Path -LiteralPath $script:ownManifest -PathType Leaf)
    if (-not $SelfTest) {
        $script:register.Text = 'Install / repair SteadyXR'
        $script:register.Enabled = $script:is64Bit -and -not $script:pending -and
            (Test-Path -LiteralPath (Join-Path $versionDirectory 'SteadyXR.exe') -PathType Leaf) -and
            (Test-Path -LiteralPath $script:ownManifest -PathType Leaf)
    }
}
function Render-Layers([int]$selectedRow = 0) {
    $script:renderingGrid = $true
    $script:grid.Rows.Clear()
    for ($g = 0; $g -lt $script:draft.Count; $g++) {
        $group = $script:draft[$g]
        for ($v = 0; $v -lt $group.Values.Count; $v++) {
            $value = $group.Values[$v]
            $details = Get-LayerDetails $value $group
            $implicit = $group.SubKey.EndsWith('\Implicit')
            $scope = if ($group.Hive -eq 'LocalMachine') { 'System-wide' } else { 'Current user' }
            $kind = if ($implicit) { 'Implicit' } else { 'Explicit' }
            $position = if ($implicit) { [string]($v + 1) } else { '-' }
            $row = $script:grid.Rows.Add([object[]]@($position, $details.Name, $details.Status, $scope, $kind))
            $script:grid.Rows[$row].Tag = [pscustomobject]@{ GroupIndex = $g; ValueIndex = $v; Group = $group; Details = $details; Implicit = $implicit }
        }
    }
    if ($script:grid.Rows.Count) {
        $selectedRow = [Math]::Max(0, [Math]::Min($selectedRow, $script:grid.Rows.Count - 1))
        $script:grid.ClearSelection(); $script:grid.Rows[$selectedRow].Selected = $true
        $script:grid.CurrentCell = $script:grid.Rows[$selectedRow].Cells[0]
    }
    $script:renderingGrid = $false
    Update-Actions
}
function Read-Layers {
    try {
        $script:before = $script:store.Read($script:is64Bit)
        $script:draft = $script:store.Clone($script:before)
        Render-Layers
        $errors = @($script:before | Where-Object { $_.Error } | ForEach-Object { $_.Error })
        Set-Status $(if ($errors.Count) { 'Some registrations could not be read: ' + ($errors -join '; ') } else {
            'Configured registry order shown. A running game keeps its current layer chain until restarted.'
        })
        $runtimeText = 'Unavailable'
        if ($SelfTest) { $runtimeText = 'Private test runtime' } else {
            $view = if ($script:is64Bit) { [Microsoft.Win32.RegistryView]::Registry64 } else { [Microsoft.Win32.RegistryView]::Registry32 }
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
            try {
                $key = $base.OpenSubKey('SOFTWARE\Khronos\OpenXR\1')
                if ($key) { try { $runtimeText = [string]$key.GetValue('ActiveRuntime') } finally { $key.Close() } }
            } finally { $base.Close() }
        }
        $script:runtime.Text = 'Registered runtime: ' + $runtimeText
    } catch { Set-Status ('Could not refresh layers: ' + $_.Exception.Message) }
}
function Toggle-Layer {
    $selected = Get-Selection
    if (-not $script:toggle.Enabled -or $null -eq $selected) { return }
    $selected.Group.Values[$selected.ValueIndex].Data = [BitConverter]::GetBytes([uint32]$(if ($selected.Details.Enabled) { 1 } else { 0 }))
    $index = $script:grid.SelectedRows[0].Index
    Render-Layers $index; Set-Status 'Change staged. Click Apply changes to save it for the next game launch.'
}
function Remove-Layer {
    $selected = Get-Selection
    if (-not $script:remove.Enabled -or $null -eq $selected) { return }
    $index = $script:grid.SelectedRows[0].Index
    $selected.Group.Values.RemoveAt($selected.ValueIndex)
    Render-Layers $index; Set-Status 'Removal staged. Apply changes unregisters the layer; its files remain on disk.'
}
function Move-Layer([int]$delta) {
    $selected = Get-Selection
    if ($null -eq $selected -or ($delta -eq -1 -and -not $script:up.Enabled) -or ($delta -eq 1 -and -not $script:down.Enabled)) { return }
    $value = $selected.Group.Values[$selected.ValueIndex]
    $selected.Group.Values.RemoveAt($selected.ValueIndex)
    $selected.Group.Values.Insert($selected.ValueIndex + $delta, $value)
    Render-Layers ($script:grid.SelectedRows[0].Index + $delta)
    Set-Status 'Order staged within this registry group. Apply checks that Windows retains the requested order.'
}
function Register-OwnLayer {
    if (-not $script:register.Enabled) { return }
    if (-not $SelfTest) {
        try {
            $process = Start-Process -FilePath (Join-Path $versionDirectory 'SteadyXR.exe') -WindowStyle Hidden -PassThru -Wait -ArgumentList @('--install-connection', $script:originUserSid)
            if ($process.ExitCode -ne 0) { throw ('Windows error ' + $process.ExitCode + '. No per-user registration was installed.') }
            Read-Layers
            Set-Status 'SteadyXR installed system-wide. Restart the OpenXR game to use this build.'
        } catch { Set-Status ('System-wide installation failed: ' + $_.Exception.Message) }
        return
    }
    $group = @($script:draft | Where-Object { $_.Hive -eq 'CurrentUser' -and $_.SubKey.EndsWith('\Implicit') })[0]
    $value = New-Object SteadyXR.LayerValue
    $value.Name = $script:ownManifest; $value.Kind = 4; $value.Data = [BitConverter]::GetBytes([uint32]0)
    $group.Values.Add($value); $group.Exists = $true
    Render-Layers ($script:grid.Rows.Count)
    Set-Status 'SteadyXR registration staged for this user. Click Apply changes, then restart the OpenXR game.'
}
function Discard-Plan {
    $script:draft = $script:store.Clone($script:before)
    Render-Layers; Set-Status 'Pending changes discarded.'
}
function Apply-Plan {
    try {
        $original = $script:store.Clone($script:before)
        $script:lastBackup = $script:store.Apply($script:before, $script:draft, $script:backupDirectory)
        $script:undoBefore = $original
        $script:undoExpected = $script:store.Read($script:is64Bit)
        $script:before = $script:store.Clone($script:undoExpected)
        $script:draft = $script:store.Clone($script:before)
        Render-Layers
        Set-Status ('Changes saved. Restart the OpenXR game to use them. Backup: ' + $script:lastBackup)
    } catch { Set-Status ('Apply failed: ' + $_.Exception.Message + ' Your staged edits are still shown.') }
}
function Undo-Plan {
    if (-not $script:undo.Enabled) { return }
    try {
        $script:lastBackup = $script:store.Apply($script:undoExpected, $script:undoBefore, $script:backupDirectory)
        $script:undoBefore = $null; $script:undoExpected = $null
        Read-Layers; Set-Status 'Last apply undone. Restart the OpenXR game to use the restored configuration.'
    } catch { Set-Status ('Could not undo: ' + $_.Exception.Message) }
}
function Stage-Backup([string]$path) {
    $restored = $script:store.ReadBackup($path)
    if ($restored.Count -ne $script:before.Count -or @($restored | Where-Object { $_.Is64Bit -ne $script:is64Bit }).Count) {
        throw 'Choose the matching 32-bit or 64-bit view before loading this backup.'
    }
    $script:draft = $restored
    Render-Layers
    Set-Status 'Backup staged. Review the listed layers, then click Apply changes to restore them.'
}
function Restore-Backup {
    $dialog = New-Object Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose a SteadyXR layer backup'
    $dialog.Filter = 'SteadyXR layer backup (*.json)|*.json'
    if (Test-Path -LiteralPath $script:backupDirectory -PathType Container) { $dialog.InitialDirectory = $script:backupDirectory }
    try {
        if ($dialog.ShowDialog($script:form) -ne 'OK') { return }
        Stage-Backup $dialog.FileName
    } catch { Set-Status ('Could not stage this backup: ' + $_.Exception.Message) }
    finally { $dialog.Dispose() }
}
function Open-AdminEditor {
    try {
        $powershell = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'
        Start-Process -FilePath $powershell -Verb RunAs -WindowStyle Hidden -ArgumentList (
            '-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -OriginUserSid ' + $script:originUserSid) | Out-Null
        $script:form.Close()
    } catch { Set-Status ('Administrator editor was not opened: ' + $_.Exception.Message) }
}
$script:grid.Add_SelectionChanged({ Update-Actions })
$view64.Add_CheckedChanged({ if ($view64.Checked) { $script:is64Bit = $true; Read-Layers } })
$view32.Add_CheckedChanged({ if ($view32.Checked) { $script:is64Bit = $false; Read-Layers } })
$script:form.Add_FormClosing({
    if ($script:pending -and -not $SelfTest) {
        $answer = [Windows.Forms.MessageBox]::Show('Discard the pending layer changes and close?', 'SteadyXR', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { $_.Cancel = $true }
    }
})
Read-Layers

if ($SelfTest) {
    $testPassed = $false
    try {
        $identitySid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        if ((Resolve-OriginUserSid '') -ne $identitySid) { throw 'Default origin did not use the initiating identity.' }
        if ((Resolve-OriginUserSid 'S-1-5-21-101-202-303-1001') -ne 'S-1-5-21-101-202-303-1001') {
            throw 'Explicit origin changed to the active administrator identity.'
        }
        foreach ($invalidSid in @('not-a-sid', 'S-1-5-21-101-202-303-1001 -Report', 'BA')) {
            $rejected = $false
            try { $null = Resolve-OriginUserSid $invalidSid } catch { $rejected = $true }
            if (-not $rejected) { throw 'Invalid origin SID was accepted.' }
        }
        # Exercise both real launch boundaries without starting a process or requesting UAC.
        $originalForm = $script:form
        $savedOrigin = $script:originUserSid
        $script:originUserSid = 'S-1-5-21-101-202-303-1001'
        $expectedOrigin = $script:originUserSid
        $fakeForm = New-Object PSObject
        $fakeForm | Add-Member ScriptMethod Close { }
        function Start-Process {
            param([string]$FilePath, [string]$Verb, [string]$WindowStyle, $ArgumentList, [switch]$PassThru, [switch]$Wait)
            $script:testLaunchArguments = @($ArgumentList)
            return [pscustomobject]@{ ExitCode = 0 }
        }
        try {
            $script:form = $fakeForm
            Open-AdminEditor
            if (($script:testLaunchArguments -join ' ') -notmatch ('-OriginUserSid ' + [regex]::Escape($expectedOrigin))) {
                throw 'Administrator editor launch did not preserve the initiating user SID.'
            }
            $script:form = $originalForm
            $SelfTest = $false
            $script:register.Enabled = $true
            Register-OwnLayer
            if ($script:testLaunchArguments.Count -ne 2 -or $script:testLaunchArguments[0] -ne '--install-connection' -or
                $script:testLaunchArguments[1] -ne $expectedOrigin) {
                throw 'Registration helper targeted the administrator account instead of the initiating user.'
            }
        } finally {
            $SelfTest = $true
            $script:form = $originalForm
            $script:originUserSid = $savedOrigin
            Remove-Item Function:Start-Process
            Read-Layers
        }
        if ($script:grid.Rows.Count -ne 3) { throw 'Fixture rows were not displayed.' }
        $original = $script:store.Clone($script:before)
        Move-Layer 1
        if (-not $script:pending -or -not $script:store.Same($original, $script:store.Read($true))) { throw 'Move did not stay staged.' }
        Apply-Plan
        if ($script:pending -or $script:before[0].Values[1].Name -ne $original[0].Values[0].Name) { throw 'Apply did not reorder.' }
        Undo-Plan
        if (-not $script:store.Same($original, $script:store.Read($true))) { throw 'Undo did not restore order.' }
        Toggle-Layer
        if (-not $script:pending) { throw 'Toggle did not stage.' }
        Discard-Plan
        if ($script:pending) { throw 'Discard failed.' }
        Remove-Layer; Apply-Plan
        if ($script:before[0].Values.Count -ne 2) { throw 'Removal failed.' }
        Undo-Plan
        if (-not $script:store.Same($original, $script:store.Read($true))) { throw 'Undo removal failed.' }
        if ($script:register.Enabled) { throw 'Already registered SteadyXR must not be duplicated.' }
        $script:draft[0].Values[2].Name = Join-Path $script:fixtureDirectory 'XR_APILAYER_HORIZONLOCK_preview.json'
        Render-Layers 2
        if ($script:register.Enabled) { throw 'Legacy Horizon Lock registration must prevent a duplicate.' }
        Discard-Plan
        Render-Layers 2; Remove-Layer
        if (-not $script:register.Enabled) { throw 'Missing SteadyXR registration must be offered.' }
        Register-OwnLayer
        if ($script:pending) { throw 'Register did not restore the original bundled registration.' }
        Render-Layers 0; Toggle-Layer; Apply-Plan
        $restorePath = $script:lastBackup
        $view32.Checked = $true
        if ($script:undo.Enabled -or $script:register.Enabled) { throw '64-bit actions leaked into the 32-bit view.' }
        $view64.Checked = $true
        Stage-Backup $restorePath
        if (-not $script:pending) { throw 'Restore backup did not stage changes.' }
        Apply-Plan
        if (-not $script:store.Same($original, $script:store.Read($true))) { throw 'Restore backup failed.' }
        $explicitGroup = $script:draft[1]
        $explicitGroup.Exists = $true
        $explicitGroup.Values.Add($original[0].Values[0].Clone())
        Render-Layers 3
        if ($script:up.Enabled -or $script:down.Enabled) { throw 'Explicit layers must not offer registry ordering.' }
        Discard-Plan
        Render-Layers 2
        Move-Layer -1
        if (-not $script:pending -or $architecture.Enabled) { throw 'A staged move must lock the architecture selector.' }
        # Realize controls offscreen: a never-shown form paints a blank bitmap.
        $script:form.StartPosition = 'Manual'
        $script:form.ShowInTaskbar = $false
        $script:form.Location = New-Object Drawing.Point(([Windows.Forms.SystemInformation]::VirtualScreen.Right + 100), 0)
        $script:form.Show()
        [Windows.Forms.Application]::DoEvents()
        foreach ($control in $header.Controls) {
            if ($control -is [Windows.Forms.Label] -and $control.Bottom -gt $sidebar.Top - (Scale-Unit 1)) {
                throw "Header label '$($control.Text)' overlaps the divider."
            }
        }
        if ($footer.Right -gt $shell.Width -or $footer.Bottom -gt $shell.Height -or
            $script:close.Right -gt $footer.Width -or $script:refresh.Right -gt $registration.Width) {
            throw 'The scaled editor layout left an action outside the client surface.'
        }
        # Capture the entire client surface, including content that scrolls on smaller screens.
        $image = New-Object Drawing.Bitmap($shell.Width, $shell.Height)
        try {
            $shell.DrawToBitmap($image, (New-Object Drawing.Rectangle(0, 0, $image.Width, $image.Height)))
            $image.Save((Join-Path $TestOutput 'steadyxr-layer-editor.png'))
            for ($x = 0; $x -lt $shell.ClientSize.Width; $x++) {
                if ($image.GetPixel($x, $sidebar.Top - 1).ToArgb() -ne $line.ToArgb()) {
                    throw "Header divider is interrupted at column $x."
                }
            }
        }
        finally { $image.Dispose() }
        $script:form.ClientSize = New-Object Drawing.Size(650, 450)
        [Windows.Forms.Application]::DoEvents()
        if (-not $viewport.HorizontalScroll.Visible -or -not $viewport.VerticalScroll.Visible -or
            $shell.Width -lt $viewport.AutoScrollMinSize.Width -or $shell.Height -lt $viewport.AutoScrollMinSize.Height) {
            throw 'The layer editor must keep its full content reachable on a small display.'
        }
        $testPassed = $true
        'PASS: editor staging, ordering, apply, undo, toggle, discard, removal, own registration, backup restore, architecture/type boundaries and original-user SID handoff.'
    } finally {
        $script:form.Dispose()
        if ($script:fixturePath.StartsWith('SOFTWARE\SteadyXR\Tests\Editor-')) { [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($script:fixturePath, $false) }
        # Keep private artifacts in the requested ignored output directory for inspection.
    }
    if (-not $testPassed) { exit 1 }
} else {
    try { [Windows.Forms.Application]::Run($script:form) } finally { $script:form.Dispose() }
}
