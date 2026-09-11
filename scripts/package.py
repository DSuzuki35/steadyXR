#!/usr/bin/env python3
"""Package previously built Windows binaries; never silently build or publish."""
import argparse
import hashlib
from pathlib import Path
import shutil
import zipfile

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, default=root / 'build-windows')
p.add_argument('--openvr', type=Path, required=True)
p.add_argument('--openxr', type=Path, help='Include the standalone preview and its pinned OpenXR source headers')
p.add_argument('--version', default='0.7.0')
p.add_argument('--delivery-dir', type=Path, help='Also deliver a ZIP and ready-to-copy Gaming-PC/Stream-PC folders here')
a = p.parse_args()
stage = root / 'dist' / f'SteadyXR-{a.version}-Windows-x64'
if stage.resolve().parent != (root / 'dist').resolve():
    raise SystemExit('Invalid package version/path')
if stage.exists():
    shutil.rmtree(stage)

def copy(src, relative):
    if not src.is_file():
        raise SystemExit(f'Missing required build output: {src}')
    dest = stage / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)

# Accommodate both single-configuration and Visual Studio CMake layouts.
def output(relative):
    path = a.build / relative
    if path.is_file():
        return path
    return path.parent / 'Release' / path.name

copy(output(Path('plugin/vr-stabilizer.dll')), 'obs-plugin/bin/64bit/vr-stabilizer.dll')
copy(output(Path('vrst-sender.exe')), 'sender/vrst-sender.exe')
copy(a.openvr / 'bin/win64/openvr_api.dll', 'sender/openvr_api.dll')
for item in (root / 'plugin/data').rglob('*'):
    if item.is_file():
        copy(item, Path('obs-plugin/data') / item.relative_to(root / 'plugin/data'))
# Drop-in layout for users who installed manually into their OBS directory.
copy(output(Path('plugin/vr-stabilizer.dll')), 'obs-install/obs-plugins/64bit/vr-stabilizer.dll')
for item in (root / 'plugin/data').rglob('*'):
    if item.is_file():
        copy(item, Path('obs-install/data/obs-plugins/vr-stabilizer') / item.relative_to(root / 'plugin/data'))
