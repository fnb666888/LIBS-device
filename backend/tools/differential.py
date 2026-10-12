"""Compare JSON contracts against the pinned baseline over real named pipes."""
import concurrent.futures
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
import time
import sys

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

ROOT = Path(__file__).resolve().parents[2]
call = runpy.run_path(str(ROOT / "src/remote/lpc.py"))["call_daemon"]


def normalize(value):
    if isinstance(value, dict):
        return {k: normalize(v) for k, v in value.items() if k not in ("wall_clock_ms", "snapshot_at_ms", "monotonic_ms", "inference_ms")}
    if isinstance(value, list):
        return [normalize(v) for v in value]
    if isinstance(value, str) and ("libs-diff-" in value or "recovery_t" in value):
        return re.sub(r"recovery_t\d+", "recovery_tTIME", re.sub(r"[^;]*libs-diff-[^/\\]*", "DATA", value)).replace("\\", "/")
    return value


def main():
    processes = []
    with tempfile.TemporaryDirectory(prefix="libs-diff-cpp-") as cpp_data, tempfile.TemporaryDirectory(prefix="libs-diff-rust-") as rust_data:
        pipes = [r"\\.\pipe\libs-diff-cpp", r"\\.\pipe\libs-diff-rust"]
        binaries = [ROOT / "src/build/businessd.exe", ROOT / "backend/target/release/businessd.exe"]
        env = dict(os.environ, LIBS_ALGORITHM_DIR=str(ROOT / "src/algorithms"))
        try:
            for binary, data, pipe in zip(binaries, [cpp_data, rust_data], pipes):
                processes.append(subprocess.Popen([str(binary), "--no-devices", "--data-dir", data, "--pipe", pipe], env=env, creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
                for _ in range(100):
                    try:
                        call(pipe, {"command": "ping"})
                        break
                    except Exception:
                        if processes[-1].poll() is not None:
                            raise RuntimeError(str(binary) + " exited")
                        time.sleep(.03)
                else:
                    raise RuntimeError("pipe startup timeout")
            capture = dict(schema_version="libs.algorithm.v1", request_id="diff", spectrum=dict(spectrum_source="LIBS", spectrum_format="wavelength_nm+counts", scan_groups=[dict(shot_id=1, complete=False, channels=[dict(serial="A", wavelength_nm=[200., 201., 202.], counts=[10., 0., 20.])])]))
            cases = [
                ("ping", {}), ("config_show", {}), ("health", {}), ("status", {}), ("gate", {}),
                ("param_get", {}), ("param_get", {"group": "laser"}), ("param_get", {"group": "unknown"}),
                ("precheck", {}), ("gate", {"action": "arm"}), ("gate", {"action": "refresh"}),
                ("gate", {"action": "disarm"}), ("gate", {"action": "bad"}),
                ("param_set", {}), ("param_set", {"group": "laser", "values": {"frequency_hz": "bad"}}),
                ("param_set", {"group": "laser", "values": {"frequency_hz": 11}}),
                ("param_set", {"group": "laser", "values": {"discharge_current_a": 65}}),
                ("param_get", {"group": "laser"}), ("param_set", {"group": "custom", "values": {"x": 3}}),
                ("param_set", {"group": "", "values": {"x": 3}}), ("param_set", {"group": "laser", "values": {}}),
                ("capture", {}), ("measurement_start", {}), ("measurement_stop", {}), ("measurement_status", {}),
                ("spectrum_stream", {}), ("spectrum_stream", {"action": "start"}), ("spectrum_stream", {"action": "subscribe"}),
                ("spectrum_stream", {"action": "latest", "subscriber": 2}), ("spectrum_stream", {"action": "latest"}),
                ("spectrum_stream", {"action": "unsubscribe", "subscriber": 2}), ("spectrum_stream", {"action": "bad"}),
                ("prediction", {"action": "contract"}), ("prediction", {"action": "validate", "input": capture}),
                ("prediction", {"action": "validate", "input": {}}), ("prediction", {}),
                ("prediction", {"action": "configure", "enabled": True, "branch": "bad"}),
                ("prediction", {"action": "configure", "enabled": False}), ("prediction", {"action": "latest"}),
                ("recovery_list", {}), ("recovery_write", {"reason": "test"}), ("recovery_list", {}), ("unknown", {}),
            ]
            count = 0
            differences = []
            for command, args in cases:
                replies = [call(pipe, dict(command=command, args=args)) for pipe in pipes]
                if normalize(replies[0]) != normalize(replies[1]):
                    differences.append(dict(command=command, args=args, cpp=replies[0], rust=replies[1]))
                count += 1
            invalid = [None, [], {}, {"command": 1}, {"command": "ping", "args": []}]
            for request in invalid:
                replies = [call(pipe, request) for pipe in pipes]
                if normalize(replies[0]) != normalize(replies[1]):
                    differences.append(dict(request=request, cpp=replies[0], rust=replies[1]))
                count += 1
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                replies = list(pool.map(lambda _: call(pipes[1], {"command": "status"}), range(300)))
                assert all(reply["ok"] for reply in replies)
            print(f"PASS: {count} baseline contract comparisons; 300 concurrent Rust IPC requests")
            for pipe in pipes:
                call(pipe, {"command": "shutdown"})
            for process in processes:
                process.wait(timeout=10)
                assert process.returncode == 0
            if differences:
                raise AssertionError(json.dumps(differences, ensure_ascii=False, indent=2))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
