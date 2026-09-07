#!/usr/bin/env python3
"""Exercise the actual sender executable over UDP; no SteamVR required."""
import pathlib
import socket
import struct
import subprocess
import sys
import time

process_flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if sys.platform == "win32" else {}
if sys.platform == "win32":
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [enum_callback, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def photo_windows(pid):
        found = []
        @enum_callback
        def inspect(window, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
            name = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(window, name, len(name))
            if owner.value == pid and name.value == "VRStabilizerPhotoTiming":
                found.append(window)
            return True
        user32.EnumWindows(inspect, 0)
        return found

binary = pathlib.Path(sys.argv[1]).resolve()
assert binary.is_file(), "sender executable has not been implemented/built"
for args in (["--help"],):
    result = subprocess.run([str(binary), *args], capture_output=True, timeout=3, **process_flags)
    assert result.returncode == 0 and result.stdout, args
    assert b"--overlay" in result.stdout and b"Windows" in result.stdout, "help must explain the opt-in Windows photo panel"
for args in (["--bogus"], ["--port"], ["--eye"], ["--mode"],
             ["--port", "0"], ["--port", "-1"], ["--port", "65536"],
             ["--port", "1oops"], ["--port", "9" * 30],
             ["--eye", "both"], ["--mode", "future"], ["--overlay", "yes"]):
    result = subprocess.run([str(binary), *args], capture_output=True, timeout=3, **process_flags)
    assert result.returncode != 0, args
print("PASS CLI help and 12 invalid/missing argument cases")

clients = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(9)]
for client in clients:
    client.bind(("127.0.0.1", 0))
    client.settimeout(.3)
probe_port = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
probe_port.bind(("127.0.0.1", 0))
port = probe_port.getsockname()[1]
probe_port.close()
peer = ("127.0.0.1", port)
process = subprocess.Popen([str(binary), "--demo", "--port", str(port)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **process_flags)

def decode(data):
    magic, version, kind, length = struct.unpack_from("<IHHI", data)
    assert magic == 0x54535256 and version == 1 and len(data) == length + 12
    if kind == 2:
        assert len(data) == 52
        return kind, struct.unpack_from("<QQQQQ", data, 12)
    assert kind == 3 and len(data) == 104
    return kind, struct.unpack_from("<QQQI8d", data, 12)

def subscribe(client, nonce):
    stamp = time.monotonic_ns()
    client.sendto(struct.pack("<IHHIQQ", 0x54535256, 1, 1, 16, nonce, stamp), peer)
    until = time.monotonic() + 1
    while time.monotonic() < until:
        kind, fields = decode(client.recv(256))
        if kind == 2:
            assert fields[0:2] == (nonce, stamp)
            assert 0 < fields[2] <= fields[3] and fields[4] != 0
            return fields[4]
    raise AssertionError("missing sync response")

try:
    time.sleep(.15)
    assert process.poll() is None, "demo sender exited during startup"
    if sys.platform == "win32":
        assert not photo_windows(process.pid), "photo timing must remain off by default"
    clients[0].sendto(b"malformed", peer)
    try:
        clients[0].recv(256)
        raise AssertionError("malformed packet subscribed or received a reply")
    except socket.timeout:
        pass
    session = subscribe(clients[0], 123456789)
    poses = []
    until = time.monotonic() + .7
    while time.monotonic() < until:
        kind, fields = decode(clients[0].recv(256))
        if kind == 3:
            poses.append(fields)
    assert 45 <= len(poses) <= 80, len(poses)
    assert all(p[0] == session and p[3] == 7 for p in poses)
    assert all(abs(sum(x*x for x in p[4:8]) - 1) < 1e-6 for p in poses)
    assert all(p[8] < 0 < p[9] and p[11] < 0 < p[10] for p in poses)
    assert all(a[1] < b[1] and a[2] < b[2] for a, b in zip(poses, poses[1:]))
    assert any(abs(poses[0][5] - p[5]) > .001 for p in poses[1:])
    print(f"PASS malformed rejection, clock echo, {len(poses)} valid changing demo poses, monotonic sequence/time")
    for index, client in enumerate(clients[1:8], 1):
        assert subscribe(client, index) == session
    clients[8].sendto(struct.pack("<IHHIQQ", 0x54535256, 1, 1, 16, 9, time.monotonic_ns()), peer)
    try:
        clients[8].recv(256)
        raise AssertionError("ninth client exceeded subscription bound")
    except socket.timeout:
        pass
    time.sleep(3.2)
    for client in clients[:8]:
        client.setblocking(False)
        try:
            while client.recv(256):
                pass
        except BlockingIOError:
            pass
        client.settimeout(.1)
        try:
            client.recv(256)
            raise AssertionError("expired subscriber still receives poses")
        except socket.timeout:
            pass
    assert subscribe(clients[8], 10) == session
    print("PASS eight-subscriber bound, idle expiry, and freed-slot subscription")
finally:
    process.terminate()
    output = process.communicate(timeout=3)[0]
    for client in clients:
        client.close()
    # Windows terminate() uses TerminateProcess, which does not run signal handlers.
    # communicate(timeout=3) verifies bounded cleanup; only POSIX tests graceful exit.
    if sys.platform != "win32":
        assert process.returncode == 0, output
if sys.platform == "win32":
    print("PASS bounded forced process cleanup on Windows; graceful shutdown not tested")
else:
    print("PASS graceful SIGTERM shutdown")

if sys.platform == "win32":
    # Launch only our isolated demo process. No SteamVR or existing sender is touched.
    foreground = user32.GetForegroundWindow()
    process = subprocess.Popen([str(binary), "--demo", "--overlay", "--port", str(port)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **process_flags)
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind(("127.0.0.1", 0))
    client.settimeout(.3)
    try:
        deadline = time.monotonic() + 3
        windows = []
        while not windows and time.monotonic() < deadline and process.poll() is None:
            windows = photo_windows(process.pid)
            time.sleep(.01)
        assert len(windows) == 1, "--overlay must open exactly one desktop panel without an OBS subscriber"
        assert user32.GetForegroundWindow() == foreground, "--overlay must not steal keyboard focus"
        session = subscribe(client, 456)
        kind, before = decode(client.recv(256))
        assert kind == 3 and before[0] == session and before[3] == 7
        assert user32.PostMessageW(windows[0], 0x0010, 0, 0), "could not close our photo panel"
        deadline = time.monotonic() + 1
        while photo_windows(process.pid) and time.monotonic() < deadline:
            time.sleep(.01)
        assert not photo_windows(process.pid), "the panel must close promptly"
        assert process.poll() is None, "closing the panel must leave tracking running"
        after = []
        deadline = time.monotonic() + .35
        while time.monotonic() < deadline:
            kind, fields = decode(client.recv(256))
            if kind == 3:
                after.append(fields)
        assert len(after) >= 18, "UDP pose delivery slowed or stopped after closing the panel"
        assert all(p[0] == session and p[1] > before[1] and p[2] > before[2] and p[3] == 7 for p in after)
        assert not photo_windows(process.pid), "publishing more poses must not reopen the panel"
        assert user32.GetForegroundWindow() == foreground, "panel lifecycle must preserve focus"
        print(f"PASS default-off window, opt-in startup before subscribing, preserved focus, close continues {len(after)} compatible UDP poses")
    finally:
        process.terminate()
        process.communicate(timeout=3)
        client.close()
else:
    result = subprocess.run([str(binary), "--demo", "--overlay"], capture_output=True, timeout=3)
    assert result.returncode != 0 and b"Windows" in result.stderr, "unsupported --overlay must explain its platform requirement"
    print("PASS clear rejection of the Windows-only photo panel")
