import os
from pathlib import Path
import tempfile
import unittest
from preview_delivery import previous_viewer_settings


class ViewerSettingsTests(unittest.TestCase):
    def test_layer_render_save_cannot_replace_newer_viewer_framing(self):
        with tempfile.TemporaryDirectory(prefix='SteadyXR-delivery-test-') as fixture:
            gaming = Path(fixture)
            layer = gaming / 'Preview-0.6.1' / 'preview.ini'
            viewer = gaming / 'Preview-0.7.0' / 'preview.ini'
            for item in (layer, viewer):
                item.parent.mkdir()
                item.write_text('[Preview]\n')
            os.utime(layer, (200, 200))
            os.utime(viewer, (100, 100))
            (gaming / 'Start-Preview.cmd').write_text('@start "" "%~dp0Preview-0.7.0\\horizonlock-preview.exe"')
            destination = gaming / 'SteadyXR-0.7.0'
            self.assertEqual(previous_viewer_settings(gaming, destination), viewer)
            # Missing/custom launchers fall back to the newest release, not a render file's mtime.
            (gaming / 'Start-Preview.cmd').unlink()
            self.assertEqual(previous_viewer_settings(gaming, destination), viewer)

    def test_new_filename_wins_and_external_launcher_paths_are_ignored(self):
        with tempfile.TemporaryDirectory(prefix='SteadyXR-delivery-test-') as fixture:
            gaming = Path(fixture)
            folder = gaming / 'SteadyXR-0.7.0'
            folder.mkdir()
            current = folder / 'SteadyXR.ini'
            legacy = folder / 'preview.ini'
            current.write_text('new')
            legacy.write_text('old')
            os.utime(legacy, (200, 200))
            os.utime(current, (100, 100))
            (gaming / 'Start-Preview.cmd').write_text('@start "" "%~dp0..\\other\\SteadyXR.exe"')
            self.assertEqual(previous_viewer_settings(gaming, gaming / 'SteadyXR-0.8.0'), current)


if __name__ == '__main__':
    unittest.main()
