#!/usr/bin/env python3
"""Exercise the actual sender executable over UDP; no SteamVR required."""
import pathlib
import socket
import struct
import subprocess
import sys
import time

binary = pathlib.Path(sys.argv[1]).resolve()
assert binary.is_file(), "sender executable has not been implemented/built"
for args in (["--help"],):
    result = subprocess.run([str(binary), *args], capture_output=True, timeout=3)
    assert result.returncode == 0 and result.stdout, args
for args in (["--bogus"], ["--port"], ["--eye"], ["--mode"],
             ["--port", "0"], ["--port", "-1"], ["--port", "65536"],
             ["--port", "1oops"], ["--port", "9" * 30],
             ["--eye", "both"], ["--mode", "future"]):
    result = subprocess.run([str(binary), *args], capture_output=True, timeout=3)
    assert result.returncode != 0, args
print("PASS CLI help and 11 invalid/missing argument cases")

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
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

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
    assert process.returncode == 0, output
print("PASS signal shutdown")
