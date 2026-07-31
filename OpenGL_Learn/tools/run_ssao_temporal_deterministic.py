#!/usr/bin/env python3
"""Run deterministic moving-camera SSAO performance and quality captures."""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CONFIGURATIONS = (
    {
        "name": "legacy-full64",
        "mode": "legacy-full",
        "samples": 64,
        "halfResolution": False,
        "bilateral": False,
    },
    {
        "name": "legacy-full32",
        "mode": "legacy-full",
        "samples": 32,
        "halfResolution": False,
        "bilateral": False,
    },
    {
        "name": "half-bilateral64",
        "mode": "half-bilateral",
        "samples": 64,
        "halfResolution": True,
        "bilateral": True,
    },
)
CONFIG_BY_NAME = {item["name"]: item for item in CONFIGURATIONS}
SCENE_IDS = ("sponza", "san-miguel")
QUALITY_ROIS = {
    "sponza": (
        {"name": "edge", "x": 642, "y": 606, "width": 256, "height": 192},
        {"name": "contact", "x": 70, "y": 731, "width": 256, "height": 192},
    ),
    "san-miguel": (
        {"name": "edge", "x": 822, "y": 760, "width": 256, "height": 192},
        {"name": "contact", "x": 283, "y": 868, "width": 256, "height": 192},
    ),
}
CAPTURE_HALO_PIXELS = 4
CAMERA_POSITION_RADIUS_RATIO = 0.005
CAMERA_TARGET_RADIUS_RATIO = 0.002
NVIDIA_FIELDS = (
    "timestamp",
    "index",
    "name",
    "driver_version",
    "temperature.gpu",
    "clocks.current.graphics",
    "clocks.current.sm",
    "clocks.current.memory",
    "power.draw",
    "power.limit",
    "utilization.gpu",
    "pstate",
)
SOURCE_CHECKPOINT_FILES = (
    "test.cpp",
    "BenchmarkMotionTimeline.cpp",
    "BenchmarkMotionTimeline.h",
    "DeferRenderPass.h",
    "Global.h",
    "PerformanceBenchmark.cpp",
    "PerformanceBenchmark.h",
    "Profiler.h",
    "SSAORenderPass.cpp",
    "SSAORenderPass.h",
    "OpenGL_Learn.vcxproj",
    "shaders/ssaoFragment.glsl",
    "shaders/ssaoVertex.glsl",
    "shaders/ssaoUpsampleFragment.glsl",
    "shaders/ssaoUpsampleVertex.glsl",
    "tools/run_ssao_temporal_deterministic.py",
)
OLD_BATCH_DIRECTORIES = (
    "benchmark-results/ssao-half-resolution/ssao-half-formal-20260731/formal",
    "benchmark-results/ssao-config-comparison/full32-vs-half64-bilateral-20260731",
    "benchmark-results/ssao-factorial/ssao-factorial-balanced-20260731",
    "benchmark-results/ssao-temporal/ssao-temporal-deterministic-20260731",
)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    expect(isinstance(value, dict), f"JSON root is not an object: {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def snapshot_directories(
    project_directory: Path, relative_directories: tuple[str, ...]
) -> dict[str, Any]:
    directories = []
    aggregate = hashlib.sha256()
    for relative_directory in relative_directories:
        root = project_directory / relative_directory
        expect(root.is_dir(), f"protected old batch is missing: {root}")
        files = []
        for path in sorted(item for item in root.rglob("*") if item.is_file()):
            relative = path.relative_to(root).as_posix()
            digest = sha256_file(path)
            size = path.stat().st_size
            files.append({"path": relative, "bytes": size, "sha256": digest})
            aggregate.update(relative_directory.encode("utf-8"))
            aggregate.update(b"\0")
            aggregate.update(relative.encode("utf-8"))
            aggregate.update(b"\0")
            aggregate.update(str(size).encode("ascii"))
            aggregate.update(b"\0")
            aggregate.update(digest.encode("ascii"))
            aggregate.update(b"\n")
        directories.append(
            {
                "path": relative_directory,
                "fileCount": len(files),
                "files": files,
            }
        )
    return {
        "schemaVersion": 1,
        "generatedAtUtc": utc_now(),
        "aggregateSha256": aggregate.hexdigest(),
        "directoryCount": len(directories),
        "fileCount": sum(item["fileCount"] for item in directories),
        "directories": directories,
    }


def project_relative(path: Path, project_directory: Path) -> str:
    return path.resolve().relative_to(project_directory.resolve()).as_posix()


