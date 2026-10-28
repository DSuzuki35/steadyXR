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
[Windows.Forms.Application]::EnableVisualStyles()
$bg = [Drawing.Color]::FromArgb(16, 21, 29)
$panel = [Drawing.Color]::FromArgb(24, 31, 42)
$field = [Drawing.Color]::FromArgb(33, 43, 56)
$textColor = [Drawing.Color]::FromArgb(231, 239, 248)
$accent = [Drawing.Color]::FromArgb(63, 218, 231)
$muted = [Drawing.Color]::FromArgb(153, 172, 191)
$script:form = New-Object Windows.Forms.Form
$script:form.Text = 'SteadyXR - OpenXR layers'
$script:form.BackColor = $bg
$script:form.ForeColor = $textColor
$script:form.Font = New-Object Drawing.Font('Segoe UI', 10)
$script:form.ClientSize = New-Object Drawing.Size(1120, 780)
$script:form.MinimumSize = New-Object Drawing.Size(1080, 790)
$script:form.StartPosition = 'CenterScreen'
$script:form.AutoScaleMode = 'Dpi'
$icon = Join-Path $versionDirectory 'SteadyXR.ico'
if (-not (Test-Path -LiteralPath $icon)) { $icon = Join-Path $versionDirectory 'assets\SteadyXR.ico' }
if (Test-Path -LiteralPath $icon) { $script:form.Icon = New-Object Drawing.Icon($icon) }

function Add-Label([string]$caption, [int]$x, [int]$y, [int]$width, [int]$height) {
    $label = New-Object Windows.Forms.Label
    $label.Text = $caption; $label.Location = New-Object Drawing.Point($x, $y)
    $label.Size = New-Object Drawing.Size($width, $height)
    $script:form.Controls.Add($label)
    return $label
}
function Add-Button([string]$caption, [int]$x, [int]$y, [int]$width, [scriptblock]$action) {
    $button = New-Object Windows.Forms.Button
    $button.Text = $caption; $button.Location = New-Object Drawing.Point($x, $y)
    $button.Size = New-Object Drawing.Size($width, 34)
    $button.BackColor = $field; $button.ForeColor = $textColor; $button.FlatStyle = 'Flat'
    $button.FlatAppearance.BorderColor = [Drawing.Color]::FromArgb(53, 66, 82)
    $button.Add_Paint({
        if (-not $this.Enabled) {
            $inside = New-Object Drawing.Rectangle(1, 1, ($this.Width - 2), ($this.Height - 2))
            $brush = New-Object Drawing.SolidBrush($this.BackColor)
            try { $_.Graphics.FillRectangle($brush, $inside) } finally { $brush.Dispose() }
            [Windows.Forms.TextRenderer]::DrawText($_.Graphics, $this.Text, $this.Font, $inside,
                [Drawing.Color]::FromArgb(119, 139, 159), ([Windows.Forms.TextFormatFlags]::HorizontalCenter -bor
                [Windows.Forms.TextFormatFlags]::VerticalCenter -bor [Windows.Forms.TextFormatFlags]::SingleLine))
        }
    })
    $button.Add_Click($action)
    $script:form.Controls.Add($button)
    return $button
}
$title = Add-Label 'SteadyXR' 22 16 300 36
$title.Font = New-Object Drawing.Font('Segoe UI Semibold', 22)
$title.ForeColor = $accent
$subtitle = Add-Label 'OPENXR LAYERS' 25 58 600 22
$subtitle.ForeColor = $muted
$script:runtime = Add-Label '' 25 86 1050 24
$script:runtime.ForeColor = $muted
$architecture = New-Object Windows.Forms.ComboBox
$architecture.Location = New-Object Drawing.Point(25, 121); $architecture.Size = New-Object Drawing.Size(220, 30)
$architecture.DropDownStyle = 'DropDownList'; $architecture.BackColor = $field; $architecture.ForeColor = $textColor
$architecture.Items.AddRange(@('64-bit (iRacing)', '32-bit applications'))
$architecture.SelectedIndex = 0; $script:form.Controls.Add($architecture)
$script:scopeHint = Add-Label 'Implicit layers: system first, then current user. Restart the game after changes.' 265 125 830 28

