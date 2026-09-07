#!/usr/bin/env python3
"""Opt-in live Windows/SteamVR check; never changes runtime or headset settings."""
import argparse
import ctypes
from ctypes import wintypes
import pathlib
import socket
import struct
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('sender', type=pathlib.Path)
p.add_argument('--seconds', type=float, default=5)
p.add_argument('--require-tracking', action='store_true')
a = p.parse_args()
assert a.seconds >= 2
binary = a.sender.resolve()
user32 = ctypes.WinDLL('user32', use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wintypes.HWND]

def panel_visible(pid):
    found = []
    @callback_type
    def visit(window, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(window, name, len(name))
        if owner.value == pid and name.value == 'VRStabilizerPhotoTiming':
            found.append(bool(user32.IsWindowVisible(window)))
        return True
    user32.EnumWindows(visit, 0)
    return any(found)

# All eye/mode combinations exercise the real matrix-return boundary. In
# particular MinGW C++ struct returns must not corrupt SteamVR's MSVC ABI.
for mode, eye, overlay in [('frame', 'left', True), ('frame', 'right', False),
                           ('live', 'left', False), ('live', 'right', True)]:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.bind(('127.0.0.1', 0))
        client.settimeout(.1)
        args = [str(binary), '--mode', mode, '--eye', eye, '--port', str(port)]
        if overlay:
            args.append('--overlay')
        child = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        poses, synced, panel = [], False, False
        deadline, next_subscribe = time.monotonic() + a.seconds, 0
        try:
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise AssertionError(f'{mode}/{eye}: exited with {child.returncode:#x}')
                now = time.monotonic()
                if now >= next_subscribe:
                    client.sendto(struct.pack('<IHHIQQ', 0x54535256, 1, 1, 16, 123,
                                              time.monotonic_ns()), ('127.0.0.1', port))
                    next_subscribe = now + .5
                panel |= panel_visible(child.pid)
                try:
                    packet = client.recv(256)
                except (socket.timeout, ConnectionResetError):
                    continue
                magic, version, kind, size = struct.unpack_from('<IHHI', packet)
                assert magic == 0x54535256 and version == 1 and len(packet) == size + 12
                if kind == 2:
                    synced = True
                elif kind == 3:
                    poses.append(struct.unpack_from('<QQQI8d', packet, 12))
            assert synced and len(poses) >= 20, 'live sender stopped serving UDP'
            assert all(x[1] < y[1] and x[2] < y[2] for x, y in zip(poses, poses[1:]))
            valid = [pose for pose in poses if pose[3] & 1]
            if a.require_tracking:
                assert valid, 'headset must be tracking for this hardware check'
            for pose in valid:
                assert abs(sum(q*q for q in pose[4:8]) - 1) < 1e-5
                assert pose[8] < 0 < pose[9] and pose[11] < 0 < pose[10]
            assert panel == overlay, 'wrong photo panel visibility'
        finally:
            if child.poll() is None:
                child.terminate()
            output = child.communicate(timeout=5)[0].decode(errors='replace')
            print(output, flush=True)
        assert 'Connected to SteamVR' in output, 'SteamVR must be running for this opt-in check'
        print(f'PASS {mode}/{eye}, overlay={overlay}: {len(poses)} poses, {len(valid)} valid', flush=True)
