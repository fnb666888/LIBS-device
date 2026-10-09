"""One accepted model: mixed FeO plus the required oxide/sulfide component heads."""
import os
for key in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ[key] = '1'
from pathlib import Path
from functools import lru_cache
import hashlib, io, json
import joblib
import numpy as np
import pandas as pd
from spectrum_features import prepare, extract_areas, features, wavelength_grid
from model_definition import TARGETS, MASS, FE_IN_FEO, DEFINITION
from project_utils import sha

ROOT = Path(__file__).resolve().parent
DEFAULT_MODEL = ROOT / 'model' / 'current'


@lru_cache(maxsize=2)
def load_model(folder=DEFAULT_MODEL):
    folder = Path(folder).resolve()
    manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf8'))
    for name, expected in manifest['asset_sha256'].items():
        if sha(folder / name) != expected:
            raise ValueError('Model asset hash mismatch: ' + name)
    pack = {name: joblib.load(folder / (name + '.joblib')) for name in
            ('oxide_main', 'oxide_added', 'sulfide_main', 'sulfide_added', 'FeO_mixed')}
    pack['windows'] = pd.read_csv(folder / 'feature_windows.csv')
    # Legacy backups predate explicit band metadata and used 200–600 nm.
    band = manifest.get('wavelength_range_nm', [200., 600.])
    pack['wave'] = wavelength_grid(*band, manifest.get('wavelength_step_nm', .05))
    pack['manifest'] = manifest
    if manifest.get('n_features', 2 * len(pack['windows'])) != 2 * len(pack['windows']):
        raise ValueError('Model feature manifest does not match its windows')
    if np.any((pack['windows'].wavelength - .65 < band[0]) | (pack['windows'].wavelength + .65 > band[1])):
        raise ValueError('Model windows extend outside its wavelength range')
    pack['folder'] = folder
    return pack


def flag_iron(values):
    available_fe = values[:, 0] - values[:, 8] * MASS['Fe'] / (2 * MASS['S'])
    return (values[:, 9] < 0) | (values[:, 9] * FE_IN_FEO > available_fe + 1e-8)


def predict_features(X, branch='oxide', folder=DEFAULT_MODEL):
    if branch not in ('oxide', 'sulfide'):
        raise ValueError('Feature prediction requires oxide or sulfide branch')
    X = np.atleast_2d(np.asarray(X, float))
    pack = load_model(folder)
    if X.shape[1] != 2 * len(pack['windows']) or not np.isfinite(X).all():
        raise ValueError(f'Expected finite {2 * len(pack["windows"])} spectral features')
    values = np.empty((len(X), len(TARGETS)))
    domain = None
    for head in ('main', 'added'):
        base = pack[branch + '_' + head]
        predicted = base['trees'].predict(X)
        if base['log_targets']:
            predicted = 10 ** predicted - base['offsets']
        for j, target in enumerate(base['targets']):
            values[:, TARGETS.index(target)] = predicted[:, j]
        if head == 'main' and base.get('domain') is not None:
            d = base['domain']
            xs = d['real_scaler'].transform(X)
            distance = np.min(np.sqrt(np.mean((xs[:, None, :] - d['ood_references'][None, :, :]) ** 2, axis=2)), axis=1)
            domain = dict(distance=distance, outside=distance > d['ood_threshold'], threshold=d['ood_threshold'])
    before = values[:, 9].copy()
    if branch == 'oxide':
        values[:, 9] = pack['FeO_mixed']['model'].predict(X)
    return values, before, domain


def predict_paths(paths, branch='auto', folder=DEFAULT_MODEL):
    if branch not in ('auto', 'oxide', 'sulfide'):
        raise ValueError('Invalid branch')
    pack = load_model(folder)
    rows, xs, indices = [], {'oxide': [], 'sulfide': []}, {'oxide': [], 'sulfide': []}
    training_hashes = set(pack['FeO_mixed']['train_sha256'])
    for path in paths:
        path = Path(path)
        selected = ('sulfide' if any('黄铁矿' in p for p in path.parts) else 'oxide') if branch == 'auto' else branch
        row = dict(source=str(path.resolve()), branch=selected, method='mixed_training',
                   model_run=str(pack['folder']), validated_for_quantification=False,
                   FeO_definition=DEFINITION, FeO_iron_already_in_TFe=True,
                   prediction_wavelength_min_nm=float(pack['wave'][0]),
                   prediction_wavelength_max_nm=float(pack['wave'][-1]),
                   n_features=2 * len(pack['windows']))
        try:
            content = path.read_bytes()
            row['sha256'] = hashlib.sha256(content).hexdigest()
            status, signal, meta = prepare(np.loadtxt(io.BytesIO(content)), wave=pack['wave'])
            row.update(meta)
            row['status'] = status
            if status == 'feature_eligible':
                xs[selected].append(features(extract_areas(pack['wave'], signal, pack['windows']), pack['windows']))
                indices[selected].append(len(rows))
        except (ValueError, OSError) as error:
            row.update(status='invalid_or_unreadable_file', detail=str(error)[:160])
        rows.append(row)
    for selected, X in xs.items():
        if not X:
            continue
        values, before, domain = predict_features(np.array(X), selected, folder)
        violations = flag_iron(values)
        for k, i in enumerate(indices[selected]):
            row, v = rows[i], values[k]
            row['status'] = 'research_prediction'
            row['unphysical_output'] = bool(np.any((v < 0) | (v > 100) | ~np.isfinite(v)))
            row['iron_pool_violation'] = bool(violations[k])
            row['outside_observed_domain'] = bool(domain['outside'][k]) if domain is not None else None
            row['ood_distance'] = float(domain['distance'][k]) if domain is not None else None
            if selected == 'oxide':
                row.update(FeO_before_mixed_training=float(before[k]),
                           FeO_training_candidate=pack['FeO_mixed']['selected'],
                           real_FeO_labels_used_for_training=True,
                           other_nine_outputs_frozen=pack['FeO_mixed'].get('other_nine_outputs_frozen', False),
                           FeO_training_status='mixed_training_research_prediction',
                           same_spectrum_used_for_training=row['sha256'] in training_hashes)
            else:
                row['FeO_training_status'] = 'unchanged_sulfide_branch'
                row['same_spectrum_used_for_training'] = False
            for j, target in enumerate(TARGETS):
                row[target] = float(v[j]) if np.isfinite(v[j]) else None
    return rows
