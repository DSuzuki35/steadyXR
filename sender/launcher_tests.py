#!/usr/bin/env python3
"""Run both packaged launchers with success, ordinary failure and NTSTATUS exits."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='VR sender launcher ') as directory:
    stage = Path(directory)
    (stage / 'sender').mkdir()
    shutil.copy2(sys.argv[1], stage / 'sender/vrst-sender.exe')
    for launcher in ('Start-Tracking.cmd', 'Start-Delay-Check.cmd'):
        shutil.copy2(root / launcher, stage / launcher)
    for launcher in ('Start-Tracking.cmd', 'Start-Delay-Check.cmd'):
        for code in (0, 1, -1073741819):  # 0xC0000005 access violation
            env = dict(os.environ, VRST_TEST_EXIT=str(code))
            env.pop('VRST_TEST_OVERLAY', None)
            if launcher == 'Start-Delay-Check.cmd':
                env['VRST_TEST_OVERLAY'] = '1'
            result = subprocess.run(['cmd.exe', '/d', '/c', str(stage / launcher)],
                                    input=b'\n', capture_output=True, cwd=stage.parent,
                                    timeout=5, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
            assert result.returncode & 0xffffffff == code & 0xffffffff, (launcher, code, result)
            if code:
                assert str(code).encode() in result.stdout, (launcher, code, result.stdout)
            else:
                assert not result.stdout, (launcher, result.stdout)
            print(f'PASS {launcher}: exit {code}, argument forwarding and failure visibility')
