"""Exercise release packaging in a temporary fixture, without live installations."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='SteadyXR-package-test-')
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name)
        self.delivery = self.root / 'delivery'
        self.gaming = self.delivery / 'Gaming-PC'
        self.viewer = self.gaming / 'SteadyXR-0.9.0'
        self.previous = self.gaming / 'SteadyXR-0.8.0' / 'SteadyXR.ini'
        self.settings = b'[Preview]\r\nMode=OpenXR\r\nCropLeft=0.173\r\nFutureKey=keep me\r\n'
        self.write(self.previous, self.settings)
        self.write(self.gaming / 'Start-Preview.cmd',
                   b'@start "" "%~dp0SteadyXR-0.8.0\\SteadyXR.exe"\r\n')
        self.write(self.delivery / 'SteadyXR-0.8.0-Windows-x64.zip', b'previous release')
        required = [
            'AGENTS.md', 'CMakeLists.txt', '.clang-format', '.gitignore', 'LICENSE',
            'README.md', 'THIRD-PARTY-NOTICES.md', 'TODO.md',
            'Install-OBS.cmd', 'Uninstall-OBS.cmd', 'Start-Tracking.cmd',
            'Start-Delay-Check.cmd', 'Start-Preview.cmd', 'Start-Layer-Editor.cmd',
            'scripts/install-obs.ps1', 'scripts/uninstall-obs.ps1',
            'docs/obs-openxr-source.md', 'docs/openxr-overlays.md', 'docs/local-delivery-de.txt',
            'preview/README.md', 'preview/assets/SteadyXR.ico',
            'preview/assets/SteadyXR.png', 'preview/layers/LayerEditor.ps1',
            'preview/layers/RegistryStore.cs', 'preview/main.cpp',
            'plugin/openxr_source.cpp', 'plugin/data/locale/en-US.ini',
            'plugin/data/stabilize.effect', 'include/fixture.h', 'src/fixture.cpp',
        ]
        for name in required:
            self.write(self.root / name, name.encode('ascii'))
        for name in ('package.py', 'preview_delivery.py', 'package_tests.py'):
            shutil.copy2(Path(__file__).parent / name, self.root / 'scripts' / name)
        for name in ('plugin/vr-stabilizer.dll', 'vrst-sender.exe',
                     'preview/SteadyXR.exe', 'preview/SteadyXR-OpenXR.dll',
                     'preview/XR_APILAYER_STEADYXR_preview.json', 'preview/steadyxr-build.json'):
            self.write(self.root / 'build' / name, name.encode('ascii'))
        for name in ('bin/win64/openvr_api.dll', 'LICENSE'):
            self.write(self.root / 'openvr' / name, name.encode('ascii'))
        for name in ('include/openxr/openxr.h', 'include/openxr/openxr_platform.h',
                     'include/openxr/openxr_platform_defines.h',
                     'include/openxr/openxr_loader_negotiation.h', 'LICENSE', 'LICENSES/MIT.txt'):
            self.write(self.root / 'openxr' / name, name.encode('ascii'))

    @staticmethod
    def write(path, content):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def package(self):
        result = subprocess.run([
            sys.executable, str(self.root / 'scripts/package.py'),
            '--build', str(self.root / 'build'), '--version', '0.9.0',
            '--openvr', str(self.root / 'openvr'), '--openxr', str(self.root / 'openxr'),
            '--delivery-dir', str(self.delivery),
        ], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_consolidated_obs_dropins_ship_beside_app_and_in_complete_archive(self):
        # Missing consolidation would leave the user unable to copy OBS folders from the app.
        self.package()
        for folder in (self.viewer, self.delivery / 'Stream-PC'):
            self.assertTrue((folder / 'obs-plugins/64bit/vr-stabilizer.dll').is_file(),
                            f'OBS DLL must be directly available under {folder}')
            self.assertEqual((folder / 'obs-plugins/64bit/vr-stabilizer.dll').read_bytes(),
                             b'plugin/vr-stabilizer.dll')
            self.assertEqual((folder / 'data/obs-plugins/vr-stabilizer/stabilize.effect').read_bytes(),
                             b'plugin/data/stabilize.effect')
            self.assertEqual((folder / 'data/obs-plugins/vr-stabilizer/locale/en-US.ini').read_bytes(),
                             b'plugin/data/locale/en-US.ini')
        self.assertTrue((self.viewer / 'SteadyXR.exe').is_file())
        self.assertTrue((self.viewer / 'SteadyXR-OpenXR.dll').is_file())
        self.assertFalse((self.viewer / 'obs-install').exists())
        archive = self.delivery / 'SteadyXR-0.9.0-Windows-x64.zip'
        with zipfile.ZipFile(archive) as binary:
            prefix = 'SteadyXR-0.9.0-Windows-x64/'
            self.assertEqual(binary.read(prefix + 'preview/obs-plugins/64bit/vr-stabilizer.dll'),
                             b'plugin/vr-stabilizer.dll')
            self.assertEqual(binary.read(prefix + 'preview/data/obs-plugins/vr-stabilizer/stabilize.effect'),
                             b'plugin/data/stabilize.effect')
            self.assertNotIn(prefix + 'preview/SteadyXR.ini', binary.namelist())
            for line in binary.read(prefix + 'SHA256SUMS.txt').decode('ascii').splitlines():
                expected, name = line.split('  ', 1)
                self.assertEqual(hashlib.sha256(binary.read(prefix + name)).hexdigest(), expected)
        source_archive = self.delivery / 'SteadyXR-0.9.0-Source.zip'
        with zipfile.ZipFile(source_archive) as source:
            prefix = 'SteadyXR-0.9.0-Source/'
            for name in ('scripts/package.py', 'scripts/preview_delivery.py',
                         'scripts/package_tests.py', 'preview/main.cpp', 'plugin/openxr_source.cpp',
                         'third_party/openxr/include/openxr/openxr.h'):
                self.assertIn(prefix + name, source.namelist())
        for item in (archive, source_archive):
            expected, name = item.with_suffix('.zip.sha256').read_text().strip().split('  ', 1)
            self.assertEqual(name, item.name)
            self.assertEqual(hashlib.sha256(item.read_bytes()).hexdigest(), expected)

    def test_upgrade_preserves_current_ini_and_prior_version_packages(self):
        self.package()
        self.assertEqual((self.viewer / 'SteadyXR.ini').read_bytes(), self.settings)
        self.assertEqual(self.previous.read_bytes(), self.settings)
        self.assertIn('SteadyXR-0.9.0\\SteadyXR.exe',
                      (self.gaming / 'Start-Preview.cmd').read_text())
        self.assertIn('SteadyXR-0.9.0\\layers\\LayerEditor.ps1',
                      (self.gaming / 'Start-Layer-Editor.cmd').read_text())
        latest = b'[Preview]\r\nMode=OpenXR\r\nCropLeft=0.227\r\nFutureKey=latest tuning\r\n'
        (self.viewer / 'SteadyXR.ini').write_bytes(latest)
        binary_name = 'SteadyXR-0.9.0-Windows-x64.zip'
        previous_archive = (self.delivery / binary_name).read_bytes()
        self.write(self.root / 'build/plugin/vr-stabilizer.dll', b'corrected plugin')
        self.package()
        self.assertEqual((self.viewer / 'SteadyXR.ini').read_bytes(), latest)
        self.assertEqual(self.previous.read_bytes(), self.settings)
        self.assertEqual((self.delivery / 'SteadyXR-0.8.0-Windows-x64.zip').read_bytes(),
                         b'previous release')
        backups = list((self.delivery / 'Previous-packages').glob('SteadyXR-0.9.0-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / binary_name).read_bytes(), previous_archive)
        self.assertTrue((backups[0] / 'SteadyXR-0.9.0-Source.zip').is_file())
        self.assertTrue((backups[0] / 'SteadyXR-0.9.0-Source.zip.sha256').is_file())
        self.assertTrue((backups[0] / (binary_name + '.sha256')).is_file())


if __name__ == '__main__':
    unittest.main()
