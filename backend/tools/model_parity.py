"""Differential numerical test using generated forests, never production predictions."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")
sys.path.insert(0, str(ROOT / "src/algorithms"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from predict_worker import Predictor
from export_model import export
import joblib
import numpy as np
import pandas as pd
from sklearn.ensemble import ExtraTreesRegressor
from sklearn.preprocessing import StandardScaler
from spectrum_features import extract_areas, features, wavelength_grid


def compare(expected, actual, path="result"):
    if isinstance(expected, dict):
        assert expected.keys() == actual.keys(), (path, expected.keys(), actual.keys())
        for key in expected:
            if key != "inference_ms":
                compare(expected[key], actual[key], path + "." + key)
    elif isinstance(expected, list):
        assert len(expected) == len(actual), path
        for index, (left, right) in enumerate(zip(expected, actual)):
            compare(left, right, f"{path}[{index}]")
    elif isinstance(expected, (int, float)) and not isinstance(expected, bool):
        assert np.isclose(expected, actual, rtol=2e-10, atol=2e-10), (path, expected, actual)
    else:
        assert expected == actual, (path, expected, actual)


def main():
    rng = np.random.default_rng(81071)
    with tempfile.TemporaryDirectory(prefix="libs-native-model-") as temporary:
        folder = Path(temporary)
        original = ROOT / "src/algorithms/iron_ore/model/current"
        shutil.copyfile(original / "feature_windows.csv", folder / "feature_windows.csv")
        windows = pd.read_csv(folder / "feature_windows.csv")
        wave = wavelength_grid()

        def spectrum():
            values = 30 + 2 * np.sin(wave / 20)
            for center, amplitude in zip(windows.wavelength, rng.uniform(50, 2500, len(windows))):
                values += amplitude * np.exp(-.5 * ((wave - center) / rng.uniform(.04, .12)) ** 2)
            return values

        signals = [spectrum() for _ in range(40)]
        x = np.asarray([features(extract_areas(wave, s, windows), windows) for s in signals])
        targets = ["TFe", "SiO2", "Al2O3", "TiO2", "CaO", "MgO", "MnO", "P", "S", "FeO"]
        scaler = StandardScaler().fit(x)
        for index, name in enumerate(("oxide_main", "oxide_added", "sulfide_main", "sulfide_added", "FeO_mixed")):
            selected = ["FeO"] if name == "FeO_mixed" else targets[:6] if name.endswith("main") else targets[6:]
            y = rng.uniform(.1, 65, (len(x), len(selected)))
            logged = name.endswith("main")
            offsets = np.linspace(.001, .1, len(selected))
            fitted = ExtraTreesRegressor(n_estimators=13, max_depth=7, random_state=index, n_jobs=1).fit(x, np.log10(y + offsets) if logged else y[:, 0] if len(selected) == 1 else y)
            base = dict(targets=selected, trees=fitted, log_targets=logged, offsets=offsets,
                        domain=dict(real_scaler=scaler, ood_references=scaler.transform(x[:8]), ood_threshold=1.1))
            if name == "FeO_mixed":
                base = dict(model=fitted)
            joblib.dump(base, folder / (name + ".joblib"))
        hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir()}
        manifest = dict(format="current_mixed_v2", n_features=len(windows) * 2, wavelength_range_nm=[200, 565], wavelength_step_nm=.05, unit="mass_fraction_percent", asset_sha256=hashes)
        (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        export(folder, folder)
        predictor = Predictor(folder)
        requests = []
        def channel(w, y, serial="A"):
            return dict(serial=serial, pixels=len(w), wavelength_nm=w.tolist(), counts=y.tolist())
        cases = [([channel(wave, y)], True) for y in signals[:8]]
        crop = wave <= 564.7
        cases += [([channel(wave[crop], signals[8][crop])], True),
                  ([channel(wave, np.ones(len(wave)))], True),
                  ([channel(wave, rng.normal(0, 1, len(wave)))], True),
                  ([channel(wave, signals[0])], False),
                  ([channel(wave[30:], signals[0][30:])], True)]
        gap = (wave < 399) | (wave > 401)
        cases += [([channel(wave[gap], signals[1][gap])], True),
                  ([channel(wave[:4100], signals[2][:4100]), channel(wave[3500:], signals[2][3500:], "B")], True),
                  ([channel(wave[::-1], signals[0][::-1])], True)]
        for branch in ("oxide", "sulfide"):
            for index, (channels, complete) in enumerate(cases):
                requests.append(dict(schema_version="libs.algorithm.v1", request_id=f"{branch}-{index}", parameters=dict(branch=branch),
                                     spectrum=dict(spectrum_source="LIBS", spectrum_format="wavelength_nm+counts", scan_groups=[dict(shot_id=1, complete=complete, hardware_synchronized=False, channels=channels)])))
            mixed = json.loads(json.dumps(requests[-1]))
            mixed["request_id"] = branch + "-aggregate"
            mixed["spectrum"]["scan_groups"] = [dict(shot_id=i, complete=True, channels=[channel(wave, s)], hardware_synchronized=True) for i, s in enumerate(signals[:3])]
            requests.append(mixed)
        probe = ROOT / "backend/target/release/examples/model_probe.exe"
        result = subprocess.run([str(probe), str(folder)], input="".join(json.dumps(r) + "\n" for r in requests), text=True, encoding="utf-8", capture_output=True, check=True)
        actual = [json.loads(line) for line in result.stdout.splitlines()]
        assert len(actual) == len(requests)
        for request, output in zip(requests, actual):
            compare(predictor.analyze(request), output)
        print(f"PASS: {len(requests)} full result comparisons against numpy/scipy/sklearn, tolerance 2e-10")


if __name__ == "__main__":
    main()
