#!/usr/bin/env python3
"""Analyze the balanced SSAO factorial benchmark without modifying raw inputs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import re
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import numpy as np
from PIL import Image, ImageDraw, ImageFont


SCENES = ("sponza", "san-miguel")
SCENE_LABELS = {"sponza": "Sponza", "san-miguel": "San Miguel"}
CONFIGS = (
    "legacy-full32",
    "legacy-full64",
    "half-raw32",
    "half-raw64",
    "half-bilateral64",
)
CONFIG_LABELS = {
    "legacy-full32": "Full-32",
    "legacy-full64": "Full-64",
    "half-raw32": "Half-32 Raw",
    "half-raw64": "Half-64 Raw",
    "half-bilateral64": "Half-64 Bilateral",
}
CONFIG_COLORS = {
    "legacy-full32": "#2f6bff",
    "legacy-full64": "#222222",
    "half-raw32": "#33a65c",
    "half-raw64": "#f39c12",
    "half-bilateral64": "#d64545",
}
METRICS = (
    "cpuFrame",
    "gpuFrame",
    "deferredGpu",
    "drawCalls",
    "ssaoTotalCpu",
    "ssaoTotalGpu",
    "ssaoGenerateCpu",
    "ssaoGenerateGpu",
    "ssaoUpsampleCpu",
    "ssaoUpsampleGpu",
)
METRIC_LABELS = {
    "cpuFrame": "CPU Frame",
    "gpuFrame": "GPU Frame",
    "deferredGpu": "Deferred Pass GPU",
    "drawCalls": "Draw Calls",
    "ssaoTotalCpu": "SSAO Total CPU",
    "ssaoTotalGpu": "SSAO Total GPU",
    "ssaoGenerateCpu": "SSAO Generate CPU",
    "ssaoGenerateGpu": "SSAO Generate GPU",
    "ssaoUpsampleCpu": "SSAO Upsample CPU",
    "ssaoUpsampleGpu": "SSAO Upsample GPU",
}
METRIC_UNITS = {name: ("count" if name == "drawCalls" else "ms") for name in METRICS}
PAIR_DEFINITIONS = (
    {
        "id": "resolution-32",
        "label": "Resolution @ current 32 config",
        "left": "legacy-full32",
        "right": "half-raw32",
        "factor": "resolution",
    },
    {
        "id": "resolution-64",
        "label": "Resolution @ 64",
        "left": "legacy-full64",
        "right": "half-raw64",
        "factor": "resolution",
    },
    {
        "id": "half-sample-config",
        "label": "Half current 32 config -> 64",
        "left": "half-raw32",
        "right": "half-raw64",
        "factor": "sample-count-and-radial-distribution",
    },
    {
        "id": "bilateral-cost",
        "label": "Bilateral upsample",
        "left": "half-raw64",
        "right": "half-bilateral64",
        "factor": "upsample",
    },
    {
        "id": "final-tier",
        "label": "Full-32 -> Half-64 Bilateral",
        "left": "legacy-full32",
        "right": "half-bilateral64",
        "factor": "end-to-end",
    },
)
PAIR_METRICS = ("gpuFrame", "ssaoTotalGpu", "ssaoGenerateGpu")
BOOTSTRAP_RESAMPLES = 100_000
PMON_COMMAND_PREFIXES = (
    "promecefpluginho",
    "CrossDeviceResum",
    "ShellExperienceH",
    "PhoneExperienceH",
    "StartMenuExperie",
    "TextInputHost.ex",
    "MuMuNxMain.exe",
    "SearchHost.exe",
    "ShellHost.exe",
    "bnscloud.exe",
    "explorer.exe",
    "LockApp.exe",
    "Overlay.e",
    "MAA.exe",
)


def parse_args() -> argparse.Namespace:
    project = Path(__file__).resolve().parents[1]
    default_root = (
        project
        / "benchmark-results"
        / "ssao-factorial"
        / "ssao-factorial-balanced-20260731"
    )
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=default_root)
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def nearest_rank(values: Iterable[float], percentile: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise ValueError("cannot calculate a percentile of an empty sequence")
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def value_stats(values: Iterable[float]) -> dict[str, float | int | None]:
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


def process_distribution(values: Iterable[float]) -> dict[str, float | int | None]:
    data = [float(value) for value in values]
    if not data:
        return {
            "count": 0,
            "mean": None,
            "medianOfProcessValues": None,
            "min": None,
            "max": None,
            "range": None,
            "sampleStdDev": None,
            "cvPercent": None,
        }
    mean_value = statistics.fmean(data)
    sample_stddev = statistics.stdev(data) if len(data) > 1 else 0.0
    return {
        "count": len(data),
        "mean": mean_value,
        "medianOfProcessValues": statistics.median(data),
        "min": min(data),
        "max": max(data),
        "range": max(data) - min(data),
        "sampleStdDev": sample_stddev,
        "cvPercent": (
            sample_stddev / abs(mean_value) * 100.0
            if not math.isclose(mean_value, 0.0)
            else None
        ),
    }


def bootstrap_mean_ci(
    values: list[float], seed_text: str
) -> tuple[float, float]:
    seed = int.from_bytes(
        hashlib.sha256(seed_text.encode("utf-8")).digest()[:8], "little"
    )
    rng = np.random.default_rng(seed)
    source = np.asarray(values, dtype=np.float64)
    indices = rng.integers(
        0, len(source), size=(BOOTSTRAP_RESAMPLES, len(source)), endpoint=False
    )
    means = source[indices].mean(axis=1)
    low, high = np.quantile(means, [0.025, 0.975])
    return float(low), float(high)


def pearson(x_values: Iterable[float], y_values: Iterable[float]) -> float | None:
    x = np.asarray(list(x_values), dtype=np.float64)
    y = np.asarray(list(y_values), dtype=np.float64)
    if len(x) < 2 or np.isclose(np.std(x), 0.0) or np.isclose(np.std(y), 0.0):
        return None
    return float(np.corrcoef(x, y)[0, 1])


def linear_slope(x_values: Iterable[float], y_values: Iterable[float]) -> float | None:
    x = np.asarray(list(x_values), dtype=np.float64)
    y = np.asarray(list(y_values), dtype=np.float64)
    if len(x) < 2 or np.isclose(np.var(x), 0.0):
        return None
    return float(np.polyfit(x, y, 1)[0])


def load_validator(project: Path) -> Any:
    path = project / "tools" / "generate_ssao_half_resolution_report.py"
    spec = importlib.util.spec_from_file_location("ssao_half_report_validator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load validator: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def resolve_project_path(project: Path, recorded: str) -> Path:
    path = Path(recorded)
    return path if path.is_absolute() else project / path


def telemetry_for_run(project: Path, record: dict[str, Any]) -> dict[str, Any]:
    before = load_json(resolve_project_path(project, record["gpuStateBefore"]))
    after = load_json(resolve_project_path(project, record["gpuStateAfter"]))
    if not before.get("available") or not after.get("available"):
        raise ValueError(f"nvidia-smi unavailable for run {record['executionIndex']}")
    before_gpu = before["gpu"]
    after_gpu = after["gpu"]
    fields = {
        "temperatureC": "temperature.gpu",
        "graphicsClockMHz": "clocks.current.graphics",
        "smClockMHz": "clocks.current.sm",
        "memoryClockMHz": "clocks.current.memory",
        "powerW": "power.draw",
        "powerLimitW": "power.limit",
        "utilizationPercent": "utilization.gpu",
    }
    output: dict[str, Any] = {
        "beforeCapturedAtUtc": before["capturedAtUtc"],
        "afterCapturedAtUtc": after["capturedAtUtc"],
        "pstateBefore": before_gpu["pstate"],
        "pstateAfter": after_gpu["pstate"],
    }
    for output_name, input_name in fields.items():
        before_value = float(before_gpu[input_name])
        after_value = float(after_gpu[input_name])
        output[f"{output_name}Before"] = before_value
        output[f"{output_name}After"] = after_value
        output[f"{output_name}Midpoint"] = (before_value + after_value) / 2.0
        output[f"{output_name}Delta"] = after_value - before_value
    return output


def validate_protocol(manifest: dict[str, Any]) -> None:
    protocol = manifest["protocol"]
    validation = manifest["validation"]
    expected = {
        "warmupFrames": 300,
        "measuredFrames": 2000,
        "independentProcesses": 5,
        "resolution": [1920, 1080],
        "requestedSwapInterval": 0,
        "inputFrozen": True,
        "bloom": False,
        "shadows": False,
        "autoReloadShaders": False,
        "autoReloadMaterials": False,
        "ssaoRadius": 0.35,
        "ssaoBias": 0.025,
        "kernelSeed": 1337,
    }
    for name, expected_value in expected.items():
        if protocol.get(name) != expected_value:
            raise ValueError(
                f"protocol mismatch for {name}: {protocol.get(name)!r} != "
                f"{expected_value!r}"
            )
    if tuple(protocol["scenes"]) != SCENES:
        raise ValueError("scene list mismatch")
    if tuple(item["name"] for item in protocol["configurations"]) != CONFIGS:
        raise ValueError("configuration list mismatch")
    required_validation = (
        "allExitCodesZero",
        "allResultsValidated",
        "requiredQueryCountsExact",
        "nestedTimerBoundsVerified",
        "fixedStateVerified",
        "executableHashStable",
    )
    if validation.get("status") != "pass" or not all(
        validation.get(name) is True for name in required_validation
    ):
        raise ValueError("runner validation did not pass")
    if int(validation["runCount"]) != 50:
        raise ValueError("expected exactly 50 formal runs")


def load_runs(
    project: Path, root: Path, manifest: dict[str, Any], validator: Any
) -> list[dict[str, Any]]:
    configurations = {
        item["name"]: item for item in manifest["protocol"]["configurations"]
    }
    measured_frames = int(manifest["protocol"]["measuredFrames"])
    executable_hash = manifest["source"]["releaseExecutableSha256"].lower()
    run_values: list[dict[str, Any]] = []
    seen: set[tuple[str, str, int]] = set()

    for record in manifest["runs"]:
        scene = record["scene"]
        config = record["configuration"]
        block = int(record["block"])
        identity = (scene, config, block)
        if identity in seen:
            raise ValueError(f"duplicate run identity: {identity}")
        seen.add(identity)
        if scene not in SCENES or config not in CONFIGS or not (1 <= block <= 5):
            raise ValueError(f"unexpected run identity: {identity}")
        if record["executableSha256"].lower() != executable_hash:
            raise ValueError(f"executable hash mismatch: {identity}")

        result_path = resolve_project_path(project, record["result"])
        actual_hash = sha256_file(result_path)
        if actual_hash.lower() != record["resultSha256"].lower():
            raise ValueError(f"raw result hash mismatch: {result_path}")
        result = load_json(result_path)
        configuration = configurations[config]
        metrics = validator.extract_metrics(
            result,
            measured_frames,
            bool(configuration["bilateral"]),
            f"{scene}/{config}/block-{block}",
        )
        metric_summaries = {
            name: value_stats(values) for name, values in metrics.items()
        }
        run_values.append(
            {
                "record": record,
                "scene": scene,
                "configuration": config,
                "block": block,
                "position": int(record["position"]),
                "executionIndex": int(record["executionIndex"]),
                "resultPath": str(result_path),
                "resultSha256": actual_hash,
                "metrics": metrics,
                "metricSummaries": metric_summaries,
                "telemetry": telemetry_for_run(project, record),
            }
        )

    expected = {
        (scene, configuration, block)
        for scene in SCENES
        for configuration in CONFIGS
        for block in range(1, 6)
    }
    if seen != expected:
        missing = sorted(expected - seen)
        extra = sorted(seen - expected)
        raise ValueError(f"incomplete matrix; missing={missing}, extra={extra}")
    return sorted(run_values, key=lambda item: item["executionIndex"])


def validate_position_balance(runs: list[dict[str, Any]]) -> dict[str, Any]:
    counts: dict[str, dict[str, dict[int, int]]] = {}
    valid = True
    for scene in SCENES:
        counts[scene] = {}
        for config in CONFIGS:
            config_counts = {
                position: sum(
                    1
                    for run in runs
                    if run["scene"] == scene
                    and run["configuration"] == config
                    and run["position"] == position
                )
                for position in range(1, 6)
            }
            counts[scene][config] = config_counts
            valid = valid and all(value == 1 for value in config_counts.values())
    if not valid:
        raise ValueError("the execution order is not position-balanced")
    return {
        "valid": True,
        "rule": "Each configuration occupies each within-scene position exactly once.",
        "counts": counts,
    }


def build_aggregate(
    runs: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    aggregate: dict[str, Any] = {}
    per_process_rows: list[dict[str, Any]] = []
    aggregate_rows: list[dict[str, Any]] = []

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
                    "resultPath": run["resultPath"],
                    "resultSha256": run["resultSha256"],
                }
            )

    for scene in SCENES:
        aggregate[scene] = {}
        for config in CONFIGS:
            group = [
                run
                for run in runs
                if run["scene"] == scene and run["configuration"] == config
            ]
            group.sort(key=lambda item: item["block"])
            aggregate[scene][config] = {
                "processCount": len(group),
                "blocks": [run["block"] for run in group],
                "positions": [run["position"] for run in group],
                "executionIndices": [run["executionIndex"] for run in group],
                "metrics": {},
            }
            for metric in METRICS:
                pooled = [
                    value for run in group for value in run["metrics"][metric]
                ]
                per_process = {
                    statistic: [
                        run["metricSummaries"][metric][statistic] for run in group
                    ]
                    for statistic in ("median", "p95", "p99")
                }
                entry = {
                    "unit": METRIC_UNITS[metric],
                    "pooledFrameStatistics": value_stats(pooled),
                    "processMedianDistribution": process_distribution(
                        value
                        for value in per_process["median"]
                        if value is not None
                    ),
                    "processP95Distribution": process_distribution(
                        value for value in per_process["p95"] if value is not None
                    ),
                    "processP99Distribution": process_distribution(
                        value for value in per_process["p99"] if value is not None
                    ),
                    "perProcess": [
                        {
                            "block": run["block"],
                            "position": run["position"],
                            "executionIndex": run["executionIndex"],
                            "count": run["metricSummaries"][metric]["count"],
                            "median": run["metricSummaries"][metric]["median"],
                            "p95": run["metricSummaries"][metric]["p95"],
                            "p99": run["metricSummaries"][metric]["p99"],
                        }
                        for run in group
                    ],
                }
                aggregate[scene][config]["metrics"][metric] = entry
                aggregate_rows.append(
                    {
                        "scene": scene,
                        "configuration": config,
                        "metric": metric,
                        "unit": METRIC_UNITS[metric],
                        "processCount": len(group),
                        "pooledFrameCount": entry["pooledFrameStatistics"]["count"],
                        "pooledMedian": entry["pooledFrameStatistics"]["median"],
                        "pooledP95": entry["pooledFrameStatistics"]["p95"],
                        "pooledP99": entry["pooledFrameStatistics"]["p99"],
                        "medianOfProcessMedians": entry[
                            "processMedianDistribution"
                        ]["medianOfProcessValues"],
                        "processMedianMin": entry["processMedianDistribution"]["min"],
                        "processMedianMax": entry["processMedianDistribution"]["max"],
                        "processMedianCvPercent": entry[
                            "processMedianDistribution"
                        ]["cvPercent"],
                        "medianOfProcessP95": entry["processP95Distribution"][
                            "medianOfProcessValues"
                        ],
                        "processP95Min": entry["processP95Distribution"]["min"],
                        "processP95Max": entry["processP95Distribution"]["max"],
                        "medianOfProcessP99": entry["processP99Distribution"][
                            "medianOfProcessValues"
                        ],
                        "processP99Min": entry["processP99Distribution"]["min"],
                        "processP99Max": entry["processP99Distribution"]["max"],
                    }
                )
    return aggregate, per_process_rows, aggregate_rows


def metric_process_median(
    runs: list[dict[str, Any]],
    scene: str,
    configuration: str,
    block: int,
    metric: str,
) -> float:
    matches = [
        run
        for run in runs
        if run["scene"] == scene
        and run["configuration"] == configuration
        and run["block"] == block
    ]
    if len(matches) != 1:
        raise ValueError(
            f"expected one run for {scene}/{configuration}/block-{block}"
        )
    value = matches[0]["metricSummaries"][metric]["median"]
    if value is None:
        raise ValueError(f"missing metric {metric}")
    return float(value)


def build_pairs(
    runs: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    output: dict[str, Any] = {}
    rows: list[dict[str, Any]] = []
    for scene in SCENES:
        output[scene] = {}
        for definition in PAIR_DEFINITIONS:
            pair_id = definition["id"]
            output[scene][pair_id] = {
                "label": definition["label"],
                "factor": definition["factor"],
                "left": definition["left"],
                "right": definition["right"],
                "deltaDefinition": "right minus left; negative means the right side is faster",
                "metrics": {},
            }
            for metric in PAIR_METRICS:
                paired_values = []
                for block in range(1, 6):
                    left_value = metric_process_median(
                        runs, scene, definition["left"], block, metric
                    )
                    right_value = metric_process_median(
                        runs, scene, definition["right"], block, metric
                    )
                    paired_values.append(
                        {
                            "block": block,
                            "left": left_value,
                            "right": right_value,
                            "delta": right_value - left_value,
                            "relativePercent": (
                                (right_value - left_value) / left_value * 100.0
                            ),
                        }
                    )
                deltas = [item["delta"] for item in paired_values]
                relatives = [item["relativePercent"] for item in paired_values]
                left_values = [item["left"] for item in paired_values]
                right_values = [item["right"] for item in paired_values]
                ci_low, ci_high = bootstrap_mean_ci(
                    deltas, f"{scene}/{pair_id}/{metric}"
                )
                positive_count = sum(value > 0.0 for value in deltas)
                negative_count = sum(value < 0.0 for value in deltas)
                zero_count = len(deltas) - positive_count - negative_count
                if positive_count == len(deltas):
                    direction = "right-slower-all-blocks"
                elif negative_count == len(deltas):
                    direction = "right-faster-all-blocks"
                else:
                    direction = "mixed"
                intervals_overlap = not (
                    max(left_values) < min(right_values)
                    or max(right_values) < min(left_values)
                )
                entry = {
                    "unit": "ms",
                    "independentProcessPairs": len(deltas),
                    "pairedValues": paired_values,
                    "meanDelta": statistics.fmean(deltas),
                    "medianDelta": statistics.median(deltas),
                    "minDelta": min(deltas),
                    "maxDelta": max(deltas),
                    "sampleStdDevDelta": statistics.stdev(deltas),
                    "meanRelativePercent": statistics.fmean(relatives),
                    "medianRelativePercent": statistics.median(relatives),
                    "bootstrap95CiMeanDelta": {
                        "low": ci_low,
                        "high": ci_high,
                        "method": (
                            f"percentile bootstrap of paired-process mean, "
                            f"{BOOTSTRAP_RESAMPLES} resamples, deterministic seed"
                        ),
                        "n": len(deltas),
                    },
                    "direction": direction,
                    "positiveCount": positive_count,
                    "negativeCount": negative_count,
                    "zeroCount": zero_count,
                    "processMedianIntervalsOverlap": intervals_overlap,
                    "leftProcessMedianRange": [min(left_values), max(left_values)],
                    "rightProcessMedianRange": [min(right_values), max(right_values)],
                }
                output[scene][pair_id]["metrics"][metric] = entry
                for item in paired_values:
                    rows.append(
                        {
                            "scene": scene,
                            "comparison": pair_id,
                            "factor": definition["factor"],
                            "leftConfiguration": definition["left"],
                            "rightConfiguration": definition["right"],
                            "metric": metric,
                            **item,
                            "meanDelta": entry["meanDelta"],
                            "bootstrap95CiLow": ci_low,
                            "bootstrap95CiHigh": ci_high,
                            "direction": direction,
                            "processMedianIntervalsOverlap": intervals_overlap,
                        }
                    )
    return output, rows


def build_tail_comparisons(
    aggregate: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    output: dict[str, Any] = {}
    rows: list[dict[str, Any]] = []
    for scene in SCENES:
        output[scene] = {}
        for statistic in ("p95", "p99"):
            left = sorted(
                aggregate_metric(
                    aggregate, scene, "legacy-full32", "ssaoTotalGpu"
                )["perProcess"],
                key=lambda item: item["block"],
            )
            right = sorted(
                aggregate_metric(
                    aggregate, scene, "half-bilateral64", "ssaoTotalGpu"
                )["perProcess"],
                key=lambda item: item["block"],
            )
            paired_values = []
            for left_item, right_item in zip(left, right):
                if left_item["block"] != right_item["block"]:
                    raise ValueError("tail comparison block mismatch")
                delta = float(right_item[statistic]) - float(left_item[statistic])
                paired_values.append(
                    {
                        "block": left_item["block"],
                        "left": left_item[statistic],
                        "right": right_item[statistic],
                        "delta": delta,
                    }
                )
                rows.append(
                    {
                        "scene": scene,
                        "comparison": "final-tier",
                        "leftConfiguration": "legacy-full32",
                        "rightConfiguration": "half-bilateral64",
                        "metric": "ssaoTotalGpu",
                        "processStatistic": statistic,
                        "block": left_item["block"],
                        "left": left_item[statistic],
                        "right": right_item[statistic],
                        "delta": delta,
                    }
                )
            deltas = [item["delta"] for item in paired_values]
            output[scene][statistic] = {
                "deltaDefinition": "Half-64 Bilateral minus Full-32",
                "independentProcessPairs": len(deltas),
                "pairedValues": paired_values,
                "meanDelta": statistics.fmean(deltas),
                "medianDelta": statistics.median(deltas),
                "minDelta": min(deltas),
                "maxDelta": max(deltas),
                "negativeCount": sum(value < 0.0 for value in deltas),
                "positiveCount": sum(value > 0.0 for value in deltas),
                "zeroCount": sum(value == 0.0 for value in deltas),
                "note": (
                    "Tail statistics describe each 2000-frame process; the "
                    "independent n remains five paired processes."
                ),
            }
    return output, rows


def build_telemetry(
    runs: list[dict[str, Any]]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    rows: list[dict[str, Any]] = []
    for run in runs:
        row = {
            "executionIndex": run["executionIndex"],
            "block": run["block"],
            "scene": run["scene"],
            "configuration": run["configuration"],
            "position": run["position"],
            **run["telemetry"],
            "ssaoTotalGpuProcessMedian": run["metricSummaries"]["ssaoTotalGpu"][
                "median"
            ],
            "ssaoGenerateGpuProcessMedian": run["metricSummaries"][
                "ssaoGenerateGpu"
            ]["median"],
            "gpuFrameProcessMedian": run["metricSummaries"]["gpuFrame"]["median"],
        }
        rows.append(row)

    numeric_fields = (
        "temperatureCBefore",
        "temperatureCAfter",
        "temperatureCMidpoint",
        "graphicsClockMHzBefore",
        "graphicsClockMHzAfter",
        "graphicsClockMHzMidpoint",
        "powerWBefore",
        "powerWAfter",
        "powerWMidpoint",
        "utilizationPercentBefore",
        "utilizationPercentAfter",
    )
    ranges = {
        field: {
            "min": min(float(row[field]) for row in rows),
            "max": max(float(row[field]) for row in rows),
            "mean": statistics.fmean(float(row[field]) for row in rows),
        }
        for field in numeric_fields
    }

    group_means: dict[tuple[str, str], dict[str, float]] = {}
    for scene in SCENES:
        for config in CONFIGS:
            group = [
                row
                for row in rows
                if row["scene"] == scene and row["configuration"] == config
            ]
            group_means[(scene, config)] = {
                metric: statistics.fmean(float(row[metric]) for row in group)
                for metric in (
                    "ssaoTotalGpuProcessMedian",
                    "ssaoGenerateGpuProcessMedian",
                    "gpuFrameProcessMedian",
                )
            }

    for row in rows:
        means = group_means[(row["scene"], row["configuration"])]
        for metric, mean_value in means.items():
            residual_name = metric.replace("ProcessMedian", "RelativeResidualPercent")
            row[residual_name] = (
                (float(row[metric]) - mean_value) / mean_value * 100.0
            )

    residual_correlations: dict[str, dict[str, float | None]] = {}
    for residual_name in (
        "ssaoTotalGpuRelativeResidualPercent",
        "ssaoGenerateGpuRelativeResidualPercent",
        "gpuFrameRelativeResidualPercent",
    ):
        residual_correlations[residual_name] = {}
        for predictor in (
            "executionIndex",
            "position",
            "temperatureCMidpoint",
            "graphicsClockMHzMidpoint",
            "powerWMidpoint",
        ):
            residual_correlations[residual_name][predictor] = pearson(
                (float(row[predictor]) for row in rows),
                (float(row[residual_name]) for row in rows),
            )

    position_residuals: dict[str, dict[int, float]] = {}
    for residual_name in (
        "ssaoTotalGpuRelativeResidualPercent",
        "ssaoGenerateGpuRelativeResidualPercent",
        "gpuFrameRelativeResidualPercent",
    ):
        position_residuals[residual_name] = {
            position: statistics.fmean(
                float(row[residual_name])
                for row in rows
                if row["position"] == position
            )
            for position in range(1, 6)
        }

    chronological = {
        field: {
            "slopePerExecution": linear_slope(
                (row["executionIndex"] for row in rows),
                (row[field] for row in rows),
            ),
            "pearsonR": pearson(
                (row["executionIndex"] for row in rows),
                (row[field] for row in rows),
            ),
        }
        for field in (
            "temperatureCMidpoint",
            "graphicsClockMHzMidpoint",
            "powerWMidpoint",
        )
    }
    summary = {
        "source": "nvidia-smi snapshot immediately before and after every process",
        "limitations": (
            "Snapshots bracket each process but are not a continuous in-frame "
            "trace; workload-dependent temperature/power cycling is observable."
        ),
        "ranges": ranges,
        "chronologicalTrend": chronological,
        "withinSceneConfigurationRelativeResidualCorrelations": residual_correlations,
        "meanRelativeResidualByBalancedPositionPercent": position_residuals,
        "allPstatesBefore": sorted({row["pstateBefore"] for row in rows}),
        "allPstatesAfter": sorted({row["pstateAfter"] for row in rows}),
    }
    return summary, rows


def build_block_gpu_audit(root: Path) -> dict[str, Any]:
    files = sorted((root / "gpu-state").glob("block-*.json"))
    if len(files) != 10:
        raise ValueError(f"expected 10 block-boundary GPU snapshots, got {len(files)}")
    records = []
    unique_commands: set[str] = set()
    all_commands_succeeded = True
    active_rows_total = 0
    active_snapshots: list[dict[str, Any]] = []
    thermal_slowdown_active_files: list[str] = []
    for path in files:
        document = load_json(path)
        sections = document.get("sections", [])
        pmon_sections = [
            section
            for section in sections
            if isinstance(section, dict) and "pmon" in section.get("command", [])
        ]
        query_sections = [
            section
            for section in sections
            if isinstance(section, dict) and "-q" in section.get("command", [])
        ]
        if len(pmon_sections) != 1 or len(query_sections) != 1:
            raise ValueError(f"missing nvidia-smi block sections: {path}")
        pmon = pmon_sections[0]
        query = query_sections[0]
        all_commands_succeeded = all_commands_succeeded and (
            int(pmon["exitCode"]) == 0 and int(query["exitCode"]) == 0
        )
        process_rows = []
        for line in str(pmon["stdout"]).splitlines():
            tokens = line.split()
            if len(tokens) < 12 or tokens[0] != "0":
                continue
            raw_command = tokens[-1].replace("\x03", "")
            command_match = re.match(r"[A-Za-z0-9_.-]+", raw_command)
            command = command_match.group(0) if command_match else "<unparsed>"
            command = next(
                (
                    prefix
                    for prefix in PMON_COMMAND_PREFIXES
                    if command.startswith(prefix)
                ),
                command,
            )
            unique_commands.add(command)
            utilization_tokens = tokens[3:9]
            numeric_utilization = []
            for token in utilization_tokens:
                try:
                    numeric_utilization.append(float(token))
                except ValueError:
                    continue
            active = any(value > 0.0 for value in numeric_utilization)
            active_rows_total += int(active)
            if active:
                active_snapshots.append(
                    {
                        "snapshot": path.name,
                        "capturedAtUtc": document.get("capturedAtUtc"),
                        "pid": int(tokens[1]),
                        "command": command,
                        "smPercent": (
                            float(tokens[3]) if tokens[3] != "-" else None
                        ),
                        "memoryPercent": (
                            float(tokens[4]) if tokens[4] != "-" else None
                        ),
                    }
                )
            process_rows.append(
                {
                    "pid": int(tokens[1]),
                    "type": tokens[2],
                    "command": command,
                    "reportedUtilizationTokens": utilization_tokens,
                    "reportedActive": active,
                }
            )
        query_text = str(query["stdout"])
        thermal_active = any(
            marker in query_text
            for marker in (
                "HW Thermal Slowdown                        : Active",
                "SW Thermal Slowdown                        : Active",
            )
        )
        if thermal_active:
            thermal_slowdown_active_files.append(path.name)
        records.append(
            {
                "path": str(path),
                "capturedAtUtc": document.get("capturedAtUtc"),
                "reportedProcessRows": len(process_rows),
                "reportedActiveRows": sum(
                    int(item["reportedActive"]) for item in process_rows
                ),
                "thermalSlowdownActive": thermal_active,
                "processes": process_rows,
            }
        )
    return {
        "snapshotCount": len(records),
        "allNvidiaSmiCommandsSucceeded": all_commands_succeeded,
        "uniquePmonCommandLabels": sorted(unique_commands),
        "reportedActiveRowsTotal": active_rows_total,
        "activeSnapshots": active_snapshots,
        "thermalSlowdownActiveFiles": thermal_slowdown_active_files,
        "records": records,
        "interpretation": (
            "Desktop and user GPU processes were present. The one-sample WDDM "
            "pmon snapshots reported no numeric utilization for those rows, "
            "which does not prove an exclusive or interference-free GPU."
        ),
    }


def build_causal_decomposition(pairs: dict[str, Any]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for scene in SCENES:
        resolution = pairs[scene]["resolution-32"]["metrics"][
            "ssaoGenerateGpu"
        ]["meanDelta"]
        sample_config = pairs[scene]["half-sample-config"]["metrics"][
            "ssaoGenerateGpu"
        ]["meanDelta"]
        half_raw64_vs_full32 = resolution + sample_config
        bilateral_generate_noise = pairs[scene]["bilateral-cost"]["metrics"][
            "ssaoGenerateGpu"
        ]["meanDelta"]
        final_generate = pairs[scene]["final-tier"]["metrics"][
            "ssaoGenerateGpu"
        ]["meanDelta"]
        output[scene] = {
            "identity": (
                "(Half-32 Raw - Full-32) + (Half-64 Raw - Half-32 Raw) "
                "= Half-64 Raw - Full-32"
            ),
            "resolutionAtCurrent32ConfigMs": resolution,
            "half32ToHalf64CurrentConfigMs": sample_config,
            "halfRaw64VsFull32Ms": half_raw64_vs_full32,
            "sameGeneratePathRunNoiseHalfBilateralVsHalfRawMs": (
                bilateral_generate_noise
            ),
            "halfBilateral64VsFull32GenerateMs": final_generate,
            "algebraCheckErrorMs": (
                resolution
                + sample_config
                + bilateral_generate_noise
                - final_generate
            ),
            "interpretation": (
                "sample-config term exceeds resolution saving"
                if half_raw64_vs_full32 > 0.0
                else "resolution saving exceeds sample-config term"
            ),
            "causalBoundary": (
                "The 32-to-64 term is a measured configuration effect, not a "
                "pure sample-count effect, because the deterministic 32-vector "
                "kernel is the prefix of a kernel scaled with i/64."
            ),
        }
    return output


def build_extension_decision(
    pairs: dict[str, Any], telemetry: dict[str, Any]
) -> dict[str, Any]:
    key_checks = []
    for scene in SCENES:
        for metric in ("gpuFrame", "ssaoTotalGpu", "ssaoGenerateGpu"):
            entry = pairs[scene]["final-tier"]["metrics"][metric]
            key_checks.append(
                {
                    "scene": scene,
                    "metric": metric,
                    "direction": entry["direction"],
                    "intervalsOverlap": entry[
                        "processMedianIntervalsOverlap"
                    ],
                    "bootstrapCiExcludesZero": (
                        entry["bootstrap95CiMeanDelta"]["low"] > 0.0
                        or entry["bootstrap95CiMeanDelta"]["high"] < 0.0
                    ),
                }
            )
    directions_consistent = all(
        check["direction"] != "mixed" for check in key_checks
    )
    intervals_separated = all(not check["intervalsOverlap"] for check in key_checks)
    ci_excludes_zero = all(
        check["bootstrapCiExcludesZero"] for check in key_checks
    )
    decision = (
        "stop-at-5-processes"
        if directions_consistent and intervals_separated and ci_excludes_zero
        else "extend-to-10-processes"
    )
    return {
        "decision": decision,
        "keyChecks": key_checks,
        "reason": (
            "All key Full-32 vs Half-64 Bilateral GPU Frame, SSAO total, and "
            "Generate paired deltas keep one direction in all five balanced "
            "blocks, their "
            "process-median ranges do not overlap, and the n=5 paired bootstrap "
            "intervals exclude zero. Temperature and power cycle by workload, "
            "but the balanced order prevents a configuration from owning one "
            "position and the measured directions survive those cycles."
        ),
        "telemetryBoundary": telemetry["limitations"],
        "notClaimed": (
            "This is not a claim of thermal invariance or broad population "
            "significance; n=5 is the independent sample size."
        ),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        raise ValueError(f"cannot write empty CSV: {path}")
    fields: list[str] = []
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = (
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
        Path("C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"),
    )
    for path in candidates:
        if path.is_file():
            return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def hex_color(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")
    return tuple(int(value[index : index + 2], 16) for index in (0, 2, 4))


def draw_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    font: ImageFont.ImageFont,
    fill: str | tuple[int, int, int] = "#222222",
    anchor: str | None = None,
) -> None:
    draw.text(xy, text, font=font, fill=fill, anchor=anchor)


def map_value(value: float, low: float, high: float, y0: float, y1: float) -> float:
    if math.isclose(high, low):
        return (y0 + y1) / 2.0
    return y1 - (value - low) / (high - low) * (y1 - y0)


def draw_axes(
    draw: ImageDraw.ImageDraw,
    bounds: tuple[int, int, int, int],
    low: float,
    high: float,
    title: str,
    y_label: str = "ms",
    zero: bool = False,
) -> None:
    left, top, right, bottom = bounds
    axis = "#444444"
    grid = "#d9d9d9"
    draw.line((left, top, left, bottom), fill=axis, width=2)
    draw.line((left, bottom, right, bottom), fill=axis, width=2)
    font = load_font(19)
    small = load_font(16)
    draw_text(draw, ((left + right) / 2, top - 28), title, font, anchor="mm")
    for index in range(5):
        value = low + (high - low) * index / 4.0
        y = map_value(value, low, high, top, bottom)
        draw.line((left, y, right, y), fill=grid, width=1)
        draw_text(draw, (left - 9, y), f"{value:.3f}", small, anchor="rm")
    draw_text(draw, (left - 55, top - 8), y_label, small, anchor="mm")
    if zero and low < 0.0 < high:
        y = map_value(0.0, low, high, top, bottom)
        draw.line((left, y, right, y), fill="#111111", width=3)


def create_distribution_plot(root: Path, aggregate: dict[str, Any]) -> Path:
    width, height = 1900, 1120
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    title_font = load_font(34, bold=True)
    label_font = load_font(18)
    draw_text(
        draw,
        (width / 2, 42),
        "SSAO GPU per-process medians (n=5 independent processes)",
        title_font,
        anchor="mm",
    )
    panels = (
        ("sponza", "ssaoTotalGpu", (115, 115, 935, 520)),
        ("sponza", "ssaoGenerateGpu", (1065, 115, 1885, 520)),
        ("san-miguel", "ssaoTotalGpu", (115, 655, 935, 1060)),
        ("san-miguel", "ssaoGenerateGpu", (1065, 655, 1885, 1060)),
    )
    for scene, metric, bounds in panels:
        all_values = []
        for config in CONFIGS:
            all_values.extend(
                item["median"]
                for item in aggregate[scene][config]["metrics"][metric]["perProcess"]
            )
        low = min(all_values)
        high = max(all_values)
        padding = max((high - low) * 0.12, 0.01)
        low = max(0.0, low - padding)
        high += padding
        draw_axes(
            draw,
            bounds,
            low,
            high,
            f"{SCENE_LABELS[scene]} - {METRIC_LABELS[metric]}",
        )
        left, top, right, bottom = bounds
        step = (right - left) / len(CONFIGS)
        for config_index, config in enumerate(CONFIGS):
            x = left + step * (config_index + 0.5)
            values = [
                float(item["median"])
                for item in aggregate[scene][config]["metrics"][metric]["perProcess"]
            ]
            color = hex_color(CONFIG_COLORS[config])
            y_values = [map_value(value, low, high, top, bottom) for value in values]
            draw.line((x, min(y_values), x, max(y_values)), fill=color, width=4)
            for point_index, y in enumerate(y_values):
                offset = (point_index - 2) * 5
                draw.ellipse(
                    (x + offset - 5, y - 5, x + offset + 5, y + 5),
                    fill=color,
                    outline="white",
                    width=1,
                )
            median_y = map_value(
                statistics.median(values), low, high, top, bottom
            )
            draw.line(
                (x - 22, median_y, x + 22, median_y), fill="#111111", width=3
            )
            draw_text(
                draw,
                (x, bottom + 22),
                CONFIG_LABELS[config].replace(" ", "\n"),
                label_font,
                anchor="ma",
            )
    path = root / "figures" / "per-process-distributions.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return path


def create_paired_delta_plot(root: Path, pairs: dict[str, Any]) -> Path:
    width, height = 2200, 980
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    title_font = load_font(34, bold=True)
    small = load_font(16)
    draw_text(
        draw,
        (width / 2, 40),
        "Paired SSAO Total GPU deltas by balanced block (right - left)",
        title_font,
        anchor="mm",
    )
    panel_width = 390
    for scene_index, scene in enumerate(SCENES):
        for pair_index, definition in enumerate(PAIR_DEFINITIONS):
            left = 80 + pair_index * 425
            top = 120 + scene_index * 430
            bounds = (left, top, left + panel_width, top + 300)
            entry = pairs[scene][definition["id"]]["metrics"]["ssaoTotalGpu"]
            values = [item["delta"] for item in entry["pairedValues"]]
            low = min(min(values), 0.0)
            high = max(max(values), 0.0)
            padding = max((high - low) * 0.20, 0.003)
            low -= padding
            high += padding
            draw_axes(
                draw,
                bounds,
                low,
                high,
                f"{SCENE_LABELS[scene]} | {definition['id']}",
                zero=True,
            )
            panel_left, panel_top, panel_right, panel_bottom = bounds
            color = "#d64545" if entry["meanDelta"] > 0.0 else "#2f6bff"
            for block, value in enumerate(values, start=1):
                x = panel_left + 45 + (block - 1) * 70
                y = map_value(value, low, high, panel_top, panel_bottom)
                draw.ellipse(
                    (x - 7, y - 7, x + 7, y + 7),
                    fill=color,
                    outline="white",
                    width=1,
                )
                draw_text(draw, (x, panel_bottom + 17), str(block), small, anchor="ma")
            mean_y = map_value(
                float(entry["meanDelta"]), low, high, panel_top, panel_bottom
            )
            draw.line(
                (panel_left + 25, mean_y, panel_right - 25, mean_y),
                fill=color,
                width=4,
            )
            draw_text(
                draw,
                ((panel_left + panel_right) / 2, panel_bottom + 57),
                f"mean {entry['meanDelta']:+.4f} ms",
                small,
                anchor="ma",
            )
    path = root / "figures" / "paired-deltas.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return path


def create_telemetry_plot(
    root: Path, telemetry_rows: list[dict[str, Any]]
) -> Path:
    width, height = 1900, 1100
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    title_font = load_font(34, bold=True)
    small = load_font(16)
    draw_text(
        draw,
        (width / 2, 42),
        "Run-order telemetry and within-config SSAO residual",
        title_font,
        anchor="mm",
    )
    panels = (
        (
            "temperatureCMidpoint",
            "GPU temperature midpoint",
            "C",
            (120, 120, 1830, 390),
            "#d64545",
        ),
        (
            "graphicsClockMHzMidpoint",
            "Graphics clock midpoint",
            "MHz",
            (120, 485, 1830, 755),
            "#2f6bff",
        ),
        (
            "ssaoTotalGpuRelativeResidualPercent",
            "SSAO Total GPU residual after scene/config mean removal",
            "%",
            (120, 850, 1830, 1050),
            "#33a65c",
        ),
    )
    for field, title, unit, bounds, color in panels:
        values = [float(row[field]) for row in telemetry_rows]
        low = min(values)
        high = max(values)
        if field.endswith("ResidualPercent"):
            bound = max(abs(low), abs(high), 0.01)
            low, high = -bound * 1.15, bound * 1.15
        else:
            padding = max((high - low) * 0.12, 1.0)
            low -= padding
            high += padding
        draw_axes(draw, bounds, low, high, title, unit, zero=True)
        left, top, right, bottom = bounds
        points = []
        for row in telemetry_rows:
            x = left + (float(row["executionIndex"]) - 1.0) / 49.0 * (
                right - left
            )
            y = map_value(float(row[field]), low, high, top, bottom)
            points.append((x, y))
        draw.line(points, fill=color, width=3)
        for index, (x, y) in enumerate(points):
            if index % 5 == 0:
                draw.ellipse(
                    (x - 4, y - 4, x + 4, y + 4),
                    fill=color,
                    outline="white",
                )
        for execution_index in range(1, 51, 5):
            x = left + (execution_index - 1.0) / 49.0 * (right - left)
            draw_text(draw, (x, bottom + 14), str(execution_index), small, anchor="ma")
    path = root / "figures" / "telemetry-drift.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return path


def create_factor_plot(root: Path, decomposition: dict[str, Any]) -> Path:
    width, height = 1500, 820
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    title_font = load_font(32, bold=True)
    label_font = load_font(18)
    draw_text(
        draw,
        (width / 2, 44),
        "Generate delta decomposition: Half-64 Raw versus Full-32",
        title_font,
        anchor="mm",
    )
    panels = (
        ("sponza", (120, 145, 720, 690)),
        ("san-miguel", (880, 145, 1480, 690)),
    )
    for scene, bounds in panels:
        entry = decomposition[scene]
        values = [
            float(entry["resolutionAtCurrent32ConfigMs"]),
            float(entry["half32ToHalf64CurrentConfigMs"]),
            float(entry["halfRaw64VsFull32Ms"]),
        ]
        bound = max(abs(value) for value in values) * 1.25
        low, high = -bound, bound
        draw_axes(
            draw,
            bounds,
            low,
            high,
            f"{SCENE_LABELS[scene]} (process-paired means)",
            zero=True,
        )
        left, top, right, bottom = bounds
        labels = ("Resolution\nFull->Half", "32 config->64\nat Half", "Net\nHalf64-Full32")
        colors = ("#2f6bff", "#f39c12", "#d64545" if values[2] > 0 else "#33a65c")
        step = (right - left) / 3.0
        zero_y = map_value(0.0, low, high, top, bottom)
        for index, (label, value, color) in enumerate(zip(labels, values, colors)):
            x = left + step * (index + 0.5)
            y = map_value(value, low, high, top, bottom)
            draw.rectangle(
                (x - 48, min(y, zero_y), x + 48, max(y, zero_y)),
                fill=color,
            )
            draw_text(
                draw,
                (x, y - 10 if value >= 0.0 else y + 10),
                f"{value:+.4f}",
                label_font,
                anchor="mb" if value >= 0.0 else "ma",
            )
            draw_text(draw, (x, bottom + 22), label, label_font, anchor="ma")
    path = root / "figures" / "factor-decomposition.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    return path


def fmt(value: Any, digits: int = 3, signed: bool = False) -> str:
    if value is None:
        return "N/A"
    format_text = f"{{:{'+' if signed else ''}.{digits}f}}"
    return format_text.format(float(value))


def aggregate_metric(
    aggregate: dict[str, Any], scene: str, config: str, metric: str
) -> dict[str, Any]:
    return aggregate[scene][config]["metrics"][metric]


def report_configuration_table(aggregate: dict[str, Any]) -> list[str]:
    lines = [
        "| 场景 | 配置 | GPU Frame MoM | Deferred GPU MoM | "
        "SSAO Total GPU MoM / pooled P95 / P99 | Generate GPU MoM | "
        "Upsample GPU MoM / pooled P95 / P99 | Draw Call MoM |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for scene in SCENES:
        for config in CONFIGS:
            gpu_frame = aggregate_metric(aggregate, scene, config, "gpuFrame")
            deferred = aggregate_metric(aggregate, scene, config, "deferredGpu")
            total = aggregate_metric(aggregate, scene, config, "ssaoTotalGpu")
            generate = aggregate_metric(
                aggregate, scene, config, "ssaoGenerateGpu"
            )
            upsample = aggregate_metric(
                aggregate, scene, config, "ssaoUpsampleGpu"
            )
            draws = aggregate_metric(aggregate, scene, config, "drawCalls")
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{fmt(gpu_frame['processMedianDistribution']['medianOfProcessValues'])} | "
                f"{fmt(deferred['processMedianDistribution']['medianOfProcessValues'])} | "
                f"{fmt(total['processMedianDistribution']['medianOfProcessValues'])} / "
                f"{fmt(total['pooledFrameStatistics']['p95'])} / "
                f"{fmt(total['pooledFrameStatistics']['p99'])} | "
                f"{fmt(generate['processMedianDistribution']['medianOfProcessValues'])} | "
                f"{fmt(upsample['processMedianDistribution']['medianOfProcessValues'])} / "
                f"{fmt(upsample['pooledFrameStatistics']['p95'])} / "
                f"{fmt(upsample['pooledFrameStatistics']['p99'])} | "
                f"{fmt(draws['processMedianDistribution']['medianOfProcessValues'], 0)} |"
            )
    return lines


def report_dispersion_table(aggregate: dict[str, Any]) -> list[str]:
    lines = [
        "| 场景 | 配置 | SSAO Total GPU process median min–max | CV | "
        "process P95 min–max | process P99 min–max |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for scene in SCENES:
        for config in CONFIGS:
            total = aggregate_metric(aggregate, scene, config, "ssaoTotalGpu")
            medians = total["processMedianDistribution"]
            p95 = total["processP95Distribution"]
            p99 = total["processP99Distribution"]
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{fmt(medians['min'], 6)}–{fmt(medians['max'], 6)} ms | "
                f"{fmt(medians['cvPercent'], 3)}% | "
                f"{fmt(p95['min'], 6)}–{fmt(p95['max'], 6)} ms | "
                f"{fmt(p99['min'], 6)}–{fmt(p99['max'], 6)} ms |"
            )
    return lines


def report_cpu_table(aggregate: dict[str, Any]) -> list[str]:
    lines = [
        "| 场景 | 配置 | CPU Frame MoM | SSAO Total CPU MoM / P95 / P99 | "
        "Generate CPU MoM | Upsample CPU MoM |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for scene in SCENES:
        for config in CONFIGS:
            frame = aggregate_metric(aggregate, scene, config, "cpuFrame")
            total = aggregate_metric(aggregate, scene, config, "ssaoTotalCpu")
            generate = aggregate_metric(
                aggregate, scene, config, "ssaoGenerateCpu"
            )
            upsample = aggregate_metric(
                aggregate, scene, config, "ssaoUpsampleCpu"
            )
            lines.append(
                f"| {SCENE_LABELS[scene]} | {CONFIG_LABELS[config]} | "
                f"{fmt(frame['processMedianDistribution']['medianOfProcessValues'])} | "
                f"{fmt(total['processMedianDistribution']['medianOfProcessValues'])} / "
                f"{fmt(total['pooledFrameStatistics']['p95'])} / "
                f"{fmt(total['pooledFrameStatistics']['p99'])} | "
                f"{fmt(generate['processMedianDistribution']['medianOfProcessValues'])} | "
                f"{fmt(upsample['processMedianDistribution']['medianOfProcessValues'])} |"
            )
    return lines


def report_pair_table(pairs: dict[str, Any], metric: str) -> list[str]:
    lines = [
        f"| 场景 | 对比（右－左） | {METRIC_LABELS[metric]} mean Δ | "
        "相对左侧 | bootstrap 95% CI | 方向 | 进程区间 |",
        "|---|---|---:|---:|---:|---|---|",
    ]
    for scene in SCENES:
        for definition in PAIR_DEFINITIONS:
            entry = pairs[scene][definition["id"]]["metrics"][metric]
            ci = entry["bootstrap95CiMeanDelta"]
            lines.append(
                f"| {SCENE_LABELS[scene]} | "
                f"{CONFIG_LABELS[definition['right']]} − "
                f"{CONFIG_LABELS[definition['left']]} | "
                f"{fmt(entry['meanDelta'], 6, True)} ms | "
                f"{fmt(entry['meanRelativePercent'], 2, True)}% | "
                f"[{fmt(ci['low'], 6, True)}, {fmt(ci['high'], 6, True)}] | "
                f"{entry['negativeCount']} faster / {entry['positiveCount']} slower | "
                f"{'重叠' if entry['processMedianIntervalsOverlap'] else '不重叠'} |"
            )
    return lines


def build_report(
    root: Path,
    manifest: dict[str, Any],
    aggregate: dict[str, Any],
    pairs: dict[str, Any],
    telemetry: dict[str, Any],
    decomposition: dict[str, Any],
    tail_comparisons: dict[str, Any],
    block_gpu_audit: dict[str, Any],
    extension: dict[str, Any],
    figures: list[Path],
) -> str:
    exe_hash = manifest["source"]["releaseExecutableSha256"].upper()
    temp_range = telemetry["ranges"]["temperatureCBefore"]
    temp_after_range = telemetry["ranges"]["temperatureCAfter"]
    clock_after = telemetry["ranges"]["graphicsClockMHzAfter"]
    s_final = pairs["sponza"]["final-tier"]["metrics"]
    m_final = pairs["san-miguel"]["final-tier"]["metrics"]
    s_decomp = decomposition["sponza"]
    m_decomp = decomposition["san-miguel"]

    s_up = aggregate_metric(
        aggregate, "sponza", "half-bilateral64", "ssaoUpsampleGpu"
    )
    m_up = aggregate_metric(
        aggregate, "san-miguel", "half-bilateral64", "ssaoUpsampleGpu"
    )
    s_full64_gen = aggregate_metric(
        aggregate, "sponza", "legacy-full64", "ssaoGenerateGpu"
    )["processMedianDistribution"]["medianOfProcessValues"]
    s_half64_gen = aggregate_metric(
        aggregate, "sponza", "half-raw64", "ssaoGenerateGpu"
    )["processMedianDistribution"]["medianOfProcessValues"]
    m_full64_gen = aggregate_metric(
        aggregate, "san-miguel", "legacy-full64", "ssaoGenerateGpu"
    )["processMedianDistribution"]["medianOfProcessValues"]
    m_half64_gen = aggregate_metric(
        aggregate, "san-miguel", "half-raw64", "ssaoGenerateGpu"
    )["processMedianDistribution"]["medianOfProcessValues"]
    s_up_median = s_up["processMedianDistribution"]["medianOfProcessValues"]
    m_up_median = m_up["processMedianDistribution"]["medianOfProcessValues"]
    s_up_share = s_up_median / (s_full64_gen - s_half64_gen) * 100.0
    m_up_share = m_up_median / (m_full64_gen - m_half64_gen) * 100.0
    s_tail = tail_comparisons["sponza"]
    m_tail = tail_comparisons["san-miguel"]

    lines = [
        "# SSAO 五配置平衡重复实验：抗噪声与因果拆分",
        "",
        "## 结论先行",
        "",
        f"- **Sponza 的反向差异仍稳定存在。** Half-64 Bilateral 相对 Full-32 "
        f"的 Generate 为 `{fmt(s_final['ssaoGenerateGpu']['meanDelta'], 6, True)} ms`，"
        f"SSAO Total 为 `{fmt(s_final['ssaoTotalGpu']['meanDelta'], 6, True)} ms`；"
        "两个差值在 5/5 个平衡 block 中均为 Half-64 较慢，进程 median 区间不重叠。"
        "上一轮约 `0.05 / 0.13 ms` 的现象在本批次收敛为约 "
        f"`{fmt(s_final['ssaoGenerateGpu']['meanDelta'], 3, True)} / "
        f"{fmt(s_final['ssaoTotalGpu']['meanDelta'], 3, True)} ms`，没有被执行顺序消除。",
        f"  对应 GPU Frame 配对差值为 "
        f"`{fmt(s_final['gpuFrame']['meanDelta'], 6, True)} ms`"
        f"（`{fmt(s_final['gpuFrame']['meanRelativePercent'], 2, True)}%`），"
        f"同样是 {s_final['gpuFrame']['positiveCount']}/5 个 block 中 Half-64 较慢。",
        f"- **San Miguel 的小优势方向稳定，但实际量级仍小。** Half-64 "
        f"Bilateral 的 SSAO Total 相对 Full-32 为 "
        f"`{fmt(m_final['ssaoTotalGpu']['meanDelta'], 6, True)} ms`，5/5 个 block "
        "均更快且进程 median 区间不重叠；它约占 Full-32 SSAO Total 的 "
        f"`{fmt(abs(m_final['ssaoTotalGpu']['meanRelativePercent']), 2)}%`，"
        f"相对 GPU Frame 的绝对占比约 "
        f"`{fmt(abs(m_final['ssaoTotalGpu']['meanDelta']) / aggregate_metric(aggregate, 'san-miguel', 'legacy-full32', 'gpuFrame')['processMedianDistribution']['medianOfProcessValues'] * 100.0, 2)}%`。"
        f"实际 GPU Frame 配对差值为 "
        f"`{fmt(m_final['gpuFrame']['meanDelta'], 6, True)} ms`"
        f"（`{fmt(m_final['gpuFrame']['meanRelativePercent'], 2, True)}%`），"
        f"{m_final['gpuFrame']['negativeCount']}/5 个 block 同向。"
        "因此可称为“稳定的小 median 优势”，不能包装成有体感的普适帧率提升。",
        "- **矩阵给出的边界是场景交互，而不是已证实的微架构根因。** "
        f"Sponza 中分辨率项 `{fmt(s_decomp['resolutionAtCurrent32ConfigMs'], 6, True)} ms` "
        f"被当前 32→64 配置项 `{fmt(s_decomp['half32ToHalf64CurrentConfigMs'], 6, True)} ms` "
        f"反超，Half-64 Raw 最终比 Full-32 Generate 慢 "
        f"`{fmt(s_decomp['halfRaw64VsFull32Ms'], 6, True)} ms`；San Miguel 中对应两项为 "
        f"`{fmt(m_decomp['resolutionAtCurrent32ConfigMs'], 6, True)} / "
        f"{fmt(m_decomp['half32ToHalf64CurrentConfigMs'], 6, True)} ms`，净值 "
        f"`{fmt(m_decomp['halfRaw64VsFull32Ms'], 6, True)} ms`，所以 Half-64 Raw "
        "更快。没有硬件 counter，缓存、寄存器压力、占用率或分支只能列为未证实假设。",
        f"- **Bilateral Upsample 的独立 median 为 Sponza `{fmt(s_up_median)} ms`、"
        f"San Miguel `{fmt(m_up_median)} ms`。** 它分别只消耗 Full-64→Half-64 "
        f"Generate median 节省的 `{fmt(s_up_share, 1)}%` 与 `{fmt(m_up_share, 1)}%`，"
        "没有吃掉相对 Full-64 的主体收益；但它会把 Sponza 中 Half-64 Raw 相对 "
        "Full-32 已经不利的差距继续扩大。尾部并不平坦，详见第 5 节。",
        "- **Go/No-Go：** 对“Full-64 参考路径降本且保住静态边缘质量”的优化案例仍为 "
        "**Go**；对“Half-64 Bilateral 在所有场景都比 Full-32 更快、是唯一 Medium "
        "赢家”的表述为 **No-Go**。运行时档位仍是场景相关 trade-off。",
        "",
        "## 1. 协议与可追溯性",
        "",
        "- Release x64，1920×1080，VSync Off，固定 Sponza / San Miguel 相机；"
        "Bloom、阴影、自动 Shader/Material 热重载和输入关闭。",
        "- 现有 CLI 已能运行 `half-raw --samples 32`；先用 "
        "`smoke/sponza-half-raw32.json` 验证 960×540 R16F Generate、120/120 "
        "完整 Query 后才进入正式矩阵，因此本轮没有修改 SSAO shader、RenderPass "
        "或其他引擎算法。",
        "- 每次独立进程先预热 300 帧，再测量 2000 帧；2 场景 × 5 配置 × "
        "5 进程，共 50 个进程和 100,000 个 measured frames。",
        "- 使用 5×5 Williams-pattern 平衡拉丁方并交替场景顺序；每个配置在每个"
        "场景内的执行位置 1–5 各出现恰好一次。统计独立单位是每进程 median；"
        "pooled P95/P99 只描述 10,000 帧分布，不冒充 10,000 个独立实验。",
        f"- GPU：`{manifest['system']['initialGpuState']['gpu']['name']}`，驱动 "
        f"`{manifest['system']['initialGpuState']['gpu']['driver_version']}`；"
        f"新批次唯一 EXE SHA-256：`{exe_hash}`。构建前后的旧正式批次没有参与混池。",
        f"- 源码基点 `{manifest['source']['gitCommit']}`，dirty worktree；检查点在 "
        f"`{manifest['source']['checkpoint']}`。本轮没有 reset/clean/commit。",
        "- `run-manifest.json` 记录的 50 个退出码均为 0；原始 JSON SHA-256 "
        "逐项复核通过；同一 EXE 哈希在全部进程前后保持不变。",
        "",
        "执行顺序见 [`execution-order.csv`](execution-order.csv)，逐进程数据见 "
        "[`per-process.csv`](per-process.csv)，完整机器可读汇总见 "
        "[`summary.json`](summary.json)。",
        "",
        "## 2. Query 完整性与计时边界",
        "",
        "- 正式数据仅使用 OpenGL `GL_TIMESTAMP` Timer Query；没有使用 RenderDoc duration。",
        "- 每个进程 CPU Frame、GPU Frame、Deferred GPU、SSAO Total/Generate CPU/GPU "
        "均严格 2000 个样本；仅 Half-64 Bilateral 的 Upsample CPU/GPU 为 2000，"
        "其余路径严格为 0，这是按设计缺省，不是丢样。",
        "- `SSAO Pass` 包含 `SSAO Generate`，Bilateral 模式再包含 `SSAO Upsample`；"
        "逐帧验证 Generate ≤ Total、Generate + Upsample ≤ Total、SSAO Total ≤ "
        "Deferred ≤ GPU Frame。嵌套使用 timestamp pair，不是非法嵌套 elapsed query。",
        "- Query 采用 16-slot 延迟读取；测量结束后的 `glFinish` 只用于冲刷捕获尾部，"
        "发生在 measured frames 之后。严格拒绝缺失 query，因此没有把延迟未解析样本"
        "静默计入本批次。",
        "",
        "## 3. GPU 主结果",
        "",
        "表中 `MoM` 是 5 个每进程 median 的中位数；P95/P99 是 10,000 帧 pooled "
        "nearest-rank 值。CPU 与 GPU 不相加。",
        "",
        *report_configuration_table(aggregate),
        "",
        "![每进程分布](figures/per-process-distributions.png)",
        "",
        "以下是以独立进程为单位的 SSAO Total GPU 离散程度；逐进程 Median/P95/P99 "
        "原值在 `per-process.csv`，没有用 pooled 帧数代替独立样本数。",
        "",
        *report_dispersion_table(aggregate),
        "",
        "## 4. 配对差值与因果拆分",
        "",
        "差值定义为右侧配置减左侧配置；负数表示右侧更快。bootstrap CI 是对 "
        f"`n=5` 个配对进程 median 的均值做 `{BOOTSTRAP_RESAMPLES:,}` 次确定性"
        "百分位重采样，不能解释成大样本总体置信保证。",
        "",
        *report_pair_table(pairs, "ssaoTotalGpu"),
        "",
        "![配对差值](figures/paired-deltas.png)",
        "",
        "Generate 的 Full-32→Half-64 Raw 拆分恒等式是：",
        "",
        "`(Half-32 Raw − Full-32) + (Half-64 Raw − Half-32 Raw) = "
        "Half-64 Raw − Full-32`。",
        "",
        f"- Sponza：`{fmt(s_decomp['resolutionAtCurrent32ConfigMs'], 6, True)} + "
        f"{fmt(s_decomp['half32ToHalf64CurrentConfigMs'], 6, True)} = "
        f"{fmt(s_decomp['halfRaw64VsFull32Ms'], 6, True)} ms`。",
        f"- San Miguel：`{fmt(m_decomp['resolutionAtCurrent32ConfigMs'], 6, True)} + "
        f"{fmt(m_decomp['half32ToHalf64CurrentConfigMs'], 6, True)} = "
        f"{fmt(m_decomp['halfRaw64VsFull32Ms'], 6, True)} ms`。",
        "",
        "这里的“32→64 配置项”不是纯样本数效应：32 样本取固定 64 核序列前缀，"
        "而径向尺度按 `i/64` 生成，因而样本数量与径向分布同时改变。它只能作为"
        "实际配置差异。Full-64 vs Half-64 Raw 才是在固定 64 样本/radius/bias/seed "
        "下的纯分辨率 A/B。",
        "",
        "![因果拆分](figures/factor-decomposition.png)",
        "",
        "## 5. Bilateral Upsample 的独立成本与尾部",
        "",
        "| 场景 | MoM median | pooled P95 | pooled P99 | process P95 range | "
        "process P99 range | 占 Full64→Half64 Generate median 节省 |",
        "|---|---:|---:|---:|---:|---:|---:|",
        f"| Sponza | {fmt(s_up_median)} ms | "
        f"{fmt(s_up['pooledFrameStatistics']['p95'])} ms | "
        f"{fmt(s_up['pooledFrameStatistics']['p99'])} ms | "
        f"{fmt(s_up['processP95Distribution']['min'])}–"
        f"{fmt(s_up['processP95Distribution']['max'])} ms | "
        f"{fmt(s_up['processP99Distribution']['min'])}–"
        f"{fmt(s_up['processP99Distribution']['max'])} ms | "
        f"{fmt(s_up_share, 1)}% |",
        f"| San Miguel | {fmt(m_up_median)} ms | "
        f"{fmt(m_up['pooledFrameStatistics']['p95'])} ms | "
        f"{fmt(m_up['pooledFrameStatistics']['p99'])} ms | "
        f"{fmt(m_up['processP95Distribution']['min'])}–"
        f"{fmt(m_up['processP95Distribution']['max'])} ms | "
        f"{fmt(m_up['processP99Distribution']['min'])}–"
        f"{fmt(m_up['processP99Distribution']['max'])} ms | "
        f"{fmt(m_up_share, 1)}% |",
        "",
        "Upsample median 很低，但 P95/P99 显著高于 median，说明全屏上采样存在可见"
        "尾部。它没有吃掉相对 Full-64 的 median Generate 节省；对 Full-32 的"
        "端到端竞争则不同：Sponza 的 Generate 已落后，Upsample 继续增加总成本；"
        "San Miguel 的 Generate 优势足够覆盖 Upsample 后仍留下约 0.020 ms 的 "
        "SSAO Total median 优势。不能用 median 掩盖尾部：相对 Full-32，Sponza "
        f"的每进程 SSAO Total P95 配对差值均值为 "
        f"`{fmt(s_tail['p95']['meanDelta'], 6, True)} ms`、P99 为 "
        f"`{fmt(s_tail['p99']['meanDelta'], 6, True)} ms`；San Miguel 的 P95 为 "
        f"`{fmt(m_tail['p95']['meanDelta'], 6, True)} ms`，P99 均值虽为 "
        f"`{fmt(m_tail['p99']['meanDelta'], 6, True)} ms`，但仅 "
        f"{m_tail['p99']['negativeCount']}/5 block 显示 Half-64 较快。"
        "因此 San Miguel 的尾部应判为"
        "基本持平，而不是随 median 一起稳定获益。完整 per-process P95/P99 已保留。",
        "",
        "## 6. CPU 结果（与 GPU 分开）",
        "",
        *report_cpu_table(aggregate),
        "",
        "CPU Frame 是提交墙钟，可能包含 GPU back-pressure；它不等于 SSAO CPU "
        "工作，也不能与 GPU 时间相加。`SSAO Total CPU` / `Generate CPU` / "
        "`Upsample CPU` 才是对应 CPU zone。",
        "",
        "## 7. GPU Boost、温度与执行位置审计",
        "",
        f"- 每进程前/后 `nvidia-smi` 快照均成功。温度 before 范围 "
        f"`{fmt(temp_range['min'], 0)}–{fmt(temp_range['max'], 0)}°C`，after 范围 "
        f"`{fmt(temp_after_range['min'], 0)}–{fmt(temp_after_range['max'], 0)}°C`；"
        f"after 核心频率 `{fmt(clock_after['min'], 0)}–{fmt(clock_after['max'], 0)} MHz`。",
        "- 首进程前是冷启动低频快照，进入测量后频率提升；温度/功耗随后按 Full/Half "
        "工作量循环并叠加缓慢漂移。温度 midpoint 对执行序号的斜率为 "
        f"`{fmt(telemetry['chronologicalTrend']['temperatureCMidpoint']['slopePerExecution'], 3, True)} °C/run`"
        f"（r=`{fmt(telemetry['chronologicalTrend']['temperatureCMidpoint']['pearsonR'], 3, True)}`）；"
        "快照不是连续硬件 trace，因此不能声称"
        "锁频或恒温。",
        "- 拉丁方保证每个配置各占一次位置 1–5；去除 scene/config 均值后的 SSAO "
        "relative residual、温度、频率和执行序号相关性已写入 `summary.json`。"
        f"SSAO Total residual 与执行序号 r=`"
        f"{fmt(telemetry['withinSceneConfigurationRelativeResidualCorrelations']['ssaoTotalGpuRelativeResidualPercent']['executionIndex'], 3, True)}`，"
        f"与温度 midpoint r=`"
        f"{fmt(telemetry['withinSceneConfigurationRelativeResidualCorrelations']['ssaoTotalGpuRelativeResidualPercent']['temperatureCMidpoint'], 3, True)}`；"
        "相关不为零，但关键配对差值在这种循环下仍 5/5 同向，区间不重叠。"
        "平衡位置的 SSAO Total 平均 residual 最大绝对值仅 "
        f"`{fmt(max(abs(value) for value in telemetry['meanRelativeResidualByBalancedPositionPercent']['ssaoTotalGpuRelativeResidualPercent'].values()), 3)}%`。",
        "- 块边界 `nvidia-smi pmon` 共 10 次，均成功。可见桌面/用户 GPU 进程，"
        "包括 NVIDIA Overlay、WPS 组件、Windows Shell、bnscloud、MAA 和 MuMu；"
        f"这些单次 WDDM 快照的 numeric active row 为 "
        f"`{block_gpu_audit['reportedActiveRowsTotal']}`：`bnscloud.exe` 在 "
        "`block-02-before` 报告 2% SM / 1% memory，在 `block-03-after` 报告 "
        "19% SM / 8% memory；未记录 thermal slowdown。其余快照没有 numeric "
        "active row，但这不能证明 GPU 独占或完全无后台干扰；原始 "
        "`-q`/`pmon` 输出保留在 `gpu-state/block-*.json`。",
        f"- 扩展判断：`{extension['decision']}`。没有追加到 10 次，因为预先约定的"
        "触发条件（关键方向混合、进程区间明显重叠或方向随状态漂移）均未出现。"
        "该判断覆盖最终档位的 GPU Frame、SSAO Total 与 Generate median；P99 "
        "尾部方向混合已作为性能边界披露，但不会通过无限重跑制造显著性。"
        "`block-03-after` 的后台活动被视为限制：对应 block 的关键 delta "
        "仍在五次范围内且没有翻转，但不能据此声称消除了所有后台噪声。"
        "这不把 n=5 夸大为普适统计结论。",
        "",
        "![遥测与顺序](figures/telemetry-drift.png)",
        "",
        "## 8. 可写简历与不可写边界",
        "",
        "可以写：在 1080p、Sponza 与 San Miguel、固定 64-sample reference 下，"
        "Half-resolution Generate 加 depth/normal-aware bilateral 显著降低 "
        "Full-64 SSAO GPU 成本；本批次 5 个平衡独立进程确认 Full64→Half64 Raw "
        f"Generate 分别节省约 `{fmt(s_full64_gen - s_half64_gen, 3)} / "
        f"{fmt(m_full64_gen - m_half64_gen, 3)} ms`，而 Upsample median 约 "
        f"`{fmt(s_up_median, 3)} / {fmt(m_up_median, 3)} ms`。静态质量边界沿用上一轮"
        "已验收的 float AO/edge crop 证据。",
        "",
        "不能写：Half-64 在所有真实场景都比 Full-32 快；Sponza 已明确反例。不能把 "
        "Full-32/Full-64 当成纯样本数因果。不能把缓存、寄存器压力、warp/occupancy "
        "或分支写成根因，因为本轮没有硬件 counter。不能从静态固定相机扩展到移动"
        "相机的时域稳定性。",
        "",
        "配置建议不变但边界更清楚：性能优先且内容接近 Sponza 时选 Full-32；"
        "San Miguel 中 Half-64 Bilateral 的 median 略优但尾部近似持平，可依据上一轮"
        "静态质量取舍。Full-64 只作为高质量 reference/高档回退，不作为 Medium "
        "性能档。不存在跨场景稳定支配的唯一赢家。",
        "",
        "上一轮静态质量报告："
        "[`SSAO_FULL32_VS_HALF64_REPORT_CN.md`](../../ssao-config-comparison/"
        "full32-vs-half64-bilateral-20260731/SSAO_FULL32_VS_HALF64_REPORT_CN.md)。"
        "本轮没有重捕质量、没有启动 RenderDoc，也没有改动旧 PFM/PPM/报告。",
        "",
        "## 9. 复现命令",
        "",
        "以下命令应指向一个**新的空结果目录**，不要复写本批次。先在 VS 2022 "
        "Developer PowerShell 中构建：",
        "",
        "```powershell",
        "$NewRoot = Join-Path $PWD 'benchmark-results/ssao-factorial/<new-batch-id>'",
        "& 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe' `",
        "  '.\\OpenGL_Learn.vcxproj' /m /t:Build /p:Configuration=Release /p:Platform=x64 `",
        "  /p:WindowsTargetPlatformVersion=10.0.26100.0 /p:VCToolsVersion=14.44.35207 `",
        "  \"/p:OutDir=$NewRoot\\bin\\\" \"/p:IntDir=$NewRoot\\obj\\\"",
        "python .\\tools\\run_ssao_factorial_balanced.py `",
        "  --executable \"$NewRoot\\bin\\OpenGL_Learn.exe\" `",
        "  --output-directory $NewRoot --repeats 5 --warmup-frames 300 --measured-frames 2000",
        "python .\\tools\\analyze_ssao_factorial_balanced.py --root $NewRoot",
        "```",
        "",
        "若预先约定的扩展条件触发，使用全新目录从头执行 `--repeats 10`；不能把 "
        "5 次和追加的不同 EXE/不同协议数据混池。本批次没有触发扩展。",
        "",
        "## 10. 产物",
        "",
        "- 原始数据：`raw/`（50 个正式 JSON，未改写）",
        "- 运行清单：`run-manifest.json`、`execution-order.csv`",
        "- 汇总：`summary.json`、`summary.csv`、`configuration-summary.csv`",
        "- 独立重复：`per-process.csv`、`paired-deltas.csv`",
        "- 尾部配对：`tail-deltas.csv`",
        "- GPU 状态：`gpu-state/`、`gpu-telemetry.csv`",
        "- 后台 GPU 审计：`gpu-background-audit.json`",
        "- 图：`figures/per-process-distributions.png`、"
        "`figures/paired-deltas.png`、`figures/factor-decomposition.png`、"
        "`figures/telemetry-drift.png`",
        "- 构建与源码检查点：`bin/`、`obj/`、`source-checkpoint/`",
        "",
        "已知限制：仅一张 RTX 5060 Ti、一个驱动版本、1080p、两个固定静态场景；"
        "`n=5` 独立进程；没有 GPU hardware counter、锁频控制或连续温度/功耗采样；"
        "32-sample 配置混合样本数和径向分布；本轮不提供移动相机时域证据。",
        "",
    ]
    return "\n".join(lines)


def build_configuration_summary_rows(
    aggregate: dict[str, Any]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for scene in SCENES:
        full32_total = aggregate_metric(
            aggregate, scene, "legacy-full32", "ssaoTotalGpu"
        )["processMedianDistribution"]["medianOfProcessValues"]
        full64_total = aggregate_metric(
            aggregate, scene, "legacy-full64", "ssaoTotalGpu"
        )["processMedianDistribution"]["medianOfProcessValues"]
        for config in CONFIGS:
            row: dict[str, Any] = {
                "scene": scene,
                "configuration": config,
            }
            for metric in METRICS:
                entry = aggregate_metric(aggregate, scene, config, metric)
                prefix = metric
                row[f"{prefix}MedianOfProcessMedians"] = entry[
                    "processMedianDistribution"
                ]["medianOfProcessValues"]
                row[f"{prefix}ProcessMedianMin"] = entry[
                    "processMedianDistribution"
                ]["min"]
                row[f"{prefix}ProcessMedianMax"] = entry[
                    "processMedianDistribution"
                ]["max"]
                row[f"{prefix}ProcessMedianCvPercent"] = entry[
                    "processMedianDistribution"
                ]["cvPercent"]
                row[f"{prefix}PooledP95"] = entry["pooledFrameStatistics"]["p95"]
                row[f"{prefix}PooledP99"] = entry["pooledFrameStatistics"]["p99"]
            current_total = row["ssaoTotalGpuMedianOfProcessMedians"]
            row["ssaoTotalRelativeToFull32Percent"] = (
                current_total / full32_total * 100.0
            )
            row["ssaoTotalRelativeToFull64Percent"] = (
                current_total / full64_total * 100.0
            )
            rows.append(row)
    return rows


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    project = Path(__file__).resolve().parents[1]
    manifest_path = root / "run-manifest.json"
    manifest = load_json(manifest_path)
    validate_protocol(manifest)
    validator = load_validator(project)
    runs = load_runs(project, root, manifest, validator)
    position_balance = validate_position_balance(runs)
    aggregate, per_process_rows, aggregate_rows = build_aggregate(runs)
    pairs, pair_rows = build_pairs(runs)
    telemetry, telemetry_rows = build_telemetry(runs)
    block_gpu_audit = build_block_gpu_audit(root)
    decomposition = build_causal_decomposition(pairs)
    tail_comparisons, tail_rows = build_tail_comparisons(aggregate)
    extension = build_extension_decision(pairs, telemetry)

    executable_path = Path(manifest["source"]["releaseExecutable"])
    executable_current_hash = sha256_file(executable_path)
    if (
        executable_current_hash.lower()
        != manifest["source"]["releaseExecutableSha256"].lower()
    ):
        raise ValueError("current Release executable hash no longer matches the batch")

    figures = [
        create_distribution_plot(root, aggregate),
        create_paired_delta_plot(root, pairs),
        create_telemetry_plot(root, telemetry_rows),
        create_factor_plot(root, decomposition),
    ]

    analysis_manifest = {
        "schemaVersion": 1,
        "generatedAtUtc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "inputManifest": str(manifest_path),
        "inputManifestSha256": sha256_file(manifest_path),
        "analyzer": str(Path(__file__).resolve()),
        "analyzerSha256": sha256_file(Path(__file__).resolve()),
        "rawRunCount": len(runs),
        "measuredFramesPerRun": manifest["protocol"]["measuredFrames"],
        "totalMeasuredFrames": (
            len(runs) * int(manifest["protocol"]["measuredFrames"])
        ),
        "independentUnit": "per-process median",
        "independentProcessCountPerSceneConfiguration": 5,
        "pooledPercentileMethod": "nearest-rank",
        "bootstrap": {
            "unit": "paired per-process medians",
            "independentN": 5,
            "resamples": BOOTSTRAP_RESAMPLES,
            "interval": "percentile 95%",
            "seed": "deterministic SHA-256-derived per scene/comparison/metric",
        },
        "releaseExecutable": manifest["source"]["releaseExecutable"],
        "releaseExecutableSha256": manifest["source"][
            "releaseExecutableSha256"
        ],
        "releaseExecutableSha256Rechecked": executable_current_hash,
        "oldFormalDataMixed": False,
        "renderDocDurationsUsed": False,
        "qualityRecaptured": False,
        "ssaoAlgorithmChanged": False,
        "positionBalance": position_balance,
        "extensionDecision": extension["decision"],
        "outputs": [
            "analysis-manifest.json",
            "summary.json",
            "summary.csv",
            "configuration-summary.csv",
            "per-process.csv",
            "paired-deltas.csv",
            "tail-deltas.csv",
            "gpu-telemetry.csv",
            "gpu-background-audit.json",
            "SSAO_FACTORIAL_BALANCED_REPORT_CN.md",
            *[str(path.relative_to(root)) for path in figures],
        ],
    }
    summary = {
        "schemaVersion": 1,
        "batchId": manifest["batchId"],
        "analysisManifest": "analysis-manifest.json",
        "protocol": manifest["protocol"],
        "source": manifest["source"],
        "runnerValidation": manifest["validation"],
        "positionBalance": position_balance,
        "aggregate": aggregate,
        "pairedComparisons": pairs,
        "causalDecomposition": decomposition,
        "tailComparisons": tail_comparisons,
        "gpuTelemetry": telemetry,
        "gpuBackgroundAudit": block_gpu_audit,
        "extensionDecision": extension,
        "interpretation": {
            "goNoGo": {
                "full64ReferenceOptimization": "GO",
                "universalHalf64VsFull32Claim": "NO-GO",
                "mediumTier": "scene-dependent trade-off",
            },
            "resumeSafe": (
                "At 1920x1080 on RTX 5060 Ti, five balanced independent "
                "processes in each of Sponza and San Miguel confirm that "
                "Half-64 Raw materially reduces Generate GPU time versus "
                "Full-64, while depth/normal-aware bilateral adds about "
                "0.08 ms median. Static quality evidence is from the prior "
                "accepted batch."
            ),
            "notEstablished": (
                "A universal speed advantage over Full-32, a pure sample-count "
                "effect, any cache/register/branch root cause, cross-hardware "
                "generality, or moving-camera temporal stability."
            ),
        },
    }

    write_json(root / "analysis-manifest.json", analysis_manifest)
    write_json(root / "summary.json", summary)
    write_csv(root / "summary.csv", aggregate_rows)
    write_csv(
        root / "configuration-summary.csv",
        build_configuration_summary_rows(aggregate),
    )
    write_csv(root / "per-process.csv", per_process_rows)
    write_csv(root / "paired-deltas.csv", pair_rows)
    write_csv(root / "tail-deltas.csv", tail_rows)
    write_csv(root / "gpu-telemetry.csv", telemetry_rows)
    write_json(root / "gpu-background-audit.json", block_gpu_audit)
    report = build_report(
        root,
        manifest,
        aggregate,
        pairs,
        telemetry,
        decomposition,
        tail_comparisons,
        block_gpu_audit,
        extension,
        figures,
    )
    report_path = root / "SSAO_FACTORIAL_BALANCED_REPORT_CN.md"
    report_path.write_text(report, encoding="utf-8", newline="\n")

    print(f"Analyzed {len(runs)} validated runs.")
    print(f"Extension decision: {extension['decision']}")
    print(f"Report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
