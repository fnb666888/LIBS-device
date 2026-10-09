"""Model parity on retained real spectra plus adapter rejection and grouping checks.

Run with the model's Python environment. Set LIBS_MODEL_SOURCE to the source project
to run the independent, previously accepted prediction regression checks as well.
"""
import hashlib
import json
import os
from pathlib import Path
import sys

import numpy as np
import pytest

ALGORITHMS = Path(__file__).resolve().parents[1] / 'algorithms'
sys.path.insert(0, str(ALGORITHMS))
from predict_worker import Predictor


@pytest.fixture(scope='module')
def predictor():
    return Predictor(ALGORITHMS / 'iron_ore/model/current')


def input_for(data, branch='oxide', serial='imported'):
    return dict(schema_version='libs.algorithm.v1', request_id='regression', parameters=dict(branch=branch),
                spectrum=dict(spectrum_source='LIBS', spectrum_format='wavelength_nm+counts',
                    scan_groups=[dict(complete=True, hardware_synchronized=True,
                        channels=[dict(serial=serial, pixels=len(data),
                                       wavelength_nm=data[:, 0].tolist(), counts=data[:, 1].tolist())])]))


@pytest.fixture(scope='module')
def accepted_samples():
    source = os.environ.get('LIBS_MODEL_SOURCE')
    if not source:
        pytest.skip('LIBS_MODEL_SOURCE is needed for real-spectrum parity tests')
    import pandas as pd
    results = Path(source) / 'model/current/validation'
    standards = pd.read_csv(results / 'standards_per_spectrum.csv')
    iron = pd.read_csv(results / 'iron_ore_per_spectrum.csv')
    selected = pd.concat([standards[~standards.same_spectrum_used_for_training].groupby('ore_directory').head(1),
                          iron[(iron.branch == 'sulfide') & (iron.status == 'research_prediction')].head(1)])
    samples = []
    for _, row in selected.iterrows():
        content = Path(row.source).read_bytes()
        assert hashlib.sha256(content).hexdigest() == row.sha256
        samples.append((np.loadtxt(Path(row.source)), row))
    assert len(samples) >= 2
    return samples


def test_real_spectra_match_previously_accepted_values(predictor, accepted_samples):
    for data, expected in accepted_samples:
        result = predictor.analyze(input_for(data, expected.branch))
        assert result['executed']
        actual = result['composition']['target_values']
        np.testing.assert_allclose([actual[t] for t in predictor.runtime.TARGETS],
                                   [expected[t] for t in predictor.runtime.TARGETS], rtol=0, atol=1e-10)
        assert result['composition']['values']['COMPO_P2O5'] is None
        assert result['composition']['values']['COMPO_TFE'] == actual['TFe']
        expected_domain = expected.get('outside_observed_domain', np.nan)
        if np.isnan(expected_domain):
            actual_domain = result['results'][0]['outside_observed_domain']
            if predictor.pack[expected.branch + '_main'].get('domain') is None:
                assert actual_domain is None
            else:
                assert isinstance(actual_domain, bool)
        else:
            assert result['results'][0]['outside_observed_domain'] == bool(expected_domain)
        assert result['validated_for_quantification'] is False


def test_four_channel_join_preserves_predictions(predictor, accepted_samples):
    data, expected = accepted_samples[0]
    input_data = input_for(data, expected.branch)
    chunks = np.array_split(data, 4)
    input_data['spectrum']['scan_groups'][0]['channels'] = [
        dict(serial=f'CH{i}', wavelength_nm=c[:, 0].tolist(), counts=c[:, 1].tolist()) for i, c in enumerate(chunks)]
    joined = predictor.analyze(input_data)
    single = predictor.analyze(input_for(data, expected.branch))
    assert joined['composition'] == single['composition']


def test_multiple_shots_are_predicted_separately_and_averaged(predictor, accepted_samples):
    a, b = accepted_samples[:2]
    input_data = input_for(a[0])
    input_data['spectrum']['scan_groups'] += input_for(b[0])['spectrum']['scan_groups']
    result = predictor.analyze(input_data)
    assert result['aggregation']['valid_groups'] == 2
    for target in predictor.runtime.TARGETS:
        assert result['composition']['target_values'][target] == pytest.approx(np.mean([
            row['composition']['target_values'][target] for row in result['results']]))


