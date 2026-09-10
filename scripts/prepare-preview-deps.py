#!/usr/bin/env python3
"""Fetch the pinned, checksum-verified OpenXR headers used by the preview."""
import argparse
import hashlib
from pathlib import Path
import urllib.request

COMMIT = '781f2eab3698d653c804ecbd11e0aed47eaad1c6'
FILES = {
    'include/openxr/openxr.h': '80145a0a76c6938b1de22b6846aa32cd8dcf28bc0f79ae84667e42219af05dba',
    'include/openxr/openxr_platform.h': '2a798e9b3ad64861711441197ec330715b123a3abcca01f34bcc17ccee3fd38a',
    'include/openxr/openxr_platform_defines.h': 'd9cb4c72dc145dd2403ad14cc0c212a0a1f8f057c20d4135b33b54e726b621ef',
    'include/openxr/openxr_loader_negotiation.h': '0b332352016be617596ce98166c00d6469ce89dfef91e5442eae243d5a986d73',
    'LICENSE': '3ddf9be5c28fe27dad143a5dc76eea25222ad1dd68934a047064e56ed2fa40c5',
    'LICENSES/MIT.txt': '0459805ebfeff6e7cbe7a1913fe79e75ad192965dcfebceac11ec4c233acf53c',
}

def prepare(destination):
    destination = Path(destination)
    for name, digest in FILES.items():
        path = destination / name
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_name(path.name + '.part')
            urllib.request.urlretrieve(f'https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/{COMMIT}/{name}', temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
                raise SystemExit(f'OpenXR checksum mismatch: {name}')
            temporary.replace(path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise SystemExit(f'OpenXR checksum mismatch: {name}')
    print(f'OpenXR headers 1.1.43 verified: {destination}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--destination', type=Path, default=Path(__file__).resolve().parents[1] / '.deps/openxr-sdk')
    prepare(parser.parse_args().destination)
