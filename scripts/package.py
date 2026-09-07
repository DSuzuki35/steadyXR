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
p.add_argument('--version', default='0.3.3')
p.add_argument('--delivery-dir', type=Path, help='Also deliver a ZIP and ready-to-copy Gaming-PC/Stream-PC folders here')
a = p.parse_args()
stage = root / 'dist' / f'VR-Stabilizer-{a.version}-Windows-x64'
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
for item in (root / 'licenses').glob('*'):
    if item.is_file():
        copy(item, Path('licenses') / item.name)
# Include corresponding project source so the binary package is self-contained.
for name in ['AGENTS.md', 'CMakeLists.txt', '.clang-format', '.gitignore', 'LICENSE', 'README.md', 'THIRD-PARTY-NOTICES.md']:
    copy(root / name, Path('source') / name)
for directory in ['include', 'src', 'plugin', 'sender', 'scripts', 'cmake', 'tests', 'docs', '.github', 'licenses']:
    for item in (root / directory).rglob('*'):
        if item.is_file() and '__pycache__' not in item.parts:
            copy(item, Path('source') / item.relative_to(root))
for name in ['Install-OBS.cmd', 'Uninstall-OBS.cmd', 'Start-Tracking.cmd', 'Start-Delay-Check.cmd']:
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
if a.delivery_dir:
    delivery = a.delivery_dir.resolve()
    delivery.mkdir(parents=True, exist_ok=True)
    # Copy only project files. Preserve previous ZIPs and any user-added files.
    for item in (archive, archive_sum):
        destination = delivery / item.name
        if item.resolve() != destination:
            shutil.copy2(item, destination)
    for folder, source in [('Gaming-PC/sender', stage / 'sender'),
                           ('Stream-PC', stage / 'obs-install')]:
        shutil.copytree(source, delivery / folder, dirs_exist_ok=True)
    for name in ['Start-Tracking.cmd', 'Start-Delay-Check.cmd']:
        shutil.copy2(stage / name, delivery / 'Gaming-PC' / name)
    shutil.copy2(root / 'docs/local-delivery-de.txt', delivery / 'LIESMICH.txt')
    print(f'Ready-to-use files: {delivery}')
print(archive)
print(f'{archive.stat().st_size:,} bytes; {len(checks)} files; ZIP integrity passed')
