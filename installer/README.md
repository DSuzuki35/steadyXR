# SteadyXR 1.1.0 Windows installer

This installer wraps the unchanged, signed SteadyXR 1.1.0 portable release.
It installs for the current Windows account, offers an installation folder
picker, creates a Start menu shortcut and optionally a desktop shortcut.
The English installer and uninstaller use Inno Setup's modern dark appearance.

The default location is `%LOCALAPPDATA%\Programs\SteadyXR`. The application
lives in its `SteadyXR-1.1.0` child folder. Shortcuts use the stable
`Start-Preview.cmd` launcher in the parent folder, so the existing in-app updater
can add a new version folder and redirect those shortcuts. Installing an older
setup does not reset a launcher already pointing at a newer version. Custom
launchers are preserved. No existing portable copy is automatically imported.
When upgrading a recognized older version in the same installation root, setup
copies its four settings INIs into the new version only when those files are
absent, before redirecting the launcher.

Existing `SteadyXR.ini`, `SteadyXR-ui.ini`, `SteadyXR-shortcuts.ini`, `preview.ini`
and the parent `Presets` folder are not included in the installer and are never
overwritten on reinstall. OBS integration and OpenXR connection installation
remain explicit actions in SteadyXR Settings. Setup does not change OBS, SteamVR
or OpenXR configuration. Close SteadyXR before installation or uninstallation.

Uninstall removes installer-owned files and shortcuts, including the stable
launcher. Personal settings, presets and newer version folders created by the
in-app updater remain. You can remove the remaining installation folder manually
when its contents are no longer needed. A remaining newer executable can also be
opened directly. Separately installed OBS integration remains in the selected
OBS installation and is removed separately.

If an enabled OpenXR connection points inside this installation, uninstall asks
you to remove that connection in SteadyXR Settings first. Close VR games before
doing so. The uninstaller only reads registry entries; it does not disable a
connection belonging to any other SteadyXR installation.

## Build from this source archive

This directory is self-contained. It needs Windows, PowerShell, the Windows SDK
signing tools, the SteadyXR code-signing certificate and an authenticated
SimplySign Desktop session. The public certificate alone cannot sign a release.

Run `./Build.ps1 -PayloadArchive 'path/to/SteadyXR-1.1.0-Standalone-Windows-x64.zip'`.
Omitting `-PayloadArchive` downloads the immutable portable release specified in
`release.json`. `-Destination` selects the finished installer directory.
`-BuildDirectory` selects ignored build intermediates. The source archive does
not contain the application payload or third-party compiler.

Build.ps1 verifies the original ZIP's pinned SHA256 and valid timestamped
Authenticode signatures on the application, OpenXR DLL, updater, OBS plugin and
OBS installer helper. It verifies product versions and rejects user-setting
paths in the input archive. It downloads official Inno Setup 7.1.1 if needed,
checks its **Pyrsys B.V.** signature, then installs that build tool for the current
user under the build directory without shortcuts or associations. A supplied
`-Compiler` must also have a valid official publisher signature.

The compiler signs both installer and embedded uninstaller using certificate
`89F7F00A4FD4BD66F5617F1202B8E7B9DD662247` and the Certum SHA256 timestamp service.
It compiles a separate test installer with an isolated AppId and mutex, installs
into a temporary custom folder, verifies signatures and payload byte identity,
checks stable shortcuts, settings preservation, launcher upgrade/downgrade
behavior and uninstall preservation. Test shortcuts remain inside the test
folder; production user shortcuts and application registrations are untouched.
Test build intermediates are retained for inspection. `-SkipTests` is available
for development, but should not be used for a delivered release.

The original portable archive and signed receipt remain available at the
[unchanged v1.1.0 release](https://github.com/DSuzuki35/steadyXR/releases/tag/v1.1.0).
Installer build references are in `release.json`.
Official [Inno Setup downloads](https://jrsoftware.org/isdl.php) and
[signature verification](https://jrsoftware.org/inno-setup/downloads/verify).

Installer scripts use the same license as the standalone SteadyXR application,
included in the verified portable payload. Inno Setup retains its own license;
no compiler binaries are redistributed in this source archive.