for name in ['README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'Install-OBS.cmd', 'Uninstall-OBS.cmd', 'Start-Tracking.cmd', 'Start-Delay-Check.cmd', 'scripts/install-obs.ps1', 'scripts/uninstall-obs.ps1']:
    copy(root / name, name)
copy(a.openvr / 'LICENSE', 'licenses/OpenVR-LICENSE.txt')
if a.openxr:
    for name in ['horizonlock-preview.exe', 'horizonlock-openxr-layer.dll']:
        copy(output(Path('preview') / name), Path('preview') / name)
    copy(a.build / 'preview/XR_APILAYER_HORIZONLOCK_preview.json', 'preview/XR_APILAYER_HORIZONLOCK_preview.json')
    copy(root / 'preview/README.md', 'preview/README.md')
    for name in ['horizon-lock.ico', 'horizon-lock.png']:
        copy(root / 'preview/assets' / name, Path('preview') / name)
    copy(root / 'Start-Preview.cmd', 'Start-Preview.cmd')
    copy(root / 'Start-Layer-Editor.cmd', 'Start-Layer-Editor.cmd')
    for name in ['LayerEditor.ps1', 'RegistryStore.cs']:
        copy(root / 'preview/layers' / name, Path('preview/layers') / name)
    for name in ['include/openxr/openxr.h', 'include/openxr/openxr_platform.h',
                 'include/openxr/openxr_platform_defines.h', 'include/openxr/openxr_loader_negotiation.h',
                 'LICENSE', 'LICENSES/MIT.txt']:
        copy(a.openxr / name, Path('source/third_party/openxr') / name)
    copy(a.openxr / 'LICENSE', 'licenses/OpenXR-Apache-2.0.txt')
    copy(a.openxr / 'LICENSES/MIT.txt', 'licenses/OpenXR-MIT.txt')
for item in (root / 'licenses').glob('*'):
    if item.is_file():
        copy(item, Path('licenses') / item.name)
# Include corresponding project source so the binary package is self-contained.
for name in ['AGENTS.md', 'CMakeLists.txt', '.clang-format', '.gitignore', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.md']:
    copy(root / name, Path('source') / name)
for directory in ['include', 'src', 'plugin', 'sender', 'preview', 'scripts', 'cmake', 'tests', 'docs', '.github', 'licenses']:
    for item in (root / directory).rglob('*'):
        if item.is_file() and '__pycache__' not in item.parts:
            copy(item, Path('source') / item.relative_to(root))
for name in ['Install-OBS.cmd', 'Uninstall-OBS.cmd', 'Start-Tracking.cmd', 'Start-Delay-Check.cmd', 'Start-Preview.cmd', 'Start-Layer-Editor.cmd']:
    copy(root / name, Path('source') / name)
checks = []
for item in sorted(stage.rglob('*')):
    if item.is_file():
        checks.append(f'{hashlib.sha256(item.read_bytes()).hexdigest()}  {item.relative_to(stage).as_posix()}')
(stage / 'SHA256SUMS.txt').write_text('\n'.join(checks) + '\n')
archive = stage.parent / (stage.name + '.zip')
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for item in sorted(stage.rglob('*')):
        if item.is_file():
            z.write(item, Path(stage.name) / item.relative_to(stage))
with zipfile.ZipFile(archive) as z:
    if z.testzip():
        raise SystemExit('ZIP integrity check failed')
archive_sum = archive.with_suffix('.zip.sha256')
archive_sum.write_text(f'{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n')
source_archive = stage.parent / f'SteadyXR-{a.version}-Source.zip'
with zipfile.ZipFile(source_archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for item in sorted((stage / 'source').rglob('*')):
        if item.is_file():
            z.write(item, Path(f'SteadyXR-{a.version}-Source') / item.relative_to(stage / 'source'))
source_sum = source_archive.with_suffix('.zip.sha256')
source_sum.write_text(f'{hashlib.sha256(source_archive.read_bytes()).hexdigest()}  {source_archive.name}\n')
if a.delivery_dir:
    delivery = a.delivery_dir.resolve()
    delivery.mkdir(parents=True, exist_ok=True)
    # Copy only project files. Preserve previous ZIPs and any user-added files.
    for item in (archive, archive_sum, source_archive, source_sum):
        destination = delivery / item.name
        if item.resolve() != destination:
            shutil.copy2(item, destination)
    for folder, source in [('Gaming-PC/sender', stage / 'sender'),
                           ('Stream-PC', stage / 'obs-install')]:
        shutil.copytree(source, delivery / folder, dirs_exist_ok=True)
    for name in ['Start-Tracking.cmd', 'Start-Delay-Check.cmd']:
        shutil.copy2(stage / name, delivery / 'Gaming-PC' / name)
    if a.openxr:
        preview_folder = delivery / 'Gaming-PC' / f'Preview-{a.version}'
        shutil.copytree(stage / 'preview', preview_folder, dirs_exist_ok=True)
        # Keep the user's working OpenXR mode and tuning across viewer upgrades.
        # Do not register or replace their active OpenXR layer.
        if not (preview_folder / 'preview.ini').exists():
            settings = [item for item in (delivery / 'Gaming-PC').glob('Preview-*/preview.ini')
                        if item.is_file() and item.parent != preview_folder]
            if settings:
                shutil.copy2(max(settings, key=lambda item: item.stat().st_mtime), preview_folder / 'preview.ini')
        (delivery / 'Gaming-PC/Start-Preview.cmd').write_text(
            '@echo off\nstart "" "%~dp0Preview-' + a.version + '\\horizonlock-preview.exe"\n', encoding='ascii')
        (delivery / 'Gaming-PC/Start-Layer-Editor.cmd').write_text(
            '@echo off\nstart "" "%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" '
            '-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0Preview-' +
            a.version + '\\layers\\LayerEditor.ps1"\n', encoding='ascii')
        shutil.copy2(stage / 'preview/README.md', delivery / 'Gaming-PC/Preview-Guide.md')
        shutil.copy2(stage / 'preview/README.md', delivery / 'Gaming-PC/Preview-Anleitung.md')
    shutil.copy2(root / 'docs/local-delivery-de.txt', delivery / 'LIESMICH.txt')
    print(f'Ready-to-use files: {delivery}')
print(archive)
print(f'{archive.stat().st_size:,} bytes; {len(checks)} files; ZIP integrity passed')
