#!/usr/bin/env python3
"""Compare independent local benchmark runs; preserve raw per-boot means."""
import argparse
import itertools
import json
from pathlib import Path
import random
import statistics


def read(paths):
    values, hashes, kernels = [], set(), set()
    for path in paths:
        evidence = json.loads(path.read_text())
        if not evidence["success"]:
            raise ValueError(f"failed benchmark: {path}")
        local = [row for row in evidence["measurements"] if row["mode"] == "local"]
        if len(local) != 1 or local[0]["calls"] < 100000 or local[0]["bytes"] != 64:
            raise ValueError(f"expected one local 100k-call measurement: {path}")
        values.append(local[0]["mean_ns"])
        hashes.add(evidence["source_sha256"]["user/tests/test_native_cluster_bench.c"])
        kernels.add(evidence["kernel_sha256"])
    if len(kernels) != 1:
        raise ValueError("each group must use one kernel ELF")
    return values, hashes, kernels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, nargs="+", required=True)
    parser.add_argument("--candidate", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 2 <= len(args.baseline) <= 8 or not 2 <= len(args.candidate) <= 8:
        parser.error("provide 2..8 independent boots per kernel; alternate their order")
    files = args.baseline + args.candidate
    if len({path.resolve() for path in files}) != len(files):
        raise ValueError("each measurement must come from a distinct evidence file")
    before, before_hashes, before_kernels = read(args.baseline)
    after, after_hashes, after_kernels = read(args.candidate)
    if len(before_hashes | after_hashes) != 1:
        raise ValueError("both kernels must run the identical benchmark source")
    if before_kernels == after_kernels:
        raise ValueError("baseline and candidate kernel ELFs must differ")
    b, a = statistics.mean(before), statistics.mean(after)
    effect = (a / b - 1) * 100
    # Whole-boot means are the independent units, not correlated RPC samples.
    pooled = before + after
    extreme = total = 0
    for indices in itertools.combinations(range(len(pooled)), len(before)):
        chosen = set(indices)
        left = statistics.mean(pooled[i] for i in chosen)
        right = statistics.mean(pooled[i] for i in range(len(pooled)) if i not in chosen)
        extreme += abs(right - left) >= abs(a - b) - 1e-9
        total += 1
    rng = random.Random(20261011)
    bootstrap = sorted((statistics.mean(rng.choices(after, k=len(after))) /
                        statistics.mean(rng.choices(before, k=len(before))) - 1) * 100
                       for _ in range(10000))
    result = {"baseline_files": [str(p) for p in args.baseline],
              "candidate_files": [str(p) for p in args.candidate],
              "baseline_boot_means_ns": before, "candidate_boot_means_ns": after,
              "baseline_mean_ns": b, "candidate_mean_ns": a,
              "difference_percent": effect,
              "bootstrap_95_percent_interval": [bootstrap[249], bootstrap[9749]],
              "two_sided_exact_permutation_p": extreme / total,
              "permutations": total, "bootstrap_seed": 20261011,
              "observed_absolute_difference_below_3_percent": abs(effect) < 3,
              "regression_over_20_percent": effect > 20,
              "scope": "QEMU TCG per-boot means; host scheduling and code layout remain confounders"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"local IPC difference: {effect:+.3f}%, permutation p={extreme / total:.6f}, "
          f"bootstrap 95% interval [{bootstrap[249]:+.3f}, {bootstrap[9749]:+.3f}]%")
    if effect > 20:
        raise SystemExit("local IPC regression exceeds 20%; investigate before merging")


if __name__ == "__main__":
    main()
