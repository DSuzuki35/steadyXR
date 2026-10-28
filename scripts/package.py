#!/usr/bin/env python3
"""Package previously built Windows binaries; never silently build or publish."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
from preview_delivery import previous_viewer_settings

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, default=root / 'build-windows')
p.add_argument('--openvr', type=Path, required=True)
p.add_argument('--openxr', type=Path, help='Include the standalone preview and its pinned OpenXR source headers')
p.add_argument('--version', default='0.14.0')
p.add_argument('--openxr-layer', type=Path, help='Preserve an existing capture DLL byte for byte')
p.add_argument('--openxr-layer-sha256', help='Required expected SHA-256 for the preserved DLL')
p.add_argument('--openxr-layer-build', type=Path, help='Original build metadata, including capture expiry')
p.add_argument('--openxr-layer-source', type=Path, help='Original corresponding source ZIP for the preserved DLL')
p.add_argument('--delivery-dir', type=Path, help='Also deliver a ZIP and ready-to-copy Gaming-PC/Stream-PC folders here')
a = p.parse_args()
preserved = (a.openxr_layer, a.openxr_layer_sha256, a.openxr_layer_build, a.openxr_layer_source)
capture_metadata = None
if any(preserved):
    if not all(preserved) or not a.openxr:
        p.error('Preserving a capture DLL requires --openxr and all four --openxr-layer options')
    for path in (a.openxr_layer, a.openxr_layer_build, a.openxr_layer_source):
        if not path.is_file():
            raise SystemExit(f'Missing preserved capture input: {path}')
    digest = hashlib.sha256(a.openxr_layer.read_bytes()).hexdigest()
    if digest != a.openxr_layer_sha256.lower():
        raise SystemExit('Preserved capture DLL SHA-256 mismatch; no package was changed')
    capture_metadata = json.loads(a.openxr_layer_build.read_text(encoding='utf-8-sig'))
    capture_metadata.update(sha256=digest, preserved_binary=True)
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
for name in ['README.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'Install-OBS.cmd', 'Uninstall-OBS.cmd', 'Start-Tracking.cmd', 'Start-Delay-Check.cmd', 'scripts/install-obs.ps1', 'scripts/uninstall-obs.ps1', 'docs/obs-openxr-source.md', 'docs/openxr-overlays.md']:
    copy(root / name, name)
copy(a.openvr / 'LICENSE', 'licenses/OpenVR-LICENSE.txt')
copy(root / 'docs/spectator-features.md', 'docs/spectator-features.md')
copy(root / 'docs/release-0.14.0-validation.md', 'docs/release-0.14.0-validation.md')
for guide in ['viewer-director.md', 'telemetry.md']:
    copy(root / 'docs' / guide, Path('docs') / guide)
if a.openxr:
    for name in ['SteadyXR.exe', 'SteadyXR-OpenXR.dll']:
        src = a.openxr_layer if name == 'SteadyXR-OpenXR.dll' and a.openxr_layer else output(Path('preview') / name)
        copy(src, Path('preview') / name)
    if capture_metadata:
        (stage / 'preview/SteadyXR-OpenXR-build.json').write_text(
            json.dumps(capture_metadata, indent=2) + '\n', encoding='utf-8')
        copy(a.openxr_layer_source, Path('source/prebuilt-capture') / a.openxr_layer_source.name)
        (stage / 'source/prebuilt-capture/README.txt').write_text(
            'This release preserves the capture DLL from the accompanying source archive.\n'
            f'Original capture version: {capture_metadata.get("version", "unknown")}\n'
            f'SHA-256: {capture_metadata["sha256"]}\n'
            f'Capture expiry (UTC): {capture_metadata.get("expires_at_utc", "unknown")}\n'
            'Rebuilding that DLL changes its bytes and requires a new signature.\n', encoding='utf-8')
    copy(a.build / 'preview/XR_APILAYER_STEADYXR_preview.json', 'preview/XR_APILAYER_STEADYXR_preview.json')
    copy(a.build / 'preview/steadyxr-build.json', 'preview/SteadyXR-build.json')
    copy(root / 'preview/README.md', 'preview/README.md')
    copy(root / 'docs/spectator-features.md', 'preview/Spectator-Guide.md')
    copy(root / 'docs/viewer-director.md', 'preview/Viewer-Director-Guide.md')
    copy(root / 'docs/telemetry.md', 'preview/Telemetry-Guide.md')
    for name, old, new in [
        ('Spectator-Guide.md', '(viewer-director.md)', '(Viewer-Director-Guide.md)'),
        ('Viewer-Director-Guide.md', '(telemetry.md)', '(Telemetry-Guide.md)'),
    ]:
        guide = stage / 'preview' / name
        guide.write_text(guide.read_text(encoding='utf-8').replace(old, new), encoding='utf-8')
    for name in ['SteadyXR.ico', 'SteadyXR.png']:
        copy(root / 'preview/assets' / name, Path('preview') / name)
    copy(root / 'Start-Preview.cmd', 'Start-Preview.cmd')
    copy(root / 'Start-Layer-Editor.cmd', 'Start-Layer-Editor.cmd')
    for name in ['LayerEditor.ps1', 'RegistryStore.cs']:
        copy(root / 'preview/layers' / name, Path('preview/layers') / name)
    # Keep both OBS drop-in folders beside the standalone app in the ZIP and delivery.
    # This stage was recreated above; no user settings or installed OBS files are touched.
    shutil.copytree(stage / 'obs-install', stage / 'preview', dirs_exist_ok=True)
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
for name in ['AGENTS.md', 'CMakeLists.txt', '.clang-format', '.gitignore', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.md', 'TODO.md']:
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
    # Same-version corrections retain the complete prior binary/source package together.
    archive_files = (archive, archive_sum, source_archive, source_sum)
    replaced = any((delivery / item.name).is_file() and
                   (delivery / item.name).read_bytes() != item.read_bytes() for item in archive_files)
    if replaced:
        backup = delivery / 'Previous-packages' / (
            f'SteadyXR-{a.version}-' + datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f'))
        backup.mkdir(parents=True, exist_ok=False)
        for item in archive_files:
            previous = delivery / item.name
            if previous.is_file():
                shutil.copy2(previous, backup / item.name)
    # Copy only project files. Preserve previous ZIPs and any user-added files.
    for item in archive_files:
        destination = delivery / item.name
        if item.resolve() != destination:
            shutil.copy2(item, destination)
    for folder, source in [('Gaming-PC/sender', stage / 'sender'),
                           ('Stream-PC', stage / 'obs-install')]:
        shutil.copytree(source, delivery / folder, dirs_exist_ok=True)
    for name in ['Start-Tracking.cmd', 'Start-Delay-Check.cmd']:
        shutil.copy2(stage / name, delivery / 'Gaming-PC' / name)
    if a.openxr:
        preview_folder = delivery / 'Gaming-PC' / f'SteadyXR-{a.version}'
        shutil.copytree(stage / 'preview', preview_folder, dirs_exist_ok=True)
        # Keep the user's working OpenXR mode and tuning across viewer upgrades.
        # Do not register or replace their active OpenXR layer.
        if not (preview_folder / 'SteadyXR.ini').exists():
            settings = previous_viewer_settings(delivery / 'Gaming-PC', preview_folder)
            if settings:
                shutil.copy2(settings, preview_folder / 'SteadyXR.ini')
        (delivery / 'Gaming-PC/Start-Preview.cmd').write_text(
            '@echo off\nstart "" "%~dp0SteadyXR-' + a.version + '\\SteadyXR.exe"\n', encoding='ascii')
        (delivery / 'Gaming-PC/Start-Layer-Editor.cmd').write_text(
            '@echo off\nstart "" "%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" '
            '-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0SteadyXR-' +
            a.version + '\\layers\\LayerEditor.ps1"\n', encoding='ascii')
        shutil.copy2(stage / 'preview/README.md', delivery / 'Gaming-PC/Preview-Guide.md')
        shutil.copy2(stage / 'preview/README.md', delivery / 'Gaming-PC/Preview-Anleitung.md')
    shutil.copy2(root / 'docs/local-delivery-de.txt', delivery / 'LIESMICH.txt')
    shutil.copy2(root / 'docs/obs-openxr-source.md', delivery / 'OBS-OpenXR-Guide.md')
    shutil.copy2(root / 'docs/openxr-overlays.md', delivery / 'openxr-overlays.md')
    shutil.copy2(root / 'docs/spectator-features.md', delivery / 'Spectator-Guide.md')
    print(f'Ready-to-use files: {delivery}')
print(archive)
print(f'{archive.stat().st_size:,} bytes; {len(checks)} files; ZIP integrity passed')