def load_validator(project_directory: Path) -> Any:
    validator_path = (
        project_directory / "tools" / "generate_ssao_half_resolution_report.py"
    )
    spec = importlib.util.spec_from_file_location(
        "ssao_temporal_validator", validator_path
    )
    expect(spec is not None and spec.loader is not None, "validator import failed")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def balanced_rows() -> list[list[dict[str, Any]]]:
    # A=Full64, B=Full32, C=Half64 Bilateral. The first three rows are a
    # Latin square; their reversed companions balance direction/carryover.
    names = (
        ("legacy-full64", "legacy-full32", "half-bilateral64"),
        ("legacy-full32", "half-bilateral64", "legacy-full64"),
        ("half-bilateral64", "legacy-full64", "legacy-full32"),
        ("half-bilateral64", "legacy-full32", "legacy-full64"),
        ("legacy-full64", "half-bilateral64", "legacy-full32"),
        ("legacy-full32", "legacy-full64", "half-bilateral64"),
    )
    rows = [[CONFIG_BY_NAME[name] for name in row] for row in names]
    for configuration in CONFIGURATIONS:
        positions = [
            position
            for row in rows
            for position, item in enumerate(row, start=1)
            if item["name"] == configuration["name"]
        ]
        expect(
            positions.count(1) == positions.count(2) == positions.count(3) == 2,
            f"unbalanced sequence for {configuration['name']}",
        )
    return rows


def nvidia_smi_path() -> Path | None:
    candidate = (
        Path(os.environ.get("WINDIR", "C:/Windows"))
        / "System32"
        / "nvidia-smi.exe"
    )
    return candidate if candidate.is_file() else None


def query_nvidia_smi(executable: Path | None) -> dict[str, Any]:
    if executable is None:
        return {
            "capturedAtUtc": utc_now(),
            "available": False,
            "reason": "nvidia-smi.exe was not found",
        }
    command = [
        str(executable),
        "--query-gpu=" + ",".join(NVIDIA_FIELDS),
        "--format=csv,noheader,nounits",
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "capturedAtUtc": utc_now(),
            "available": False,
            "reason": str(error),
            "command": command,
        }
    record: dict[str, Any] = {
        "capturedAtUtc": utc_now(),
        "available": completed.returncode == 0 and bool(completed.stdout.strip()),
        "exitCode": completed.returncode,
        "stderr": completed.stderr.strip(),
        "raw": completed.stdout.strip(),
    }
    if record["available"]:
        values = [
            value.strip()
            for value in completed.stdout.strip().splitlines()[0].split(",")
        ]
        if len(values) == len(NVIDIA_FIELDS):
            record["gpu"] = dict(zip(NVIDIA_FIELDS, values))
        else:
            record["available"] = False
            record["reason"] = "unexpected field count"
    return record


