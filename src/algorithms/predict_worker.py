"""Resident, offline model worker. stdout is reserved for the C++ JSON protocol."""
from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
from pathlib import Path
import sys
import time

for key in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ[key] = '1'
sys.path.insert(0, str(Path(__file__).resolve().parent / 'iron_ore'))

COMPONENTS = ('AL2O3', 'CAO', 'FEO', 'MGO', 'MNO', 'P2O5', 'S', 'SIO2', 'TFE', 'TIO2')


def empty_composition():
    return dict(availability='unavailable', unit='mass_fraction_percent', unit_confirmed=True,
                values={'COMPO_' + c: None for c in COMPONENTS}, target_values={},
                phosphorus_definition='elemental_P; COMPO_P2O5 is not predicted')


class Predictor:
    def __init__(self, model_dir):
        import numpy as np
        import model_runtime as runtime
        self.np, self.runtime = np, runtime
        self.model_dir = Path(model_dir).resolve()
        self.pack = runtime.load_model(self.model_dir)
        self.manifest = json.loads((self.model_dir / 'manifest.json').read_text(encoding='utf8'))
        formats = {'current_mixed_v1': 'iron_ore_current_mixed_v1',
                   'current_mixed_v2': 'iron_ore_current_mixed_v2'}
        if self.manifest['format'] not in formats:
            raise ValueError('Unsupported model format')
        self.model_id = formats[self.manifest['format']]
        self.wave = self.pack['wave']
        self.feature_count = 2 * len(self.pack['windows'])
        # This reviewed boundary policy applies only to the 200–565 nm v2
        # package. It never relaxes the missing 565–600 nm bands of v1.
        self.boundary_crop = (self.manifest['format'] == 'current_mixed_v2'
                              and self.wave[0] == 200 and self.wave[-1] == 565
                              and self.feature_count == 240)
        self.cropped_wave = self.wave[self.wave <= 564.7] if self.boundary_crop else self.wave
        windows = self.pack['windows'].wavelength
        if self.boundary_crop and ((windows - .65 < self.cropped_wave[0]) |
                                   (windows + .65 > self.cropped_wave[-1])).any():
            raise ValueError('Boundary policy would remove a model feature window')
        for name in ('oxide_main', 'oxide_added', 'sulfide_main', 'sulfide_added'):
            if self.pack[name]['trees'].n_features_in_ != self.feature_count:
                raise ValueError('Model estimator feature count mismatch: ' + name)
            self.pack[name]['trees'].n_jobs = 1
        if self.pack['FeO_mixed']['model'].n_features_in_ != self.feature_count:
            raise ValueError('Model estimator feature count mismatch: FeO_mixed')
        self.pack['FeO_mixed']['model'].n_jobs = 1

    def status(self):
        return dict(availability='available', implemented=True, model_id=self.model_id,
                    model_format=self.manifest['format'], model_dir=str(self.model_dir),
                    targets=list(self.runtime.TARGETS), unit=self.manifest['unit'],
                    branches=['oxide', 'sulfide'], required_wavelength_nm=[float(self.wave[0]), float(self.wave[-1])],
                    minimum_accepted_wavelength_nm=[float(self.cropped_wave[0]), float(self.cropped_wave[-1])],
                    feature_count=self.feature_count, asset_sha256=self.manifest['asset_sha256'],
                    boundary_policy='full_grid_or_fixed_564_7_nm_crop' if self.boundary_crop else 'full_grid_only',
                    validated_for_quantification=False, inference_network_required=False,
                    phosphorus_definition='elemental_P; no automatic P2O5 conversion')

    def stitch(self, channels):
        """Same-shot channels: sort wavelengths, average exact duplicates, no gain scaling."""
        np = self.np
        arrays, serials = [], set()
        for channel in channels:
            serial = channel.get('serial')
            if not isinstance(serial, str) or not serial.strip() or serial in serials:
                raise ValueError('Each channel must have a unique nonempty serial')
            serials.add(serial)
            wave = np.asarray(channel.get('wavelength_nm'), float)
            counts = np.asarray(channel.get('counts'), float)
            if wave.ndim != 1 or len(wave) < 3 or counts.shape != wave.shape:
                raise ValueError('Expected matching wavelength_nm and counts arrays of at least 3 points')
            if not np.isfinite(wave).all() or not np.isfinite(counts).all() or (wave <= 0).any():
                raise ValueError('Nonfinite intensities or invalid wavelengths')
            if not (np.diff(wave) > 0).all():
                raise ValueError('nonmonotonic_wavelength')
            if 'pixels' in channel and channel['pixels'] != len(wave):
                raise ValueError('pixels does not match array lengths')
            arrays.append(np.column_stack([wave, counts]))
        if not arrays:
            raise ValueError('No captured channels')
        data = np.concatenate(arrays)
        data = data[np.argsort(data[:, 0], kind='stable')]
        unique, inverse, multiplicity = np.unique(data[:, 0], return_inverse=True, return_counts=True)
        intensity = np.bincount(inverse, weights=data[:, 1]) / multiplicity
        data = np.column_stack([unique, intensity])
        if unique[0] > self.cropped_wave[0] or unique[-1] < self.cropped_wave[-1]:
            raise ValueError('insufficient_model_wavelength_coverage')
        # Reject missing bands rather than interpolate across a missing hardware channel.
        relevant = (unique[:-1] < self.wave[-1]) & (unique[1:] > self.wave[0])
        if (np.diff(unique)[relevant] > 1.0).any():
            raise ValueError('spectral_gap_exceeds_1_nm')
        return data

    def analyze(self, input_data):
        started = time.perf_counter()
        if not isinstance(input_data, dict) or input_data.get('schema_version') != 'libs.algorithm.v1':
            raise ValueError('input.schema_version must be libs.algorithm.v1')
        request_id = input_data.get('request_id')
        if not isinstance(request_id, str) or not request_id.strip():
            raise ValueError('input.request_id is required')
        for key in ('context', 'parameters', 'model'):
            if key in input_data and not isinstance(input_data[key], dict):
                raise ValueError('input.' + key + ' must be an object')
        parameters = input_data.get('parameters', {})
        if set(parameters) - {'branch'}:
            raise ValueError('Only parameters.branch is supported; preprocessing is fixed by the model')
        branch = parameters.get('branch')
        if branch not in ('oxide', 'sulfide'):
            raise ValueError('input.parameters.branch must explicitly be oxide or sulfide')
        model = input_data.get('model', {})
        if set(model) - {'id'} or model.get('id', self.model_id) != self.model_id:
            raise ValueError('Unsupported model selection; use ' + self.model_id)
        spectrum = input_data.get('spectrum', input_data.get('raw_capture'))
        if not isinstance(spectrum, dict) or spectrum.get('spectrum_source') != 'LIBS' or spectrum.get('spectrum_format') != 'wavelength_nm+counts':
            raise ValueError('Expected captured LIBS wavelength_nm+counts data')
        groups = spectrum.get('scan_groups')
        if not isinstance(groups, list) or not 1 <= len(groups) <= 128:
            raise ValueError('Expected 1..128 scan groups')
        rows, vectors = [], []
        for index, group in enumerate(groups):
            if not isinstance(group, dict):
                raise ValueError('Each scan group must be an object')
            row = dict(group_index=index, shot_id=group.get('shot_id'), branch=branch,
                       availability='unavailable', composition=empty_composition(),
                       hardware_synchronized=group.get('hardware_synchronized'),
                       issues=[])
            rows.append(row)
            if group.get('complete') is False:
                row['status'] = 'incomplete_scan_group'
                continue
            if group.get('hardware_synchronized') is not True:
                row['issues'].append('hardware_synchronization_not_confirmed')
            try:
                data = self.stitch(group.get('channels', []))
                wave = self.wave if data[-1, 0] >= self.wave[-1] else self.cropped_wave
                status, signal, metadata = self.runtime.prepare(data, wave=wave)
                metadata['model_wavelength_range_nm'] = [float(self.wave[0]), float(self.wave[-1])]
                metadata['effective_grid_nm'] = [float(wave[0]), float(wave[-1]), float(wave[1] - wave[0])]
                metadata['boundary_adapted'] = bool(wave[-1] != self.wave[-1])
                if metadata['boundary_adapted']:
                    row['issues'].append('model_boundary_adapted')
                row.update(status=status, metadata=metadata)
                if status != 'feature_eligible':
                    continue
                X = self.runtime.features(self.runtime.extract_areas(wave, signal, self.pack['windows']), self.pack['windows'])
                values, before, domain = self.runtime.predict_features(X, branch, self.model_dir)
                value = values[0]
                if not self.np.isfinite(value).all():
                    row['status'] = 'nonfinite_model_output'
                    continue
                composition = empty_composition()
                composition['availability'] = 'available'
                composition['target_values'] = {t: float(v) for t, v in zip(self.runtime.TARGETS, value)}
                for t, v in composition['target_values'].items():
                    if t != 'P':
                        composition['values']['COMPO_' + t.upper()] = v
                row.update(status='research_prediction', availability='available', composition=composition,
                           unphysical_output=bool(((value < 0) | (value > 100)).any()),
                           iron_pool_violation=bool(self.runtime.flag_iron(values)[0]),
                           outside_observed_domain=bool(domain['outside'][0]) if domain else None,
                           ood_distance=float(domain['distance'][0]) if domain else None,
                           validated_for_quantification=False, FeO_iron_already_in_TFe=True)
                if branch == 'oxide':
                    row['FeO_before_mixed_training'] = float(before[0])
                for key in ('unphysical_output', 'iron_pool_violation', 'outside_observed_domain'):
                    if row.get(key):
                        row['issues'].append(key)
                vectors.append(value)
            except (ValueError, TypeError) as error:
                row.update(status='ineligible_spectrum', detail=str(error))
        composition = empty_composition()
        if vectors:
            mean = self.np.mean(vectors, axis=0)
            composition['availability'] = 'available'
            composition['target_values'] = {t: float(v) for t, v in zip(self.runtime.TARGETS, mean)}
            for t, v in composition['target_values'].items():
                if t != 'P':
                    composition['values']['COMPO_' + t.upper()] = v
        issues = sorted({issue for row in rows for issue in row['issues']})
        if len(vectors) < len(rows):
            issues.append('some_or_all_groups_ineligible')
        return dict(schema_version='libs.algorithm.result.v1', request_id=request_id,
                    availability=composition['availability'], executed=bool(vectors),
                    status=dict(state='completed' if vectors else 'not_executed',
                                reason='research_prediction' if vectors else 'no_eligible_spectrum'),
                    composition=composition, results=rows,
                    aggregation=dict(method='mean_of_valid_group_predictions', valid_groups=len(vectors), total_groups=len(rows)),
                    classification=dict(label=None, confidence=None, classes=[]),
                    similarity=dict(score=None, reference_id=None, matches=[]),
                    quality=dict(status='review_required' if issues or not vectors else 'research_prediction', issues=issues),
                    context=input_data.get('context', {}), model=self.status(), branch=branch,
                    validated_for_quantification=False, FeO_definition=self.runtime.DEFINITION,
                    FeO_iron_already_in_TFe=True, parameters_applied=True,
                    preprocessing=dict(grid_nm=[float(self.wave[0]), float(self.wave[-1]),
                                                float(self.manifest.get('wavelength_step_nm', .05))],
                                       effective_grids_nm=[row['metadata']['effective_grid_nm'] for row in rows if 'metadata' in row],
                                       feature_count=self.feature_count, extrapolation=False,
                                       boundary_policy=self.status()['boundary_policy'],
                                       channel_join='sort_and_average_exact_duplicate_wavelengths',
                                       max_wavelength_gap_nm=1.0, channel_gain_correction=False),
                    inference_ms=round((time.perf_counter() - started) * 1000, 3))


