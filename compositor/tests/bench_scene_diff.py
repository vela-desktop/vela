#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect repeated CPU scene-diff measurements, optionally paired with a baseline."""
import argparse
import csv
import io
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--counts", type=int, nargs="+", default=[0, 8, 32, 128, 512, 1024])
    parser.add_argument("--outputs", type=int, nargs="+", default=[1, 2, 4])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--cpu", type=int, help="Pin this runner and its children to one CPU")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    binaries = [("candidate", args.candidate.resolve())]
    if args.baseline:
        binaries.insert(0, ("baseline", args.baseline.resolve()))
    scenarios = [
        "stable", "move", "rotate", "reverse", "churn-quarter", "replace-all",
        "insert-front", "insert-middle", "remove-front", "restack-window",
        "popup-open-close", "animation-churn", "duplicate-keys",
    ]
    # A baseline built before this PR only recognizes the original six cases.
    # New cases still run for the candidate, without aborting the sweep.
    baseline_scenarios = set(scenarios[:6])
    fields = ["binary", "run", "scenario", "elements", "outputs", "ticks_per_sample", "samples",
              "median_ns_per_tick", "p95_batch_ns_per_tick"]
    with args.csv.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for count in args.counts:
            for outputs in args.outputs:
                for scenario in scenarios:
                    selected_binaries = [(name, binary) for name, binary in binaries
                                         if name == "candidate" or scenario in baseline_scenarios]
                    medians = {name: [] for name, _ in selected_binaries}
                    for run in range(args.runs):
                        # Alternate order to reduce systematic warmup/frequency bias.
                        order = selected_binaries if run % 2 == 0 else selected_binaries[::-1]
                        for name, binary in order:
                            result = subprocess.run([str(binary), str(count), str(outputs), scenario],
                                                    check=True, capture_output=True, text=True)
                            row, = csv.DictReader(io.StringIO(result.stdout))
                            writer.writerow(dict(binary=name, run=run + 1, **row))
                            output.flush()
                            medians[name].append(float(row["median_ns_per_tick"]))
                    candidate = statistics.median(medians["candidate"])
                    summary = f"{scenario:13} n={count:5} outputs={outputs}: {candidate / 1000:9.3f} us/tick"
                    if "baseline" in medians:
                        baseline = statistics.median(medians["baseline"])
                        summary += f" (baseline {baseline / 1000:.3f}, change {(candidate / baseline - 1) * 100:+.1f}%)"
                    elif args.baseline:
                        summary += " (candidate only; not supported by old baseline)"
                    print(summary, flush=True)


if __name__ == "__main__":
    main()
