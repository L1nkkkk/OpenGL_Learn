#!/usr/bin/env python3
"""Independent raw-data verifier for the Analytic Screen versus Tile S1 A/B."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


ABS_MS = 0.05
REL_PERCENT = 3.0
CELLS = ("low-boundary", "low-radius", "wide-coverage", "representative", "heavy")


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def median(values: list[float]) -> float:
    return float(np.median(np.asarray(values, dtype=np.float64)))


def wall_samples(result: dict[str, Any]) -> list[float]:
    return [float(value) for value in result["profiler"]["samples"]["wallFrame"]]


def camera_trace(result: dict[str, Any]) -> list[tuple[Any, ...]]:
    return [
        (
            int(sample["measurementFrame"]),
            int(sample["timelineFrame"]),
            int(sample["cycleFrame"]),
            tuple(float(value) for value in sample["cameraPosition"]),
            tuple(float(value) for value in sample["cameraTarget"]),
        )
        for sample in result["motionTimeline"]["samples"]
    ]


def compare(analytic: dict[int, float], tile: dict[int, float]) -> dict[str, Any]:
    pairs = []
    for round_index in (1, 2, 3):
        av = analytic[round_index]
        tv = tile[round_index]
        delta = tv - av
        pairs.append(
            {
                "round": round_index,
                "analyticMedianMs": av,
                "tileMedianMs": tv,
                "tileMinusAnalyticMs": delta,
                "tileRelativePercent": delta / av * 100.0 if av else 0.0,
            }
        )
    deltas = [item["tileMinusAnalyticMs"] for item in pairs]
    relatives = [item["tileRelativePercent"] for item in pairs]
    if all(value < 0.0 for value in deltas):
        direction = "tile-faster"
    elif all(value > 0.0 for value in deltas):
        direction = "analytic-faster"
    elif all(value == 0.0 for value in deltas):
        direction = "equal"
    else:
        direction = "mixed"
    paired_delta = median(deltas)
    paired_relative = median(relatives)
    significant = (
        direction in ("tile-faster", "analytic-faster")
        and abs(paired_delta) >= ABS_MS
        and abs(paired_relative) >= REL_PERCENT
    )
    return {
        "winner": (
            "tile"
            if significant and direction == "tile-faster"
            else "analytic"
            if significant and direction == "analytic-faster"
            else "tie"
        ),
        "direction": direction,
        "significant": significant,
        "medianPairedDeltaMs": paired_delta,
        "medianPairedRelativePercent": paired_relative,
        "pairs": pairs,
    }


def quality(a_path: Path, b_path: Path, gate: dict[str, Any]) -> dict[str, Any]:
    a = np.asarray(Image.open(a_path).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(b_path).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        raise ValueError(f"image shape mismatch: {a_path} / {b_path}")
    difference = np.abs(a - b)
    maximum = int(np.max(difference))
    mean = float(np.mean(difference))
    p99 = float(np.percentile(difference, 99))
    return {
        "maxChannelLsb": maximum,
        "meanChannelLsb": mean,
        "p99ChannelLsb": p99,
        "differentPixelCount": int(np.count_nonzero(np.any(difference != 0, axis=2))),
        "passed": maximum <= int(gate["maxChannelLsb"])
        and mean <= float(gate["meanChannelLsb"])
        and p99 <= float(gate["p99ChannelLsb"]),
    }


def close(left: float, right: float, tolerance: float = 1e-9) -> bool:
    return abs(float(left) - float(right)) <= tolerance


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    project = Path(__file__).resolve().parent.parent

    pre_path = run_dir / "pre-capture-manifest.json"
    manifest_path = run_dir / "capture-manifest.json"
    aggregate_path = run_dir / "aggregate.json"
    pre = load(pre_path)
    manifest = load(manifest_path)
    aggregate = load(aggregate_path)
    protocol_hash = sha256(run_dir / pre["protocol"])
    if protocol_hash != pre["protocolSha256"] or protocol_hash != manifest["protocolSha256"]:
        raise ValueError("protocol hash chain mismatch")
    if sha256(pre_path) != manifest["preCaptureManifestSha256"]:
        raise ValueError("pre-capture manifest hash chain mismatch")
    if sha256(manifest_path) != aggregate["captureManifestSha256"]:
        raise ValueError("aggregate capture-manifest hash mismatch")
    if sha256(Path(pre["executable"])) != pre["executableSha256"]:
        raise ValueError("frozen executable changed")

    expected = {item["stem"]: item for item in pre["expectedRuns"]}
    completed = {item["stem"]: item for item in manifest["completedRuns"]}
    if len(expected) != 60 or len(completed) != 60:
        raise ValueError("expected exactly 60 formal processes")

    raw: dict[str, dict[str, Any]] = {}
    artifact_count = 0
    for stem, item in expected.items():
        record = completed.get(stem)
        if record is None:
            raise ValueError(f"missing completed record: {stem}")
        for field, hash_field in (
            ("result", "resultSha256"),
            ("capture", "captureSha256"),
            ("log", "logSha256"),
        ):
            path = run_dir / item[field]
            if sha256(path) != record[hash_field]:
                raise ValueError(f"artifact hash mismatch: {stem}/{field}")
            artifact_count += 1
        result = load(run_dir / item["result"])
        point = result["pointLightStress"]
        grid = point["gridRuntime"]
        total = int(pre["warmupFrames"]) + int(pre["sampleFrames"])
        if (
            not result["success"]
            or result["buildConfiguration"] != "Release"
            or result["architecture"] != "x64"
            or result["resolution"] != [1920, 1080]
            or len(wall_samples(result)) != int(pre["sampleFrames"])
            or point["renderMode"] != item["renderMode"]
            or int(point["generatedLightCount"]) != int(item["lightCount"])
            or abs(float(point["volumeRadius"]) - float(item["radius"])) > 1e-4
        ):
            raise ValueError(f"basic raw semantic mismatch: {stem}")
        if item["tile"]:
            moving = item["camera"] == "moving"
            builds = total if moving else 1
            hits = 0 if moving else total - 1
            if (
                not grid["valid"]
                or grid["overflow"]
                or grid["error"]
                or int(grid["sliceCount"]) != 1
                or int(grid["buildCount"]) != builds
                or int(grid["uploadCount"]) != builds
                or int(grid["cacheHitCount"]) != hits
                or int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) != 1
            ):
                raise ValueError(f"tile raw semantic mismatch: {stem}")
        else:
            if int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) != int(
                item["lightCount"]
            ):
                raise ValueError(f"analytic draw-count mismatch: {stem}")
        raw[stem] = result

    aggregate_cells = {item["name"]: item for item in aggregate["cells"]}
    recomputed: list[dict[str, Any]] = []
    quality_pairs: list[dict[str, Any]] = []
    motion_pair_count = 0
    signature_groups = 0
    static_tile_wins = 0
    static_analytic_wins = 0
    moving_tile_wins = 0
    moving_analytic_wins = 0
    for cell in CELLS:
        cell_items = [item for item in expected.values() if item["cell"] == cell]
        signatures = {
            (
                raw[item["stem"]]["pointLightStress"]["sceneSignature"],
                raw[item["stem"]]["pointLightStress"]["submissionSignature"],
            )
            for item in cell_items
        }
        if len(signatures) != 1:
            raise ValueError(f"signature drift: {cell}")
        signature_groups += 1
        for camera in ("static", "moving"):
            a_by_round: dict[int, float] = {}
            t_by_round: dict[int, float] = {}
            for round_index in (1, 2, 3):
                a_item = next(
                    item
                    for item in cell_items
                    if item["camera"] == camera and not item["tile"] and int(item["round"]) == round_index
                )
                t_item = next(
                    item
                    for item in cell_items
                    if item["camera"] == camera and item["tile"] and int(item["round"]) == round_index
                )
                a_result = raw[a_item["stem"]]
                t_result = raw[t_item["stem"]]
                a_by_round[round_index] = median(wall_samples(a_result))
                t_by_round[round_index] = median(wall_samples(t_result))
                if camera == "moving":
                    if camera_trace(a_result) != camera_trace(t_result):
                        raise ValueError(f"motion mismatch: {cell}/round{round_index}")
                    motion_pair_count += 1
                q = quality(run_dir / a_item["capture"], run_dir / t_item["capture"], pre["qualityGate"])
                if not q["passed"]:
                    raise ValueError(f"quality gate failed: {cell}/{camera}/round{round_index}: {q}")
                quality_pairs.append({"cell": cell, "camera": camera, "round": round_index, **q})
            value = compare(a_by_round, t_by_round)
            stored = aggregate_cells[cell]["cameras"][camera]["comparison"]
            if (
                value["winner"] != stored["winner"]
                or value["direction"] != stored["direction"]
                or bool(value["significant"]) != bool(stored["significant"])
                or not close(value["medianPairedDeltaMs"], stored["medianPairedDeltaMs"])
                or not close(
                    value["medianPairedRelativePercent"], stored["medianPairedRelativePercent"]
                )
            ):
                raise ValueError(f"aggregate comparison mismatch: {cell}/{camera}")
            recomputed.append({"cell": cell, "camera": camera, **value})
            if camera == "static":
                static_tile_wins += value["winner"] == "tile"
                static_analytic_wins += value["winner"] == "analytic"
            else:
                moving_tile_wins += value["winner"] == "tile"
                moving_analytic_wins += value["winner"] == "analytic"

    decision = aggregate["decision"]
    if (
        int(decision["staticTileWins"]) != static_tile_wins
        or int(decision["staticAnalyticWins"]) != static_analytic_wins
        or int(decision["movingTileWins"]) != moving_tile_wins
        or int(decision["movingAnalyticWins"]) != moving_analytic_wins
    ):
        raise ValueError("aggregate decision counts mismatch")
    expected_verdict = (
        "default-go"
        if moving_tile_wins == len(CELLS)
        else "conditional-go"
        if static_tile_wins > 0
        else "no-go"
    )
    if decision["verdict"] != expected_verdict:
        raise ValueError("aggregate verdict mismatch")

    grid_shader = (project / "shaders" / "pointLightGridFragment.glsl").read_text(encoding="utf-8")
    global_header = (project / "Global.h").read_text(encoding="utf-8")
    source_contract = {
        "exactSpherePredicate": "if (distanceSquared > positionRadius.w) continue;" in grid_shader,
        "invalidGBufferDiscardBeforeGrid": grid_shader.find("discard") < grid_shader.find("gridMetadata"),
        "defaultRemainsAnalyticScreen": bool(
            re.search(
                r"POINT_LIGHT_RENDER_MODE\s*=\s*PointLightRenderProperty::AnalyticScreen",
                global_header,
            )
        ),
    }
    if not all(source_contract.values()):
        raise ValueError(f"source contract failed: {source_contract}")

    verification = {
        "schemaVersion": 1,
        "passed": True,
        "method": "independent raw JSON medians and paired thresholds + direct image diffs + full camera sample equality",
        "protocolSha256": protocol_hash,
        "preCaptureManifestSha256": sha256(pre_path),
        "captureManifestSha256": sha256(manifest_path),
        "aggregateSha256BeforeVerification": sha256(aggregate_path),
        "artifactCount": artifact_count,
        "processCount": len(raw),
        "comparisonCount": len(recomputed),
        "motionPairCount": motion_pair_count,
        "signatureGroupCount": signature_groups,
        "qualityPairCount": len(quality_pairs),
        "qualityAllPassed": all(item["passed"] for item in quality_pairs),
        "worstQuality": {
            "maxChannelLsb": max(item["maxChannelLsb"] for item in quality_pairs),
            "meanChannelLsb": max(item["meanChannelLsb"] for item in quality_pairs),
            "p99ChannelLsb": max(item["p99ChannelLsb"] for item in quality_pairs),
        },
        "decision": {
            "verdict": expected_verdict,
            "staticTileWins": static_tile_wins,
            "staticAnalyticWins": static_analytic_wins,
            "movingTileWins": moving_tile_wins,
            "movingAnalyticWins": moving_analytic_wins,
        },
        "comparisons": recomputed,
        "sourceContract": source_contract,
    }
    dump(run_dir / "verification" / "independent-verification.json", verification)
    print(
        "[verification] PASS "
        f"artifacts={artifact_count} comparisons={len(recomputed)} motionPairs={motion_pair_count} "
        f"quality={len(quality_pairs)} verdict={expected_verdict}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
