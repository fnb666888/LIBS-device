"""One-time, lossless ExtraTrees export. The Rust daemon never starts Python."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct


def export(source, output):
    import joblib
    import numpy as np
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    for name, expected in manifest["asset_sha256"].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Model asset hash mismatch: " + name)
    output.mkdir(parents=True, exist_ok=True)
    if source.resolve() != output.resolve():
        shutil.copyfile(source / "feature_windows.csv", output / "feature_windows.csv")
    heads, hashes = {}, {}
    for name in ("oxide_main", "oxide_added", "sulfide_main", "sulfide_added", "FeO_mixed"):
        base = joblib.load(source / (name + ".joblib"))
        forest = base["model" if name == "FeO_mixed" else "trees"]
        if not hasattr(forest, "estimators_") or not all(hasattr(t, "tree_") for t in forest.estimators_):
            raise ValueError("Expected a fitted sklearn ExtraTrees/RandomForest regressor: " + name)
        if forest.n_features_in_ != manifest["n_features"]:
            raise ValueError("Estimator feature count mismatch: " + name)
        targets = ["FeO"] if name == "FeO_mixed" else list(base["targets"])
        file = name + ".trees.bin"
        with (output / file).open("wb") as stream:
            stream.write(b"LIBSTRE1")
            stream.write(struct.pack("<I", len(forest.estimators_)))
            for estimator in forest.estimators_:
                tree = estimator.tree_
                stream.write(struct.pack("<II", tree.node_count, len(targets)))
                for array in (tree.children_left, tree.children_right, tree.feature):
                    stream.write(np.asarray(array, dtype="<i4").tobytes())
                stream.write(np.asarray(tree.threshold, dtype="<f8").tobytes())
                stream.write(np.asarray(tree.value.reshape(tree.node_count, -1), dtype="<f8").tobytes())
        domain = base.get("domain")
        heads[name] = dict(file=file, targets=targets,
                           log_targets=False if name == "FeO_mixed" else bool(base["log_targets"]),
                           offsets=[0.] if name == "FeO_mixed" else np.asarray(base["offsets"]).tolist(),
                           domain=None if domain is None else dict(
                               mean=domain["real_scaler"].mean_.tolist(),
                               scale=domain["real_scaler"].scale_.tolist(),
                               references=domain["ood_references"].tolist(),
                               threshold=float(domain["ood_threshold"])))
        hashes[file] = hashlib.sha256((output / file).read_bytes()).hexdigest()
    hashes["feature_windows.csv"] = hashlib.sha256((output / "feature_windows.csv").read_bytes()).hexdigest()
    package = dict(format="libs.extratrees.v1", manifest=manifest, heads=heads, asset_sha256=hashes)
    (output / "native_model.json").write_text(json.dumps(package, ensure_ascii=False, allow_nan=False, indent=2), encoding="utf-8")
    print("Exported all five model heads without changing thresholds or leaf values:", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    export(args.source.resolve(), (args.output or args.source).resolve())
