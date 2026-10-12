"""Freeze the public schema from the pinned C++ baseline without opening devices."""
import json
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[2]
call = runpy.run_path(str(root / "src/remote/lpc.py"))["call_daemon"]
pipe = r"\\.\pipe\libs-contract-snapshot"
with tempfile.TemporaryDirectory() as data:
    process = subprocess.Popen(
        [str(root / "src/build/businessd.exe"), "--no-devices", "--data-dir", data, "--pipe", pipe],
        creationflags=subprocess.CREATE_NO_WINDOW,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        for _ in range(100):
            try:
                result = call(pipe, {"command": "prediction", "args": {"action": "contract"}})
                break
            except Exception:
                if process.poll() is not None:
                    raise RuntimeError("baseline daemon exited")
                time.sleep(.05)
        else:
            raise RuntimeError("baseline pipe did not start")
        assert result["ok"], result
        target = root / "backend/contracts/algorithm-v1.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(result["payload"], ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Public prediction schema captured from baseline 351b17c")
        call(pipe, {"command": "shutdown"})
        process.wait(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
