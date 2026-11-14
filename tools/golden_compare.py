"""Compares the GOLDEN summaries printed by the C++ and the Swift model tests. Same scenario, same seed: the ports
must agree, within the room that float rounding gives. Usage: golden_compare.py cpp-output.txt swift-output.txt"""
import json
import sys


def load(path, tag):
    for line in open(path):
        if tag + " {" in line:
            return json.loads(line.split(tag, 1)[1])
    sys.exit(f"no {tag} line in {path}")


# (tag, key, allowed relative difference). Piles are the sharpest test: they add up thousands of landings. The extras
# scenario checks snowmen, the plough and snowballs.
limits = [("GOLDEN", "normal", 0), ("GOLDEN", "live", 0.02), ("GOLDEN", "pile1", 0.03), ("GOLDEN", "pile2", 0.03),
          ("GOLDEN", "floor", 0.03), ("GOLDEN", "mean_x", 0.10), ("GOLDEN", "mean_y", 0.20),
          ("EXTRAS", "snowmen", 0), ("EXTRAS", "plough", 0), ("EXTRAS", "balls", 0), ("EXTRAS", "window", 0.03),
          ("EXTRAS", "floor", 0.03)]
data = {(f, t): load(f, t) for f in sys.argv[1:3] for t in ("GOLDEN", "EXTRAS")}
bad = 0
for tag, key, tol in limits:
    x, y = data[(sys.argv[1], tag)][key], data[(sys.argv[2], tag)][key]
    diff = abs(x - y) / max(abs(x), abs(y), 1e-9)
    ok = diff <= tol
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {key:7} C++ {x:>9} Swift {y:>9} difference {diff * 100:.2f}% (limit {tol * 100:.0f}%)")
sys.exit(1 if bad else 0)
