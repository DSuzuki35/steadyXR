#!/usr/bin/env python3
"""Fetch pinned upstream headers/libraries and prepare an OBS C import library."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile
import importlib.util

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--llvm-mingw', type=Path, help='Cross toolchain root on macOS/Linux')
a = p.parse_args()
deps = root / '.deps'
deps.mkdir(exist_ok=True)

def download(name, url, digest=None):
    path = deps / name
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temporary = path.with_name(path.name + '.part')
        print('Downloading', name, flush=True)
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(path)
    if digest and hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Checksum mismatch: {path}; remove it and retry')
    return path

source_tar = download('obs-source.tar.gz', 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Sources.tar.gz', 'ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4')
source = deps / 'obs-studio-32.2.2-sources'
if not (source / 'libobs/obs.h').exists():
    with tarfile.open(source_tar) as t:
        t.extractall(deps, filter='data')
windows_zip = download('obs-windows.zip', 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Windows-x64.zip', '4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1')
obs_dll = deps / 'obs-windows/bin/64bit/obs.dll'
if not obs_dll.exists():
    with zipfile.ZipFile(windows_zip) as z:
        z.extract('bin/64bit/obs.dll', deps / 'obs-windows')
openvr = deps / 'openvr-2.15.6'
for name in ['headers/openvr.h', 'headers/openvr_capi.h', 'lib/win64/openvr_api.lib', 'bin/win64/openvr_api.dll', 'LICENSE']:
    download(f'openvr-2.15.6/{name}', f'https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6/{name}')
include = deps / 'obs-sdk/include'
preview_spec = importlib.util.spec_from_file_location('preview_deps', root / 'scripts/prepare-preview-deps.py')
preview_deps = importlib.util.module_from_spec(preview_spec)
preview_spec.loader.exec_module(preview_deps)
preview_deps.prepare(deps / 'openxr-sdk')
lib = deps / 'obs-sdk/lib'
include.mkdir(parents=True, exist_ok=True)
lib.mkdir(parents=True, exist_ok=True)
(include / 'obsconfig.h').write_text('#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n')
definition = lib / 'obs.def'
if a.llvm_mingw:
    tools = a.llvm_mingw.resolve() / 'bin'
    exports = subprocess.check_output([str(tools / 'llvm-readobj'), '--coff-exports', str(obs_dll)], text=True)
    names = re.findall(r'^\s*Name: (\S+)', exports, flags=re.M)
    library = lib / 'libobs.dll.a'
    link = [str(tools / 'llvm-dlltool'), '-m', 'i386:x86-64', '-d', str(definition), '-l', str(library), '-D', 'obs.dll']
else:
    if not shutil.which('dumpbin') or not shutil.which('lib'):
        raise SystemExit('Run in an x64 Visual Studio Developer shell (dumpbin.exe and lib.exe required).')
    exports = subprocess.check_output(['dumpbin', '/nologo', '/exports', str(obs_dll)], text=True)
    names = re.findall(r'^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)', exports, flags=re.M)
    library = lib / 'obs.lib'
    link = ['lib', '/nologo', '/machine:x64', f'/def:{definition}', f'/out:{library}']
if 'obs_register_source_s' not in names or 'gs_effect_create_from_file' not in names:
    raise SystemExit('OBS export extraction failed')
definition.write_text('LIBRARY obs.dll\nEXPORTS\n' + '\n'.join(names) + '\n')
subprocess.run(link, check=True)
manifest = {'obs_source': str(source), 'obs_library': str(library), 'obs_generated': str(include), 'openvr': str(openvr), 'openxr': str(deps / 'openxr-sdk')}
(deps / 'paths.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
