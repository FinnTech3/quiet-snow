"""Compares the GOLDEN summaries printed by the C++ and the Swift model tests. Same scenario, same seed: the ports
must agree, within the room that float rounding gives. Usage: golden_compare.py cpp-output.txt swift-output.txt"""
import json
import sys


def load(path):
    for line in open(path):
        if "GOLDEN" in line:
            return json.loads(line.split("GOLDEN", 1)[1])
    sys.exit(f"no GOLDEN line in {path}")


a, b = load(sys.argv[1]), load(sys.argv[2])
# (key, allowed relative difference). Piles are the sharpest test: they add up thousands of landings.
limits = {"normal": 0, "live": 0.02, "pile1": 0.03, "pile2": 0.03, "floor": 0.03, "mean_x": 0.10, "mean_y": 0.20}
bad = 0
for key, tol in limits.items():
    x, y = a[key], b[key]
    diff = abs(x - y) / max(abs(x), abs(y), 1e-9)
    ok = diff <= tol
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {key:7} C++ {x:>9} Swift {y:>9} difference {diff * 100:.2f}% (limit {tol * 100:.0f}%)")
sys.exit(1 if bad else 0)
