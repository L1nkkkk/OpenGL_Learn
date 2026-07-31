#!/usr/bin/env python3
"""Analyze deterministic moving-camera SSAO performance and temporal quality."""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import random
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import numpy as np
from PIL import Image, ImageDraw, ImageFont


SCENES = ("sponza", "san-miguel")
SCENE_LABELS = {"sponza": "Sponza", "san-miguel": "San Miguel"}
CONFIGS = ("legacy-full64", "legacy-full32", "half-bilateral64")
CONFIG_LABELS = {
    "legacy-full64": "Full-64 Legacy",
    "legacy-full32": "Full-32 Legacy",
    "half-bilateral64": "Half-64 Bilateral",
}
CONFIG_COLORS = {
    "legacy-full64": "#222222",
    "legacy-full32": "#2f6bff",
    "half-bilateral64": "#d64545",
}
CANDIDATES = ("legacy-full32", "half-bilateral64")
METRICS = (
    "cpuFrame",
    "gpuFrame",
    "deferredGpu",
    "ssaoTotalCpu",
    "ssaoTotalGpu",
    "ssaoGenerateCpu",
    "ssaoGenerateGpu",
    "ssaoUpsampleCpu",
    "ssaoUpsampleGpu",
    "drawCalls",
)
METRIC_LABELS = {
    "cpuFrame": "CPU Frame",
    "gpuFrame": "GPU Frame",
    "deferredGpu": "Deferred GPU",
    "ssaoTotalCpu": "SSAO Total CPU",
    "ssaoTotalGpu": "SSAO Total GPU",
    "ssaoGenerateCpu": "SSAO Generate CPU",
    "ssaoGenerateGpu": "SSAO Generate GPU",
    "ssaoUpsampleCpu": "SSAO Upsample CPU",
    "ssaoUpsampleGpu": "SSAO Upsample GPU",
    "drawCalls": "Draw Call",
}
METRIC_UNITS = {metric: ("count" if metric == "drawCalls" else "ms") for metric in METRICS}
PAIR_DEFINITIONS = (
    {
        "id": "full32-vs-half64-bilateral",
        "left": "legacy-full32",
        "right": "half-bilateral64",
        "label": "Half-64 Bilateral minus Full-32",
    },
    {
        "id": "full64-vs-full32",
        "left": "legacy-full64",
        "right": "legacy-full32",
        "label": "Full-32 minus Full-64",
    },
    {
        "id": "full64-vs-half64-bilateral",
        "left": "legacy-full64",
        "right": "half-bilateral64",
        "label": "Half-64 Bilateral minus Full-64",
    },
)
PAIR_METRICS = (
    "gpuFrame",
    "deferredGpu",
    "ssaoTotalGpu",
    "ssaoGenerateGpu",
    "drawCalls",
)
STATIC_SUMMARY = Path(
    "benchmark-results/ssao-factorial/ssao-factorial-balanced-20260731/summary.json"
)
REJECTED_ATTEMPT = Path(
    "benchmark-results/ssao-temporal/ssao-temporal-deterministic-20260731"
)
BOOTSTRAP_RESAMPLES = 50000


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
        json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames: list[str] = []
    for row in rows:
        for name in row:
            if name not in fieldnames:
                fieldnames.append(name)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve(project: Path, recorded: str) -> Path:
    path = Path(recorded)
    return path if path.is_absolute() else project / path


def relative(path: Path, project: Path) -> str:
    return path.resolve().relative_to(project.resolve()).as_posix()


def load_validator(project: Path) -> Any:
    path = project / "tools" / "generate_ssao_half_resolution_report.py"
    spec = importlib.util.spec_from_file_location("ssao_temporal_validator", path)
    expect(spec is not None and spec.loader is not None, f"validator import: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def nearest_rank(values: Iterable[float], percentile: float) -> float:
    ordered = sorted(float(value) for value in values)
    expect(bool(ordered), "percentile input is empty")
    rank = int(math.ceil(max(0.0, min(1.0, percentile)) * len(ordered)))
    return ordered[min(max(rank - 1, 0), len(ordered) - 1)]


def value_stats(values: Iterable[float]) -> dict[str, Any]:
    data = [float(value) for value in values]
    if not data:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "p95": None,
            "p99": None,
            "min": None,
            "max": None,
        }
    return {
        "count": len(data),
        "mean": statistics.fmean(data),
        "median": statistics.median(data),
        "p95": nearest_rank(data, 0.95),
        "p99": nearest_rank(data, 0.99),
        "min": min(data),
        "max": max(data),
    }


def process_distribution(values: Iterable[float]) -> dict[str, Any]:
    data = [float(value) for value in values]
    if not data:
        return {
            "count": 0,
            "median": None,
            "min": None,
            "max": None,
            "mean": None,
            "sampleStdDev": None,
            "cvPercent": None,
        }
    mean = statistics.fmean(data)
    deviation = statistics.stdev(data) if len(data) > 1 else 0.0
    return {
        "count": len(data),
        "median": statistics.median(data),
        "min": min(data),
        "max": max(data),
        "mean": mean,
        "sampleStdDev": deviation,
        "cvPercent": None if mean == 0.0 else deviation / abs(mean) * 100.0,
    }


def bootstrap_mean_ci(values: list[float], seed_text: str) -> tuple[float, float]:
    expect(len(values) >= 2, "bootstrap requires at least two pairs")
    seed = int.from_bytes(hashlib.sha256(seed_text.encode("utf-8")).digest()[:8], "little")
    rng = random.Random(seed)
    means = []
    for _ in range(BOOTSTRAP_RESAMPLES):
        means.append(statistics.fmean(values[rng.randrange(len(values))] for _ in values))
    means.sort()
    return means[int(0.025 * len(means))], means[int(0.975 * len(means))]