$script:grid = New-Object Windows.Forms.DataGridView
$script:grid.Location = New-Object Drawing.Point(25, 165)
$script:grid.Size = New-Object Drawing.Size(1070, 302)
$script:grid.Anchor = 'Top, Left, Right'
$script:grid.BackgroundColor = $panel; $script:grid.BorderStyle = 'None'
$script:grid.AllowUserToAddRows = $false; $script:grid.AllowUserToDeleteRows = $false
$script:grid.AllowUserToResizeRows = $false; $script:grid.RowHeadersVisible = $false
$script:grid.ReadOnly = $true; $script:grid.MultiSelect = $false; $script:grid.SelectionMode = 'FullRowSelect'
$script:grid.AutoGenerateColumns = $false; $script:grid.EnableHeadersVisualStyles = $false
$script:grid.ColumnHeadersDefaultCellStyle.BackColor = $field; $script:grid.ColumnHeadersDefaultCellStyle.ForeColor = $textColor
$script:grid.DefaultCellStyle.BackColor = $panel; $script:grid.DefaultCellStyle.ForeColor = $textColor
$script:grid.DefaultCellStyle.SelectionBackColor = [Drawing.Color]::FromArgb(32, 84, 96)
$script:grid.DefaultCellStyle.SelectionForeColor = $textColor
$script:grid.GridColor = $field; $script:grid.RowTemplate.Height = 32
foreach ($columnSpec in @(@('Position', 70), @('Layer', 275), @('Status', 280), @('Scope', 150), @('Type', 110))) {
    $column = New-Object Windows.Forms.DataGridViewTextBoxColumn
    $column.HeaderText = $columnSpec[0]; $column.Width = $columnSpec[1]; $column.SortMode = 'NotSortable'
    $script:grid.Columns.Add($column) | Out-Null
}
$script:grid.Columns[1].AutoSizeMode = 'Fill'
$script:form.Controls.Add($script:grid)

$script:toggle = Add-Button 'Disable' 25 481 110 { Toggle-Layer }
$script:remove = Add-Button 'Remove registration' 146 481 183 { Remove-Layer }
$script:up = Add-Button 'Move up' 340 481 106 { Move-Layer -1 }
$script:down = Add-Button 'Move down' 457 481 116 { Move-Layer 1 }
$script:register = Add-Button 'Register SteadyXR' 585 481 180 { Register-OwnLayer }
$script:admin = Add-Button 'Administrator editor...' 785 481 220 { Open-AdminEditor }
$script:detailBox = New-Object Windows.Forms.TextBox
$script:detailBox.Location = New-Object Drawing.Point(25, 529)
$script:detailBox.Size = New-Object Drawing.Size(1070, 120)
$script:detailBox.Multiline = $true; $script:detailBox.ReadOnly = $true; $script:detailBox.ScrollBars = 'Vertical'
$script:detailBox.BorderStyle = 'FixedSingle'; $script:detailBox.BackColor = $panel; $script:detailBox.ForeColor = $muted
$script:detailBox.Anchor = 'Top, Left, Right'; $script:form.Controls.Add($script:detailBox)
$script:status = New-Object Windows.Forms.TextBox
$script:status.Text = 'Choose a layer to inspect it. No registration changes until you click Apply changes.'
$script:status.Location = New-Object Drawing.Point(25, 659)
$script:status.Size = New-Object Drawing.Size(1070, 56)
$script:status.Multiline = $true; $script:status.ReadOnly = $true; $script:status.ScrollBars = 'Vertical'
$script:status.BorderStyle = 'None'; $script:status.BackColor = $bg; $script:status.ForeColor = $textColor
$script:status.Anchor = 'Top, Left, Right'; $script:form.Controls.Add($script:status)
$script:apply = Add-Button 'Apply changes' 25 726 158 { Apply-Plan }
$script:apply.BackColor = $accent; $script:apply.ForeColor = $bg
$script:discard = Add-Button 'Discard changes' 195 726 161 { Discard-Plan }
$script:refresh = Add-Button 'Refresh' 368 726 110 { Read-Layers }
$script:undo = Add-Button 'Undo last apply' 490 726 162 { Undo-Plan }
$script:restore = Add-Button 'Restore backup...' 665 726 175 { Restore-Backup }
$script:close = Add-Button 'Close' 985 726 110 { $script:form.Close() }
$script:close.Anchor = 'Top, Right'

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
$architecture.Add_SelectedIndexChanged({ $script:is64Bit = $architecture.SelectedIndex -eq 0; Read-Layers })
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
        $architecture.SelectedIndex = 1
        if ($script:undo.Enabled -or $script:register.Enabled) { throw '64-bit actions leaked into the 32-bit view.' }
        $architecture.SelectedIndex = 0
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
        Set-Status 'Choose a layer to inspect it. Changes are staged until you click Apply changes.'
        # Realize controls offscreen: a never-shown form paints a blank bitmap.
        $script:form.StartPosition = 'Manual'
        $script:form.ShowInTaskbar = $false
        $script:form.Location = New-Object Drawing.Point(([Windows.Forms.SystemInformation]::VirtualScreen.Right + 100), 0)
        $script:form.Show()
        [Windows.Forms.Application]::DoEvents()
        $image = New-Object Drawing.Bitmap($script:form.Width, $script:form.Height)
        try { $script:form.DrawToBitmap($image, (New-Object Drawing.Rectangle(0, 0, $image.Width, $image.Height))); $image.Save((Join-Path $TestOutput 'steadyxr-layer-editor.png')) }
        finally { $image.Dispose() }
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
