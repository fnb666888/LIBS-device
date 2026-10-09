"""Exercise the real model via a private no-device daemon; optionally capture its Qt UI."""
import argparse
import concurrent.futures
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

import numpy as np
import pandas as pd

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--ui', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = root / 'build' / 'prediction-preview' / uuid.uuid4().hex
output.mkdir(parents=True)
pipe = r'\\.\pipe\libs-prediction-' + uuid.uuid4().hex
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                         wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k.CreateFileW.restype = wintypes.HANDLE
k.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
k.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                       ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k.ReadFile.argtypes = k.WriteFile.argtypes
k.CloseHandle.argtypes = [wintypes.HANDLE]

def request(command, payload=None):
    for _ in range(100):
        handle = k.CreateFileW(pipe, 0xC0000000, 0, None, 3, 0, None)
        if handle != ctypes.c_void_p(-1).value:
            break
        k.WaitNamedPipeW(pipe, 100)
        time.sleep(.02)
    else:
        raise OSError('private daemon not ready')
    try:
        blob = (json.dumps(dict(command=command, args=payload or {})) + '\n').encode()
        count = wintypes.DWORD()
        assert k.WriteFile(handle, blob, len(blob), ctypes.byref(count), None)
        assert count.value == len(blob)
        data = b''
        while b'\n' not in data:
            buf = ctypes.create_string_buffer(65536)
            assert k.ReadFile(handle, buf, len(buf), ctypes.byref(count), None)
            assert count.value
            data += buf.raw[:count.value]
        return json.loads(data.split(b'\n', 1)[0])
    finally:
        k.CloseHandle(handle)

def wait_result(request_id):
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        status = request('prediction')['payload']
        latest = status.get('latest') or {}
        if latest.get('request_id') == request_id:
            return latest
        assert status['state'] != 'model_unavailable', status
        time.sleep(.05)
    raise AssertionError('prediction did not complete')

def input_for(data, branch, request_id):
    return dict(schema_version='libs.algorithm.v1', request_id=request_id,
        parameters=dict(branch=branch), context=dict(sample_id=request_id),
        spectrum=dict(spectrum_source='LIBS', spectrum_format='wavelength_nm+counts',
            scan_groups=[dict(complete=True, shot_id=12, hardware_synchronized=True,
                channels=[dict(serial='offline-spectrum', pixels=len(data),
                    wavelength_nm=data[:, 0].tolist(), counts=data[:, 1].tolist())])]))

def screenshot(name, size='1440x900', extra_env=None):
    if not args.ui:
        return
    env = os.environ.copy()
    env.update(LIBS_BUSINESSD_PIPE=pipe, LIBS_UI_SHOT=str(output / name),
               LIBS_UI_SHOT_SIZE=size, LIBS_UI_SHOT_DELAY_MS='1700', QT_QPA_PLATFORM='windows')
    env.update(extra_env or {})
    with open(output / (name + '.log'), 'wb') as log:
        subprocess.run([str(args.ui.resolve())], env=env, cwd=root,
            stdout=log, stderr=log, check=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
    assert (output / name).exists()

reports = args.source / 'model/current/validation'
table = pd.read_csv(reports / 'standards_per_spectrum.csv')
expected = table[~table.same_spectrum_used_for_training].iloc[0]
data = np.loadtxt(Path(expected.source))
with open(output / 'daemon.log', 'wb') as log:
    process = subprocess.Popen([str(root / 'src/build/businessd.exe'), '--no-devices',
        '--pipe', pipe, '--data-dir', str(output / 'data')], stdout=log, stderr=log,
        cwd=root, creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        assert request('ping')['ok']
        params = request('param_get')['payload']
        assert request('prediction', dict(action='configure', enabled=True, branch='oxide'))['ok']
        # The model starts loading on its own thread; control requests stay responsive.
        start = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: request('ping'), range(40)))
        assert all(r['ok'] for r in replies)
        assert time.monotonic() - start < 3
        payload = input_for(data, expected.branch, 'accepted-real-spectrum')
        accepted = request('prediction', dict(action='analyze', input=payload))
        assert accepted['ok'] and accepted['payload']['accepted'], accepted
        valid = wait_result(payload['request_id'])
        assert valid['executed'] and valid['saved'], valid
        targets = ('TFe', 'SiO2', 'Al2O3', 'TiO2', 'CaO', 'MgO', 'MnO', 'P', 'S', 'FeO')
        np.testing.assert_allclose([valid['composition']['target_values'][t] for t in targets],
                                  [expected[t] for t in targets], rtol=0, atol=1e-10)
        saved = json.loads(Path(valid['result_path']).read_text(encoding='utf8'))
        assert saved['request_id'] == payload['request_id']
        assert saved['context']['sample_id'] == payload['request_id']
        assert saved['composition']['values']['COMPO_P2O5'] is None
        screenshot('prediction-valid.png')
        screenshot('prediction-compact.png', '1180x720')
        if args.ui:
            # Exercise the actual Qt selector, file parser, asynchronous submit and rendering.
            assert request('prediction', dict(action='configure', enabled=False))['ok']
            imported_path = output / 'imported-spectrum.csv'
            np.savetxt(imported_path, data, delimiter=',', header='wavelength_nm,counts', comments='# ')
            screenshot('prediction-imported.png', extra_env=dict(LIBS_UI_SHOT_IMPORT=str(imported_path),
                LIBS_UI_SHOT_BRANCH=expected.branch, LIBS_UI_SHOT_DELAY_MS='4500'))
            imported = request('prediction')['payload']['latest']
            assert imported['context']['source_file'] == str(imported_path), imported
            assert imported['executed'] and imported['saved']
            np.testing.assert_allclose([imported['composition']['target_values'][t] for t in targets],
                                      [expected[t] for t in targets], rtol=0, atol=1e-10)
        truncated = data[data[:, 0] <= 564.6]
        bad = input_for(truncated, 'oxide', 'insufficient-live-band')
        assert request('prediction', dict(action='analyze', input=bad))['ok']
        invalid = wait_result(bad['request_id'])
        assert not invalid['executed'] and not invalid['composition']['target_values']
        assert 'insufficient_model_wavelength_coverage' in invalid['results'][0]['detail']
        screenshot('prediction-insufficient-band.png')
        assert request('param_get')['payload'] == params
        assert not request('status')['payload']['laser_running']
        assert request('prediction', dict(action='configure', enabled=False))['ok']
        assert request('prediction')['payload']['latest'] is None
        assert request('shutdown')['ok']
        assert process.wait(timeout=10) == 0
        print('PASS: real-model IPC parity, responsive control, correlated saved results, missing-band rejection, Qt preview and shutdown')
        print('Artifacts:', output)
    finally:
        if process.poll() is None:
            try:
                request('shutdown')
                process.wait(timeout=10)
            except Exception:
                process.terminate()
                process.wait(timeout=10)

