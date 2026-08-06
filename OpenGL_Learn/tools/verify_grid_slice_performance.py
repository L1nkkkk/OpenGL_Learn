"""Independently recalculate the frozen S* decision table from raw process JSON."""

import argparse
import hashlib
import json
import pathlib
import re
import statistics


SLICES = (1, 2, 4, 8, 16)
ABS_MS = 0.05
REL_PERCENT = 3.0
NAME = re.compile(r"^(cached|rebuild)-n(\d+)-r(\d+)-s(\d+)-round(\d+)\.json$")


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def compare(data, regime, light_count, radius, a, b):
    pairs = []
    for round_index in (1, 2, 3):
        av = data[(regime, light_count, radius, a, round_index)]
        bv = data[(regime, light_count, radius, b, round_index)]
        delta = av - bv
        pairs.append({
            "round": round_index,
            "aMedianMs": av,
            "bMedianMs": bv,
            "deltaMs": delta,
            "relativePercent": delta / bv * 100.0 if bv else 0.0,
        })
    deltas = [pair["deltaMs"] for pair in pairs]
    relatives = [pair["relativePercent"] for pair in pairs]
    median_delta = statistics.median(deltas)
    median_relative = statistics.median(relatives)
    significant = abs(median_delta) >= ABS_MS and abs(median_relative) >= REL_PERCENT
    winner = 0
    if all(delta < 0.0 for delta in deltas) and significant:
        winner = a
    elif all(delta > 0.0 for delta in deltas) and significant:
        winner = b
    return {
        "winner": winner,
        "medianPairedDeltaMs": median_delta,
        "medianPairedRelativePercent": median_relative,
        "pairs": pairs,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    run_dir = arguments.run_dir.resolve()
    aggregate_path = run_dir / "aggregate.json"
    aggregate = load(aggregate_path)
    data = {}
    source_files = []
    for path in sorted((run_dir / "raw").glob("*.json")):
        match = NAME.match(path.name)
        if match is None:
            continue
        regime, light_count, radius_tenths, slices, round_index = match.groups()
        result = load(path)
        key = (regime, int(light_count), int(radius_tenths) / 10.0, int(slices), int(round_index))
        data[key] = float(result["profiler"]["summary"]["wallFrame"]["median"])
        source_files.append({"path": str(path.relative_to(run_dir)), "sha256": sha256(path)})
    if len(data) != 615:
        raise RuntimeError("expected 615 performance process files, found {}".format(len(data)))

    mismatches = []
    cells = []
    for expected in aggregate["cells"]:
        regime = str(expected["regime"])
        light_count = int(expected["lightCount"])
        radius = float(expected["radius"])
        medians = {
            slices: statistics.median(
                data[(regime, light_count, radius, slices, round_index)]
                for round_index in (1, 2, 3)
            )
            for slices in SLICES
        }
        leader = min(SLICES, key=lambda slices: medians[slices])
        top = []
        for candidate in SLICES:
            beaten = False
            for other in SLICES:
                if other == candidate:
                    continue
                a, b = sorted((other, candidate))
                if compare(data, regime, light_count, radius, a, b)["winner"] == other:
                    beaten = True
                    break
            if not beaten:
                top.append(candidate)
        candidate = min(top)
        go_comparison = compare(data, regime, light_count, radius, candidate, 1)
        go = candidate > 1 and go_comparison["winner"] == candidate
        final_runtime = candidate if go else 1
        actual = {
            "numericLeader": int(expected["numericLeader"]),
            "statisticalTopSet": [int(value) for value in expected["statisticalTopSet"]],
            "statisticalCandidateSliceCount": int(expected["statisticalCandidateSliceCount"]),
            "finalRuntimeSliceCount": int(expected["finalRuntimeSliceCount"]),
            "recommendedSliceCount": int(expected["recommendedSliceCount"]),
            "depthSlicingGo": bool(expected["depthSlicingGo"]),
        }
        calculated = {
            "numericLeader": leader,
            "statisticalTopSet": top,
            "statisticalCandidateSliceCount": candidate,
            "finalRuntimeSliceCount": final_runtime,
            "recommendedSliceCount": final_runtime,
            "depthSlicingGo": go,
        }
        if actual != calculated:
            mismatches.append({
                "regime": regime,
                "lightCount": light_count,
                "radius": radius,
                "actual": actual,
                "calculated": calculated,
            })
        cells.append({
            "regime": regime,
            "lightCount": light_count,
            "radius": radius,
            "wallProcessMedians": {str(key): value for key, value in medians.items()},
            **calculated,
            "recommendedVsS1": go_comparison,
        })

    output = {
        "schemaVersion": 2,
        "success": not mismatches,
        "method": "stdlib-only independent raw JSON recalculation",
        "winnerThreshold": {
            "absoluteMilliseconds": ABS_MS,
            "relativePercent": REL_PERCENT,
            "pairedDirectionAgreement": "3/3",
        },
        "performanceProcessCount": len(data),
        "cellCount": len(cells),
        "cachedGoCount": sum(cell["depthSlicingGo"] for cell in cells if cell["regime"] == "cached"),
        "rebuildGoCount": sum(cell["depthSlicingGo"] for cell in cells if cell["regime"] == "rebuild"),
        "aggregateSha256": sha256(aggregate_path),
        "mismatches": mismatches,
        "cells": cells,
        "sourceFiles": source_files,
    }
    output_path = run_dir / "verification" / "performance-recalculation.json"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(output, indent=2, sort_keys=True), encoding="utf-8")
    print("[performance-verification] {} processes={} cells={} cachedGo={} rebuildGo={}".format(
        "PASS" if output["success"] else "FAIL",
        output["performanceProcessCount"], output["cellCount"],
        output["cachedGoCount"], output["rebuildGoCount"],
    ))
    raise SystemExit(0 if output["success"] else 2)


if __name__ == "__main__":
    main()