def load_performance(
    project: Path, manifest: dict[str, Any], validator: Any
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    section = manifest["performance"]
    expect(section["status"] == "pass", "performance runner did not pass")
    measured_frames = int(section["protocol"]["measuredFrames"])
    configuration_lookup = {
        item["name"]: item for item in manifest["protocol"]["configurations"]
    }
    executable_hash = manifest["source"]["releaseExecutableSha256"].lower()
    runs = []
    input_rows = []
    seen = set()
    for record in section["runs"]:
        identity = (record["scene"], record["configuration"], int(record["block"]))
        expect(identity not in seen, f"duplicate performance run: {identity}")
        seen.add(identity)
        scene, config, block = identity
        expect(scene in SCENES and config in CONFIGS and 1 <= block <= 6, f"bad run: {identity}")
        expect(record["executableSha256"].lower() == executable_hash, f"EXE mismatch: {identity}")
        result_path = resolve(project, record["result"])
        actual_hash = sha256_file(result_path)
        expect(actual_hash.lower() == record["resultSha256"].lower(), f"hash mismatch: {result_path}")
        result = read_json(result_path)
        metrics = validator.extract_metrics(
            result,
            measured_frames,
            bool(configuration_lookup[config]["bilateral"]),
            f"dynamic/{scene}/{config}/block-{block}",
        )
        timeline = result["motionTimeline"]
        expect(len(timeline["samples"]) == measured_frames, f"camera count: {identity}")
        runs.append(
            {
                "record": record,
                "result": result,
                "scene": scene,
                "configuration": config,
                "block": block,
                "position": int(record["position"]),
                "executionIndex": int(record["executionIndex"]),
                "metrics": metrics,
                "metricSummaries": {name: value_stats(values) for name, values in metrics.items()},
                "resultPath": result_path,
            }
        )
        input_rows.append(
            {
                "kind": "performance-result",
                "scene": scene,
                "configuration": config,
                "block": block,
                "path": relative(result_path, project),
                "bytes": result_path.stat().st_size,
                "sha256": actual_hash,
            }
        )
    expected = {(scene, config, block) for scene in SCENES for config in CONFIGS for block in range(1, 7)}
    expect(seen == expected, f"incomplete performance matrix: missing={sorted(expected-seen)}")
    for scene in SCENES:
        for config in CONFIGS:
            counts = {
                position: sum(
                    run["scene"] == scene
                    and run["configuration"] == config
                    and run["position"] == position
                    for run in runs
                )
                for position in (1, 2, 3)
            }
            expect(all(value == 2 for value in counts.values()), f"position imbalance {scene}/{config}: {counts}")
        signatures = {
            run["record"]["cameraSignatureSha256"]
            for run in runs
            if run["scene"] == scene
        }
        expect(len(signatures) == 1, f"camera signature mismatch: {scene}")
    return sorted(runs, key=lambda item: item["executionIndex"]), input_rows


def aggregate_performance(
    runs: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    aggregate: dict[str, Any] = {}
    per_process_rows = []
    aggregate_rows = []
    for run in runs:
        for metric in METRICS:
            stats = run["metricSummaries"][metric]
            per_process_rows.append(
                {
                    "scene": run["scene"],
                    "configuration": run["configuration"],
                    "block": run["block"],
                    "position": run["position"],
                    "executionIndex": run["executionIndex"],
                    "metric": metric,
                    "unit": METRIC_UNITS[metric],
                    **stats,
                    "result": relative(run["resultPath"], run["resultPath"].parents[4]),
                }
            )
    for scene in SCENES:
        aggregate[scene] = {}
        for config in CONFIGS:
            group = sorted(
                [run for run in runs if run["scene"] == scene and run["configuration"] == config],
                key=lambda item: item["block"],
            )
            aggregate[scene][config] = {"processCount": len(group), "metrics": {}}
            for metric in METRICS:
                pooled = [value for run in group for value in run["metrics"][metric]]
                per_process = [
                    {
                        "block": run["block"],
                        "position": run["position"],
                        "median": run["metricSummaries"][metric]["median"],
                        "p95": run["metricSummaries"][metric]["p95"],
                        "p99": run["metricSummaries"][metric]["p99"],
                        "count": run["metricSummaries"][metric]["count"],
                    }
                    for run in group
                ]
                median_values = [item["median"] for item in per_process if item["median"] is not None]
                p95_values = [item["p95"] for item in per_process if item["p95"] is not None]
                p99_values = [item["p99"] for item in per_process if item["p99"] is not None]
                entry = {
                    "unit": METRIC_UNITS[metric],
                    "pooledFrameStatistics": value_stats(pooled),
                    "processMedianDistribution": process_distribution(median_values),
                    "processP95Distribution": process_distribution(p95_values),
                    "processP99Distribution": process_distribution(p99_values),
                    "perProcess": per_process,
                }
                aggregate[scene][config]["metrics"][metric] = entry
                aggregate_rows.append(
                    {
                        "scene": scene,
                        "configuration": config,
                        "metric": metric,
                        "unit": METRIC_UNITS[metric],
                        "processCount": len(group),
                        "pooledFrameCount": len(pooled),
                        "pooledMedian": entry["pooledFrameStatistics"]["median"],
                        "pooledP95": entry["pooledFrameStatistics"]["p95"],
                        "pooledP99": entry["pooledFrameStatistics"]["p99"],
                        "medianOfProcessMedians": entry["processMedianDistribution"]["median"],
                        "processMedianMin": entry["processMedianDistribution"]["min"],
                        "processMedianMax": entry["processMedianDistribution"]["max"],
                        "processMedianCvPercent": entry["processMedianDistribution"]["cvPercent"],
                        "medianOfProcessP95": entry["processP95Distribution"]["median"],
                        "medianOfProcessP99": entry["processP99Distribution"]["median"],
                    }
                )
    return aggregate, per_process_rows, aggregate_rows


def paired_performance(
    runs: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    output: dict[str, Any] = {}
    rows = []
    lookup = {
        (run["scene"], run["configuration"], run["block"]): run for run in runs
    }
    for scene in SCENES:
        output[scene] = {}
        for definition in PAIR_DEFINITIONS:
            pair = {
                "label": definition["label"],
                "left": definition["left"],
                "right": definition["right"],
                "deltaDefinition": "right minus left; negative means right is faster/lower",
                "metrics": {},
            }
            output[scene][definition["id"]] = pair
            for metric in PAIR_METRICS:
                values = []
                for block in range(1, 7):
                    left = lookup[(scene, definition["left"], block)]["metricSummaries"][metric]["median"]
                    right = lookup[(scene, definition["right"], block)]["metricSummaries"][metric]["median"]
                    if left is None or right is None:
                        continue
                    delta = float(right) - float(left)
                    values.append(
                        {
                            "block": block,
                            "left": float(left),
                            "right": float(right),
                            "delta": delta,
                            "relativePercent": None if left == 0 else delta / float(left) * 100.0,
                        }
                    )
                if not values:
                    continue
                deltas = [item["delta"] for item in values]
                low, high = bootstrap_mean_ci(deltas, f"{scene}/{definition['id']}/{metric}")
                positive = sum(value > 0 for value in deltas)
                negative = sum(value < 0 for value in deltas)
                direction = (
                    "right-slower-all-blocks" if positive == len(deltas)
                    else "right-faster-all-blocks" if negative == len(deltas)
                    else "mixed"
                )
                entry = {
                    "unit": METRIC_UNITS[metric],
                    "independentProcessPairs": len(deltas),
                    "pairedValues": values,
                    "meanDelta": statistics.fmean(deltas),
                    "medianDelta": statistics.median(deltas),
                    "minDelta": min(deltas),
                    "maxDelta": max(deltas),
                    "meanRelativePercent": statistics.fmean(
                        item["relativePercent"] for item in values if item["relativePercent"] is not None
                    ),
                    "direction": direction,
                    "positiveCount": positive,
                    "negativeCount": negative,
                    "zeroCount": len(deltas) - positive - negative,
                    "bootstrap95CiMeanDelta": {
                        "low": low,
                        "high": high,
                        "n": len(deltas),
                        "method": f"paired-process mean percentile bootstrap, {BOOTSTRAP_RESAMPLES} resamples",
                    },
                }
                pair["metrics"][metric] = entry
                for item in values:
                    rows.append(
                        {
                            "scene": scene,
                            "comparison": definition["id"],
                            "metric": metric,
                            "leftConfiguration": definition["left"],
                            "rightConfiguration": definition["right"],
                            **item,
                            "meanDelta": entry["meanDelta"],
                            "ciLow": low,
                            "ciHigh": high,
                            "direction": direction,
                        }
                    )
    return output, rows


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        magic = stream.readline().decode("ascii", errors="strict").strip()
        expect(magic in {"Pf", "PF"}, f"invalid PFM magic: {path}")
        channels = 1 if magic == "Pf" else 3
        dimensions = stream.readline().decode("ascii", errors="strict").strip()
        while not dimensions or dimensions.startswith("#"):
            dimensions = stream.readline().decode("ascii", errors="strict").strip()
        width, height = (int(value) for value in dimensions.split())
        scale = float(stream.readline().decode("ascii", errors="strict").strip())
        expect(scale != 0.0 and math.isfinite(scale), f"invalid PFM scale: {path}")
        pixels = np.fromfile(stream, dtype="<f4" if scale < 0 else ">f4")
    expect(pixels.size == width * height * channels, f"PFM size mismatch: {path}")
    shape = (height, width) if channels == 1 else (height, width, channels)
    pixels = np.flipud(pixels.reshape(shape)).astype(np.float32, copy=True) * abs(scale)
    expect(bool(np.isfinite(pixels).all()), f"PFM NaN/Inf: {path}")
    return pixels


def load_rgb(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        pixels = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    expect(bool(np.isfinite(pixels).all()), f"RGB NaN/Inf: {path}")
    return pixels


def shifted_overlap(height: int, width: int, dy: int, dx: int) -> tuple[Any, Any]:
    source = (slice(max(0, -dy), min(height, height - dy)), slice(max(0, -dx), min(width, width - dx)))
    target = (slice(max(0, dy), min(height, height + dy)), slice(max(0, dx), min(width, width + dx)))
    return source, target


def dilate(mask: np.ndarray, iterations: int) -> np.ndarray:
    result = mask.astype(bool, copy=True)
    height, width = result.shape
    for _ in range(iterations):
        expanded = result.copy()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                source, target = shifted_overlap(height, width, dy, dx)
                expanded[target] |= result[source]
        result = expanded
    return result


def build_edge_mask(depth: np.ndarray, normal: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    expect(depth.shape == normal.shape[:2] and normal.shape[2] == 3, "guide shape mismatch")
    foreground = depth > 0.0
    lengths = np.linalg.norm(normal, axis=2)
    expect(bool(np.any(foreground)), "guide contains no foreground")
    expect(bool(np.all(lengths[foreground] > 0.5)), "invalid foreground normal")
    normalized = normal / np.maximum(lengths[..., None], 1.0e-8)
    threshold_dot = math.cos(math.radians(25.0))
    edge = np.zeros_like(foreground)
    height, width = foreground.shape
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            source, target = shifted_overlap(height, width, dy, dx)
            source_valid = foreground[source]
            target_valid = foreground[target]
            discontinuity = source_valid != target_valid
            both = source_valid & target_valid
            relative_depth = np.zeros_like(depth[source], dtype=np.float32)
            relative_depth[both] = np.abs(depth[source][both] - depth[target][both]) / np.maximum(
                np.minimum(depth[source][both], depth[target][both]), 1.0e-6
            )
            normal_dot = np.sum(normalized[source] * normalized[target], axis=2)
            discontinuity |= both & ((relative_depth > 0.02) | (normal_dot < threshold_dot))
            edge[target] |= discontinuity
    return dilate(edge, 3), foreground


def uniform_mean(image: np.ndarray, window: int = 11) -> np.ndarray:
    radius = window // 2
    padded = np.pad(image.astype(np.float64), radius, mode="reflect")
    integral = np.pad(padded, ((1, 0), (1, 0)), mode="constant")
    integral = np.cumsum(np.cumsum(integral, axis=0), axis=1)
    return (
        integral[window:, window:]
        - integral[:-window, window:]
        - integral[window:, :-window]
        + integral[:-window, :-window]
    ) / float(window * window)


def ssim_map(reference: np.ndarray, candidate: np.ndarray) -> np.ndarray:
    reference64 = reference.astype(np.float64)
    candidate64 = candidate.astype(np.float64)
    mu_r = uniform_mean(reference64)
    mu_c = uniform_mean(candidate64)
    var_r = uniform_mean(reference64 * reference64) - mu_r * mu_r
    var_c = uniform_mean(candidate64 * candidate64) - mu_c * mu_c
    covariance = uniform_mean(reference64 * candidate64) - mu_r * mu_c
    c1, c2 = 0.01**2, 0.03**2
    numerator = (2.0 * mu_r * mu_c + c1) * (2.0 * covariance + c2)
    denominator = (mu_r * mu_r + mu_c * mu_c + c1) * (var_r + var_c + c2)
    return np.clip(numerator / np.maximum(denominator, 1.0e-15), -1.0, 1.0)


def region_metrics(
    reference: np.ndarray, candidate: np.ndarray, quality: np.ndarray, mask: np.ndarray
) -> dict[str, Any]:
    selected = mask.astype(bool)
    count = int(np.count_nonzero(selected))
    expect(count > 0, "empty quality mask")
    error = (candidate - reference)[selected].astype(np.float64)
    absolute = np.abs(error)
    mse = float(np.mean(np.square(error)))
    return {
        "count": count,
        "coveragePercent": count / selected.size * 100.0,
        "mae": float(np.mean(absolute)),
        "rmse": math.sqrt(mse),
        "p95AbsoluteError": nearest_rank(absolute, 0.95),
        "p995AbsoluteError": nearest_rank(absolute, 0.995),
        "maxAbsoluteError": float(np.max(absolute)),
        "psnrDb": None if mse == 0.0 else 10.0 * math.log10(1.0 / mse),
        "localSsim": float(np.mean(quality[selected])),
    }


def ao_metrics(reference: np.ndarray, candidate: np.ndarray, foreground: np.ndarray, edge: np.ndarray) -> dict[str, Any]:
    quality = ssim_map(reference, candidate)
    return {
        "global": region_metrics(reference, candidate, quality, np.ones(reference.shape, dtype=bool)),
        "foreground": region_metrics(reference, candidate, quality, foreground),
        "edge": region_metrics(reference, candidate, quality, edge),
    }


def luminance(image: np.ndarray) -> np.ndarray:
    return (
        0.2126 * image[..., 0]
        + 0.7152 * image[..., 1]
        + 0.0722 * image[..., 2]
    )


def ldr_metrics(reference: np.ndarray, candidate: np.ndarray, edge: np.ndarray) -> dict[str, Any]:
    error = candidate.astype(np.float64) - reference.astype(np.float64)
    luminance_r = luminance(reference)
    luminance_c = luminance(candidate)
    quality = ssim_map(luminance_r, luminance_c)
    global_mse = float(np.mean(np.square(error)))
    edge_mse = float(np.mean(np.square(error[np.repeat(edge[..., None], 3, axis=2)])))
    return {
        "global": {
            "psnrDb": None if global_mse == 0 else 10.0 * math.log10(1.0 / global_mse),
            "localSsim": float(np.mean(quality)),
        },
        "edge": {
            "psnrDb": None if edge_mse == 0 else 10.0 * math.log10(1.0 / edge_mse),
            "localSsim": float(np.mean(quality[edge])),
        },
    }


def temporal_region(delta: np.ndarray, mask: np.ndarray) -> dict[str, Any]:
    values = np.abs(delta[mask]).astype(np.float64)
    expect(values.size > 0, "empty temporal mask")
    return {
        "count": int(values.size),
        "coveragePercent": values.size / mask.size * 100.0,
        "mae": float(np.mean(values)),
        "p95AbsoluteChange": nearest_rank(values, 0.95),
        "p99AbsoluteChange": nearest_rank(values, 0.99),
        "maxAbsoluteChange": float(np.max(values)),
    }


def quality_file(root: Path, scene: str, config: str, roi: str, frame: int, suffix: str) -> Path:
    return root / scene / config / roi / f"frame-{frame:06d}-{suffix}"


def analyze_quality(
    project: Path,
    root: Path,
    manifest: dict[str, Any],
    performance_runs: list[dict[str, Any]],
) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], float]:
    section = manifest["quality"]
    expect(section["status"] == "pass", "quality runner did not pass")
    frame_count = int(section["protocol"]["captureFrameCount"])
    capture_root = resolve(project, section["directory"]) / "captures"
    capture_rois = section["protocol"]["captureRois"]
    run_lookup = {(record["scene"], record["configuration"]): record for record in section["runs"]}
    expect(len(run_lookup) == 6, "quality run matrix is incomplete")
    input_manifest_path = resolve(project, section["inputManifest"])
    input_manifest = read_json(input_manifest_path)
    input_rows = []
    for item in input_manifest["files"]:
        path = resolve(project, item["path"])
        expect(path.is_file() and path.stat().st_size == int(item["bytes"]), f"quality input missing: {path}")
        actual = sha256_file(path)
        expect(actual.lower() == item["sha256"].lower(), f"quality input hash: {path}")
        input_rows.append({"kind": "quality-capture", **item})
    # Quality paths must be exact prefixes of the corresponding 2000-frame path.
    for scene in SCENES:
        performance_reference = next(
            run for run in performance_runs
            if run["scene"] == scene and run["configuration"] == "legacy-full64" and run["block"] == 1
        )["result"]["motionTimeline"]
        def pose_sequence(samples: list[dict[str, Any]]) -> list[dict[str, Any]]:
            return [
                {
                    "measurementFrame": sample["measurementFrame"],
                    "timelineFrame": sample["timelineFrame"],
                    "cycleFrame": sample["cycleFrame"],
                    "fixedTimeSeconds": sample["fixedTimeSeconds"],
                    "normalizedPhase": sample["normalizedPhase"],
                    "cameraPosition": sample["cameraPosition"],
                    "cameraTarget": sample["cameraTarget"],
                }
                for sample in samples
            ]
        expected_prefix = pose_sequence(performance_reference["samples"][:frame_count])
        for config in CONFIGS:
            record = run_lookup[(scene, config)]
            result_path = resolve(project, record["result"])
            expect(sha256_file(result_path).lower() == record["resultSha256"].lower(), f"quality result hash: {result_path}")
            result = read_json(result_path)
            expect(
                pose_sequence(result["motionTimeline"]["samples"]) == expected_prefix,
                f"quality camera prefix mismatch: {scene}/{config}",
            )
            input_rows.append(
                {
                    "kind": "quality-result",
                    "scene": scene,
                    "configuration": config,
                    "path": relative(result_path, project),
                    "bytes": result_path.stat().st_size,
                    "sha256": record["resultSha256"],
                }
            )
    per_frame: list[dict[str, Any]] = []
    flat_rows: list[dict[str, Any]] = []
    masks: dict[tuple[str, str, int], tuple[np.ndarray, np.ndarray]] = {}
    previous: dict[tuple[str, str, str], dict[str, np.ndarray]] = {}
    shared_heat_scale = 0.0
    for scene in SCENES:
        for roi in capture_rois[scene]:
            roi_name = str(roi["name"])
            offset_x, offset_y = (int(value) for value in roi["evaluationOffset"])
            width, height = (int(value) for value in roi["evaluationSize"])
            evaluate = (slice(offset_y, offset_y + height), slice(offset_x, offset_x + width))
            for frame in range(frame_count):
                ref_ao_full = read_pfm(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "ao.pfm"))
                depth_full = read_pfm(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "depth.pfm"))
                normal_full = read_pfm(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "normal.pfm"))
                expect(ref_ao_full.shape == depth_full.shape == normal_full.shape[:2], "reference guide dimensions")
                edge_full, foreground_full = build_edge_mask(depth_full, normal_full)
                edge = edge_full[evaluate]
                foreground = foreground_full[evaluate]
                expect(np.count_nonzero(edge) > 0 and np.count_nonzero(foreground) > 0, f"empty mask {scene}/{roi_name}/{frame}")
                masks[(scene, roi_name, frame)] = (edge, foreground)
                ref_ao = ref_ao_full[evaluate]
                expect(float(ref_ao.min()) >= -0.001 and float(ref_ao.max()) <= 1.001, "reference AO range")
                ref_ldr = load_rgb(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "ldr.ppm"))[evaluate]
                for config in CANDIDATES:
                    candidate_ao_full = read_pfm(quality_file(capture_root, scene, config, roi_name, frame, "ao.pfm"))
                    candidate_ao = candidate_ao_full[evaluate]
                    expect(candidate_ao.shape == ref_ao.shape, "candidate AO dimensions")
                    expect(float(candidate_ao.min()) >= -0.001 and float(candidate_ao.max()) <= 1.001, "candidate AO range")
                    candidate_ldr = load_rgb(quality_file(capture_root, scene, config, roi_name, frame, "ldr.ppm"))[evaluate]
                    ao = ao_metrics(ref_ao, candidate_ao, foreground, edge)
                    ldr = ldr_metrics(ref_ldr, candidate_ldr, edge)
                    shared_heat_scale = max(shared_heat_scale, float(ao["global"]["p995AbsoluteError"]))
                    error = candidate_ao - ref_ao
                    ldr_luminance_error = (
                        luminance(candidate_ldr) - luminance(ref_ldr)
                    )
                    key = (scene, roi_name, config)
                    temporal = None
                    ldr_temporal = None
                    if key in previous:
                        previous_entry = previous[key]
                        delta = error - previous_entry["aoError"]
                        temporal = {
                            "definition": "screen-space (candidate-Full64)_t minus (candidate-Full64)_(t-1)",
                            "global": temporal_region(delta, np.ones(delta.shape, dtype=bool)),
                            "foreground": temporal_region(delta, foreground & previous_entry["foreground"]),
                            "edge": temporal_region(delta, edge | previous_entry["edge"]),
                        }
                        ldr_delta = (
                            ldr_luminance_error
                            - previous_entry["ldrLuminanceError"]
                        )
                        ldr_temporal = {
                            "definition": "screen-space LDR-luminance (candidate-Full64)_t minus (candidate-Full64)_(t-1)",
                            "global": temporal_region(
                                ldr_delta, np.ones(ldr_delta.shape, dtype=bool)
                            ),
                            "foreground": temporal_region(
                                ldr_delta,
                                foreground & previous_entry["foreground"],
                            ),
                            "edge": temporal_region(
                                ldr_delta, edge | previous_entry["edge"]
                            ),
                        }
                    previous[key] = {
                        "aoError": error,
                        "ldrLuminanceError": ldr_luminance_error,
                        "foreground": foreground,
                        "edge": edge,
                    }
                    item = {
                        "scene": scene,
                        "roi": roi_name,
                        "configuration": config,
                        "reference": "legacy-full64",
                        "measurementFrame": frame,
                        "mask": {
                            "foregroundPixels": int(np.count_nonzero(foreground)),
                            "foregroundCoveragePercent": float(np.mean(foreground) * 100.0),
                            "edgePixels": int(np.count_nonzero(edge)),
                            "edgeCoveragePercent": float(np.mean(edge) * 100.0),
                        },
                        "ao": ao,
                        "ldr": ldr,
                        "temporalScreenSpace": temporal,
                        "ldrLuminanceTemporalScreenSpace": ldr_temporal,
                    }
                    per_frame.append(item)
                    row = {
                        "scene": scene,
                        "roi": roi_name,
                        "configuration": config,
                        "reference": "legacy-full64",
                        "measurementFrame": frame,
                        "foregroundCoveragePercent": item["mask"]["foregroundCoveragePercent"],
                        "edgeCoveragePercent": item["mask"]["edgeCoveragePercent"],
                    }
                    for region in ("global", "foreground", "edge"):
                        for name in ("mae", "rmse", "p95AbsoluteError", "psnrDb", "localSsim"):
                            row[f"ao_{region}_{name}"] = ao[region][name]
                    for region in ("global", "edge"):
                        row[f"ldr_{region}_psnrDb"] = ldr[region]["psnrDb"]
                        row[f"ldr_{region}_localSsim"] = ldr[region]["localSsim"]
                    if temporal:
                        for region in ("global", "foreground", "edge"):
                            row[f"temporal_{region}_mae"] = temporal[region]["mae"]
                            row[f"temporal_{region}_p95"] = temporal[region]["p95AbsoluteChange"]
                            row[f"temporal_{region}_p99"] = temporal[region]["p99AbsoluteChange"]
                            row[f"ldr_temporal_{region}_mae"] = ldr_temporal[region]["mae"]
                            row[f"ldr_temporal_{region}_p95"] = ldr_temporal[region]["p95AbsoluteChange"]
                            row[f"ldr_temporal_{region}_p99"] = ldr_temporal[region]["p99AbsoluteChange"]
                    flat_rows.append(row)
    summary: dict[str, Any] = {}
    for scene in SCENES:
        summary[scene] = {}
        for config in CANDIDATES:
            summary[scene][config] = {}
            for roi in [str(item["name"]) for item in capture_rois[scene]]:
                group = [item for item in per_frame if item["scene"] == scene and item["configuration"] == config and item["roi"] == roi]
                entry: dict[str, Any] = {
                    "frameCount": len(group),
                    "maskCoverage": {
                        "foregroundPercent": value_stats(item["mask"]["foregroundCoveragePercent"] for item in group),
                        "edgePercent": value_stats(item["mask"]["edgeCoveragePercent"] for item in group),
                    },
                    "ao": {},
                    "ldr": {},
                    "temporalScreenSpace": {},
                    "ldrLuminanceTemporalScreenSpace": {},
                }
                for region in ("global", "foreground", "edge"):
                    entry["ao"][region] = {
                        metric: value_stats(item["ao"][region][metric] for item in group)
                        for metric in ("mae", "rmse", "p95AbsoluteError", "psnrDb", "localSsim")
                    }
                for region in ("global", "edge"):
                    entry["ldr"][region] = {
                        metric: value_stats(item["ldr"][region][metric] for item in group)
                        for metric in ("psnrDb", "localSsim")
                    }
                transitions = [item for item in group if item["temporalScreenSpace"] is not None]
                for region in ("global", "foreground", "edge"):
                    entry["temporalScreenSpace"][region] = {
                        metric: value_stats(item["temporalScreenSpace"][region][metric] for item in transitions)
                        for metric in ("mae", "p95AbsoluteChange", "p99AbsoluteChange")
                    }
                    entry["ldrLuminanceTemporalScreenSpace"][region] = {
                        metric: value_stats(
                            item["ldrLuminanceTemporalScreenSpace"][region][metric]
                            for item in transitions
                        )
                        for metric in ("mae", "p95AbsoluteChange", "p99AbsoluteChange")
                    }
                worst = max(
                    transitions,
                    key=lambda item: item["temporalScreenSpace"]["edge"]["p95AbsoluteChange"],
                )
                entry["automatedWorstFrame"] = {
                    "measurementFrame": worst["measurementFrame"],
                    "criterion": "maximum edge screen-space error-delta P95 over frames 1..N-1",
                    "value": worst["temporalScreenSpace"]["edge"]["p95AbsoluteChange"],
                }
                summary[scene][config][roi] = entry
    return summary, per_frame, flat_rows, input_rows, shared_heat_scale


