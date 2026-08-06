from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CONFIGURATIONS = (
    {
        "name": "legacy-full32",
        "mode": "legacy-full",
        "samples": 32,
        "halfResolution": False,
        "bilateral": False,
    },
    {
        "name": "legacy-full64",
        "mode": "legacy-full",
        "samples": 64,
        "halfResolution": False,
        "bilateral": False,
    },
    {
        "name": "half-raw32",
        "mode": "half-raw",
        "samples": 32,
        "halfResolution": True,
        "bilateral": False,
    },
    {
        "name": "half-raw64",
        "mode": "half-raw",
        "samples": 64,
        "halfResolution": True,
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

SCENE_IDS = ("sponza", "san-miguel")
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


def project_relative(path: Path, project_directory: Path) -> str:
    return path.resolve().relative_to(project_directory.resolve()).as_posix()


def load_validator(project_directory: Path) -> Any:
    validator_path = (
        project_directory / "tools" / "generate_ssao_half_resolution_report.py"
    )
    spec = importlib.util.spec_from_file_location(
        "ssao_formal_validator", validator_path
    )
    expect(spec is not None and spec.loader is not None, "validator import failed")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def latin_sequences(repeats: int) -> list[list[dict[str, Any]]]:
    expect(repeats in {5, 10}, "repeats must be 5 or 10")
    # Williams-style interleaving gives positional Latin balance in the first
    # five rows. The reversed companions in rows 6-10 balance direction and
    # first-order carryover if an extension is required.
    base_indices = (0, 1, 4, 2, 3)
    rows: list[list[dict[str, Any]]] = []
    for rotation in range(5):
        indices = [((index + rotation) % 5) for index in base_indices]
        rows.append([CONFIGURATIONS[index] for index in indices])
    if repeats == 10:
        rows.extend(list(reversed(row)) for row in rows[:5])
    return rows


def query_nvidia_smi(nvidia_smi: Path) -> dict[str, Any]:
    command = [
        str(nvidia_smi),
        "--query-gpu=" + ",".join(NVIDIA_FIELDS),
        "--format=csv,noheader,nounits",
    ]
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=15,
    )
    record: dict[str, Any] = {
        "capturedAtUtc": utc_now(),
        "available": completed.returncode == 0,
        "exitCode": completed.returncode,
        "stderr": completed.stderr.strip(),
        "raw": completed.stdout.strip(),
    }
    if completed.returncode != 0 or not completed.stdout.strip():
        return record
    values = [value.strip() for value in completed.stdout.strip().splitlines()[0].split(",")]
    expect(len(values) == len(NVIDIA_FIELDS), "unexpected nvidia-smi field count")
    parsed: dict[str, Any] = dict(zip(NVIDIA_FIELDS, values))
    for field in (
        "index",
        "temperature.gpu",
        "clocks.current.graphics",
        "clocks.current.sm",
        "clocks.current.memory",
        "utilization.gpu",
    ):
        parsed[field] = int(float(parsed[field]))
    for field in ("power.draw", "power.limit"):
        parsed[field] = float(parsed[field])
    record["gpu"] = parsed
    return record


def capture_block_snapshot(nvidia_smi: Path, path: Path) -> None:
    commands = (
        [str(nvidia_smi), "-q"],
        [str(nvidia_smi), "pmon", "-c", "1", "-s", "um"],
    )
    sections = []
    for command in commands:
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
    write_json(path, {"capturedAtUtc": utc_now(), "sections": sections})


def validate_result(
    result: dict[str, Any],
    configuration: dict[str, Any],
    scene_id: str,
    measured_frames: int,
    validator: Any,
) -> None:
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
    expect(
        int(ssao["requestedSamples"]) == configuration["samples"],
        f"{context}: samples",
    )
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
    expect(
        ssao["generate"]["internalFormatName"] == "GL_R16F",
        f"{context}: generate format",
    )
    expected_output = [1920, 1080] if configuration["bilateral"] else expected_generate
    expect(
        [ssao["output"]["width"], ssao["output"]["height"]] == expected_output,
        f"{context}: output size",
    )
    expect(
        ssao["output"]["internalFormatName"] == "GL_R16F",
        f"{context}: output format",
    )
    validator.extract_metrics(
        result,
        measured_frames,
        bool(configuration["bilateral"]),
        context,
    )


def make_arguments(
    scene: dict[str, Any],
    configuration: dict[str, Any],
    result_relative: str,
    warmup_frames: int,
    measured_frames: int,
) -> list[str]:
    def number(value: Any) -> str:
        return format(float(value), ".17g")

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
        "--classic-scene-no-capture",
    ]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--repeats", type=int, choices=(5, 10), default=5)
    parser.add_argument("--warmup-frames", type=int, default=300)
    parser.add_argument("--measured-frames", type=int, default=2000)
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
    expect(arguments.measured_frames == 2000, "formal measurement must be 2000")
    output_directory.mkdir(parents=True, exist_ok=True)
    raw_root = output_directory / "raw"
    log_root = output_directory / "logs"
    gpu_root = output_directory / "gpu-state"
    checkpoint_root = output_directory / "source-checkpoint"
    for directory in (raw_root, log_root, gpu_root, checkpoint_root):
        directory.mkdir(parents=True, exist_ok=True)
    manifest_path = output_directory / "run-manifest.json"
    scene_manifest = read_json(project_directory / "classic-scenes.manifest.json")
    scene_lookup = {
        str(scene["id"]): scene for scene in scene_manifest["scenes"]
    }
    scenes = [scene_lookup[scene_id] for scene_id in SCENE_IDS]
    for scene in scenes:
        asset_path = (
            project_directory
            / str(scene_manifest["assetRoot"])
            / str(scene["modelPath"])
        )
        expect(asset_path.is_file(), f"scene asset is missing: {asset_path}")
    validator = load_validator(project_directory)
    nvidia_smi = Path(os.environ.get("WINDIR", "C:/Windows")) / "System32" / "nvidia-smi.exe"
    expect(nvidia_smi.is_file(), "nvidia-smi is unavailable")
    executable_hash = sha256_file(executable)
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
    git_status = subprocess.check_output(
        ["git", "status", "--porcelain=v2", "--untracked-files=all"],
        cwd=repository_directory,
        text=True,
        encoding="utf-8",
    )
    (checkpoint_root / "git-status-porcelain-v2.txt").write_text(
        git_status, encoding="utf-8"
    )
    git_patch = subprocess.run(
        ["git", "diff", "--binary", "--no-ext-diff", "HEAD"],
        cwd=repository_directory,
        capture_output=True,
        check=True,
    ).stdout
    (checkpoint_root / "working-tree.patch").write_bytes(git_patch)
    sequences = latin_sequences(arguments.repeats)
    order_rows: list[dict[str, Any]] = []
    for block, sequence in enumerate(sequences, start=1):
        scene_order = scenes if block % 2 == 1 else list(reversed(scenes))
        for scene_order_index, scene in enumerate(scene_order, start=1):
            for position, configuration in enumerate(sequence, start=1):
                order_rows.append(
                    {
                        "executionIndex": len(order_rows) + 1,
                        "block": block,
                        "sceneOrder": scene_order_index,
                        "scene": scene["id"],
                        "position": position,
                        "configuration": configuration["name"],
                    }
                )
    with (output_directory / "execution-order.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=order_rows[0].keys())
        writer.writeheader()
        writer.writerows(order_rows)
    existing_records: dict[tuple[str, str, int], dict[str, Any]] = {}
    if arguments.resume and manifest_path.is_file():
        previous = read_json(manifest_path)
        expect(
            previous["source"]["releaseExecutableSha256"] == executable_hash,
            "resume executable hash mismatch",
        )
        for record in previous.get("runs", []):
            existing_records[
                (record["scene"], record["configuration"], int(record["block"]))
            ] = record
    run_records: list[dict[str, Any]] = []
    manifest: dict[str, Any] = {
        "schemaVersion": 1,
        "generatedAtUtc": utc_now(),
        "batchId": output_directory.name,
        "protocol": {
            "warmupFrames": arguments.warmup_frames,
            "measuredFrames": arguments.measured_frames,
            "independentProcesses": arguments.repeats,
            "resolution": [1920, 1080],
            "configurations": list(CONFIGURATIONS),
            "scenes": list(SCENE_IDS),
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
            "percentileMethod": "nearest-rank",
            "order": (
                "5x5 Williams-pattern positional Latin square; alternating "
                "scene order. Blocks 6-10 are reversed companions when used."
            ),
            "independentUnit": "per-process median",
            "kernelMethodCaveat": (
                "32-sample modes use the prefix of the deterministic 64-vector "
                "kernel whose radial scale is i/64; the 32-vs-64 contrast changes "
                "both count and radial distribution."
            ),
        },
        "source": {
            "gitCommit": git_commit,
            "gitBranch": git_branch,
            "worktreeDirty": bool(git_status.strip()),
            "gitStatusLineCount": len(git_status.splitlines()),
            "checkpoint": project_relative(checkpoint_root, project_directory),
            "releaseExecutable": str(executable),
            "releaseExecutableSha256": executable_hash,
            "executableUnchangedDuringRuns": True,
        },
        "system": {
            "nvidiaSmiAvailable": True,
            "initialGpuState": query_nvidia_smi(nvidia_smi),
        },
        "executionOrder": order_rows,
        "runs": run_records,
    }
    write_json(manifest_path, manifest)
    for block, sequence in enumerate(sequences, start=1):
        capture_block_snapshot(
            nvidia_smi, gpu_root / f"block-{block:02d}-before.json"
        )
        scene_order = scenes if block % 2 == 1 else list(reversed(scenes))
        for scene_order_index, scene in enumerate(scene_order, start=1):
            scene_id = str(scene["id"])
            for position, configuration in enumerate(sequence, start=1):
                configuration_name = str(configuration["name"])
                key = (scene_id, configuration_name, block)
                raw_directory = raw_root / scene_id / configuration_name
                raw_directory.mkdir(parents=True, exist_ok=True)
                result_path = raw_directory / f"run-{block}.json"
                log_path = log_root / (
                    f"{len(run_records)+1:03d}-{scene_id}-"
                    f"{configuration_name}-run-{block}.log"
                )
                before_path = gpu_root / (
                    f"{scene_id}-{configuration_name}-run-{block}-before.json"
                )
                after_path = gpu_root / (
                    f"{scene_id}-{configuration_name}-run-{block}-after.json"
                )
                if key in existing_records and result_path.is_file():
                    result = read_json(result_path)
                    validate_result(
                        result,
                        configuration,
                        scene_id,
                        arguments.measured_frames,
                        validator,
                    )
                    run_records.append(existing_records[key])
                    print(
                        f"reuse block={block} scene={scene_id} "
                        f"config={configuration_name}",
                        flush=True,
                    )
                    continue
                before = query_nvidia_smi(nvidia_smi)
                write_json(before_path, before)
                expect(
                    sha256_file(executable) == executable_hash,
                    "executable changed before run",
                )
                command = [
                    str(executable),
                    *make_arguments(
                        scene,
                        configuration,
                        project_relative(result_path, project_directory),
                        arguments.warmup_frames,
                        arguments.measured_frames,
                    ),
                ]
                started_utc = utc_now()
                started = time.perf_counter()
                with log_path.open("w", encoding="utf-8", newline="") as log:
                    completed = subprocess.run(
                        command,
                        cwd=project_directory,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=False,
                    )
                elapsed_seconds = time.perf_counter() - started
                ended_utc = utc_now()
                after = query_nvidia_smi(nvidia_smi)
                write_json(after_path, after)
                expect(completed.returncode == 0, f"run failed: {log_path}")
                expect(result_path.is_file(), f"result missing: {result_path}")
                result = read_json(result_path)
                validate_result(
                    result,
                    configuration,
                    scene_id,
                    arguments.measured_frames,
                    validator,
                )
                expect(
                    sha256_file(executable) == executable_hash,
                    "executable changed during run",
                )
                record = {
                    "executionIndex": len(run_records) + 1,
                    "block": block,
                    "sceneOrder": scene_order_index,
                    "scene": scene_id,
                    "position": position,
                    "configuration": configuration_name,
                    "mode": configuration["mode"],
                    "samples": configuration["samples"],
                    "result": project_relative(result_path, project_directory),
                    "resultSha256": sha256_file(result_path),
                    "log": project_relative(log_path, project_directory),
                    "gpuStateBefore": project_relative(
                        before_path, project_directory
                    ),
                    "gpuStateAfter": project_relative(
                        after_path, project_directory
                    ),
                    "startedAtUtc": started_utc,
                    "endedAtUtc": ended_utc,
                    "elapsedSeconds": elapsed_seconds,
                    "exitCode": completed.returncode,
                    "executableSha256": executable_hash,
                }
                run_records.append(record)
                manifest["runs"] = run_records
                manifest["generatedAtUtc"] = utc_now()
                write_json(manifest_path, manifest)
                gpu_before = before.get("gpu", {})
                gpu_after = after.get("gpu", {})
                print(
                    f"done {len(run_records):02d}/{len(order_rows)} "
                    f"block={block} scene={scene_id} pos={position} "
                    f"config={configuration_name} elapsed={elapsed_seconds:.1f}s "
                    f"temp={gpu_before.get('temperature.gpu','?')}→"
                    f"{gpu_after.get('temperature.gpu','?')}C "
                    f"clock={gpu_before.get('clocks.current.graphics','?')}→"
                    f"{gpu_after.get('clocks.current.graphics','?')}MHz",
                    flush=True,
                )
        capture_block_snapshot(
            nvidia_smi, gpu_root / f"block-{block:02d}-after.json"
        )
    expect(
        len(run_records) == len(order_rows),
        f"run count {len(run_records)} != {len(order_rows)}",
    )
    expect(
        sha256_file(executable) == executable_hash,
        "executable changed after experiment",
    )
    manifest["generatedAtUtc"] = utc_now()
    manifest["source"]["executableUnchangedDuringRuns"] = True
    manifest["system"]["finalGpuState"] = query_nvidia_smi(nvidia_smi)
    manifest["validation"] = {
        "status": "pass",
        "runCount": len(run_records),
        "allExitCodesZero": True,
        "allResultsValidated": True,
        "requiredQueryCountsExact": True,
        "nestedTimerBoundsVerified": True,
        "fixedStateVerified": True,
        "executableHashStable": True,
    }
    write_json(manifest_path, manifest)
    print(
        json.dumps(
            {
                "status": "pass",
                "runs": len(run_records),
                "manifest": str(manifest_path),
                "executableSha256": executable_hash,
            },
            indent=2,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