def capture_background_snapshot(executable: Path | None, path: Path) -> None:
    if executable is None:
        write_json(
            path,
            {
                "capturedAtUtc": utc_now(),
                "available": False,
                "reason": "nvidia-smi.exe was not found",
            },
        )
        return
    sections = []
    for arguments in (("-q",), ("pmon", "-c", "1", "-s", "um")):
        command = [str(executable), *arguments]
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=False,
                timeout=20,
            )
            sections.append(
                {
                    "command": command,
                    "exitCode": completed.returncode,
                    "stdout": completed.stdout,
                    "stderr": completed.stderr,
                }
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            sections.append({"command": command, "error": str(error)})
    write_json(path, {"capturedAtUtc": utc_now(), "sections": sections})


def number(value: Any) -> str:
    return format(float(value), ".17g")


def base_arguments(
    scene: dict[str, Any],
    configuration: dict[str, Any],
    result_relative: str,
    warmup_frames: int,
    measured_frames: int,
    timeline_cycle_frames: int,
) -> list[str]:
    return [
        "--classic-scene-test",
        "classic-scenes/" + str(scene["modelPath"]).replace("\\", "/"),
        "--classic-scene-name",
        str(scene["id"]),
        "--classic-scene-result",
        result_relative,
        "--classic-scene-camera",
        *(number(value) for value in scene["camera"]),
        "--classic-scene-target",
        *(number(value) for value in scene["target"]),
        "--classic-scene-up",
        *(number(value) for value in scene["up"]),
        "--classic-scene-radius",
        number(scene["normalizedRadius"]),
        "--classic-scene-world-scale",
        "1",
        "--classic-scene-fov",
        number(scene["fov"]),
        "--classic-scene-render-path",
        "pbr-deferred",
        "--classic-scene-width",
        "1920",
        "--classic-scene-height",
        "1080",
        "--classic-scene-ssao-mode",
        str(configuration["mode"]),
        "--classic-scene-ssao-samples",
        str(configuration["samples"]),
        "--classic-scene-warmup-frames",
        str(warmup_frames),
        "--classic-scene-capture-frame",
        str(warmup_frames + measured_frames),
        "--classic-scene-timeline-fps",
        "60",
        "--classic-scene-timeline-cycle-frames",
        str(timeline_cycle_frames),
        "--classic-scene-camera-timeline-position-radius-ratio",
        number(CAMERA_POSITION_RADIUS_RATIO),
        "--classic-scene-camera-timeline-target-radius-ratio",
        number(CAMERA_TARGET_RADIUS_RATIO),
        "--classic-scene-deterministic-camera-timeline",
        "--classic-scene-no-capture",
    ]


def camera_signature(result: dict[str, Any]) -> str:
    timeline = result["motionTimeline"]
    payload = {
        "resolution": result["resolution"],
        "fovDegrees": result["camera"]["fovDegrees"],
        "fixedFramesPerSecond": timeline["fixedFramesPerSecond"],
        "cycleFrames": timeline["cycleFrames"],
        "baseCameraUp": timeline["baseState"]["cameraUp"],
        "samples": [
            {
                "measurementFrame": sample["measurementFrame"],
                "timelineFrame": sample["timelineFrame"],
                "cycleFrame": sample["cycleFrame"],
                "fixedTimeSeconds": sample["fixedTimeSeconds"],
                "normalizedPhase": sample["normalizedPhase"],
                "cameraPosition": sample["cameraPosition"],
                "cameraTarget": sample["cameraTarget"],
            }
            for sample in timeline["samples"]
        ],
    }
    encoded = json.dumps(
        payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def validate_result(
    result: dict[str, Any],
    configuration: dict[str, Any],
    scene_id: str,
    measured_frames: int,
    timeline_cycle_frames: int,
    validator: Any,
) -> str:
    context = f"{scene_id}/{configuration['name']}"
    expect(result.get("success") is True, f"{context}: success is false")
    expect(result.get("scene") == scene_id, f"{context}: scene mismatch")
    expect(result.get("buildConfiguration") == "Release", f"{context}: not Release")
    expect(result.get("architecture") == "x64", f"{context}: not x64")
    expect(result.get("resolution") == [1920, 1080], f"{context}: resolution")
    expect(result.get("renderPath") == "pbr-deferred", f"{context}: render path")
    expect(result.get("measuredFrames") == measured_frames, f"{context}: frames")
    settings = result["settings"]
    expect(settings["requestedSwapInterval"] == 0, f"{context}: VSync")
    expect(settings["inputFrozen"] is True, f"{context}: input")
    expect(settings["bloom"] is False, f"{context}: Bloom")
    expect(settings["autoReloadShaders"] is False, f"{context}: shader reload")
    expect(settings["autoReloadMaterials"] is False, f"{context}: material reload")
    expect(settings["shadowCastingLights"] == 0, f"{context}: shadows")
    ssao = result["ssao"]
    expect(ssao["mode"] == configuration["mode"], f"{context}: SSAO mode")
    expect(int(ssao["requestedSamples"]) == configuration["samples"], f"{context}: samples")
    expect(float(ssao["radius"]) == 0.35, f"{context}: radius")
    expect(float(ssao["bias"]) == 0.025, f"{context}: bias")
    expect(ssao["kernelGeneration"]["seed"] == 1337, f"{context}: seed")
    expect(
        ssao["kernelGeneration"]["radialScaleDenominator"] == 64,
        f"{context}: radial denominator",
    )
    expected_generate = [960, 540] if configuration["halfResolution"] else [1920, 1080]
    expect(
        [ssao["generate"]["width"], ssao["generate"]["height"]]
        == expected_generate,
        f"{context}: generate size",
    )
    expect(ssao["generate"]["internalFormatName"] == "GL_R16F", f"{context}: generate format")
    expect(
        [ssao["output"]["width"], ssao["output"]["height"]] == [1920, 1080],
        f"{context}: output size",
    )
    expect(ssao["output"]["internalFormatName"] == "GL_R16F", f"{context}: output format")
    timeline = result["motionTimeline"]
    expect(timeline["enabled"] is True, f"{context}: timeline disabled")
    expect(timeline["profile"] == "camera", f"{context}: timeline profile")
    expect(timeline["tracks"] == ["camera"], f"{context}: timeline tracks")
    expect(timeline["fixedFramesPerSecond"] == 60, f"{context}: fixed FPS")
    expect(timeline["cycleFrames"] == timeline_cycle_frames, f"{context}: cycle")
    expect(
        float(timeline["amplitudeRatios"]["cameraPosition"])
        == CAMERA_POSITION_RADIUS_RATIO,
        f"{context}: camera position amplitude",
    )
    expect(
        float(timeline["amplitudeRatios"]["cameraTarget"])
        == CAMERA_TARGET_RADIUS_RATIO,
        f"{context}: camera target amplitude",
    )
    samples = timeline["samples"]
    expect(len(samples) == measured_frames, f"{context}: timeline count")
    for frame, sample in enumerate(samples):
        expect(sample["measurementFrame"] == frame, f"{context}: measurement frame {frame}")
        expect(sample["timelineFrame"] == frame, f"{context}: timeline phase {frame}")
        expect(sample["cycleFrame"] == frame % timeline_cycle_frames, f"{context}: cycle frame {frame}")
    validator.extract_metrics(
        result,
        measured_frames,
        bool(configuration["bilateral"]),
        context,
    )
    return camera_signature(result)


def create_source_checkpoint(
    project_directory: Path,
    repository_directory: Path,
    checkpoint_directory: Path,
) -> dict[str, Any]:
    checkpoint_directory.mkdir(parents=True, exist_ok=True)
    git_status = subprocess.check_output(
        ["git", "status", "--porcelain=v2", "--untracked-files=all"],
        cwd=repository_directory,
        text=True,
        encoding="utf-8",
    )
    (checkpoint_directory / "git-status-porcelain-v2.txt").write_text(
        git_status, encoding="utf-8"
    )
    patch = subprocess.run(
        ["git", "diff", "--binary", "--no-ext-diff", "HEAD"],
        cwd=repository_directory,
        capture_output=True,
        check=True,
    ).stdout
    (checkpoint_directory / "working-tree.patch").write_bytes(patch)
    copied = []
    files_root = checkpoint_directory / "files"
    for relative in SOURCE_CHECKPOINT_FILES:
        source = project_directory / relative
        if not source.is_file():
            continue
        destination = files_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied.append(
            {
                "path": relative,
                "sha256": sha256_file(source),
                "bytes": source.stat().st_size,
            }
        )
    write_json(checkpoint_directory / "files.json", copied)
    return {
        "path": project_relative(checkpoint_directory, project_directory),
        "gitStatusLineCount": len(git_status.splitlines()),
        "worktreeDirty": bool(git_status.strip()),
        "copiedFileCount": len(copied),
    }


def execute(
    executable: Path,
    arguments: list[str],
    project_directory: Path,
    log_path: Path,
) -> tuple[int, float, str, str]:
    command = [str(executable), *arguments]
    started_utc = utc_now()
    started = time.perf_counter()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="") as log:
        completed = subprocess.run(
            command,
            cwd=project_directory,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    elapsed = time.perf_counter() - started
    return completed.returncode, elapsed, started_utc, utc_now()


def run_performance(
    manifest: dict[str, Any],
    manifest_path: Path,
    executable: Path,
    executable_hash: str,
    project_directory: Path,
    output_directory: Path,
    scenes: list[dict[str, Any]],
    validator: Any,
    nvidia_smi: Path | None,
    warmup_frames: int,
    measured_frames: int,
    timeline_cycle_frames: int,
    resume: bool,
) -> None:
    root = output_directory / "performance"
    raw_root = root / "raw"
    log_root = root / "logs"
    gpu_root = root / "gpu-state"
    for directory in (raw_root, log_root, gpu_root):
        directory.mkdir(parents=True, exist_ok=True)
    rows = balanced_rows()
    order: list[dict[str, Any]] = []
    for block, row in enumerate(rows, start=1):
        scene_order = scenes if block % 2 == 1 else list(reversed(scenes))
        for scene_position, scene in enumerate(scene_order, start=1):
            for position, configuration in enumerate(row, start=1):
                order.append(
                    {
                        "executionIndex": len(order) + 1,
                        "block": block,
                        "sceneOrder": scene_position,
                        "scene": scene["id"],
                        "position": position,
                        "configuration": configuration["name"],
                    }
                )
    order_path = root / "execution-order.csv"
    with order_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=order[0].keys())
        writer.writeheader()
        writer.writerows(order)
    section = manifest.setdefault("performance", {})
    existing = {
        (record["scene"], record["configuration"], int(record["block"])): record
        for record in section.get("runs", [])
    }
    records: list[dict[str, Any]] = []
    section.update(
        {
            "protocol": {
                "warmupFrames": warmup_frames,
                "measuredFrames": measured_frames,
                "independentProcessesPerConfigurationScene": 6,
                "balancedRows": [[item["name"] for item in row] for row in rows],
                "sceneOrder": "Sponza-first odd blocks; San-Miguel-first even blocks",
                "independentUnit": "per-process median",
                "imageReadback": False,
            },
            "executionOrder": project_relative(order_path, project_directory),
            "runs": records,
            "status": "running",
        }
    )
    write_json(manifest_path, manifest)
    for block, row in enumerate(rows, start=1):
        capture_background_snapshot(
            nvidia_smi, gpu_root / f"block-{block:02d}-before.json"
        )
        scene_order = scenes if block % 2 == 1 else list(reversed(scenes))
        for scene_position, scene in enumerate(scene_order, start=1):
            scene_id = str(scene["id"])
            for position, configuration in enumerate(row, start=1):
                config_name = str(configuration["name"])
                key = (scene_id, config_name, block)
                result_path = raw_root / scene_id / config_name / f"run-{block}.json"
                log_path = log_root / (
                    f"{len(records)+1:03d}-{scene_id}-{config_name}-run-{block}.log"
                )
                before_path = gpu_root / f"{scene_id}-{config_name}-run-{block}-before.json"
                after_path = gpu_root / f"{scene_id}-{config_name}-run-{block}-after.json"
                if resume and key in existing and result_path.is_file():
                    result = read_json(result_path)
                    signature = validate_result(
                        result,
                        configuration,
                        scene_id,
                        measured_frames,
                        timeline_cycle_frames,
                        validator,
                    )
                    record = dict(existing[key])
                    expect(record["cameraSignatureSha256"] == signature, "resume camera signature")
                    records.append(record)
                    print(f"reuse performance {scene_id}/{config_name}/block-{block}", flush=True)
                    continue
                before = query_nvidia_smi(nvidia_smi)
                write_json(before_path, before)
                expect(sha256_file(executable) == executable_hash, "executable changed before run")
                arguments = base_arguments(
                    scene,
                    configuration,
                    project_relative(result_path, project_directory),
                    warmup_frames,
                    measured_frames,
                    timeline_cycle_frames,
                )
                return_code, elapsed, started_utc, ended_utc = execute(
                    executable, arguments, project_directory, log_path
                )
                after = query_nvidia_smi(nvidia_smi)
                write_json(after_path, after)
                expect(return_code == 0, f"performance run failed: {log_path}")
                expect(result_path.is_file(), f"result missing: {result_path}")
                result = read_json(result_path)
                signature = validate_result(
                    result,
                    configuration,
                    scene_id,
                    measured_frames,
                    timeline_cycle_frames,
                    validator,
                )
                expect(sha256_file(executable) == executable_hash, "executable changed during run")
                record = {
                    "executionIndex": len(records) + 1,
                    "block": block,
                    "sceneOrder": scene_position,
                    "scene": scene_id,
                    "position": position,
                    "configuration": config_name,
                    "mode": configuration["mode"],
                    "samples": configuration["samples"],
                    "result": project_relative(result_path, project_directory),
                    "resultSha256": sha256_file(result_path),
                    "cameraSignatureSha256": signature,
                    "log": project_relative(log_path, project_directory),
                    "gpuStateBefore": project_relative(before_path, project_directory),
                    "gpuStateAfter": project_relative(after_path, project_directory),
                    "startedAtUtc": started_utc,
                    "endedAtUtc": ended_utc,
                    "elapsedSeconds": elapsed,
                    "exitCode": return_code,
                    "executableSha256": executable_hash,
                }
                records.append(record)
                section["runs"] = records
                manifest["generatedAtUtc"] = utc_now()
                write_json(manifest_path, manifest)
                print(
                    f"done performance {len(records):02d}/{len(order)} block={block} "
                    f"scene={scene_id} pos={position} config={config_name} "
                    f"elapsed={elapsed:.1f}s",
                    flush=True,
                )
        capture_background_snapshot(
            nvidia_smi, gpu_root / f"block-{block:02d}-after.json"
        )
    expect(len(records) == len(order), "performance run count mismatch")
    for scene_id in SCENE_IDS:
        signatures = {
            record["cameraSignatureSha256"]
            for record in records
            if record["scene"] == scene_id
        }
        expect(len(signatures) == 1, f"{scene_id}: camera paths differ")
    section["status"] = "pass"
    section["validation"] = {
        "runCount": len(records),
        "requiredQueryCountsExact": True,
        "nestedTimerBoundsVerified": True,
        "fixedStateVerified": True,
        "cameraPathIdenticalWithinScene": True,
        "allReadbackDisabled": True,
    }
    write_json(manifest_path, manifest)


def expanded_roi(roi: dict[str, Any]) -> dict[str, Any]:
    halo = CAPTURE_HALO_PIXELS
    return {
        "name": roi["name"],
        "x": int(roi["x"]) - halo,
        "y": int(roi["y"]) - halo,
        "width": int(roi["width"]) + halo * 2,
        "height": int(roi["height"]) + halo * 2,
        "evaluationOffset": [halo, halo],
        "evaluationSize": [int(roi["width"]), int(roi["height"])],
        "originalTopLeft": [int(roi["x"]), int(roi["y"])],
    }


def verify_quality_files(
    capture_directory: Path,
    rois: list[dict[str, Any]],
    frame_count: int,
    reference_guides: bool,
) -> list[Path]:
    files: list[Path] = []
    for roi in rois:
        roi_directory = capture_directory / str(roi["name"])
        for frame in range(frame_count):
            prefix = roi_directory / f"frame-{frame:06d}"
            required = [
                Path(str(prefix) + "-ao.pfm"),
                Path(str(prefix) + "-ldr.ppm"),
            ]
            if reference_guides:
                required.extend(
                    [
                        Path(str(prefix) + "-depth.pfm"),
                        Path(str(prefix) + "-normal.pfm"),
                    ]
                )
            for path in required:
                expect(path.is_file() and path.stat().st_size > 32, f"capture missing: {path}")
                files.append(path)
            if not reference_guides:
                expect(not Path(str(prefix) + "-depth.pfm").exists(), "candidate depth guide exists")
                expect(not Path(str(prefix) + "-normal.pfm").exists(), "candidate normal guide exists")
    return files


def run_quality(
    manifest: dict[str, Any],
    manifest_path: Path,
    executable: Path,
    executable_hash: str,
    project_directory: Path,
    output_directory: Path,
    scenes: list[dict[str, Any]],
    validator: Any,
    warmup_frames: int,
    quality_frames: int,
    timeline_cycle_frames: int,
    resume: bool,
    quality_section_name: str,
    quality_directory_name: str,
    quality_rois: dict[str, Any],
    quality_roi_manifest: str | None,
) -> None:
    root = output_directory / quality_directory_name
    result_root = root / "results"
    capture_root = root / "captures"
    log_root = root / "logs"
    for directory in (result_root, capture_root, log_root):
        directory.mkdir(parents=True, exist_ok=True)
    section = manifest.setdefault(quality_section_name, {})
    existing = {
        (record["scene"], record["configuration"]): record
        for record in section.get("runs", [])
    }
    records: list[dict[str, Any]] = []
    section.update(
        {
            "protocol": {
                "warmupFrames": warmup_frames,
                "capturedMeasurementFrames": list(range(quality_frames)),
                "captureFrameCount": quality_frames,
                "captureStride": 1,
                "captureSeparatedFromFormalPerformance": True,
                "floatAoFormal": True,
                "ldrSupplementary": True,
                "referenceGuidesOnlyFrom": "legacy-full64",
                "captureHaloPixels": CAPTURE_HALO_PIXELS,
                "evaluationRois": quality_rois,
                "captureRois": {
                    scene_id: [expanded_roi(roi) for roi in quality_rois[scene_id]]
                    for scene_id in SCENE_IDS
                },
                "selectionManifest": quality_roi_manifest,
            },
            "directory": project_relative(root, project_directory),
            "runs": records,
            "status": "running",
        }
    )
    write_json(manifest_path, manifest)
    all_files: list[dict[str, Any]] = []
    for scene_index, scene in enumerate(scenes):
        scene_id = str(scene["id"])
        # Alternate the candidate order, but always capture the common reference first.
        candidates = [CONFIG_BY_NAME["legacy-full32"], CONFIG_BY_NAME["half-bilateral64"]]
        if scene_index % 2 == 1:
            candidates.reverse()
        run_order = [CONFIG_BY_NAME["legacy-full64"], *candidates]
        rois = [expanded_roi(roi) for roi in quality_rois[scene_id]]
        for order_index, configuration in enumerate(run_order, start=1):
            config_name = str(configuration["name"])
            key = (scene_id, config_name)
            result_path = result_root / scene_id / f"{config_name}.json"
            capture_directory = capture_root / scene_id / config_name
            log_path = log_root / f"{scene_id}-{config_name}.log"
            reference_guides = config_name == "legacy-full64"
            if resume and key in existing and result_path.is_file():
                result = read_json(result_path)
                signature = validate_result(
                    result,
                    configuration,
                    scene_id,
                    quality_frames,
                    timeline_cycle_frames,
                    validator,
                )
                files = verify_quality_files(
                    capture_directory, rois, quality_frames, reference_guides
                )
                record = dict(existing[key])
                expect(record["cameraSignatureSha256"] == signature, "resume quality signature")
                records.append(record)
                print(f"reuse quality {scene_id}/{config_name}", flush=True)
            else:
                expect(sha256_file(executable) == executable_hash, "executable changed before capture")
                arguments = base_arguments(
                    scene,
                    configuration,
                    project_relative(result_path, project_directory),
                    warmup_frames,
                    quality_frames,
                    timeline_cycle_frames,
                )
                arguments.extend(
                    [
                        "--classic-scene-ssao-temporal-capture-directory",
                        project_relative(capture_directory, project_directory),
                        "--classic-scene-ssao-temporal-capture-start",
                        "0",
                        "--classic-scene-ssao-temporal-capture-count",
                        str(quality_frames),
                        "--classic-scene-ssao-temporal-capture-stride",
                        "1",
                    ]
                )
                for roi in rois:
                    arguments.extend(
                        [
                            "--classic-scene-ssao-temporal-capture-roi",
                            str(roi["name"]),
                            str(roi["x"]),
                            str(roi["y"]),
                            str(roi["width"]),
                            str(roi["height"]),
                        ]
                    )
                if reference_guides:
                    arguments.append(
                        "--classic-scene-ssao-temporal-capture-reference-guides"
                    )
                return_code, elapsed, started_utc, ended_utc = execute(
                    executable, arguments, project_directory, log_path
                )
                expect(return_code == 0, f"quality capture failed: {log_path}")
                expect(result_path.is_file(), f"quality result missing: {result_path}")
                result = read_json(result_path)
                signature = validate_result(
                    result,
                    configuration,
                    scene_id,
                    quality_frames,
                    timeline_cycle_frames,
                    validator,
                )
                files = verify_quality_files(
                    capture_directory, rois, quality_frames, reference_guides
                )
                expect(sha256_file(executable) == executable_hash, "executable changed during capture")
                record = {
                    "executionIndex": len(records) + 1,
                    "scene": scene_id,
                    "orderWithinScene": order_index,
                    "configuration": config_name,
                    "mode": configuration["mode"],
                    "samples": configuration["samples"],
                    "referenceGuides": reference_guides,
                    "result": project_relative(result_path, project_directory),
                    "resultSha256": sha256_file(result_path),
                    "cameraSignatureSha256": signature,
                    "captureDirectory": project_relative(capture_directory, project_directory),
                    "capturedFileCount": len(files),
                    "log": project_relative(log_path, project_directory),
                    "startedAtUtc": started_utc,
                    "endedAtUtc": ended_utc,
                    "elapsedSeconds": elapsed,
                    "exitCode": return_code,
                    "executableSha256": executable_hash,
                }
                records.append(record)
                section["runs"] = records
                manifest["generatedAtUtc"] = utc_now()
                write_json(manifest_path, manifest)
                print(
                    f"done quality {len(records):02d}/6 scene={scene_id} "
                    f"config={config_name} files={len(files)} elapsed={elapsed:.1f}s",
                    flush=True,
                )
            for path in files:
                all_files.append(
                    {
                        "path": project_relative(path, project_directory),
                        "bytes": path.stat().st_size,
                        "sha256": sha256_file(path),
                    }
                )
    expect(len(records) == 6, "quality run count mismatch")
    for scene_id in SCENE_IDS:
        signatures = {
            record["cameraSignatureSha256"]
            for record in records
            if record["scene"] == scene_id
        }
        expect(len(signatures) == 1, f"{scene_id}: quality camera paths differ")
    input_manifest_path = root / "quality-input-manifest.json"
    write_json(
        input_manifest_path,
        {
            "schemaVersion": 1,
            "generatedAtUtc": utc_now(),
            "fileCount": len(all_files),
            "allFilesNonEmpty": all(item["bytes"] > 32 for item in all_files),
            "files": all_files,
        },
    )
    section["inputManifest"] = project_relative(input_manifest_path, project_directory)
    section["status"] = "pass"
    section["validation"] = {
        "runCount": len(records),
        "cameraPathIdenticalWithinScene": True,
        "captureFileCount": len(all_files),
        "allFilesHashed": True,
        "performanceSamplesExcluded": True,
    }
    write_json(manifest_path, manifest)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument(
        "--phase", choices=("performance", "quality", "all"), default="all"
    )
    parser.add_argument("--warmup-frames", type=int, default=300)
    parser.add_argument("--measured-frames", type=int, default=2000)
    parser.add_argument("--quality-frames", type=int, default=120)
    parser.add_argument("--timeline-cycle-frames", type=int, default=1200)
    parser.add_argument("--quality-rois-manifest", type=Path)
    parser.add_argument("--quality-section", default="quality")
    parser.add_argument("--quality-directory", default="quality")
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    script_path = Path(__file__).resolve()
    project_directory = script_path.parent.parent
    repository_directory = project_directory.parent
    executable = arguments.executable.resolve()
    output_directory = arguments.output_directory.resolve()
    expect(executable.is_file(), f"executable is missing: {executable}")
    expect(
        output_directory.is_relative_to(project_directory),
        "output directory must be inside the project directory",
    )
    expect(arguments.warmup_frames == 300, "formal warm-up must be 300")
    expect(arguments.measured_frames == 2000, "formal performance sample must be 2000")
    expect(arguments.quality_frames >= 60, "quality sequence must contain at least 60 frames")
    expect(arguments.timeline_cycle_frames >= 600, "camera path must remain slow")
    expect(
        arguments.quality_section.replace("-", "").replace("_", "").isalnum(),
        "quality section name must be alphanumeric/dash/underscore",
    )
    expect(
        arguments.quality_directory.replace("-", "").replace("_", "").isalnum(),
        "quality directory name must be a single safe path component",
    )
    output_directory.mkdir(parents=True, exist_ok=True)
    manifest_path = output_directory / "run-manifest.json"
    if manifest_path.exists() and not arguments.resume:
        raise RuntimeError(
            f"manifest already exists; use --resume to preserve completed runs: {manifest_path}"
        )
    executable_hash = sha256_file(executable)
    scene_manifest = read_json(project_directory / "classic-scenes.manifest.json")
    scene_lookup = {str(scene["id"]): scene for scene in scene_manifest["scenes"]}
    scenes = [scene_lookup[scene_id] for scene_id in SCENE_IDS]
    for scene in scenes:
        asset = (
            project_directory
            / str(scene_manifest["assetRoot"])
            / str(scene["modelPath"])
        )
        expect(asset.is_file(), f"scene asset is missing: {asset}")
    validator = load_validator(project_directory)
    if manifest_path.is_file():
        manifest = read_json(manifest_path)
        expect(
            manifest["source"]["releaseExecutableSha256"] == executable_hash,
            "resume executable hash mismatch",
        )
    else:
        checkpoint = create_source_checkpoint(
            project_directory,
            repository_directory,
            output_directory / "source-checkpoint",
        )
        old_batches_before = snapshot_directories(
            project_directory, OLD_BATCH_DIRECTORIES
        )
        old_batches_before_path = (
            output_directory / "source-checkpoint" / "old-batches-before.json"
        )
        write_json(old_batches_before_path, old_batches_before)
        git_commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=repository_directory,
            text=True,
            encoding="utf-8",
        ).strip()
        git_branch = subprocess.check_output(
            ["git", "branch", "--show-current"],
            cwd=repository_directory,
            text=True,
            encoding="utf-8",
        ).strip()
        nvidia_smi = nvidia_smi_path()
        manifest = {
            "schemaVersion": 1,
            "generatedAtUtc": utc_now(),
            "batchId": output_directory.name,
            "objective": "deterministic moving-camera SSAO temporal quality and dynamic performance",
            "protocol": {
                "resolution": [1920, 1080],
                "renderPath": "pbr-deferred",
                "requestedSwapInterval": 0,
                "inputFrozen": True,
                "bloom": False,
                "shadows": False,
                "autoReloadShaders": False,
                "autoReloadMaterials": False,
                "ssaoRadius": 0.35,
                "ssaoBias": 0.025,
                "kernelSeed": 1337,
                "fixedFramesPerSecond": 60,
                "timelineCycleFrames": arguments.timeline_cycle_frames,
                "cameraPositionRadiusRatio": CAMERA_POSITION_RADIUS_RATIO,
                "cameraTargetRadiusRatio": CAMERA_TARGET_RADIUS_RATIO,
                "configurations": list(CONFIGURATIONS),
                "scenes": list(SCENE_IDS),
                "kernelMethodCaveat": (
                    "Full-32 uses the prefix of the deterministic 64-vector kernel "
                    "whose radial scale is i/64; it is an end-to-end preset, not a "
                    "pure sample-count causal contrast."
                ),
            },
            "source": {
                "gitCommit": git_commit,
                "gitBranch": git_branch,
                "worktreeDirty": checkpoint["worktreeDirty"],
                "checkpoint": checkpoint,
                "protectedOldBatches": {
                    "beforeManifest": project_relative(
                        old_batches_before_path, project_directory
                    ),
                    "aggregateSha256": old_batches_before["aggregateSha256"],
                    "fileCount": old_batches_before["fileCount"],
                },
                "releaseExecutable": str(executable),
                "releaseExecutableSha256": executable_hash,
                "executableUnchangedDuringRuns": True,
            },
            "system": {
                "nvidiaSmiAvailable": nvidia_smi is not None,
                "initialGpuState": query_nvidia_smi(nvidia_smi),
            },
        }
        write_json(manifest_path, manifest)
    nvidia_smi = nvidia_smi_path()
    if arguments.phase in {"performance", "all"}:
        run_performance(
            manifest,
            manifest_path,
            executable,
            executable_hash,
            project_directory,
            output_directory,
            scenes,
            validator,
            nvidia_smi,
            arguments.warmup_frames,
            arguments.measured_frames,
            arguments.timeline_cycle_frames,
            arguments.resume,
        )
    if arguments.phase in {"quality", "all"}:
        quality_rois: dict[str, Any] = {
            scene_id: [dict(roi) for roi in QUALITY_ROIS[scene_id]]
            for scene_id in SCENE_IDS
        }
        quality_roi_manifest: str | None = None
        if arguments.quality_rois_manifest is not None:
            roi_manifest_path = arguments.quality_rois_manifest.resolve()
            expect(
                roi_manifest_path.is_relative_to(project_directory),
                "quality ROI manifest must be inside the project",
            )
            roi_manifest = read_json(roi_manifest_path)
            expect(
                roi_manifest.get("validation", {}).get("status") == "pass",
                "quality ROI selection did not pass",
            )
            expect(
                roi_manifest["releaseExecutableSha256"] == executable_hash,
                "quality ROI selection executable hash mismatch",
            )
            quality_rois = roi_manifest["rois"]
            quality_roi_manifest = project_relative(
                roi_manifest_path, project_directory
            )
        for scene_id in SCENE_IDS:
            expect(
                scene_id in quality_rois and len(quality_rois[scene_id]) >= 2,
                f"quality ROI manifest is incomplete for {scene_id}",
            )
        run_quality(
            manifest,
            manifest_path,
            executable,
            executable_hash,
            project_directory,
            output_directory,
            scenes,
            validator,
            arguments.warmup_frames,
            arguments.quality_frames,
            arguments.timeline_cycle_frames,
            arguments.resume,
            arguments.quality_section,
            arguments.quality_directory,
            quality_rois,
            quality_roi_manifest,
        )
    expect(sha256_file(executable) == executable_hash, "executable changed after experiment")
    old_batches_after = snapshot_directories(
        project_directory, OLD_BATCH_DIRECTORIES
    )
    protected = manifest["source"]["protectedOldBatches"]
    expect(
        old_batches_after["aggregateSha256"] == protected["aggregateSha256"]
        and old_batches_after["fileCount"] == protected["fileCount"],
        "a protected old SSAO batch changed during this experiment",
    )
    old_batches_after_path = (
        output_directory / "source-checkpoint" / "old-batches-after.json"
    )
    write_json(old_batches_after_path, old_batches_after)
    protected["afterManifest"] = project_relative(
        old_batches_after_path, project_directory
    )
    protected["unchanged"] = True
    manifest["source"]["executableUnchangedDuringRuns"] = True
    manifest["system"]["finalGpuState"] = query_nvidia_smi(nvidia_smi)
    manifest["generatedAtUtc"] = utc_now()
    requested_sections = (
        ("performance",) if arguments.phase == "performance" else
        (arguments.quality_section,) if arguments.phase == "quality" else
        ("performance", arguments.quality_section)
    )
    manifest["status"] = (
        "pass"
        if all(manifest.get(section, {}).get("status") == "pass" for section in requested_sections)
        else "incomplete"
    )
    write_json(manifest_path, manifest)
    print(
        json.dumps(
            {
                "status": manifest["status"],
                "manifest": str(manifest_path),
                "executableSha256": executable_hash,
                "phase": arguments.phase,
            },
            indent=2,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