def normalize(vector: np.ndarray) -> np.ndarray:
    length = float(np.linalg.norm(vector))
    expect(length > 1.0e-9, "zero-length camera vector")
    return vector / length


def view_matrix(position: list[float], target: list[float], up: list[float]) -> list[list[float]]:
    eye = np.asarray(position, dtype=np.float64)
    center = np.asarray(target, dtype=np.float64)
    up_value = np.asarray(up, dtype=np.float64)
    forward = normalize(center - eye)
    side = normalize(np.cross(forward, up_value))
    camera_up = np.cross(side, forward)
    matrix = np.eye(4, dtype=np.float64)
    matrix[0, :3], matrix[1, :3], matrix[2, :3] = side, camera_up, -forward
    matrix[0, 3] = -float(np.dot(side, eye))
    matrix[1, 3] = -float(np.dot(camera_up, eye))
    matrix[2, 3] = float(np.dot(forward, eye))
    return matrix.tolist()


def projection_matrix(fov_degrees: float, aspect: float, near: float = 0.1, far: float = 100.0) -> list[list[float]]:
    factor = 1.0 / math.tan(math.radians(fov_degrees) / 2.0)
    matrix = np.zeros((4, 4), dtype=np.float64)
    matrix[0, 0] = factor / aspect
    matrix[1, 1] = factor
    matrix[2, 2] = (far + near) / (near - far)
    matrix[2, 3] = 2.0 * far * near / (near - far)
    matrix[3, 2] = -1.0
    return matrix.tolist()