@pytest.mark.parametrize('wave,reason', [
    (np.linspace(250, 550, 1000), 'insufficient_model_wavelength_coverage'),
    (np.concatenate([np.linspace(200, 300, 1000), np.linspace(400, 600, 1000)]), 'spectral_gap_exceeds_1_nm'),
    (np.linspace(600, 200, 1000), 'nonmonotonic_wavelength'),
])
def test_missing_bands_and_unsorted_channels_do_not_predict(predictor, wave, reason):
    result = predictor.analyze(input_for(np.column_stack([wave, np.ones(len(wave))])))
    assert not result['executed']
    assert result['composition']['target_values'] == {}
    assert all(v is None for v in result['composition']['values'].values())
    assert reason in result['results'][0]['detail']


def test_blank_and_incomplete_groups_are_unavailable(predictor):
    data = np.column_stack([np.linspace(200, 600, 8001), np.zeros(8001)])
    input_data = input_for(data)
    assert predictor.analyze(input_data)['results'][0]['status'] == 'blank_or_low_snr'
    input_data['spectrum']['scan_groups'][0]['complete'] = False
    assert predictor.analyze(input_data)['results'][0]['status'] == 'incomplete_scan_group'


def test_branch_is_explicit_and_preprocessing_cannot_be_overridden(predictor):
    data = np.column_stack([np.linspace(200, 600, 8001), np.zeros(8001)])
    input_data = input_for(data)
    input_data['parameters'] = {}
    with pytest.raises(ValueError, match='branch'):
        predictor.analyze(input_data)
    input_data['parameters'] = dict(branch='oxide', smoothing=10)
    with pytest.raises(ValueError, match='preprocessing'):
        predictor.analyze(input_data)


def test_model_asset_hash_mismatch_prevents_loading(tmp_path):
    import shutil
    model_dir = ALGORITHMS / 'iron_ore/model/current'
    manifest = json.loads((model_dir / 'manifest.json').read_text(encoding='utf8'))
    manifest['asset_sha256'] = {'feature_windows.csv': '0' * 64}
    (tmp_path / 'manifest.json').write_text(json.dumps(manifest), encoding='utf8')
    shutil.copyfile(model_dir / 'feature_windows.csv', tmp_path / 'feature_windows.csv')
    with pytest.raises(ValueError, match='hash mismatch'):
        Predictor(tmp_path)


def test_real_hardware_boundary_matches_reviewed_crop_without_extrapolation(predictor):
    report_path = Path(__file__).resolve().parents[2] / 'build/model-verification/20261009-v2/boundary-diagnostic.json'
    if not report_path.exists():
        pytest.skip('Retained hardware spectra are needed for boundary parity')
    report = json.loads(report_path.read_text(encoding='utf8'))
    for expected in report['experimental_real_frames']:
        data = np.loadtxt(expected['source'])
        result = predictor.analyze(input_for(data))
        assert result['executed']
        row = result['results'][0]
        assert row['metadata']['effective_grid_nm'][1] == 564.7
        assert row['metadata']['effective_grid_nm'][1] <= data[-1, 0]
        assert 'model_boundary_adapted' in result['quality']['issues']
        assert 'outside_observed_domain' in result['quality']['issues']
        assert result['preprocessing']['extrapolation'] is False
        np.testing.assert_allclose([result['composition']['target_values'][t] for t in predictor.runtime.TARGETS],
                                   [expected['targets'][t] for t in predictor.runtime.TARGETS], rtol=0, atol=1e-10)


def test_boundary_still_requires_all_windows_and_fixed_grid(predictor):
    data = np.column_stack([np.arange(200, 564.6001, .05), np.ones(7293)])
    result = predictor.analyze(input_for(data))
    assert not result['executed']
    assert result['results'][0]['detail'] == 'insufficient_model_wavelength_coverage'
    assert predictor.status()['feature_count'] == 240
    assert predictor.status()['model_id'] == 'iron_ore_current_mixed_v2'


def test_v1_never_accepts_the_missing_565_600_nm_band(tmp_path):
    # Keep actual hash-verified estimators but declare the legacy input band:
    # the v2-only adaptation must not open a shortcut for 600 nm packages.
    import shutil
    model_dir = ALGORITHMS / 'iron_ore/model/current'
    manifest = json.loads((model_dir / 'manifest.json').read_text(encoding='utf8'))
    manifest['format'] = 'current_mixed_v1'
    manifest['wavelength_range_nm'] = [200, 600]
    for name in manifest['asset_sha256']:
        shutil.copyfile(model_dir / name, tmp_path / name)
    (tmp_path / 'manifest.json').write_text(json.dumps(manifest), encoding='utf8')
    legacy = Predictor(tmp_path)
    data = np.column_stack([np.linspace(200, 565, 7301), np.ones(7301)])
    result = legacy.analyze(input_for(data))
    assert not result['executed']
    assert legacy.status()['boundary_policy'] == 'full_grid_only'
    assert legacy.status()['minimum_accepted_wavelength_nm'] == [200, 600]
