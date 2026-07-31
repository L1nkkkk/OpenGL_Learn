#!/usr/bin/env python3
"""Validate the accepted deterministic-camera SSAO batch without rerunning it."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from PIL import Image


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    expect(isinstance(value, dict), f"JSON object expected: {path}")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve(project: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else project / path


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    script = Path(__file__).resolve()
    project = script.parent.parent
    root = arguments.input_directory.resolve()
    expect(root.is_relative_to(project), "input directory must be in the project")
    output = (
        arguments.output.resolve()
        if arguments.output is not None
        else root / "FINAL_VALIDATION.json"
    )
    expect(output.is_relative_to(root), "validation output must stay in the batch")

    manifest = read_json(root / "run-manifest.json")
    analysis = read_json(root / "analysis-manifest.json")
    summary = read_json(root / "summary.json")
    expect(manifest["status"] == "pass", "runner status")
    expect(analysis["status"] == "pass", "analysis status")
    expect(summary["decision"]["status"] == "NO-GO", "decision status")

    executable = Path(manifest["source"]["releaseExecutable"])
    executable_hash = sha256(executable)
    expect(
        executable_hash.lower()
        == manifest["source"]["releaseExecutableSha256"].lower(),
        "release executable hash changed",
    )
    expect(len(manifest["performance"]["runs"]) == 36, "performance run count")
    expect(len(manifest["quality"]["runs"]) == 6, "quality run count")
    performance_validation = manifest["performance"]["validation"]
    quality_validation = manifest["quality"]["validation"]
    for name in (
        "requiredQueryCountsExact",
        "nestedTimerBoundsVerified",
        "fixedStateVerified",
        "cameraPathIdenticalWithinScene",
        "allReadbackDisabled",
    ):
        expect(performance_validation[name] is True, f"performance {name}")
    for name in (
        "cameraPathIdenticalWithinScene",
        "allFilesHashed",
        "performanceSamplesExcluded",
    ):
        expect(quality_validation[name] is True, f"quality {name}")
    expect(quality_validation["captureFileCount"] == 3840, "capture file count")

    raw_result_count = 0
    for record in manifest["performance"]["runs"]:
        result_path = resolve(project, record["result"])
        expect(sha256(result_path) == record["resultSha256"], "performance JSON hash")
        result = read_json(result_path)
        expect(result["success"] is True, "performance result success")
        expect(result["measuredFrames"] == 2000, "performance frame count")
        expect(len(result["motionTimeline"]["samples"]) == 2000, "timeline samples")
        raw_result_count += 1
    for scene in ("sponza", "san-miguel"):
        signatures = {
            record["cameraSignatureSha256"]
            for record in manifest["performance"]["runs"]
            if record["scene"] == scene
        }
        expect(len(signatures) == 1, f"performance camera signature: {scene}")
        quality_signatures = {
            record["cameraSignatureSha256"]
            for record in manifest["quality"]["runs"]
            if record["scene"] == scene
        }
        expect(len(quality_signatures) == 1, f"quality camera signature: {scene}")

    old_before = read_json(
        resolve(
            project,
            manifest["source"]["protectedOldBatches"]["beforeManifest"],
        )
    )
    old_after = read_json(
        resolve(
            project,
            manifest["source"]["protectedOldBatches"]["afterManifest"],
        )
    )
    expect(
        old_before["aggregateSha256"] == old_after["aggregateSha256"]
        and old_before["fileCount"] == old_after["fileCount"],
        "protected old batches changed",
    )

    verified_analysis_outputs = 0
    for item in analysis["validation"]["outputFiles"]:
        path = root / item["path"]
        expect(path.is_file(), f"analysis output missing: {path}")
        expect(path.stat().st_size == item["bytes"], f"analysis output size: {path}")
        expect(sha256(path) == item["sha256"], f"analysis output hash: {path}")
        verified_analysis_outputs += 1

    gif_frames: dict[str, int] = {}
    for path in sorted((root / "videos").glob("*.gif")):
        with Image.open(path) as image:
            frame_count = int(getattr(image, "n_frames", 1))
        expect(frame_count == 120, f"GIF frame count: {path}")
        gif_frames[path.name] = frame_count
    expect(len(gif_frames) == 4, "GIF count")

    error_pattern = re.compile(
        r"GL_INVALID|GL error|framebuffer.*(?:incomplete|failed)|"
        r"FBO.*(?:incomplete|failed)|fatal|assertion failed|"
        r"query.*(?:missing|mismatch)",
        re.IGNORECASE,
    )
    log_errors: list[dict[str, str]] = []
    for directory in (root / "performance" / "logs", root / "quality" / "logs"):
        for path in sorted(directory.glob("*.log")):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if error_pattern.search(line):
                    log_errors.append({"path": str(path), "line": line})
    expect(not log_errors, "GL/FBO/query errors found in logs")

    report_path = root / "SSAO_TEMPORAL_DETERMINISTIC_REPORT_CN.md"
    report = report_path.read_text(encoding="utf-8")
    expect("�" not in report, "report contains replacement characters")
    for required in (
        "最终判断：**NO-GO**",
        "Full-32",
        "screen-space",
        "不混池",
        "浏览器插件初始化",
    ):
        expect(required in report, f"report section missing: {required}")

    script_hashes = {}
    for name in (
        "run_ssao_temporal_deterministic.py",
        "select_ssao_temporal_dynamic_rois.py",
        "analyze_ssao_temporal_deterministic.py",
        "validate_ssao_temporal_deterministic.py",
    ):
        path = script.parent / name
        script_hashes[name] = sha256(path)

    validation = {
        "schemaVersion": 1,
        "generatedAtUtc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "status": "pass",
        "decision": summary["decision"]["status"],
        "releaseExecutable": str(executable),
        "releaseExecutableSha256": executable_hash,
        "checks": {
            "performanceProcesses": raw_result_count,
            "qualityProcesses": len(manifest["quality"]["runs"]),
            "performanceFrames": raw_result_count * 2000,
            "qualityCaptureFiles": quality_validation["captureFileCount"],
            "timerQueriesExact": True,
            "nestedTimerBoundsVerified": True,
            "cameraSignaturesExact": True,
            "performanceReadbackDisabled": True,
            "protectedOldBatchesUnchanged": True,
            "analysisOutputsVerified": verified_analysis_outputs,
            "gifFrames": gif_frames,
            "logErrorCount": len(log_errors),
            "reportUtf8Valid": True,
        },
        "reproductionScriptSha256": script_hashes,
    }
    write_json(output, validation)
    print(json.dumps(validation, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
