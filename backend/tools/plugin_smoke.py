"""Exercise algorithm replacement through the daemon's actual Windows pipe."""
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
call = runpy.run_path(str(ROOT / "src/remote/lpc.py"))["call_daemon"]
sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")


def main():
    with tempfile.TemporaryDirectory(prefix="libs-plugin-pipe-") as directory:
        folder = Path(directory)
        pipe = r"\\.\pipe\libs-plugin-smoke"
        worker = ROOT / "backend/target/release/examples/algorithm_test_worker.exe"
        def config(version):
            temporary = folder / "runtime.new.json"
            temporary.write_text(json.dumps(dict(engine="external", executable=str(worker), args=[version], timeout_ms=1000)), encoding="utf-8")
            os.replace(temporary, folder / "runtime.local.json")
        config("v1")
        process = subprocess.Popen([str(ROOT / "backend/target/release/businessd.exe"), "--no-devices", "--data-dir", str(folder / "data"), "--algorithm-dir", str(folder), "--pipe", pipe], creationflags=subprocess.CREATE_NO_WINDOW, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def send(action, **args):
            result = call(pipe, dict(command="prediction", args=dict(action=action, **args)))
            assert result["ok"], result
            return result["payload"]
        def wait(ready):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                status = send("status")
                if ready(status):
                    return status
                time.sleep(.02)
            raise AssertionError(status)
        try:
            for _ in range(100):
                try:
                    call(pipe, dict(command="ping"))
                    break
                except Exception:
                    if process.poll() is not None:
                        raise RuntimeError("daemon exited")
                    time.sleep(.03)
            assert send("interface")["protocol"] == "libs.algorithm.worker.v1"
            send("reload")
            wait(lambda s: s["model"].get("version") == "v1" if s["model"] else False)
            for version, mode, expected in [("v1", "calibration_free", 6), ("v2", "calibrated", 12)]:
                if version == "v2":
                    config(version)
                    send("reload")
                    wait(lambda s: s["model"].get("version") == version if s["model"] else False)
                parameters = dict(mode=mode, calibration=dict(profile_id="test", factor=2))
                send("configure", enabled=True, parameters=parameters)
                request = dict(schema_version="libs.algorithm.v1", request_id=version, parameters=parameters,
                               spectrum=dict(spectrum_source="LIBS", spectrum_format="wavelength_nm+counts", scan_groups=[dict(channels=[dict(serial="A", wavelength_nm=[200, 201, 202], counts=[1, 2, 3])])]))
                send("analyze", input=request)
                status = wait(lambda s: s["latest"] and s["latest"].get("request_id") == version)
                result = status["latest"]
                assert result["model"]["version"] == version
                assert result["composition"]["target_values"]["TFe"] == expected
                assert result["parameters_received"] == parameters
                stored = json.loads(Path(result["result_path"]).read_text(encoding="utf-8"))
                assert stored == result
            call(pipe, dict(command="shutdown"))
            process.wait(timeout=5)
            assert process.returncode == 0
            print("PASS: plugin configure, two modes, full parameter forwarding, reload v1->v2, saved results and shutdown over real IPC")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