# Verify cancellation against a real child process, during both load and prediction.
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.OpenProcess.restype = wintypes.HANDLE
k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
for phase in ('load', 'predict'):
    pipe = r'\\.\pipe\libs-prediction-cancel-' + uuid.uuid4().hex
    fake = output / ('cancel-' + phase)
    fake.mkdir()
    code = '''import json,os,time
from pathlib import Path
flag=Path(__file__).with_name("active.json")
phase=PHASE
if phase=="load":
    flag.write_text(json.dumps({"pid":os.getpid()}))
    time.sleep(120)
print(json.dumps({"ready":True}),flush=True)
for line in __import__('sys').stdin:
    control=json.loads(line)
    payload=json.loads(Path(control["input_file"]).read_text(encoding="utf8"))
    if payload["action"]=="analyze":
        flag.write_text(json.dumps({"pid":os.getpid()}))
        time.sleep(120)
    Path(control["output_file"]).write_text(json.dumps({"ok":True,"payload":{"availability":"available"}}))
    print(json.dumps({"done":True}),flush=True)
'''.replace('PHASE', repr(phase))
    (fake / 'predict_worker.py').write_text(code, encoding='utf8')
    (fake / 'runtime.local.json').write_text(json.dumps(dict(python=sys.executable,
        model_dir=str(root / 'src/algorithms/iron_ore/model/current'), timeout_ms=60000)), encoding='utf8')
    env = os.environ.copy()
    env['LIBS_ALGORITHM_DIR'] = str(fake)
    with open(fake / 'daemon.log', 'wb') as log:
        process = subprocess.Popen([str(root / 'src/build/businessd.exe'), '--no-devices', '--pipe', pipe,
            '--data-dir', str(fake / 'data')], env=env, stdout=log, stderr=log, cwd=root,
            creationflags=subprocess.CREATE_NO_WINDOW)
        child = None
        try:
            assert request('ping')['ok']
            if phase == 'load':
                assert request('prediction', dict(action='configure', enabled=True, branch='oxide'))['ok']
            else:
                assert request('prediction', dict(action='analyze', input=payload))['ok']
            deadline = time.monotonic() + 10
            flag = fake / 'active.json'
            while not flag.exists() and time.monotonic() < deadline:
                time.sleep(.02)
            assert flag.exists(), phase
            pid = json.loads(flag.read_text())['pid']
            child = k.OpenProcess(0x00100000, False, pid)
            assert child
            assert request('ping')['ok']
            start = time.monotonic()
            assert request('shutdown')['ok']
            assert process.wait(timeout=5) == 0
            assert time.monotonic() - start < 5
            assert k.WaitForSingleObject(child, 1000) == 0, 'orphan model worker'
            print('PASS: shutdown cancels model', phase, 'and reaps its child process')
        finally:
            if child:
                k.CloseHandle(child)
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
