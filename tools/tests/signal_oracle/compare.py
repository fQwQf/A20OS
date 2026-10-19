#!/usr/bin/env python3
"""Compare model_py observations against model_c observations and the authored
expectations.  Exits nonzero on any mismatch."""
import sys
from vectors import VECTORS

expected = {}
for vec in VECTORS:
    for k, v in vec['expect'].items():
        expected[(vec['id'], k)] = v


def load(path):
    obs = {}
    for line in open(path):
        line = line.rstrip('\n')
        if not line:
            continue
        vid, key, val = line.split('\t', 2)
        obs[(vid, key)] = val
    return obs


py = load(sys.argv[1])
c = load(sys.argv[2])

bad = 0
# 1. python model vs authored expectations (already checked by model_py.py)
for (vid, key), want in sorted(expected.items()):
    got = py.get((vid, key))
    if got != want:
        print("PY-EXPECT MISMATCH %s %s: want %r got %r" % (vid, key, want, got))
        bad += 1

# 2. C model vs python model for every observation the C model produced
c_ids = sorted({vid for vid, _ in c})
py_keys = {vid: {k for (i, k) in py if i == vid} for vid in c_ids}
c_keys = {vid: {k for (i, k) in c if i == vid} for vid in c_ids}
for vid in c_ids:
    for key in sorted(c_keys[vid]):
        got = c.get((vid, key))
        want = py.get((vid, key))
        if got != want:
            print("C-PY MISMATCH %s %s: py=%r c=%r" % (vid, key, want, got))
            bad += 1
    only_py = py_keys[vid] - c_keys[vid]
    only_c = c_keys[vid] - py_keys[vid]
    if only_py or only_c:
        print("C-PY KEYSET %s: only_py=%s only_c=%s" % (vid, sorted(only_py), sorted(only_c)))
        bad += 1

print("compared: %d vectors in C subset, %d authored expectations" % (len(c_ids), len(expected)))
print("mismatches: %d" % bad)
sys.exit(1 if bad else 0)
