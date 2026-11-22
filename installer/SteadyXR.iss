; SteadyXR installer source. Same license as the standalone SteadyXR application.
; Compile using Build.ps1; the original signed portable payload is never changed.
#ifndef PayloadDir
  #error PayloadDir must name the verified signed portable payload.
#endif
#ifndef LauncherFile
  #error LauncherFile is required.
#endif
#ifndef AppVersion
  #define AppVersion "1.1.0"
#endif
#ifdef TestBuild
  #define InstallerAppId "SteadyXR-Installer-Isolated-Test"
  #define OutputName "SteadyXR-" + AppVersion + "-Setup-Test"
#else
  #define InstallerAppId "{{AD2AF759-67CB-4687-A7F8-3E3D7A6169EA}"
  #define OutputName "SteadyXR-" + AppVersion + "-Setup-Windows-x64"
#endif

[Setup]
AppId={#InstallerAppId}
AppName=SteadyXR
AppVersion={#AppVersion}
AppVerName=SteadyXR {#AppVersion}
AppPublisher=Dan Suzuki
AppPublisherURL=https://github.com/DSuzuki35/steadyXR
AppSupportURL=https://github.com/DSuzuki35/steadyXR/issues
AppUpdatesURL=https://github.com/DSuzuki35/steadyXR/releases
DefaultDirName={localappdata}\Programs\SteadyXR
DefaultGroupName=SteadyXR
DisableDirPage=no
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern dark includetitlebar
WizardSizePercent=110
DisableWelcomePage=no
SetupIconFile={#PayloadDir}\SteadyXR.ico
UninstallDisplayIcon={app}\SteadyXR.ico
LicenseFile={#PayloadDir}\LICENSE
OutputBaseFilename={#OutputName}
Compression=lzma2
SolidCompression=yes
SignTool=steadyxr
SignedUninstaller=yes
#ifdef TestBuild
AppMutex=Local\SteadyXR-Installer-Isolated-Test-App
#else
AppMutex=Local\HorizonLockVRPreview-Viewer-v1
#endif
SetupMutex=Local\SteadyXR-Setup-v1
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UninstallFilesDir={app}\uninstall
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=SteadyXR Windows installer
VersionInfoProductName=SteadyXR
VersionInfoProductVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}\SteadyXR-{#AppVersion}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\SteadyXR.ico"; DestDir: "{app}"; Flags: onlyifdoesntexist
Source: "{#LauncherFile}"; DestDir: "{app}"; Flags: ignoreversion; Check: ShouldInstallLauncher; BeforeInstall: MigrateManagedSettings
Source: "{#SourcePath}\README.md"; DestDir: "{app}"; DestName: "Installer-Readme.md"; Flags: ignoreversion

[Icons]
#ifdef TestBuild
Name: "{app}\test-shortcuts\SteadyXR"; Filename: "{app}\Start-Preview.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\SteadyXR.ico"
Name: "{app}\test-shortcuts\SteadyXR Desktop"; Filename: "{app}\Start-Preview.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\SteadyXR.ico"; Tasks: desktopicon
#else
Name: "{group}\SteadyXR"; Filename: "{app}\Start-Preview.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\SteadyXR.ico"
Name: "{autodesktop}\SteadyXR"; Filename: "{app}\Start-Preview.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\SteadyXR.ico"; Tasks: desktopicon
#endif

[Run]
Filename: "{app}\Start-Preview.cmd"; Description: "Launch SteadyXR"; Flags: shellexec postinstall skipifsilent nowait

[Code]
function ManagedLauncherVersion(): String;
var
  Lines: TArrayOfString;
  Prefix, Suffix, OldText: String;
  OldVersion: Int64;
begin
  Result := '';
  if not LoadStringsFromFile(ExpandConstant('{app}\Start-Preview.cmd'), Lines) then Exit;
  if GetArrayLength(Lines) <> 2 then Exit;
  if Lines[0] <> '@echo off' then Exit;
  Prefix := 'start "" "%~dp0SteadyXR-';
  Suffix := '\SteadyXR.exe"';
  if Copy(Lines[1], 1, Length(Prefix)) <> Prefix then Exit;
  if Copy(Lines[1], Length(Lines[1]) - Length(Suffix) + 1, Length(Suffix)) <> Suffix then Exit;
  OldText := Copy(Lines[1], Length(Prefix) + 1, Length(Lines[1]) - Length(Prefix) - Length(Suffix));
  if not StrToVersion(OldText + '.0', OldVersion) then Exit;
  Result := OldText;
end;

function ShouldInstallLauncher(): Boolean;
var
  OldText: String;
  OldVersion, NewVersion: Int64;
begin
  Result := True;
  if not FileExists(ExpandConstant('{app}\Start-Preview.cmd')) then Exit;
  Result := False;
  OldText := ManagedLauncherVersion();
  if (OldText = '') or not StrToVersion(OldText + '.0', OldVersion) then Exit;
  if not StrToVersion('{#AppVersion}.0', NewVersion) then Exit;
  Result := ComparePackedVersion(OldVersion, NewVersion) <= 0;
end;

procedure MigrateManagedSettings();
var
  Names: TArrayOfString;
  OldText, Previous, Destination: String;
  Index: Integer;
  OldVersion, NewVersion: Int64;
begin
  OldText := ManagedLauncherVersion();
  if (OldText = '') or not StrToVersion(OldText + '.0', OldVersion) then Exit;
  if not StrToVersion('{#AppVersion}.0', NewVersion) then Exit;
  if ComparePackedVersion(OldVersion, NewVersion) >= 0 then Exit;
  Names := ['SteadyXR.ini', 'SteadyXR-ui.ini', 'SteadyXR-shortcuts.ini', 'preview.ini'];
  for Index := 0 to GetArrayLength(Names) - 1 do begin
    Previous := ExpandConstant('{app}\SteadyXR-') + OldText + '\' + Names[Index];
    Destination := ExpandConstant('{app}\SteadyXR-{#AppVersion}\') + Names[Index];
    if FileExists(Previous) and not FileExists(Destination) then
      if not FileCopy(Previous, Destination, True) then
        RaiseException('Could not preserve existing settings. The launcher has not been changed.');
  end;
end;

function PathIsWithin(const FileName, DirectoryName: String): Boolean;
var
  FilePath, RootPath: String;
begin
  FilePath := Lowercase(ExpandFileName(FileName));
  RootPath := AddBackslash(Lowercase(ExpandFileName(DirectoryName)));
  Result := Pos(RootPath, FilePath) = 1;
end;

function HasEnabledInstalledLayer(RootKey: Integer): Boolean;
var
  Names: TArrayOfString;
  Index: Integer;
  Disabled: Cardinal;
  Key: String;
begin
  Result := False;
  Key := 'SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit';
  if not RegGetValueNames(RootKey, Key, Names) then Exit;
  for Index := 0 to GetArrayLength(Names) - 1 do
    if PathIsWithin(Names[Index], ExpandConstant('{app}')) then
      if RegQueryDWordValue(RootKey, Key, Names[Index], Disabled) and (Disabled = 0) then begin
        Result := True;
        Exit;
      end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := not (HasEnabledInstalledLayer(HKCU64) or HasEnabledInstalledLayer(HKLM64));
  if not Result then
    MsgBox('Remove the OpenXR connection in SteadyXR Settings, then retry uninstall.' + #13#10 + #13#10 +
      'Close running VR games first. This prevents an enabled OpenXR layer from pointing at deleted files.' + #13#10 +
      'Connections belonging to other SteadyXR folders are not changed.', mbError, MB_OK);
end;

procedure InitializeWizard();
begin
  WizardForm.WelcomeLabel2.Caption :=
    'Install SteadyXR for your Windows account.' + #13#10 + #13#10 +
    'Choose the installation folder and optional desktop shortcut. SteadyXR keeps its existing in-app updater.' + #13#10 + #13#10 +
    'OBS integration and the OpenXR connection remain optional steps in SteadyXR Settings.';
#ifdef TestBuild
  if not PathIsWithin('C:\SteadyXR\SteadyXR-1.1.0\layer.json', 'C:\SteadyXR') or
     PathIsWithin('C:\SteadyXR-other\layer.json', 'C:\SteadyXR') or
     PathIsWithin('C:\SteadyXR\..\portable\layer.json', 'C:\SteadyXR') or
     not PathIsWithin('C:\STEADYXR\layer.json', 'c:\steadyxr') then
    RaiseException('OpenXR path containment tests failed.');
  Log('PASS: OpenXR uninstall guard path containment');
#endif
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
    MsgBox('SteadyXR has been uninstalled.' + #13#10 + #13#10 +
      'Personal settings, presets, and version folders created by the in-app updater are kept in:' + #13#10 +
      ExpandConstant('{app}') + #13#10 + #13#10 +
      'You can remove that remaining folder manually when you no longer need its contents. Separately installed OBS integration is managed in your OBS installation folder.', mbInformation, MB_OK);
end;
