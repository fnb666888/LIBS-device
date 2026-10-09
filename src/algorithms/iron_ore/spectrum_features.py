"""Shared band definition and spectral feature extraction."""
import numpy as np
from scipy.ndimage import gaussian_filter1d, minimum_filter1d

MIN_NM, MAX_NM, STEP_NM = 200., 565., .05


def wavelength_grid(low=MIN_NM, high=MAX_NM, step=STEP_NM):
    if not (np.isfinite([low, high, step]).all() and low < high and step > 0):
        raise ValueError('Invalid wavelength grid')
    return np.round(np.arange(low, high + step / 2, step), 5)


WAVE = wavelength_grid()


def prepare(data, wave=WAVE):
    if data.ndim != 2 or data.shape[1] != 2 or len(data) < 3 or not np.isfinite(data).all():
        return 'invalid_spectrum', None, {}
    if not np.all(np.diff(data[:, 0]) > 0):
        return 'nonmonotonic_wavelength', None, {}
    meta = dict(n_points=len(data), wavelength_min_nm=float(data[0, 0]),
                wavelength_max_nm=float(data[-1, 0]), max_wavelength_gap_nm=float(np.diff(data[:, 0]).max()),
                required_wavelength_min_nm=float(wave[0]), required_wavelength_max_nm=float(wave[-1]))
    if data[0, 0] > wave[0] or data[-1, 0] < wave[-1]:
        return 'insufficient_wavelength_coverage', None, meta
    signal = np.interp(wave, data[:, 0], data[:, 1])
    noise = np.median(abs(np.diff(signal))) / .953872 + 1e-12
    contrast = float(np.quantile(signal, .999) - np.quantile(signal, .25))
    meta['snr_proxy'] = float(contrast / noise)
    if contrast <= 1e-10 or meta['snr_proxy'] < 8:
        return 'blank_or_low_snr', None, meta
    return 'feature_eligible', signal, meta


def extract_areas(wave, spectrum, windows, subtract_baseline=True):
    signal = np.asarray(spectrum, float)
    if subtract_baseline:
        baseline = gaussian_filter1d(minimum_filter1d(signal, size=81, mode='nearest'), 12)
        signal = np.maximum(signal - baseline, 0)
    values = []
    for width in (.30, .65):
        for wl in windows.wavelength:
            mask = abs(wave - wl) <= width
            values.append(np.trapezoid(signal[mask], wave[mask]))
    return np.asarray(values)


def features(areas, windows):
    areas = np.maximum(areas, 0.)
    primary = np.tile(windows.element.isin(['Fe', 'Si', 'Al', 'Ti']).to_numpy(), 2)
    denominator = np.maximum(areas[..., primary].sum(axis=-1, keepdims=True), 1e-30)
    return np.log10(areas / denominator + 1e-6)


def restrict_features(X, source_windows, keep):
    """Restrict saved log area ratios and recompute their primary denominator.

    Recover area ratios, not absolute counts. Their common unknown scale
    cancels in the new denominator. This preserves the original simulated
    variations without inventing replacement augmentation parameters.
    """
    X = np.asarray(X, float)
    keep = np.asarray(keep, bool)
    if X.shape[-1] != 2 * len(source_windows) or keep.shape != (len(source_windows),):
        raise ValueError('Source features/windows do not match')
    areas = np.maximum(10 ** X - 1e-6, 0.)
    return features(areas[..., np.tile(keep, 2)], source_windows.loc[keep].reset_index(drop=True))