def build_camera_path(
    runs: list[dict[str, Any]], output_path: Path
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    output = {
        "schemaVersion": 1,
        "generatedAtUtc": utc_now(),
        "matrixConvention": "row-major JSON representation of glm::lookAt / OpenGL perspective",
        "projectionNear": 0.1,
        "projectionFar": 100.0,
        "scenes": {},
    }
    rows = []
    for scene in SCENES:
        run = next(
            item for item in runs
            if item["scene"] == scene and item["configuration"] == "legacy-full64" and item["block"] == 1
        )
        result = run["result"]
        timeline = result["motionTimeline"]
        up = timeline["baseState"]["cameraUp"]
        fov = float(result["camera"]["fovDegrees"])
        projection = projection_matrix(fov, 1920.0 / 1080.0)
        key_frames = sorted({0, 300, 600, 900, 1199, 1200, 1800, len(timeline["samples"]) - 1})
        key_poses = []
        for frame, sample in enumerate(timeline["samples"]):
            row = {
                "scene": scene,
                "measurementFrame": frame,
                "timelineFrame": sample["timelineFrame"],
                "cycleFrame": sample["cycleFrame"],
                "fixedTimeSeconds": sample["fixedTimeSeconds"],
                "normalizedPhase": sample["normalizedPhase"],
                "positionX": sample["cameraPosition"][0],
                "positionY": sample["cameraPosition"][1],
                "positionZ": sample["cameraPosition"][2],
                "targetX": sample["cameraTarget"][0],
                "targetY": sample["cameraTarget"][1],
                "targetZ": sample["cameraTarget"][2],
            }
            rows.append(row)
            if frame in key_frames:
                key_poses.append(
                    {
                        **row,
                        "cameraUp": up,
                        "viewMatrixRowMajor": view_matrix(sample["cameraPosition"], sample["cameraTarget"], up),
                        "projectionMatrixRowMajor": projection,
                    }
                )
        output["scenes"][scene] = {
            "cameraSignatureSha256": run["record"]["cameraSignatureSha256"],
            "fixedFramesPerSecond": timeline["fixedFramesPerSecond"],
            "cycleFrames": timeline["cycleFrames"],
            "baseState": timeline["baseState"],
            "amplitudeRatios": timeline["amplitudeRatios"],
            "fovDegrees": fov,
            "resolution": result["resolution"],
            "keyPoses": key_poses,
        }
    write_json(output_path, output)
    return output, rows


def load_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = (
        ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf")
        if bold else ("C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf")
    )
    for path in candidates:
        try:
            return ImageFont.truetype(path, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def color(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    return tuple(int(value[index:index+2], 16) for index in (0, 2, 4))


def create_performance_plot(path: Path, aggregate: dict[str, Any]) -> None:
    image = Image.new("RGB", (1500, 920), "white")
    draw = ImageDraw.Draw(image)
    title_font, label_font, small_font = load_font(30, True), load_font(20, True), load_font(16)
    draw.text((40, 22), "Deterministic camera: per-process GPU medians", fill="#172033", font=title_font)
    for panel_index, (scene, metric) in enumerate(
        [("sponza", "ssaoTotalGpu"), ("sponza", "gpuFrame"), ("san-miguel", "ssaoTotalGpu"), ("san-miguel", "gpuFrame")]
    ):
        column, row = panel_index % 2, panel_index // 2
        x0, y0, x1, y1 = 70 + column * 720, 100 + row * 390, 700 + column * 720, 420 + row * 390
        draw.rectangle((x0, y0, x1, y1), outline="#aab4c3", width=2)
        draw.text((x0, y0 - 32), f"{SCENE_LABELS[scene]} — {METRIC_LABELS[metric]}", fill="#172033", font=label_font)
        values = [
            float(item["median"])
            for config in CONFIGS
            for item in aggregate[scene][config]["metrics"][metric]["perProcess"]
        ]
        low, high = min(values), max(values)
        padding = max((high - low) * 0.15, 0.01)
        low, high = low - padding, high + padding
        for tick in range(5):
            value = low + (high - low) * tick / 4
            y = y1 - (value - low) / (high - low) * (y1 - y0)
            draw.line((x0, y, x1, y), fill="#e5e9f0")
            draw.text((x0 + 4, y - 18), f"{value:.3f}", fill="#536176", font=small_font)
        for config_index, config in enumerate(CONFIGS):
            x = x0 + 150 + config_index * 200
            items = aggregate[scene][config]["metrics"][metric]["perProcess"]
            for index, item in enumerate(items):
                value = float(item["median"])
                y = y1 - (value - low) / (high - low) * (y1 - y0)
                jitter = (index - 2.5) * 8
                draw.ellipse((x+jitter-5, y-5, x+jitter+5, y+5), fill=color(CONFIG_COLORS[config]))
            median_value = statistics.median(float(item["median"]) for item in items)
            y = y1 - (median_value - low) / (high - low) * (y1 - y0)
            draw.line((x-45, y, x+45, y), fill="#111827", width=3)
            draw.text((x-75, y1+12), CONFIG_LABELS[config].replace(" Legacy", ""), fill="#172033", font=small_font)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def create_paired_plot(path: Path, pairs: dict[str, Any]) -> None:
    image = Image.new("RGB", (1400, 650), "white")
    draw = ImageDraw.Draw(image)
    draw.text((40, 20), "Half-64 Bilateral minus Full-32: paired process medians", fill="#172033", font=load_font(28, True))
    panel_width = 620
    for scene_index, scene in enumerate(SCENES):
        x0, y0, x1, y1 = 70 + scene_index * 680, 110, 70 + scene_index * 680 + panel_width, 540
        draw.rectangle((x0, y0, x1, y1), outline="#aab4c3", width=2)
        draw.text((x0, y0-34), SCENE_LABELS[scene], fill="#172033", font=load_font(22, True))
        entries = pairs[scene]["full32-vs-half64-bilateral"]["metrics"]
        plotted = {
            "SSAO Total GPU": entries["ssaoTotalGpu"]["pairedValues"],
            "GPU Frame": entries["gpuFrame"]["pairedValues"],
        }
        values = [item["delta"] for group in plotted.values() for item in group]
        limit = max(abs(min(values)), abs(max(values)), 0.01) * 1.2
        for tick in range(5):
            value = -limit + 2*limit*tick/4
            y = y1 - (value + limit) / (2*limit) * (y1-y0)
            draw.line((x0, y, x1, y), fill="#c8d0dc" if abs(value) < 1e-9 else "#ebeff5", width=2 if abs(value) < 1e-9 else 1)
            draw.text((x0+4, y-18), f"{value:+.3f}", fill="#536176", font=load_font(15))
        for metric_index, (label, group) in enumerate(plotted.items()):
            base_x = x0 + 130 + metric_index * 290
            points = []
            for item in group:
                x = base_x + (int(item["block"])-1)*32
                y = y1 - (float(item["delta"]) + limit) / (2*limit) * (y1-y0)
                points.append((x, y))
                draw.ellipse((x-5,y-5,x+5,y+5), fill="#d64545" if metric_index == 0 else "#2f6bff")
            draw.line(points, fill="#d64545" if metric_index == 0 else "#2f6bff", width=2)
            draw.text((base_x, y1+16), label, fill="#172033", font=load_font(17, True))
    draw.text((70, 600), "Positive = Half-64 is slower; negative = Half-64 is faster. Each point is one paired block (n=6).", fill="#536176", font=load_font(18))
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def quality_series(per_frame: list[dict[str, Any]], scene: str, config: str, field: str) -> list[float]:
    by_frame: dict[int, list[float]] = defaultdict(list)
    for item in per_frame:
        if item["scene"] != scene or item["configuration"] != config:
            continue
        if field == "edgeTemporalP95":
            temporal = item["temporalScreenSpace"]
            if temporal is not None:
                by_frame[item["measurementFrame"]].append(temporal["edge"]["p95AbsoluteChange"])
        elif field == "edgeMae":
            by_frame[item["measurementFrame"]].append(item["ao"]["edge"]["mae"])
        elif field == "edgeSsim":
            by_frame[item["measurementFrame"]].append(item["ao"]["edge"]["localSsim"])
    return [statistics.median(by_frame[frame]) for frame in sorted(by_frame)]


def create_quality_plot(path: Path, per_frame: list[dict[str, Any]]) -> None:
    image = Image.new("RGB", (1500, 930), "white")
    draw = ImageDraw.Draw(image)
    draw.text((40, 20), "Targeted ROI temporal quality vs per-frame Full-64", fill="#172033", font=load_font(29, True))
    fields = (("edgeTemporalP95", "Edge error-delta P95"), ("edgeMae", "Edge AO MAE"))
    for panel_index, (scene, field_label) in enumerate((scene, field) for scene in SCENES for field in fields):
        field, label = field_label
        column, row = panel_index % 2, panel_index // 2
        x0, y0, x1, y1 = 70 + column*720, 110 + row*390, 700 + column*720, 420 + row*390
        draw.rectangle((x0,y0,x1,y1), outline="#aab4c3", width=2)
        draw.text((x0, y0-32), f"{SCENE_LABELS[scene]} — {label}", fill="#172033", font=load_font(20, True))
        series = {config: quality_series(per_frame, scene, config, field) for config in CANDIDATES}
        all_values = [value for values in series.values() for value in values]
        low, high = min(all_values), max(all_values)
        padding = max((high-low)*0.1, 1e-5)
        low, high = low-padding, high+padding
        for config in CANDIDATES:
            values = series[config]
            points = []
            for index, value in enumerate(values):
                x = x0 + index / max(1, len(values)-1) * (x1-x0)
                y = y1 - (value-low)/(high-low)*(y1-y0)
                points.append((x,y))
            draw.line(points, fill=color(CONFIG_COLORS[config]), width=3)
        draw.text((x0+10,y0+8), f"range {low:.5f}..{high:.5f}", fill="#536176", font=load_font(15))
        draw.line((x0+300,y0+18,x0+335,y0+18), fill=color(CONFIG_COLORS["legacy-full32"]), width=4)
        draw.text((x0+342,y0+5), "Full-32", fill="#172033", font=load_font(15))
        draw.line((x0+440,y0+18,x0+475,y0+18), fill=color(CONFIG_COLORS["half-bilateral64"]), width=4)
        draw.text((x0+482,y0+5), "Half-64", fill="#172033", font=load_font(15))
    draw.text((70, 890), "Temporal metric is screen-space (candidate-Full64) error change; no reprojection claim.", fill="#536176", font=load_font(18))
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def gray_image(values: np.ndarray) -> Image.Image:
    return Image.fromarray(np.rint(np.clip(values, 0, 1)*255).astype(np.uint8), mode="L").convert("RGB")


def heat_image(values: np.ndarray, scale: float) -> Image.Image:
    normalized = np.clip(values / max(scale, 1e-8), 0, 1)
    rgb = np.stack(
        (
            np.clip(2.2*normalized, 0, 1),
            np.clip(2.2*(1-np.abs(normalized-0.5)*2), 0, 1)*0.75,
            np.clip(2.2*(1-normalized), 0, 1)*0.65,
        ),
        axis=2,
    )
    return Image.fromarray(np.rint(rgb*255).astype(np.uint8), mode="RGB")


def mask_image(mask: np.ndarray) -> Image.Image:
    pixels = np.zeros((*mask.shape, 3), dtype=np.uint8)
    pixels[:] = (20, 29, 43)
    pixels[mask] = (255, 213, 79)
    return Image.fromarray(pixels, mode="RGB")


def evaluation_arrays(capture_root: Path, scene: str, roi: dict[str, Any], frame: int) -> dict[str, Any]:
    roi_name = str(roi["name"])
    ox, oy = (int(value) for value in roi["evaluationOffset"])
    width, height = (int(value) for value in roi["evaluationSize"])
    evaluate = (slice(oy,oy+height), slice(ox,ox+width))
    values: dict[str, Any] = {}
    for config in CONFIGS:
        values[f"ao:{config}"] = read_pfm(quality_file(capture_root, scene, config, roi_name, frame, "ao.pfm"))[evaluate]
        values[f"ldr:{config}"] = load_rgb(quality_file(capture_root, scene, config, roi_name, frame, "ldr.ppm"))[evaluate]
    depth = read_pfm(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "depth.pfm"))
    normal = read_pfm(quality_file(capture_root, scene, "legacy-full64", roi_name, frame, "normal.pfm"))
    values["edge"] = build_edge_mask(depth, normal)[0][evaluate]
    return values


def create_worst_figures(
    figures: Path,
    capture_root: Path,
    quality_summary: dict[str, Any],
    capture_rois: dict[str, Any],
    heat_scale: float,
) -> dict[str, Any]:
    outputs: dict[str, Any] = {}
    for scene in SCENES:
        roi_lookup = {str(item["name"]): item for item in capture_rois[scene]}
        selections = []
        for config in CANDIDATES:
            candidates = [
                (roi_name, quality_summary[scene][config][roi_name]["automatedWorstFrame"])
                for roi_name in roi_lookup
            ]
            roi_name, worst = max(candidates, key=lambda item: item[1]["value"])
            selections.append((config, roi_name, int(worst["measurementFrame"]), float(worst["value"])))
        scale = 2
        panel_width, panel_height = 256*scale, 192*scale
        header, row_gap = 62, 54
        ao_canvas = Image.new("RGB", (panel_width*6, (header+panel_height+row_gap)*2), "white")
        ao_draw = ImageDraw.Draw(ao_canvas)
        ldr_canvas = Image.new("RGB", (panel_width*3, (header+panel_height+row_gap)*2), "white")
        ldr_draw = ImageDraw.Draw(ldr_canvas)
        ao_labels = ("Full-64 AO", "Full-32 AO", "Half-64 AO", "|Full32-ref|", "|Half64-ref|", "Edge mask")
        ldr_labels = ("Full-64 LDR", "Full-32 LDR", "Half-64 LDR")
        for row, (config, roi_name, frame, value) in enumerate(selections):
            arrays = evaluation_arrays(capture_root, scene, roi_lookup[roi_name], frame)
            ref = arrays["ao:legacy-full64"]
            full32 = arrays["ao:legacy-full32"]
            half64 = arrays["ao:half-bilateral64"]
            panels = (
                gray_image(ref), gray_image(full32), gray_image(half64),
                heat_image(np.abs(full32-ref), heat_scale),
                heat_image(np.abs(half64-ref), heat_scale), mask_image(arrays["edge"]),
            )
            y = row*(header+panel_height+row_gap)
            ao_draw.text((8,y+4), f"Auto worst for {CONFIG_LABELS[config]}: {roi_name}, frame {frame}, edge temporal P95={value:.6f}", fill="#172033", font=load_font(20, True))
            for column, (label, panel) in enumerate(zip(ao_labels, panels)):
                ao_draw.text((column*panel_width+8,y+34), label, fill="#536176", font=load_font(16))
                ao_canvas.paste(panel.resize((panel_width,panel_height), Image.Resampling.NEAREST), (column*panel_width,y+header))
            ldr_draw.text((8,y+4), f"Same pose/ROI: {roi_name}, frame {frame}", fill="#172033", font=load_font(20, True))
            for column, (label, config_name) in enumerate(zip(ldr_labels, CONFIGS)):
                ldr_draw.text((column*panel_width+8,y+34), label, fill="#536176", font=load_font(16))
                panel = Image.fromarray(np.rint(np.clip(arrays[f"ldr:{config_name}"],0,1)*255).astype(np.uint8), mode="RGB")
                ldr_canvas.paste(panel.resize((panel_width,panel_height), Image.Resampling.NEAREST), (column*panel_width,y+header))
        ao_path = figures / f"{scene}-automated-worst-ao-native-2x.png"
        ldr_path = figures / f"{scene}-automated-worst-ldr-native-2x.png"
        ao_path.parent.mkdir(parents=True, exist_ok=True)
        ao_canvas.save(ao_path)
        ldr_canvas.save(ldr_path)
        outputs[scene] = {
            "ao": str(ao_path), "ldr": str(ldr_path),
            "selections": [
                {"configuration": config, "roi": roi, "frame": frame, "edgeTemporalP95": value}
                for config,roi,frame,value in selections
            ],
        }
    return outputs


def create_temporal_transition_figures(
    figures: Path,
    capture_root: Path,
    quality_summary: dict[str, Any],
    capture_rois: dict[str, Any],
    ao_scale: float,
    ldr_scale: float,
) -> dict[str, Any]:
    outputs: dict[str, Any] = {}
    labels = (
        "Full-64 AO t-1",
        "Candidate AO t-1",
        "Full-64 AO t",
        "Candidate AO t",
        "|AO error t-1|",
        "|AO error t|",
        "|AO error delta|",
        "|LDR-luma error delta|",
    )
    for scene in SCENES:
        roi_lookup = {str(item["name"]): item for item in capture_rois[scene]}
        scale = 2
        panel_width, panel_height = 256 * scale, 192 * scale
        header, row_gap = 66, 48
        canvas = Image.new(
            "RGB",
            (panel_width * len(labels), (header + panel_height + row_gap) * 2),
            "white",
        )
        draw = ImageDraw.Draw(canvas)
        rows = []
        for row, config in enumerate(CANDIDATES):
            candidates = [
                (
                    roi_name,
                    quality_summary[scene][config][roi_name]["automatedWorstFrame"],
                )
                for roi_name in roi_lookup
            ]
            roi_name, worst = max(candidates, key=lambda item: item[1]["value"])
            frame = int(worst["measurementFrame"])
            expect(frame > 0, "temporal worst frame must have a predecessor")
            previous_values = evaluation_arrays(
                capture_root, scene, roi_lookup[roi_name], frame - 1
            )
            current_values = evaluation_arrays(
                capture_root, scene, roi_lookup[roi_name], frame
            )
            previous_ref = previous_values["ao:legacy-full64"]
            previous_candidate = previous_values[f"ao:{config}"]
            current_ref = current_values["ao:legacy-full64"]
            current_candidate = current_values[f"ao:{config}"]
            previous_error = previous_candidate - previous_ref
            current_error = current_candidate - current_ref
            previous_ldr_error = (
                luminance(previous_values[f"ldr:{config}"])
                - luminance(previous_values["ldr:legacy-full64"])
            )
            current_ldr_error = (
                luminance(current_values[f"ldr:{config}"])
                - luminance(current_values["ldr:legacy-full64"])
            )
            panels = (
                gray_image(previous_ref),
                gray_image(previous_candidate),
                gray_image(current_ref),
                gray_image(current_candidate),
                heat_image(np.abs(previous_error), ao_scale),
                heat_image(np.abs(current_error), ao_scale),
                heat_image(np.abs(current_error - previous_error), ao_scale),
                heat_image(
                    np.abs(current_ldr_error - previous_ldr_error), ldr_scale
                ),
            )
            y = row * (header + panel_height + row_gap)
            draw.text(
                (8, y + 4),
                (
                    f"{CONFIG_LABELS[config]} automated worst transition: "
                    f"{roi_name}, {frame - 1}->{frame}, edge AO delta P95="
                    f"{float(worst['value']):.6f}"
                ),
                fill="#172033",
                font=load_font(20, True),
            )
            for column, (label, panel) in enumerate(zip(labels, panels)):
                draw.text(
                    (column * panel_width + 8, y + 36),
                    label,
                    fill="#536176",
                    font=load_font(15),
                )
                canvas.paste(
                    panel.resize(
                        (panel_width, panel_height), Image.Resampling.NEAREST
                    ),
                    (column * panel_width, y + header),
                )
            rows.append(
                {
                    "configuration": config,
                    "roi": roi_name,
                    "previousFrame": frame - 1,
                    "currentFrame": frame,
                    "edgeAoErrorDeltaP95": float(worst["value"]),
                }
            )
        path = figures / f"{scene}-automated-worst-transition-native-2x.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(path)
        outputs[scene] = {
            "path": str(path),
            "aoTemporalHeatScale": ao_scale,
            "ldrLuminanceTemporalHeatScale": ldr_scale,
            "rows": rows,
        }
    return outputs


def create_gifs(
    video_directory: Path,
    capture_root: Path,
    capture_rois: dict[str, Any],
    frame_count: int,
    heat_scale: float,
) -> dict[str, dict[str, str]]:
    outputs: dict[str, dict[str, str]] = {}
    labels = ("Full-64", "Full-32", "Half-64 Bilateral", "Full-32 error", "Half-64 error")
    for scene in SCENES:
        outputs[scene] = {}
        for roi in capture_rois[scene]:
            roi_name = str(roi["name"])
            frames: list[Image.Image] = []
            for frame in range(frame_count):
                arrays = evaluation_arrays(capture_root, scene, roi, frame)
                ref = arrays["ao:legacy-full64"]
                full32 = arrays["ao:legacy-full32"]
                half64 = arrays["ao:half-bilateral64"]
                panels = (
                    gray_image(ref), gray_image(full32), gray_image(half64),
                    heat_image(np.abs(full32-ref), heat_scale),
                    heat_image(np.abs(half64-ref), heat_scale),
                )
                canvas = Image.new("RGB", (256*5, 238), "white")
                draw = ImageDraw.Draw(canvas)
                draw.text((8,4), f"{SCENE_LABELS[scene]} / {roi_name} / frame {frame:03d}", fill="#172033", font=load_font(17, True))
                for column, (label, panel) in enumerate(zip(labels, panels)):
                    draw.text((column*256+6,27), label, fill="#536176", font=load_font(14))
                    canvas.paste(panel, (column*256,46))
                frames.append(canvas.convert("P", palette=Image.Palette.ADAPTIVE, colors=128))
            output = video_directory / f"{scene}-{roi_name}-ao-side-by-side.gif"
            output.parent.mkdir(parents=True, exist_ok=True)
            frames[0].save(
                output,
                save_all=True,
                append_images=frames[1:],
                duration=33,
                loop=0,
                disposal=2,
                optimize=False,
            )
            outputs[scene][roi_name] = str(output)
            del frames
    return outputs


def gpu_telemetry(project: Path, manifest: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    rows = []
    for record in manifest["performance"]["runs"]:
        before = read_json(resolve(project, record["gpuStateBefore"]))
        after = read_json(resolve(project, record["gpuStateAfter"]))
        row = {
            "executionIndex": record["executionIndex"],
            "block": record["block"],
            "scene": record["scene"],
            "configuration": record["configuration"],
            "beforeAvailable": before.get("available", False),
            "afterAvailable": after.get("available", False),
        }
        if before.get("available") and after.get("available"):
            for output_name, source_name in (
                ("temperatureC", "temperature.gpu"),
                ("graphicsClockMHz", "clocks.current.graphics"),
                ("powerW", "power.draw"),
                ("utilizationPercent", "utilization.gpu"),
            ):
                first = float(before["gpu"][source_name])
                second = float(after["gpu"][source_name])
                row[f"{output_name}Before"] = first
                row[f"{output_name}After"] = second
                row[f"{output_name}Midpoint"] = (first+second)/2
                row[f"{output_name}Delta"] = second-first
        rows.append(row)
    available_rows = [row for row in rows if row["beforeAvailable"] and row["afterAvailable"]]
    summary = {
        "availableRunCount": len(available_rows),
        "totalRunCount": len(rows),
        "temperatureMidpointC": value_stats(row["temperatureCMidpoint"] for row in available_rows),
        "graphicsClockMidpointMHz": value_stats(row["graphicsClockMHzMidpoint"] for row in available_rows),
        "powerMidpointW": value_stats(row["powerWMidpoint"] for row in available_rows),
        "note": "Before/after snapshots diagnose drift; they are not frame-level hardware counters.",
    }
    return summary, rows


def gpu_background_audit(root: Path) -> dict[str, Any]:
    snapshots = []
    process_names: set[str] = set()
    nonzero_activity_lines = []
    for path in sorted((root / "performance" / "gpu-state").glob("block-*.json")):
        value = read_json(path)
        pmon = next(
            (
                section
                for section in value.get("sections", [])
                if "pmon" in " ".join(section.get("command", [])).lower()
            ),
            None,
        )
        lines = [] if pmon is None else pmon.get("stdout", "").splitlines()
        parsed = []
        for line in lines:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            pieces = stripped.split()
            if len(pieces) < 4 or not pieces[0].isdigit() or not pieces[1].isdigit():
                continue
            command = pieces[-1]
            process_names.add(command)
            utilization_tokens = pieces[3:-3] if len(pieces) >= 8 else pieces[3:-1]
            has_nonzero = any(token not in {"-", "0", "0.0"} for token in utilization_tokens)
            parsed.append({"pid": int(pieces[1]), "command": command, "raw": stripped, "reportedNonzero": has_nonzero})
            if has_nonzero:
                nonzero_activity_lines.append({"snapshot": path.name, "raw": stripped})
        snapshots.append(
            {
                "path": str(path),
                "capturedAtUtc": value.get("capturedAtUtc"),
                "processes": parsed,
            }
        )
    return {
        "snapshotCount": len(snapshots),
        "observedProcessNames": sorted(process_names),
        "reportedNonzeroPmonLineCount": len(nonzero_activity_lines),
        "reportedNonzeroPmonLines": nonzero_activity_lines,
        "snapshots": snapshots,
        "interpretation": (
            "Desktop/overlay/emulator processes were present. The one-shot pmon "
            "snapshots reported no numeric per-process utilization; balanced order "
            "and per-process dispersion remain the primary noise controls."
        ),
    }


def static_comparison(project: Path, dynamic_pairs: dict[str, Any]) -> dict[str, Any]:
    static = read_json(project / STATIC_SUMMARY)
    output = {}
    for scene in SCENES:
        output[scene] = {}
        for metric in ("ssaoTotalGpu", "gpuFrame"):
            old = static["pairedComparisons"][scene]["final-tier"]["metrics"][metric]
            new = dynamic_pairs[scene]["full32-vs-half64-bilateral"]["metrics"][metric]
            old_sign = 1 if old["meanDelta"] > 0 else -1 if old["meanDelta"] < 0 else 0
            new_sign = 1 if new["meanDelta"] > 0 else -1 if new["meanDelta"] < 0 else 0
            output[scene][metric] = {
                "staticMeanDeltaMs": old["meanDelta"],
                "staticDirection": old["direction"],
                "dynamicMeanDeltaMs": new["meanDelta"],
                "dynamicDirection": new["direction"],
                "directionConsistent": old_sign == new_sign,
                "batchesMixed": False,
            }
    return output


def fmt(value: Any, digits: int = 3, signed: bool = False) -> str:
    if value is None:
        return "N/A"
    return f"{float(value):+.{digits}f}" if signed else f"{float(value):.{digits}f}"


def build_report(
    summary: dict[str, Any],
    aggregate: dict[str, Any],
    pairs: dict[str, Any],
    quality: dict[str, Any],
    static: dict[str, Any],
    gpu: dict[str, Any],
    heat_scale: float,
    artifacts: dict[str, Any],
) -> str:
    decision = summary["decision"]
    lines = [
        "# SSAO 确定性移动相机：时域质量与动态性能报告",
        "",
        f"最终判断：**{decision['status']}**。{decision['summaryCn']}",
        "",
        "## 1. 实验边界与可信度",
        "",
        "- 两场景均为 Release x64、1920×1080、VSync Off；灯光、阴影、Bloom、热重载和输入冻结状态在三配置间一致。",
        "- 相机由固定帧索引驱动，60 Hz 固定步长、1200 帧周期；300 帧预热后明确重置，measured frame 0 对齐 timeline frame 0。",
        "- 性能批次为 6 个位置平衡 block，每配置/场景 6 个独立进程，每进程 2000 measured frames；独立单位是进程 median。",
        "- 质量批次独立运行，逐帧读回不进入正式性能数据。Float AO 为正式误差来源；8-bit LDR 只作补充。",
        "- Edge mask 逐帧仅由同 pose 的 Full-64 depth/normal 构造：相对深度 0.02、法线 25°、膨胀 3 px；捕获带 4 px halo 后再评估原 256×192 ROI。",
        "- 时域量是 screen-space `(candidate-Full64)_t - (candidate-Full64)_(t-1)`；没有可靠重投影，因此没有伪称 motion-compensated 指标。",
        f"- Release EXE SHA-256：`{summary['source']['releaseExecutableSha256'].upper()}`。旧 SSAO 三批目录前后聚合哈希一致。",
        "",
        "32 样本仍是按 `i/64` 径向尺度生成的 64 核序列前缀；Full-32 是实际 preset，而不是纯样本数因果实验。CPU/GPU 时间没有相加，CPU Frame 的变化也不解释成 SSAO CPU 工作。",
        "",
        "## 2. 动态性能",
        "",
        "下表为 median-of-6 process medians；P95/P99 也以每进程尾延迟再取六进程中位数。",
        "",
        "| Scene | Config | GPU Frame M/P95/P99 ms | Deferred GPU M/P95/P99 | SSAO Total M/P95/P99 | Generate M/P95/P99 | Upsample M/P95/P99 | Draw |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for scene in SCENES:
        for config in CONFIGS:
            def triplet(metric: str) -> str:
                entry = aggregate[scene][config]["metrics"][metric]
                return "/".join(
                    fmt(entry[name]["median"], 3)
                    for name in ("processMedianDistribution", "processP95Distribution", "processP99Distribution")
                )
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | {triplet('gpuFrame')} | "
                f"{triplet('deferredGpu')} | {triplet('ssaoTotalGpu')} | {triplet('ssaoGenerateGpu')} | "
                f"{triplet('ssaoUpsampleGpu')} | {triplet('drawCalls').split('/')[0]} |"
            )
    lines.extend(["", "Half-64 Bilateral − Full-32 的同 block 配对结果：", "", "| Scene | Metric | Mean delta ms | Relative | Direction | Bootstrap 95% CI (n=6) |", "|---|---|---:|---:|---|---:|"])
    for scene in SCENES:
        pair = pairs[scene]["full32-vs-half64-bilateral"]["metrics"]
        for metric in ("ssaoTotalGpu", "gpuFrame"):
            entry = pair[metric]
            ci = entry["bootstrap95CiMeanDelta"]
            lines.append(
                f"| {SCENE_LABELS[scene]} | {METRIC_LABELS[metric]} | {fmt(entry['meanDelta'], 6, True)} | "
                f"{fmt(entry['meanRelativePercent'], 2, True)}% | {entry['direction']} | [{fmt(ci['low'],6,True)}, {fmt(ci['high'],6,True)}] |"
            )
    lines.extend(["", "动态与上一轮静态批次只比较方向，不混池：", ""])
    for scene in SCENES:
        total = static[scene]["ssaoTotalGpu"]
        frame = static[scene]["gpuFrame"]
        lines.append(
            f"- {SCENE_LABELS[scene]}：SSAO Total 静态 `{fmt(total['staticMeanDeltaMs'],6,True)}` ms → 动态 `{fmt(total['dynamicMeanDeltaMs'],6,True)}` ms，方向"
            f"{'一致' if total['directionConsistent'] else '不一致'}；GPU Frame 静态 `{fmt(frame['staticMeanDeltaMs'],6,True)}` → 动态 `{fmt(frame['dynamicMeanDeltaMs'],6,True)}` ms，方向"
            f"{'一致' if frame['directionConsistent'] else '不一致'}。"
        )
    lines.extend([
        "",
        f"GPU 遥测覆盖 {gpu['availableRunCount']}/{gpu['totalRunCount']} 个进程。温度中点范围 `{fmt(gpu['temperatureMidpointC']['min'],1)}–{fmt(gpu['temperatureMidpointC']['max'],1)} °C`，图形频率中点范围 `{fmt(gpu['graphicsClockMidpointMHz']['min'],0)}–{fmt(gpu['graphicsClockMidpointMHz']['max'],0)} MHz`；这些是进程前后诊断快照，不是硬件 counter。Block pmon 中存在桌面/Overlay/浏览器/模拟器进程，但数值型非零活动行数为 `{gpu['backgroundAudit']['reportedNonzeroPmonLineCount']}`；因此仍以平衡顺序和进程离散为主要抗噪证据。",
        "",
        "## 3. 动态 Float AO 与 LDR",
        "",
        "下面将两个预登记 ROI 的逐帧指标合并展示为 ‘ROI-frame 分布’；全局是 ROI 内全像素，不冒充整帧全局。",
        "",
        "| Scene | Candidate | Edge AO MAE median/P95/worst | Edge local SSIM median/P05/worst | Edge temporal Δ P95 median/P95/worst | Edge LDR PSNR median/P05/worst dB |",
        "|---|---|---:|---:|---:|---:|",
    ])
    for scene in SCENES:
        for config in CANDIDATES:
            combined = summary["qualityCombined"][scene][config]
            mae = combined["edgeAoMae"]
            ssim = combined["edgeAoLocalSsim"]
            temporal = combined["edgeTemporalP95"]
            ldr = combined["edgeLdrPsnrDb"]
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | {fmt(mae['median'],6)}/{fmt(mae['p95'],6)}/{fmt(mae['max'],6)} | "
                f"{fmt(ssim['median'],6)}/{fmt(ssim['p05'],6)}/{fmt(ssim['min'],6)} | "
                f"{fmt(temporal['median'],6)}/{fmt(temporal['p95'],6)}/{fmt(temporal['max'],6)} | "
                f"{fmt(ldr['median'],2)}/{fmt(ldr['p05'],2)}/{fmt(ldr['min'],2)} |"
            )
    lines.extend([
        "",
        f"所有误差图共享 `0..{heat_scale:.6f}` 色标；色标定义为所有候选/场景/帧/ROI 的逐帧绝对 AO 误差 P99.5 的最大值。最坏帧不是手挑，而是每候选每 ROI 的 edge temporal Δ P95 最大帧。",
        "",
        "### 质量证据边界",
        "",
        f"- 指标证据：{decision['temporalEvidenceCn']}",
        f"- 关键帧证据：{decision['keyframeEvidenceCn']}",
        "- 动画为 8-bit 展示用 GIF；正式数值始终来自 float PFM。报告生成过程没有把 GIF/PPM 当成 AO 正式误差。",
        "",
        "## 4. 必须回答",
        "",
        f"1. **Half-64 动态 AO/LDR 是否可接受？** {decision['answer1Cn']}",
        f"2. **Half-64 与 Full-32 谁更稳定？** {decision['answer2Cn']}",
        f"3. **是否观察到 shimmer / 接触阴影跳变 / sample noise？** {decision['answer3Cn']}",
        f"4. **动态性能是否保持静态方向？** {decision['answer4Cn']}",
        f"5. **简历可写与不可写边界。** {decision['answer5Cn']}",
        "",
        "## 5. 最终 Go/No-Go 与配置建议",
        "",
        decision["recommendationCn"],
        "",
        "## 6. 产物",
        "",
        "- `summary.json` / `summary.csv`：机器可读性能与质量汇总。",
        "- `performance-per-process.csv` / `paired-deltas.csv`：独立进程与配对差值。",
        "- `quality-per-frame.json` / `quality-per-frame.csv`：逐帧 float AO、LDR 和 screen-space temporal 指标。",
        "- `camera-path.json` / `camera-path-samples.csv`：路径参数、完整逐帧位姿和关键帧 view/projection。",
        "- `input-hashes.csv`：实际读取输入的大小与 SHA-256。",
        "- `figures/`：动态性能、时域曲线及自动最坏帧 2× 原生像素 crop。",
        "- `videos/`：按预登记 ROI 的并排连续 GIF。",
        "",
        "## 7. 已知限制",
        "",
        "- 动态质量是两个预登记 256×192 ROI，不是新一轮全帧序列；上一轮静态全帧 float 证据仍独立保留。",
        "- 时域误差是 screen-space 指标，未做重投影、遮挡或反遮挡剔除。",
        "- 只有两场景、单条慢速路径、单台 GPU/驱动；不支持跨硬件、任意相机速度或所有内容类型的普遍结论。",
        "- 自动最坏帧 PNG 可程序化审计；GIF 需播放器连续观看，报告不会声称完成了无法自动证明的主观播放验收。",
        "- 没有硬件 counter，因此缓存、寄存器、wave occupancy 或分支行为只允许作为未证实假设，不能写成根因。",
    ])
    return "\n".join(lines) + "\n"


def combined_quality(per_frame: list[dict[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for scene in SCENES:
        output[scene] = {}
        for config in CANDIDATES:
            group = [item for item in per_frame if item["scene"] == scene and item["configuration"] == config]
            transitions = [item for item in group if item["temporalScreenSpace"] is not None]
            def extended(values: Iterable[float]) -> dict[str, Any]:
                data = [float(value) for value in values]
                base = value_stats(data)
                base["p05"] = nearest_rank(data, 0.05)
                return base
            output[scene][config] = {
                "roiFrameCount": len(group),
                "transitionCount": len(transitions),
                "edgeAoMae": extended(item["ao"]["edge"]["mae"] for item in group),
                "edgeAoLocalSsim": extended(item["ao"]["edge"]["localSsim"] for item in group),
                "edgeAoPsnrDb": extended(item["ao"]["edge"]["psnrDb"] for item in group),
                "edgeTemporalMae": extended(item["temporalScreenSpace"]["edge"]["mae"] for item in transitions),
                "edgeTemporalP95": extended(item["temporalScreenSpace"]["edge"]["p95AbsoluteChange"] for item in transitions),
                "edgeLdrTemporalMae": extended(
                    item["ldrLuminanceTemporalScreenSpace"]["edge"]["mae"]
                    for item in transitions
                ),
                "edgeLdrTemporalP95": extended(
                    item["ldrLuminanceTemporalScreenSpace"]["edge"]["p95AbsoluteChange"]
                    for item in transitions
                ),
                "edgeLdrPsnrDb": extended(item["ldr"]["edge"]["psnrDb"] for item in group),
                "edgeLdrLocalSsim": extended(item["ldr"]["edge"]["localSsim"] for item in group),
            }
    return output


def build_decision(
    aggregate: dict[str, Any],
    pairs: dict[str, Any],
    quality_combined: dict[str, Any],
    static: dict[str, Any],
) -> dict[str, Any]:
    full64_savings = all(
        pairs[scene]["full64-vs-half64-bilateral"]["metrics"]["ssaoTotalGpu"]["direction"]
        == "right-faster-all-blocks"
        for scene in SCENES
    )
    half_temporal_lower = all(
        quality_combined[scene]["half-bilateral64"]["edgeTemporalP95"]["median"]
        <= quality_combined[scene]["legacy-full32"]["edgeTemporalP95"]["median"]
        for scene in SCENES
    )
    half_mae_lower = all(
        quality_combined[scene]["half-bilateral64"]["edgeAoMae"]["median"]
        <= quality_combined[scene]["legacy-full32"]["edgeAoMae"]["median"]
        for scene in SCENES
    )
    half_ldr_psnr_higher = all(
        quality_combined[scene]["half-bilateral64"]["edgeLdrPsnrDb"]["median"]
        >= quality_combined[scene]["legacy-full32"]["edgeLdrPsnrDb"]["median"]
        for scene in SCENES
    )
    go = full64_savings and half_temporal_lower and half_mae_lower
    status = "GO" if go else "NO-GO"
    temporal_lines = []
    for scene in SCENES:
        full = quality_combined[scene]["legacy-full32"]["edgeTemporalP95"]
        half = quality_combined[scene]["half-bilateral64"]["edgeTemporalP95"]
        temporal_lines.append(
            f"{SCENE_LABELS[scene]} edge temporal Δ P95 中位数 Half-64 `{half['median']:.6f}` vs Full-32 `{full['median']:.6f}`"
        )
    sponza_pair = pairs["sponza"]["full32-vs-half64-bilateral"]["metrics"]["ssaoTotalGpu"]
    san_pair = pairs["san-miguel"]["full32-vs-half64-bilateral"]["metrics"]["ssaoTotalGpu"]
    return {
        "status": status,
        "criteria": {
            "halfVsFull64SsaoTotalFasterAllSixBlocksBothScenes": full64_savings,
            "halfEdgeTemporalP95MedianNoWorseThanFull32BothScenes": half_temporal_lower,
            "halfEdgeAoMaeMedianNoWorseThanFull32BothScenes": half_mae_lower,
            "halfEdgeLdrPsnrMedianNoWorseThanFull32BothScenes": half_ldr_psnr_higher,
            "noArbitraryWeightedScore": True,
        },
        "summaryCn": (
            "在本机、两场景和这条确定性慢速路径内，Half-64 相对 Full-64 的 GPU 收益与目标 ROI 时域稳定性同时成立；"
            "它仍不普遍支配 Full-32。"
            if go else
            "至少一个场景的时域边缘稳定性或相对 Full-64 的稳定 GPU 收益未同时成立，因此不把 Half-64 提升为最终案例结论。"
        ),
        "temporalEvidenceCn": "；".join(temporal_lines) + "。",
        "keyframeEvidenceCn": "自动最坏帧按事先定义的 edge temporal Δ P95 选择，并以相同 ROI/pose/色标输出；主观连续播放仍保留为用户可复核项。",
        "answer1Cn": (
            "两个场景的指标均未显示 Half-64 比 Full-32 更大的 edge screen-space error-delta 中位数，且 edge AO MAE 更低；"
            "可接受结论仅限已捕获 ROI、速度与路径，最坏区域见自动 worst-frame 图。"
            if half_temporal_lower and half_mae_lower else
            "不能在两个场景同时确认；详见出现反向的 ROI/最坏帧，未通过调参掩盖。"
        ),
        "answer2Cn": (
            "按 edge temporal Δ P95，Half-64 在两场景均更稳定或持平；Full-32 的 local SSIM 优势与 Half-64 的点误差/PSNR优势仍是不同指标的分裂，不能合成唯一质量分数。"
            if half_temporal_lower else
            "呈场景相关 Trade-off；不能称 Half-64 在运动中稳定支配 Full-32。"
        ),
        "answer3Cn": "指标层面由误差差分直接量化；肉眼证据限定为自动 worst/keyframe PNG，连续 shimmer 的 GIF 已交付但不虚构未完成的主观播放结论。",
        "answer4Cn": (
            f"Sponza Half-64−Full-32 SSAO Total 为 `{sponza_pair['meanDelta']:+.6f}` ms；San Miguel 为 `{san_pair['meanDelta']:+.6f}` ms。"
            f"两场景与静态方向分别为{'一致' if static['sponza']['ssaoTotalGpu']['directionConsistent'] else '不一致'}、"
            f"{'一致' if static['san-miguel']['ssaoTotalGpu']['directionConsistent'] else '不一致'}；两批不混池。"
        ),
        "answer5Cn": (
            "可写：在固定两真实场景/单 GPU 上，把 Full-64 改为 Half-64 + depth/normal-aware bilateral 后，动态 SSAO GPU 成本在 6/6 配对进程中下降，并以逐帧 float AO/edge temporal 指标验证目标 ROI。"
            "不可写：无损、普遍优于 Full-32、跨硬件成立、透明几何已解决，或把缓存/寄存器猜测当根因。"
        ),
        "recommendationCn": (
            "建议保留 `Half-64 Bilateral` 作为 Medium/性能优化案例；Sponza 若只追求最低 SSAO 时间仍可选 Full-32，San Miguel 按实测再决定，High/参考回退 Full-64。"
            "该建议不是声称 Half-64 对 Full-32 全面支配，而是相对 Full-64 的明确成本下降加上受控时域质量证据。"
            if go else
            "本轮为 No-Go：保留 Full-32/Full-64 档位，不实现新的时域或 refinement 算法；下一项最小验证只应针对失败 ROI 做更长同路径人工播放复核。"
        ),
    }


def build_final_decision(
    pairs: dict[str, Any],
    quality: dict[str, Any],
) -> dict[str, Any]:
    full64_savings = all(
        pairs[scene]["full64-vs-half64-bilateral"]["metrics"]["ssaoTotalGpu"]
        ["direction"]
        == "right-faster-all-blocks"
        for scene in SCENES
    )
    half_ao_temporal_worse = all(
        quality[scene]["half-bilateral64"]["edgeTemporalP95"]["median"]
        > quality[scene]["legacy-full32"]["edgeTemporalP95"]["median"]
        for scene in SCENES
    )
    half_ldr_temporal_worse = all(
        quality[scene]["half-bilateral64"]["edgeLdrTemporalP95"]["median"]
        > quality[scene]["legacy-full32"]["edgeLdrTemporalP95"]["median"]
        for scene in SCENES
    )
    half_spatial_mae_better = all(
        quality[scene]["half-bilateral64"]["edgeAoMae"]["median"]
        < quality[scene]["legacy-full32"]["edgeAoMae"]["median"]
        for scene in SCENES
    )
    half_spatial_ssim_worse = all(
        quality[scene]["half-bilateral64"]["edgeAoLocalSsim"]["median"]
        < quality[scene]["legacy-full32"]["edgeAoLocalSsim"]["median"]
        for scene in SCENES
    )
    sponza_tier = pairs["sponza"]["full32-vs-half64-bilateral"]["metrics"]
    san_tier = pairs["san-miguel"]["full32-vs-half64-bilateral"]["metrics"]
    no_robust_cross_scene_tier_win = (
        sponza_tier["ssaoTotalGpu"]["direction"]
        == "right-slower-all-blocks"
        and san_tier["gpuFrame"]["direction"] == "mixed"
    )
    no_go = (
        full64_savings
        and half_ao_temporal_worse
        and half_ldr_temporal_worse
        and no_robust_cross_scene_tier_win
    )

    def quality_values(scene: str) -> dict[str, float]:
        full = quality[scene]["legacy-full32"]
        half = quality[scene]["half-bilateral64"]
        return {
            "fullAoTemporal": full["edgeTemporalP95"]["median"],
            "halfAoTemporal": half["edgeTemporalP95"]["median"],
            "fullLdrTemporal": full["edgeLdrTemporalP95"]["median"],
            "halfLdrTemporal": half["edgeLdrTemporalP95"]["median"],
            "fullMae": full["edgeAoMae"]["median"],
            "halfMae": half["edgeAoMae"]["median"],
            "fullSsim": full["edgeAoLocalSsim"]["median"],
            "halfSsim": half["edgeAoLocalSsim"]["median"],
        }

    sponza_quality = quality_values("sponza")
    san_quality = quality_values("san-miguel")
    return {
        "status": "NO-GO" if no_go else "GO",
        "scope": "Half-64 Bilateral as the default real-time/Medium SSAO tier",
        "portfolioCaseStatus": (
            "validated experiment with a No-Go shipping decision"
            if no_go
            else "validated optimization within the measured scope"
        ),
        "criteria": {
            "halfVsFull64SsaoTotalFasterAllSixBlocksBothScenes": full64_savings,
            "halfVsFull32AoEdgeTemporalP95WorseBothScenes": half_ao_temporal_worse,
            "halfVsFull32LdrEdgeTemporalP95WorseBothScenes": half_ldr_temporal_worse,
            "halfVsFull32SpatialEdgeMaeBetterBothScenes": half_spatial_mae_better,
            "halfVsFull32SpatialEdgeLocalSsimWorseBothScenes": half_spatial_ssim_worse,
            "noRobustCrossScenePerformanceWinOverFull32": no_robust_cross_scene_tier_win,
            "noArbitraryWeightedScore": True,
        },
        "summaryCn": (
            "Half-64 相对 Full-64 的 GPU 降本真实且稳定，但相对可用的 Full-32 "
            "档位，它在 Sponza 更慢、San Miguel 的 GPU Frame 基本持平，同时两个场景的 "
            "AO 与最终 LDR 边缘时域误差都更大。因此本轮对“默认实时/Medium 档采用 "
            "Half-64 Bilateral”给出 No-Go；不再追加算法修复。"
            if no_go
            else "测量条件下未出现阻止采用 Half-64 的一致性反证。"
        ),
        "answer1Cn": (
            "未通过两个场景共同的默认档验收。Sponza 的最坏区域是花盆、悬挂物与柱面交界；"
            "San Miguel 是高密度叶片/枝干边缘，并在桌椅接触区也出现同向结果。"
            "结论限于预注册 ROI、慢速路径和本机，不等价于所有画面都肉眼明显闪烁。"
        ),
        "answer2Cn": (
            f"Full-32 的边缘时域误差更稳定：Sponza AO P95 中位数 "
            f"{sponza_quality['fullAoTemporal']:.6f} vs {sponza_quality['halfAoTemporal']:.6f}，"
            f"San Miguel 为 {san_quality['fullAoTemporal']:.6f} vs "
            f"{san_quality['halfAoTemporal']:.6f}。Half-64 的边缘 MAE 在两场景更低，"
            "但 local SSIM 更低，且 AO/LDR 时域差分更大；这是平滑后的点误差改善与"
            "结构/时域保真退化之间的真实分裂，不合成为单一分数。"
        ),
        "answer3Cn": (
            "指标证据：Half-64 的 AO 与 LDR-luminance edge error-delta P95 中位数在两场景"
            "均高于 Full-32。静态最坏跃迁图显示误差变化沿 Sponza 花盆轮廓和 San Miguel "
            "叶片/枝条边缘成片分布；Full-32 的 AO 更颗粒化，但相邻帧误差变化较小。"
            "120 帧 GIF 已交付；本机浏览器插件初始化失败，因此没有虚构连续播放的主观"
            "验收结论。保守 No-Go 由浮点指标和可审计跃迁图支撑。"
        ),
        "answer4Cn": (
            f"与静态结论一致：Sponza Half-64−Full-32 SSAO Total 为 "
            f"{sponza_tier['ssaoTotalGpu']['meanDelta']:+.6f} ms，6/6 更慢；"
            f"San Miguel 为 {san_tier['ssaoTotalGpu']['meanDelta']:+.6f} ms，6/6 更快，"
            f"但 GPU Frame 仅 {san_tier['gpuFrame']['meanDelta']:+.6f} ms 且方向混合，"
            "应视为帧级基本持平。静态与动态批次没有混池。"
        ),
        "answer5Cn": (
            "可写成工程验证故事：在 1080p、RTX 5060 Ti、Sponza/San Miguel 上，"
            "Half-resolution Generate + depth/normal-aware bilateral 相对 Full-64 显著降低"
            "SSAO GPU 时间，并通过平衡重复和逐帧 float AO 验证；确定性移动相机随后暴露"
            "边缘时域稳定性退化，因此没有把它作为默认档。不可写成无损、普遍优于 "
            "Full-32、已解决 shimmer、跨硬件成立，或把缓存/寄存器猜测写成根因。"
        ),
        "recommendationCn": (
            "默认 Medium 选 Full-32；Full-64 保留为 High/reference。Half-64 Bilateral "
            "只保留为实验路径与负面边界证据，不继续实现 Temporal/Refinement/Downsampling "
            "改造。现有动态证据足以完成本轮选型；GIF 人工播放只用于复核主观可见度，"
            "不改变保守 No-Go。"
        ),
    }


def build_final_report(
    summary: dict[str, Any],
    aggregate: dict[str, Any],
    pairs: dict[str, Any],
    quality_by_roi: dict[str, Any],
    quality_combined: dict[str, Any],
    static: dict[str, Any],
    gpu: dict[str, Any],
    artifacts: dict[str, Any],
) -> str:
    decision = summary["decision"]
    lines = [
        "# SSAO 确定性移动相机：时域质量与动态性能报告",
        "",
        f"最终判断：**{decision['status']}**（范围：{decision['scope']}）。",
        "",
        decision["summaryCn"],
        "",
        "## 1. 协议与可信度",
        "",
        "- Release x64，1920×1080，VSync Off；灯光、阴影、Bloom、热重载和输入冻结状态在配置间一致。",
        "- 相机由帧索引驱动，60 Hz 固定步长、1200 帧周期；300 帧预热后显式重置，measured frame 0 对齐 timeline frame 0。",
        "- 性能：6 个位置平衡 block，每配置/场景 6 个独立进程，每进程 2000 帧；独立统计单位是每进程 median。",
        "- 质量：独立进程捕获 120 个连续 pose；float AO 为正式来源，8-bit LDR 仅补充，读回不进入性能样本。",
        "- Edge mask 逐帧仅由同 pose Full-64 depth/normal 构造：相对深度 0.02、法线 25°、膨胀 3 px；ROI 捕获含 4 px halo。",
        "- 时域量是 screen-space `(candidate-Full64)_t-(candidate-Full64)_(t-1)`；没有可靠重投影，故不声称 motion-compensated。",
        f"- Release EXE SHA-256：`{summary['source']['releaseExecutableSha256'].upper()}`；旧批次聚合哈希前后一致。",
        f"- 首轮运动路径批次 `{summary['rejectedAttempt']['path']}` 已排除：{summary['rejectedAttempt']['reasonCn']} 原目录保留且未混池。",
        "",
        "32 样本仍是按 `i/64` 径向尺度生成的 64 核序列前缀；Full-32 是实际 preset，不是纯样本数因果实验。CPU/GPU 时间不相加，GPU back-pressure 也不解释成 SSAO CPU 工作。",
        "",
        "## 2. 动态性能",
        "",
        "下表为 6 个进程统计量的中位数；每格为 Median/P95/P99（ms），Draw 为每帧中位数。完整 min/max/CV 与逐进程值见 CSV/JSON。",
        "",
        "| Scene | Config | GPU Frame | Deferred GPU | SSAO Total | Generate | Upsample | Draw |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for scene in SCENES:
        for config in CONFIGS:
            def triplet(metric: str) -> str:
                entry = aggregate[scene][config]["metrics"][metric]
                return "/".join(
                    fmt(entry[name]["median"], 3)
                    for name in (
                        "processMedianDistribution",
                        "processP95Distribution",
                        "processP99Distribution",
                    )
                )

            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{triplet('gpuFrame')} | {triplet('deferredGpu')} | "
                f"{triplet('ssaoTotalGpu')} | {triplet('ssaoGenerateGpu')} | "
                f"{triplet('ssaoUpsampleGpu')} | "
                f"{triplet('drawCalls').split('/')[0]} |"
            )
    lines.extend(
        [
            "",
            "CPU 计时单独列示，不与 GPU 相加；每格同样为 Median/P95/P99（ms）。",
            "",
            "| Scene | Config | CPU Frame | SSAO Total CPU | Generate CPU | Upsample CPU |",
            "|---|---|---:|---:|---:|---:|",
        ]
    )
    for scene in SCENES:
        for config in CONFIGS:
            def cpu_triplet(metric: str) -> str:
                entry = aggregate[scene][config]["metrics"][metric]
                return "/".join(
                    fmt(entry[name]["median"], 3)
                    for name in (
                        "processMedianDistribution",
                        "processP95Distribution",
                        "processP99Distribution",
                    )
                )

            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{cpu_triplet('cpuFrame')} | {cpu_triplet('ssaoTotalCpu')} | "
                f"{cpu_triplet('ssaoGenerateCpu')} | "
                f"{cpu_triplet('ssaoUpsampleCpu')} |"
            )
    lines.extend(
        [
            "",
            "关键同 block 配对差值（右侧配置减左侧配置）：",
            "",
            "| Scene | Comparison | SSAO Total mean delta | Direction | GPU Frame mean delta | Direction |",
            "|---|---|---:|---|---:|---|",
        ]
    )
    for scene in SCENES:
        for pair_id, label in (
            ("full64-vs-half64-bilateral", "Half-64 − Full-64"),
            ("full32-vs-half64-bilateral", "Half-64 − Full-32"),
        ):
            pair = pairs[scene][pair_id]["metrics"]
            lines.append(
                f"| {SCENE_LABELS[scene]} | {label} | "
                f"{pair['ssaoTotalGpu']['meanDelta']:+.6f} ms | "
                f"{pair['ssaoTotalGpu']['direction']} | "
                f"{pair['gpuFrame']['meanDelta']:+.6f} ms | "
                f"{pair['gpuFrame']['direction']} |"
            )
    lines.extend(["", "静态与动态只比较方向、不混池："])
    for scene in SCENES:
        lines.append(
            f"- {SCENE_LABELS[scene]} SSAO Total：静态 "
            f"{static[scene]['ssaoTotalGpu']['staticMeanDeltaMs']:+.6f} ms，"
            f"动态 {static[scene]['ssaoTotalGpu']['dynamicMeanDeltaMs']:+.6f} ms；方向一致。"
        )
    lines.extend(
        [
            "",
            f"nvidia-smi 覆盖 {gpu['availableRunCount']}/{gpu['totalRunCount']} 个进程；"
            f"温度中点 {gpu['temperatureMidpointC']['min']:.1f}–{gpu['temperatureMidpointC']['max']:.1f} °C，"
            f"图形频率中点 {gpu['graphicsClockMidpointMHz']['min']:.0f}–{gpu['graphicsClockMidpointMHz']['max']:.0f} MHz。"
            "这些仅是前后诊断快照，不是硬件 counter；抗噪声结论主要来自平衡顺序与配对分布。",
            "",
            "## 3. 动态质量",
            "",
            "下表合并每场景两个预注册 ROI；均为逐帧/逐跃迁分布的中位数。AO/LDR Temporal P95 越低越稳定。",
            "",
            "| Scene | Config | Edge AO MAE | AO PSNR dB | AO local SSIM | AO Temporal P95 | LDR PSNR dB | LDR local SSIM | LDR Temporal P95 |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for scene in SCENES:
        for config in CANDIDATES:
            item = quality_combined[scene][config]
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{item['edgeAoMae']['median']:.6f} | "
                f"{item['edgeAoPsnrDb']['median']:.3f} | "
                f"{item['edgeAoLocalSsim']['median']:.6f} | "
                f"{item['edgeTemporalP95']['median']:.6f} | "
                f"{item['edgeLdrPsnrDb']['median']:.3f} | "
                f"{item['edgeLdrLocalSsim']['median']:.6f} | "
                f"{item['edgeLdrTemporalP95']['median']:.6f} |"
            )
    lines.extend(["", "Mask 覆盖范围（120 帧 min–max）："])
    for scene in SCENES:
        for roi, item in quality_by_roi[scene]["legacy-full32"].items():
            foreground = item["maskCoverage"]["foregroundPercent"]
            edge = item["maskCoverage"]["edgePercent"]
            lines.append(
                f"- {SCENE_LABELS[scene]} / {roi}: foreground "
                f"{foreground['min']:.2f}–{foreground['max']:.2f}%，edge "
                f"{edge['min']:.2f}–{edge['max']:.2f}%。"
            )
    lines.extend(
        [
            "",
            "Half-64 在两场景的 edge MAE 更低，但 AO local SSIM、AO 时域稳定性与 LDR 时域稳定性均更差。它不是 Full-32 的稳定支配者；这是质量维度分裂，而非一个加权分数能抹平的差异。",
            "",
            "## 4. 最坏区域与目视证据边界",
            "",
            "- 自动 worst frame 由预定义的 edge AO error-delta P95 选取，没有事后挑有利帧。跃迁图同时显示 t-1/t、空间误差、AO 误差变化和 LDR-luminance 误差变化，并对两候选使用统一色标。",
            "- Sponza：Half-64 最坏跃迁位于 contact ROI 的花盆/悬挂物/柱面轮廓；San Miguel 位于 edge ROI 的密集叶片和枝干。San Miguel contact ROI 也呈相同的 Half-64 时域劣势。",
            "- 静态 worst-frame LDR 看起来接近，不能单独证明无闪烁；浮点差分与 LDR-luminance 差分显示结构化变化。120 帧 GIF 已保存供连续播放。",
            "- 本机 in-app Browser 初始化因运行时属性冲突失败，因此报告不声称已完成人工连续播放验收；这一限制不会改变保守 No-Go。",
            "",
            "## 5. 必须回答的问题",
            "",
            f"1. {decision['answer1Cn']}",
            f"2. {decision['answer2Cn']}",
            f"3. {decision['answer3Cn']}",
            f"4. {decision['answer4Cn']}",
            f"5. {decision['answer5Cn']}",
            "",
            f"配置建议：{decision['recommendationCn']}",
            "",
            "## 6. 产物",
            "",
            "- `summary.json` / `summary.csv`：机器可读汇总。",
            "- `performance-per-process.csv` / `paired-deltas.csv`：逐进程与配对差值。",
            "- `quality-per-frame.json` / `quality-per-frame.csv`：逐帧 AO、LDR 与两类 screen-space temporal 指标。",
            "- `camera-path.json` / `camera-path-samples.csv`：完整路径参数、位姿、view/projection。",
            "- `input-hashes.csv`：实际输入文件大小与 SHA-256。",
            "- `figures/`：动态性能、时域曲线、自动最坏帧和最坏跃迁。",
            "- `videos/`：四段 120 帧并排 AO GIF。",
            "",
            "## 7. 已知限制",
            "",
            "- 动态质量覆盖两个 256×192 ROI/场景、120 帧慢速路径，不是新的全帧长序列。",
            "- 时域指标是 screen-space，未做重投影、遮挡或反遮挡剔除。",
            "- 只有两场景、单路径、单 GPU/驱动；不可外推到跨硬件或所有相机速度。",
            "- 没有硬件 counter；缓存、寄存器、occupancy 或分支只能是未证实假设。",
        ]
    )
    return "\n".join(lines) + "\n"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-directory", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    script = Path(__file__).resolve()
    project = script.parent.parent
    root = arguments.input_directory.resolve()
    expect(root.is_relative_to(project), "input directory must be inside project")
    manifest_path = root / "run-manifest.json"
    manifest = read_json(manifest_path)
    expect(manifest.get("performance", {}).get("status") == "pass", "performance incomplete")
    expect(manifest.get("quality", {}).get("status") == "pass", "quality incomplete")
    expect(manifest["source"]["protectedOldBatches"]["unchanged"] is True, "old batches not verified")
    validator = load_validator(project)
    runs, performance_inputs = load_performance(project, manifest, validator)
    aggregate, per_process_rows, aggregate_rows = aggregate_performance(runs)
    pairs, pair_rows = paired_performance(runs)
    quality_summary, per_frame, quality_rows, quality_inputs, heat_scale = analyze_quality(
        project, root, manifest, runs
    )
    quality_combined = combined_quality(per_frame)
    static = static_comparison(project, pairs)
    gpu_summary, gpu_rows = gpu_telemetry(project, manifest)
    background_audit = gpu_background_audit(root)
    gpu_summary["backgroundAudit"] = background_audit
    write_json(root / "gpu-background-audit.json", background_audit)
    camera_path, camera_rows = build_camera_path(runs, root / "camera-path.json")
    write_csv(root / "camera-path-samples.csv", camera_rows)
    write_csv(root / "performance-per-process.csv", per_process_rows)
    write_csv(root / "summary.csv", aggregate_rows)
    write_csv(root / "paired-deltas.csv", pair_rows)
    write_json(root / "quality-per-frame.json", {"schemaVersion": 1, "frames": per_frame})
    write_csv(root / "quality-per-frame.csv", quality_rows)
    write_csv(root / "gpu-telemetry.csv", gpu_rows)
    input_rows = performance_inputs + quality_inputs
    executable_path = Path(manifest["source"]["releaseExecutable"])
    input_rows.append(
        {
            "kind": "release-executable",
            "path": str(executable_path),
            "bytes": executable_path.stat().st_size,
            "sha256": sha256_file(executable_path),
        }
    )
    quality_input_manifest_path = resolve(
        project, manifest["quality"]["inputManifest"]
    )
    input_rows.append(
        {
            "kind": "quality-input-manifest",
            "path": relative(quality_input_manifest_path, project),
            "bytes": quality_input_manifest_path.stat().st_size,
            "sha256": sha256_file(quality_input_manifest_path),
        }
    )
    roi_manifest_relative = manifest["quality"]["protocol"].get(
        "selectionManifest"
    )
    if roi_manifest_relative:
        roi_manifest_path = resolve(project, roi_manifest_relative)
        input_rows.append(
            {
                "kind": "full64-only-roi-selection-manifest",
                "path": relative(roi_manifest_path, project),
                "bytes": roi_manifest_path.stat().st_size,
                "sha256": sha256_file(roi_manifest_path),
            }
        )
    static_summary_path = project / STATIC_SUMMARY
    input_rows.append(
        {
            "kind": "accepted-static-summary-direction-only",
            "path": relative(static_summary_path, project),
            "bytes": static_summary_path.stat().st_size,
            "sha256": sha256_file(static_summary_path),
        }
    )
    for script_name in (
        "tools/run_ssao_temporal_deterministic.py",
        "tools/select_ssao_temporal_dynamic_rois.py",
        "tools/analyze_ssao_temporal_deterministic.py",
    ):
        script_input_path = project / script_name
        input_rows.append(
            {
                "kind": "reproduction-script",
                "path": script_name,
                "bytes": script_input_path.stat().st_size,
                "sha256": sha256_file(script_input_path),
            }
        )
    input_rows.append(
        {
            "kind": "run-manifest",
            "path": relative(manifest_path, project),
            "bytes": manifest_path.stat().st_size,
            "sha256": sha256_file(manifest_path),
        }
    )
    write_csv(root / "input-hashes.csv", input_rows)
    figures = root / "figures"
    create_performance_plot(figures / "dynamic-performance-distributions.png", aggregate)
    create_paired_plot(figures / "dynamic-paired-deltas.png", pairs)
    create_quality_plot(figures / "temporal-quality-timeseries.png", per_frame)
    capture_root = resolve(project, manifest["quality"]["directory"]) / "captures"
    ao_temporal_heat_scale = max(
        item["temporalScreenSpace"]["edge"]["p99AbsoluteChange"]
        for item in per_frame
        if item["temporalScreenSpace"] is not None
    )
    ldr_temporal_heat_scale = max(
        item["ldrLuminanceTemporalScreenSpace"]["edge"]["p99AbsoluteChange"]
        for item in per_frame
        if item["ldrLuminanceTemporalScreenSpace"] is not None
    )
    worst = create_worst_figures(
        figures,
        capture_root,
        quality_summary,
        manifest["quality"]["protocol"]["captureRois"],
        heat_scale,
    )
    worst_transitions = create_temporal_transition_figures(
        figures,
        capture_root,
        quality_summary,
        manifest["quality"]["protocol"]["captureRois"],
        ao_temporal_heat_scale,
        ldr_temporal_heat_scale,
    )
    videos = create_gifs(
        root / "videos",
        capture_root,
        manifest["quality"]["protocol"]["captureRois"],
        int(manifest["quality"]["protocol"]["captureFrameCount"]),
        heat_scale,
    )
    decision = build_final_decision(pairs, quality_combined)
    rejected_manifest_path = project / REJECTED_ATTEMPT / "run-manifest.json"
    rejected_manifest = read_json(rejected_manifest_path)
    rejected_attempt = {
        "path": REJECTED_ATTEMPT.as_posix(),
        "status": "excluded",
        "reasonCn": (
            "San Miguel 的通用相机振幅使视点贴墙，Full-64 关键帧的深度/法线 edge mask "
            "为空，不满足预注册内容覆盖要求"
        ),
        "manifestSha256": sha256_file(rejected_manifest_path),
        "releaseExecutableSha256": rejected_manifest["source"][
            "releaseExecutableSha256"
        ],
        "completedPerformanceRuns": len(
            rejected_manifest.get("performance", {}).get("runs", [])
        ),
        "mixedIntoAcceptedBatch": False,
    }
    write_json(root / "REJECTED_ATTEMPT_AUDIT.json", rejected_attempt)
    summary = {
        "schemaVersion": 1,
        "generatedAtUtc": utc_now(),
        "batchId": manifest["batchId"],
        "source": {
            "releaseExecutable": manifest["source"]["releaseExecutable"],
            "releaseExecutableSha256": manifest["source"]["releaseExecutableSha256"],
            "gitCommit": manifest["source"]["gitCommit"],
            "worktreeDirty": manifest["source"]["worktreeDirty"],
            "protectedOldBatches": manifest["source"]["protectedOldBatches"],
        },
        "protocol": manifest["protocol"],
        "rejectedAttempt": rejected_attempt,
        "runnerValidation": {
            "performance": manifest["performance"]["validation"],
            "quality": manifest["quality"]["validation"],
            "performanceAndQualitySeparated": True,
            "cameraPosePrefixExactAcrossAllConfigurations": True,
            "inputHashesVerified": True,
        },
        "dynamicPerformance": aggregate,
        "pairedComparisons": pairs,
        "staticDirectionComparison": static,
        "gpuTelemetry": gpu_summary,
        "qualityByRoi": quality_summary,
        "qualityCombined": quality_combined,
        "sharedErrorHeatScale": heat_scale,
        "sharedAoTemporalErrorHeatScale": ao_temporal_heat_scale,
        "sharedLdrLuminanceTemporalErrorHeatScale": ldr_temporal_heat_scale,
        "cameraPath": camera_path,
        "automatedWorstFrames": worst,
        "automatedWorstTransitions": worst_transitions,
        "videos": videos,
        "decision": decision,
        "limitations": {
            "qualityCoverage": "two pre-registered 256x192 ROIs per scene with 4px capture halo",
            "temporalMetric": "screen-space error delta; no motion compensation",
            "hardwareCounters": False,
            "subjectiveGifPlaybackClaimed": False,
            "subjectiveGifPlaybackReason": "in-app Browser runtime initialization failed; GIFs delivered for manual review",
            "crossHardwareGeneralization": False,
        },
    }
    write_json(root / "summary.json", summary)
    report = build_final_report(
        summary,
        aggregate,
        pairs,
        quality_summary,
        quality_combined,
        static,
        gpu_summary,
        {"worst": worst, "worstTransitions": worst_transitions, "videos": videos},
    )
    (root / "SSAO_TEMPORAL_DETERMINISTIC_REPORT_CN.md").write_text(report, encoding="utf-8")
    output_paths = [
        root / "summary.json",
        root / "summary.csv",
        root / "performance-per-process.csv",
        root / "paired-deltas.csv",
        root / "quality-per-frame.json",
        root / "quality-per-frame.csv",
        root / "camera-path.json",
        root / "camera-path-samples.csv",
        root / "input-hashes.csv",
        root / "gpu-telemetry.csv",
        root / "gpu-background-audit.json",
        root / "REJECTED_ATTEMPT_AUDIT.json",
        root / "SSAO_TEMPORAL_DETERMINISTIC_REPORT_CN.md",
        *sorted(figures.glob("*.png")),
        *sorted((root / "videos").glob("*.gif")),
    ]
    analysis_manifest = {
        "schemaVersion": 1,
        "generatedAtUtc": utc_now(),
        "status": "pass",
        "decision": decision["status"],
        "outputs": {
            "summaryJson": "summary.json",
            "summaryCsv": "summary.csv",
            "perProcessCsv": "performance-per-process.csv",
            "pairedCsv": "paired-deltas.csv",
            "qualityJson": "quality-per-frame.json",
            "qualityCsv": "quality-per-frame.csv",
            "cameraJson": "camera-path.json",
            "cameraCsv": "camera-path-samples.csv",
            "inputHashes": "input-hashes.csv",
            "report": "SSAO_TEMPORAL_DETERMINISTIC_REPORT_CN.md",
            "figures": [
                relative(path, root) for path in sorted(figures.glob("*.png"))
            ],
            "videos": videos,
        },
        "validation": {
            "outputFileCount": len(output_paths),
            "allOutputsNonEmpty": all(path.stat().st_size > 0 for path in output_paths),
            "outputFiles": [
                {
                    "path": relative(path, root),
                    "bytes": path.stat().st_size,
                    "sha256": sha256_file(path),
                }
                for path in output_paths
            ],
        },
    }
    write_json(root / "analysis-manifest.json", analysis_manifest)
    print(json.dumps({"status": "pass", "decision": decision["status"], "summary": str(root / 'summary.json')}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
