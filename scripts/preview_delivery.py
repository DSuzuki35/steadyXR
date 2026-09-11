"""Locate the previous viewer's settings without confusing them with layer settings."""
from pathlib import Path
import re


def _settings(folder: Path):
    for filename in ('SteadyXR.ini', 'preview.ini'):
        path = folder / filename
        if path.is_file():
            return path
    return None


def previous_viewer_settings(gaming: Path, destination: Path):
    own = _settings(destination)
    if own:
        return own
    launcher = gaming / 'Start-Preview.cmd'
    if launcher.is_file():
        match = re.search(r'%~dp0((?:Preview|SteadyXR)-\d+(?:\.\d+)*)[\\/]'
                          r'(?:SteadyXR|horizonlock-preview)\.exe',
                          launcher.read_text(encoding='utf-8', errors='replace'), re.IGNORECASE)
        if match:
            selected = _settings(gaming / match[1])
            if selected:
                return selected
    # Render-only saves to an older registered DLL must not win by modification time.
    candidates = []
    for folder in gaming.iterdir():
        match = re.fullmatch(r'(Preview|SteadyXR)-(\d+(?:\.\d+)*)', folder.name, re.IGNORECASE)
        if match:
            selected = _settings(folder)
            if selected:
                key = (tuple(int(part) for part in match[2].split('.')), match[1].lower() == 'steadyxr')
                candidates.append((key, selected))
    return max(candidates, key=lambda item: item[0])[1] if candidates else None
