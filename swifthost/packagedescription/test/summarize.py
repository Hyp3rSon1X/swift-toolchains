#!/usr/bin/env python3
"""Merges the results.tsv files of repeated smoke runs into one table (medians over the repetitions).

Usage: summarize.py <label>=<dir with run*/results.tsv> ...
Columns per label: JSON equal to the host reference, peak RSS in MiB, compile+link ms, run ms.
"""
import statistics
import sys
from pathlib import Path

COLS = ["manifest", "tools", "compile_rc", "ld_warnings", "sig_mismatch", "rebuilds", "compile_ms", "wasm_bytes",
        "run_rc", "run_ms", "rss_mib", "equal"]


def load(directory):
    runs = []
    for tsv in sorted(Path(directory).glob("rep*/results.tsv")):
        rows = [dict(zip(COLS, line.rstrip("\n").split("\t"))) for line in tsv.read_text().splitlines() if line]
        runs.append(rows)
    return runs


def med(values):
    nums = [float(v) for v in values if v not in ("-", "")]
    return statistics.median(nums) if nums else float("nan")


def main():
    labels = [a.split("=", 1) for a in sys.argv[1:]]
    data = {label: load(path) for label, path in labels}
    names = [r["manifest"] for r in next(iter(data.values()))[0]]
    head = ["manifest"] + [f"{label}: equal / MiB / compile ms / run ms" for label, _ in labels]
    print("| " + " | ".join(head) + " |")
    print("|" + "-|" * len(head))
    totals = {label: {"warn": 0, "sig": 0, "rebuild": 0, "fail": 0} for label, _ in labels}
    for i, name in enumerate(names):
        cells = [name]
        for label, _ in labels:
            runs = data[label]
            rows = [run[i] for run in runs]
            equal = "yes" if all(r["equal"] == "yes" for r in rows) else "no"
            cells.append(f"{equal} / {med(r['rss_mib'] for r in rows):.1f} / {med(r['compile_ms'] for r in rows):.0f} / "
                         f"{med(r['run_ms'] for r in rows):.0f}")
            for r in rows:
                totals[label]["warn"] += int(r["ld_warnings"])
                totals[label]["sig"] += int(r["sig_mismatch"])
                totals[label]["rebuild"] += int(r["rebuilds"])
                totals[label]["fail"] += int(r["compile_rc"] != "0" or r["run_rc"] != "0")
        print("| " + " | ".join(cells) + " |")
    print()
    for label, _ in labels:
        t = totals[label]
        print(f"{label}: repetitions={len(data[label])} wasm-ld warnings={t['warn']} signature mismatches={t['sig']} "
              f"interface rebuilds={t['rebuild']} failed compiles or runs={t['fail']}")


main()
