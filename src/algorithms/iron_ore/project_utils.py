"""Utilities for the current model, training data and retained reports."""
import hashlib
import json
from pathlib import Path
import numpy as np


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def save_json(path, obj):
    Path(path).write_text(json.dumps(obj, ensure_ascii=False, indent=2, allow_nan=False), encoding='utf8')


def csv_table(frame):
    columns = list(frame.columns)
    lines = ['| ' + ' | '.join(columns) + ' |', '|' + '|'.join(['---'] * len(columns)) + '|']
    for row in frame.itertuples(index=False, name=None):
        lines.append('| ' + ' | '.join(f'{v:.4f}' if isinstance(v, (float, np.floating)) else str(v) for v in row) + ' |')
    return '\n'.join(lines)


def session_splits(frame):
    orders = {name: sorted(part.session.unique()) for name, part in frame.groupby('sample_id')}
    counts = {len(s) for s in orders.values()}
    if len(counts) != 1 or min(counts) < 2:
        raise ValueError('Balanced session counts >=2 required')
    for k in range(min(counts)):
        test = np.array([r.session == orders[r.sample_id][k] for r in frame.itertuples()])
        yield str(k + 1), ~test, test
