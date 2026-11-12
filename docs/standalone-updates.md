# Standalone updates on Windows

SteadyXR checks the latest stable release on startup. A newer version with a compatible update package opens a confirmation dialog. Choose **Yes** to download it in the background, then close SteadyXR normally when you are ready to switch. The updater starts the new executable from its own version folder next to the old one. Use that new executable for future sessions.

Updates copy your app settings and joystick bindings after the previous app exits. Named presets in the shared `Presets` folder remain available. An original `Start-Preview.cmd` pointing at the previous folder follows the update. Custom launchers keep their existing target. For rollback, close the new version and open `SteadyXR.exe` in the previous folder. Installing an update leaves OpenXR registrations alone. If you want a different capture DLL active, use the app's connection controls and restart the game.

SteadyXR checks public releases directly through GitHub. You can use the app without a GitHub account or the GitHub CLI. If you are offline, keep using the current version. You can also download and extract a standalone ZIP manually. [SteadyXR releases](https://github.com/DSuzuki35/steadyXR/releases)

## Publishing the next version

Build Release x64 with the app and OpenXR layer targets. From PowerShell, run `scripts/package-signed-standalone.ps1 -Build <directory> -Destination <existing delivery directory>`. The signing certificate must be available through SimplySign. The script verifies timestamped Authenticode signatures on the executable, capture layer, updater and release receipt. It also creates a matching source ZIP and SHA-256 files. Pass the previous version folder to `-PreviousSettings` to copy the available user INIs into the local ready-to-run directory. A path to the main settings file also works. The ZIP contains only release files.

Create a stable GitHub release tagged `vX.Y.Z` or `X.Y.Z` and attach both update assets with these exact names.

- `SteadyXR-X.Y.Z-Standalone-Windows-x64.zip` contains the `SteadyXR-X.Y.Z` directory.
- `SteadyXR-X.Y.Z-Release.ps1` is a timestamped Authenticode receipt binding the ZIP name to its SHA-256 digest. SteadyXR reads it as data.

The updater accepts only the configured publisher certificate and checks the executable and capture DLL product versions. Before a certificate rollover, publish a transition build using the current certificate and update the pins for the following release.
