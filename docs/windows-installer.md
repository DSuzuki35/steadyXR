# Installing SteadyXR on Windows

Choose **SteadyXR-1.1.0-Setup-Windows-x64.exe** from the [1.1.0 release](https://github.com/DSuzuki35/steadyXR/releases/tag/v1.1.0). The installer and its uninstaller are signed. The portable ZIP remains available and contains the same application, OpenXR layer and OBS plugin.

## Installation

Close SteadyXR before installing. The default destination is `%LOCALAPPDATA%\Programs\SteadyXR`, for your Windows account. You can choose another writable folder. The installer creates a Start menu entry and can add a desktop shortcut. Use those shortcuts to start the application.

OpenXR registration and OBS integration remain separate actions in **SteadyXR → Settings**. The installer does not change your OBS, SteamVR or game configuration. Use **Install OBS integration…** to select your OBS installation folder when you want the direct OBS source.

## Updates and settings

The installer uses a version folder such as `SteadyXR-1.1.0` under the selected installation directory. The shortcuts use `Start-Preview.cmd` in the parent directory. SteadyXR's GitHub updater switches this launcher to the new version, so you can keep using the same shortcuts.

Reinstalling does not replace your settings, shortcuts or named presets. The GitHub updater copies your settings to the new version after you close the previous app. Named presets live in the shared `Presets` folder beside the version folders.

If you previously used a portable ZIP, keep it until you have checked your installed setup. Installing into a new location does not automatically import settings from unrelated portable folders. You can keep using the portable version, or copy its `SteadyXR.ini`, `SteadyXR-shortcuts.ini` and `SteadyXR-ui.ini` into the installed version folder while SteadyXR is closed; copy named presets into the shared `Presets` folder as well.

The OBS source's optional autostart setting contains a path to `SteadyXR.exe`. Check that path in OBS source properties after moving or updating SteadyXR.

## Uninstalling

Close SteadyXR and your VR game. If the OpenXR connection uses this installation, choose **Settings → Remove connection** before uninstalling. Then use Windows **Installed apps → SteadyXR → Uninstall**.

Uninstall removes files installed by Setup and its shortcuts. Settings, named presets and additional version folders downloaded later are kept. Once you no longer need those files, you can remove them manually from your chosen installation folder. The separately installed OBS plugin remains in OBS until you remove it there.

## Source

The release's original source ZIP corresponds to the application tag `v1.1.0`. Installer packaging was added afterward without changing those application binaries or the release tag. The separate **Installer-Source.zip** contains the installer build scripts and instructions; the repository's `installer/` directory contains the same sources.