def dump(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, allow_nan=False), encoding='utf8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--model-dir', type=Path, required=True)
    args = parser.parse_args()
    sys.stdin.reconfigure(encoding='utf8')
    sys.stdout.reconfigure(encoding='utf8', line_buffering=True)
    sys.stderr.reconfigure(encoding='utf8')
    try:
        with contextlib.redirect_stdout(sys.stderr):
            predictor = Predictor(args.model_dir)
        print(json.dumps({'ready': True}))
    except Exception as error:
        print(json.dumps({'ready': False, 'error': str(error)}))
        return 1
    for line in sys.stdin:
        output_path = None
        try:
            request = json.loads(line)
            input_path, output_path = Path(request['input_file']), Path(request['output_file'])
            payload = json.loads(input_path.read_text(encoding='utf8'))
            with contextlib.redirect_stdout(sys.stderr):
                if payload['action'] == 'status':
                    result = predictor.status()
                elif payload['action'] == 'analyze':
                    result = predictor.analyze(payload['input'])
                else:
                    raise ValueError('Unsupported action')
            response = dict(ok=True, payload=result)
        except Exception as error:
            response = dict(ok=False, error=str(error),
                            error_kind='invalid_argument' if isinstance(error, (ValueError, TypeError, KeyError)) else 'internal')
        if output_path is None:
            print(json.dumps({'done': False, 'error': response['error']}))
            continue
        try:
            dump(output_path, response)
            print(json.dumps({'done': True}))
        except OSError as error:
            print(json.dumps({'done': False, 'error': str(error)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
