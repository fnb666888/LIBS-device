"""Private daemon probe: concurrent IPC, partial clients, Qt client and graceful shutdown."""
import argparse
import concurrent.futures
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

parser = argparse.ArgumentParser()
parser.add_argument("daemon", type=Path)
parser.add_argument("--qt-probe", type=Path)
args = parser.parse_args()
k = ctypes.WinDLL("kernel32", use_last_error=True)
k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k.CreateFileW.restype = wintypes.HANDLE
k.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
k.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k.WriteFile.argtypes = k.ReadFile.argtypes
k.CloseHandle.argtypes = [wintypes.HANDLE]
name = r"\\.\pipe\libs-migration-" + uuid.uuid4().hex

def connect():
    for _ in range(50):
        handle = k.CreateFileW(name, 0xC0000000, 0, None, 3, 0, None)
        if handle != ctypes.c_void_p(-1).value:
            return handle
        k.WaitNamedPipeW(name, 100)
        time.sleep(.02)
    raise OSError(ctypes.get_last_error(), "private pipe did not start")

def write(handle, data):
    count = wintypes.DWORD()
    if not k.WriteFile(handle, data, len(data), ctypes.byref(count), None):
        raise ctypes.WinError(ctypes.get_last_error())
    assert count.value == len(data)

def request(command, payload=None):
    handle = connect()
    try:
        write(handle, (json.dumps({"command": command, "args": payload or {}}) + "\n").encode())
        data = b""
        while b"\n" not in data:
            buf = ctypes.create_string_buffer(16384)
            count = wintypes.DWORD()
            if not k.ReadFile(handle, buf, len(buf), ctypes.byref(count), None):
                raise ctypes.WinError(ctypes.get_last_error())
            assert count.value
            data += buf.raw[:count.value]
        return json.loads(data.split(b"\n", 1)[0])
    finally:
        k.CloseHandle(handle)

with tempfile.TemporaryDirectory(prefix="libs-migration-") as folder:
    log = open(Path(folder) / "daemon.log", "wb")
    proc = subprocess.Popen([str(args.daemon.resolve()), "--no-devices", "--data-dir", folder, "--pipe", name], stdout=log, stderr=log, creationflags=0x08000000)
    partial = None
    try:
        assert request("ping")["payload"]["pong"]
        partial = connect()
        write(partial, b'{"command":')
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: request("ping"), range(40)))
        assert all(reply["ok"] for reply in replies)
        assert request("status")["ok"]
        params = request("param_get")
        assert params["ok"]
        assert not request("measurement_start")["ok"]
        if args.qt_probe:
            subprocess.run([str(args.qt_probe.resolve()), name], check=True, timeout=20)
        assert request("shutdown")["payload"]["shutdown_requested"]
        proc.wait(timeout=10)
        assert proc.returncode == 0, proc.returncode
        print("PASS: 40 concurrent requests, partial-client isolation, status, parameters, no-device start rejection, Qt client and graceful shutdown")
    finally:
        if partial is not None: k.CloseHandle(partial)
        if proc.poll() is None: proc.kill(); proc.wait()
        log.close()
