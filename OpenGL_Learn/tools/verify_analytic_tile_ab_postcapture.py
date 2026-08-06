#!/usr/bin/env python3
"""Audited post-capture correction for the frozen Analytic/Tile verifier."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import verify_analytic_tile_ab as frozen


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    project = Path(__file__).resolve().parent.parent
    amendment_path = run_dir / "VERIFICATION_AMENDMENT_CN.md"
    if not amendment_path.is_file():
        raise ValueError("missing post-capture verification amendment")

    pre_path = run_dir / "pre-capture-manifest.json"
    manifest_path = run_dir / "capture-manifest.json"
    aggregate_path = run_dir / "aggregate.json"
    pre = frozen.load(pre_path)
    manifest = frozen.load(manifest_path)
    aggregate = frozen.load(aggregate_path)
    frozen_verifier = Path(__file__).with_name("verify_analytic_tile_ab.py")

    # Preserve and prove the exact frozen verifier failure before applying the
    # corrected static-source predicate below.
    frozen_run = subprocess.run(
        [sys.executable, str(frozen_verifier), "--run-dir", str(run_dir)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    known_failure = (
        frozen_run.returncode != 0
        and "source contract failed" in frozen_run.stdout
        and "'invalidGBufferDiscardBeforeGrid': False" in frozen_run.stdout
        and "'exactSpherePredicate': True" in frozen_run.stdout
        and "'defaultRemainsAnalyticScreen': True" in frozen_run.stdout
    )
    if not known_failure:
        raise ValueError(
            "frozen verifier did not reproduce the single documented source-contract failure:\n"
            + frozen_run.stdout
        )
    verification_dir = run_dir / "verification"
    verification_dir.mkdir(parents=True, exist_ok=True)
    failure_log = verification_dir / "frozen-verifier-known-failure.log"
    failure_log.write_text(frozen_run.stdout, encoding="utf-8")

    protocol_hash = frozen.sha256(run_dir / pre["protocol"])
    if protocol_hash != pre["protocolSha256"] or protocol_hash != manifest["protocolSha256"]:
        raise ValueError("protocol hash chain mismatch")
    if frozen.sha256(pre_path) != manifest["preCaptureManifestSha256"]:
        raise ValueError("pre-capture manifest hash chain mismatch")
    if frozen.sha256(manifest_path) != aggregate["captureManifestSha256"]:
        raise ValueError("capture manifest / aggregate hash mismatch")
    if frozen.sha256(Path(pre["executable"])) != pre["executableSha256"]:
        raise ValueError("frozen executable changed")
    if frozen.sha256(frozen_verifier) != pre["sourceHashes"]["verifier"]:
        raise ValueError("frozen verifier changed after capture")

    expected = {item["stem"]: item for item in pre["expectedRuns"]}
    completed = {item["stem"]: item for item in manifest["completedRuns"]}
    if len(expected) != 60 or len(completed) != 60:
        raise ValueError("expected exactly 60 formal processes")
    raw: dict[str, dict[str, Any]] = {}
    artifact_count = 0
    total_frames = int(pre["warmupFrames"]) + int(pre["sampleFrames"])
    for stem, item in expected.items():
        record = completed.get(stem)
        if record is None:
            raise ValueError(f"missing manifest record: {stem}")
        for field, hash_field in (
            ("result", "resultSha256"),
            ("capture", "captureSha256"),
            ("log", "logSha256"),
        ):
            path = run_dir / item[field]
            if frozen.sha256(path) != record[hash_field]:
                raise ValueError(f"artifact hash mismatch: {stem}/{field}")
            artifact_count += 1
        result = frozen.load(run_dir / item["result"])
        point = result["pointLightStress"]
        grid = point["gridRuntime"]
        if (
            not result["success"]
            or result["buildConfiguration"] != "Release"
            or result["architecture"] != "x64"
            or result["resolution"] != [1920, 1080]
            or len(frozen.wall_samples(result)) != int(pre["sampleFrames"])
            or point["renderMode"] != item["renderMode"]
            or int(point["generatedLightCount"]) != int(item["lightCount"])
            or abs(float(point["volumeRadius"]) - float(item["radius"])) > 1e-4
            or int(result["profiler"]["summary"]["pointLightsSubmitted"]["median"])
            != int(item["lightCount"])
            or int(result["profiler"]["summary"]["pointLightsCulled"]["median"]) != 0
        ):
            raise ValueError(f"raw semantic mismatch: {stem}")
        if item["tile"]:
            moving = item["camera"] == "moving"
            expected_builds = total_frames if moving else 1
            expected_hits = 0 if moving else total_frames - 1
            if (
                not grid["valid"]
                or grid["overflow"]
                or grid["error"]
                or int(grid["sliceCount"]) != 1
                or int(grid["buildCount"]) != expected_builds
                or int(grid["uploadCount"]) != expected_builds
                or int(grid["cacheHitCount"]) != expected_hits
                or int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) != 1
            ):
                raise ValueError(f"tile semantic mismatch: {stem}")
        elif int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) != int(
            item["lightCount"]
        ):
            raise ValueError(f"analytic draw-count mismatch: {stem}")
        raw[stem] = result

    aggregate_cells = {item["name"]: item for item in aggregate["cells"]}
    comparisons: list[dict[str, Any]] = []
    qualities: list[dict[str, Any]] = []
    motion_pair_count = 0
    signature_group_count = 0
    counts = {
        "staticTileWins": 0,
        "staticAnalyticWins": 0,
        "movingTileWins": 0,
        "movingAnalyticWins": 0,
    }
    for cell in frozen.CELLS:
        cell_items = [item for item in expected.values() if item["cell"] == cell]
        signatures = {
            (
                raw[item["stem"]]["pointLightStress"]["sceneSignature"],
                raw[item["stem"]]["pointLightStress"]["submissionSignature"],
            )
            for item in cell_items
        }
        if len(signatures) != 1:
            raise ValueError(f"scene/submission signature drift: {cell}")
        signature_group_count += 1
        for camera in ("static", "moving"):
            analytic_by_round: dict[int, float] = {}
            tile_by_round: dict[int, float] = {}
            for round_index in (1, 2, 3):
                analytic_item = next(
                    item
                    for item in cell_items
                    if item["camera"] == camera
                    and not item["tile"]
                    and int(item["round"]) == round_index
                )
                tile_item = next(
                    item
                    for item in cell_items
                    if item["camera"] == camera
                    and item["tile"]
                    and int(item["round"]) == round_index
                )
                analytic_result = raw[analytic_item["stem"]]
                tile_result = raw[tile_item["stem"]]
                analytic_by_round[round_index] = frozen.median(frozen.wall_samples(analytic_result))
                tile_by_round[round_index] = frozen.median(frozen.wall_samples(tile_result))
                if camera == "moving":
                    if frozen.camera_trace(analytic_result) != frozen.camera_trace(tile_result):
                        raise ValueError(f"camera trace mismatch: {cell}/round{round_index}")
                    motion_pair_count += 1
                image = frozen.quality(
                    run_dir / analytic_item["capture"],
                    run_dir / tile_item["capture"],
                    pre["qualityGate"],
                )
                if not image["passed"]:
                    raise ValueError(f"quality gate failed: {cell}/{camera}/round{round_index}")
                qualities.append({"cell": cell, "camera": camera, "round": round_index, **image})

            comparison = frozen.compare(analytic_by_round, tile_by_round)
            stored = aggregate_cells[cell]["cameras"][camera]["comparison"]
            if (
                comparison["winner"] != stored["winner"]
                or comparison["direction"] != stored["direction"]
                or bool(comparison["significant"]) != bool(stored["significant"])
                or not frozen.close(
                    comparison["medianPairedDeltaMs"], stored["medianPairedDeltaMs"]
                )
                or not frozen.close(
                    comparison["medianPairedRelativePercent"],
                    stored["medianPairedRelativePercent"],
                )
            ):
                raise ValueError(f"aggregate mismatch: {cell}/{camera}")
            comparisons.append({"cell": cell, "camera": camera, **comparison})
            if camera == "static":
                counts["staticTileWins"] += comparison["winner"] == "tile"
                counts["staticAnalyticWins"] += comparison["winner"] == "analytic"
            else:
                counts["movingTileWins"] += comparison["winner"] == "tile"
                counts["movingAnalyticWins"] += comparison["winner"] == "analytic"

    expected_verdict = (
        "default-go"
        if counts["movingTileWins"] == len(frozen.CELLS)
        else "conditional-go"
        if counts["staticTileWins"] > 0
        else "no-go"
    )
    if aggregate["decision"]["verdict"] != expected_verdict:
        raise ValueError("aggregate verdict mismatch")
    for name, value in counts.items():
        if int(aggregate["decision"][name]) != value:
            raise ValueError(f"aggregate decision count mismatch: {name}")

    grid_shader = (project / "shaders" / "pointLightGridFragment.glsl").read_text(encoding="utf-8")
    global_header = (project / "Global.h").read_text(encoding="utf-8")
    discard_token = "if (!LoadWorldPosition(texCoords, fragPos)) discard;"
    fetch_token = "texelFetch(gridMetadata"
    source_contract = {
        "exactSpherePredicate": "if (distanceSquared > positionRadius.w) continue;" in grid_shader,
        "invalidGBufferDiscardBeforeFirstMetadataFetch": discard_token in grid_shader
        and fetch_token in grid_shader
        and grid_shader.index(discard_token) < grid_shader.index(fetch_token),
        "defaultRemainsAnalyticScreen": bool(
            re.search(
                r"POINT_LIGHT_RENDER_MODE\s*=\s*PointLightRenderProperty::AnalyticScreen",
                global_header,
            )
        ),
    }
    if not all(source_contract.values()):
        raise ValueError(f"corrected source contract failed: {source_contract}")

    result = {
        "schemaVersion": 2,
        "passed": True,
        "postCaptureAmendment": True,
        "method": "frozen raw verifier logic rerun with one documented static-source predicate corrected",
        "amendmentReason": (
            "the frozen predicate compared discard with the uniform declaration; "
            "the corrected predicate compares discard with the first texelFetch(gridMetadata)"
        ),
        "protocolSha256": protocol_hash,
        "preCaptureManifestSha256": frozen.sha256(pre_path),
        "captureManifestSha256": frozen.sha256(manifest_path),
        "aggregateSha256BeforeVerification": frozen.sha256(aggregate_path),
        "amendmentSha256": frozen.sha256(amendment_path),
        "frozenVerifierSha256": frozen.sha256(frozen_verifier),
        "postCaptureVerifierSha256": frozen.sha256(Path(__file__).resolve()),
        "frozenVerifierFailureLogSha256": frozen.sha256(failure_log),
        "knownFrozenVerifierFailureReproduced": True,
        "artifactCount": artifact_count,
        "processCount": len(raw),
        "comparisonCount": len(comparisons),
        "motionPairCount": motion_pair_count,
        "signatureGroupCount": signature_group_count,
        "qualityPairCount": len(qualities),
        "qualityAllPassed": all(item["passed"] for item in qualities),
        "worstQuality": {
            "maxChannelLsb": max(item["maxChannelLsb"] for item in qualities),
            "meanChannelLsb": max(item["meanChannelLsb"] for item in qualities),
            "p99ChannelLsb": max(item["p99ChannelLsb"] for item in qualities),
        },
        "decision": {"verdict": expected_verdict, **counts},
        "comparisons": comparisons,
        "sourceContract": source_contract,
    }
    frozen.dump(verification_dir / "independent-verification.json", result)
    frozen.dump(
        verification_dir / "amendment-manifest.json",
        {
            "schemaVersion": 1,
            "amendment": str(amendment_path.relative_to(run_dir)),
            "amendmentSha256": result["amendmentSha256"],
            "frozenVerifier": str(frozen_verifier),
            "frozenVerifierSha256": result["frozenVerifierSha256"],
            "postCaptureVerifier": str(Path(__file__).resolve()),
            "postCaptureVerifierSha256": result["postCaptureVerifierSha256"],
            "captureManifestSha256": result["captureManifestSha256"],
            "verificationResultSha256": frozen.sha256(
                verification_dir / "independent-verification.json"
            ),
        },
    )
    print(
        "[post-capture verification] PASS "
        f"artifacts={artifact_count} comparisons={len(comparisons)} "
        f"motionPairs={motion_pair_count} quality={len(qualities)} verdict={expected_verdict}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
