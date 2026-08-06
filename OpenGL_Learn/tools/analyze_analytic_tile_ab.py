#!/usr/bin/env python3
"""Aggregate the frozen Analytic Screen versus Tile S1 experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


ABS_MS = 0.05
REL_PERCENT = 3.0
CELL_ORDER = ("low-boundary", "low-radius", "wide-coverage", "representative", "heavy")
CELL_LABELS = {
    "low-boundary": "N16/R1.5",
    "low-radius": "N64/R1.5",
    "wide-coverage": "N64/R12",
    "representative": "N256/R6",
    "heavy": "N512/R12",
}
METRICS: dict[str, tuple[str, str]] = {
    "wallFrame": ("sample", "wallFrame"),
    "cpuFrame": ("sample", "cpuFrame"),
    "gpuFrame": ("sample", "gpuFrame"),
    "drawCalls": ("sample", "drawCalls"),
    "deferredPointCpu": ("cpu", "Deferred Point Lights"),
    "deferredPointGpu": ("gpu", "Deferred Point Lights"),
    "screenCpu": ("cpu", "Point Light Screen CPU"),
    "screenGpu": ("gpu", "Point Light Screen GPU"),
    "gridCacheCheckCpu": ("cpu", "Point Light Grid Cache Check"),
    "gridBuildCpu": ("cpu", "Point Light Grid Build"),
    "gridUploadCpu": ("cpu", "Point Light Grid Upload"),
    "gridLightingCpu": ("cpu", "Point Light Grid Lighting CPU"),
    "gridLightingGpu": ("gpu", "Point Light Grid Lighting GPU"),
    "gridBoundsCpu": ("cpu", "Point Light Grid Bounds"),
    "gridCountCpu": ("cpu", "Point Light Grid Count"),
    "gridPrefixCpu": ("cpu", "Point Light Grid Prefix"),
    "gridFillCpu": ("cpu", "Point Light Grid Fill"),
}


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


def metric_samples(result: dict[str, Any], metric: str) -> list[float]:
    kind, name = METRICS[metric]
    samples = result["profiler"]["samples"]
    if kind == "sample":
        values = samples.get(name, [])
    else:
        group = samples["cpuZones"] if kind == "cpu" else samples["gpuZones"]
        values = group.get(name, [])
    return [float(value) for value in values]


def statistics(values: Iterable[float]) -> dict[str, float | int]:
    data = np.asarray(list(values), dtype=np.float64)
    if not data.size:
        return {"count": 0, "mean": 0.0, "median": 0.0, "p95": 0.0, "p99": 0.0}
    return {
        "count": int(data.size),
        "mean": float(np.mean(data)),
        "median": float(np.median(data)),
        "p95": float(np.percentile(data, 95)),
        "p99": float(np.percentile(data, 99)),
    }


def median(values: list[float]) -> float:
    return float(np.median(np.asarray(values, dtype=np.float64))) if values else 0.0


def validate_result(
    result: dict[str, Any], expected: dict[str, Any], warmup: int, sample_count: int
) -> None:
    point = result["pointLightStress"]
    grid = point["gridRuntime"]
    motion = result["motionTimeline"]
    summary = result["profiler"]["summary"]
    tile = bool(expected["tile"])
    moving = expected["camera"] == "moving"
    total = warmup + sample_count
    checks: dict[str, bool] = {
        "success": bool(result["success"]),
        "release": result["buildConfiguration"] == "Release",
        "x64": result["architecture"] == "x64",
        "resolution": result["resolution"] == [1920, 1080],
        "warmup": int(result["warmupFrames"]) == warmup,
        "sample_count": int(result["measuredFrames"]) == sample_count,
        "deferred": bool(result["settings"]["deferredRendering"]),
        "mode": point["renderMode"] == expected["renderMode"],
        "mode_explicit": bool(point["renderModeExplicit"]),
        "offscreen_disabled": not bool(point["offscreenCulling"])
        and bool(point["offscreenCullingExplicit"]),
        "count": int(point["generatedLightCount"]) == int(expected["lightCount"]),
        "radius": abs(float(point["volumeRadius"]) - float(expected["radius"])) <= 1e-4,
        "no_point_shadows": not bool(point["pointShadowsEnabled"]),
        "wall_samples": len(metric_samples(result, "wallFrame")) == sample_count,
        "cpu_samples": len(metric_samples(result, "cpuFrame")) == sample_count,
        "gpu_samples": len(metric_samples(result, "gpuFrame")) == sample_count,
        "deferred_point_cpu": len(metric_samples(result, "deferredPointCpu")) == sample_count,
        "deferred_point_gpu": len(metric_samples(result, "deferredPointGpu")) == sample_count,
        "submitted": int(summary["pointLightsSubmitted"]["median"]) == int(expected["lightCount"]),
        "culled": int(summary["pointLightsCulled"]["median"]) == 0,
        "stencil_draws": int(summary["pointLightStencilDraws"]["median"]) == 0,
        "volume_draws": int(summary["pointLightLightingVolumeDraws"]["median"]) == 0,
    }
    if moving:
        checks.update(
            {
                "motion_enabled": bool(motion["enabled"]),
                "motion_profile": motion["profile"] == "camera",
                "motion_rate": int(motion["fixedFramesPerSecond"]) == 60,
                "motion_cycle": int(motion["cycleFrames"]) == 600,
                "motion_samples": len(motion["samples"]) == sample_count,
                "position_amplitude": abs(
                    float(motion["amplitudeRatios"]["cameraPosition"]) - 0.05
                )
                <= 1e-6,
                "target_amplitude": abs(
                    float(motion["amplitudeRatios"]["cameraTarget"]) - 0.01
                )
                <= 1e-6,
            }
        )
    else:
        checks.update(
            {
                "motion_disabled": not bool(motion["enabled"]),
                "motion_profile": motion["profile"] == "none",
                "motion_samples": len(motion["samples"]) == 0,
            }
        )
    if tile:
        expected_builds = total if moving else 1
        expected_hits = 0 if moving else total - 1
        checks.update(
            {
                "grid_mode": point["gridUpdateMode"] == "cached"
                and bool(point["gridUpdateModeExplicit"]),
                "slice": bool(point["gridSliceCountExplicit"])
                and int(point["gridSliceCountConfigured"]) == 1
                and int(grid["sliceCount"]) == 1
                and not bool(grid["clustered"]),
                "grid_valid": bool(grid["valid"])
                and not bool(grid["overflow"])
                and not grid["error"],
                "grid_count": int(grid["lightCount"]) == int(expected["lightCount"]),
                "build_count": int(grid["buildCount"]) == expected_builds,
                "upload_count": int(grid["uploadCount"]) == expected_builds,
                "cache_hits": int(grid["cacheHitCount"]) == expected_hits,
                "screen_draw": int(summary["pointLightScreenDraws"]["median"]) == 1,
                "grid_cpu": len(metric_samples(result, "gridLightingCpu")) == sample_count,
                "grid_gpu": len(metric_samples(result, "gridLightingGpu")) == sample_count,
                "grid_build": len(metric_samples(result, "gridBuildCpu")) == sample_count,
                "grid_upload": len(metric_samples(result, "gridUploadCpu")) == sample_count,
                "grid_cache": len(metric_samples(result, "gridCacheCheckCpu")) == sample_count,
            }
        )
    else:
        checks.update(
            {
                "screen_draw": int(summary["pointLightScreenDraws"]["median"])
                == int(expected["lightCount"]),
                "screen_cpu": len(metric_samples(result, "screenCpu")) == sample_count,
                "screen_gpu": len(metric_samples(result, "screenGpu")) == sample_count,
            }
        )
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise ValueError(f"{expected['stem']} failed semantic validation: {failed}")


def image_quality(oracle: Path, candidate: Path, gate: dict[str, Any]) -> dict[str, Any]:
    a = np.asarray(Image.open(oracle).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(candidate).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        raise ValueError(f"capture shape mismatch: {oracle} vs {candidate}")
    difference = np.abs(a - b)
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    maximum = int(np.max(difference))
    mean = float(np.mean(difference))
    p99 = float(np.percentile(difference, 99))
    return {
        "maxChannelLsb": maximum,
        "meanChannelLsb": mean,
        "p99ChannelLsb": p99,
        "differentPixelCount": int(np.count_nonzero(np.any(difference != 0, axis=2))),
        "psnrDb": 999.0 if mse == 0.0 else float(10.0 * math.log10(255.0**2 / mse)),
        "passed": maximum <= int(gate["maxChannelLsb"])
        and mean <= float(gate["meanChannelLsb"])
        and p99 <= float(gate["p99ChannelLsb"]),
    }


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


def compare_paths(
    analytic: dict[str, Any], tile: dict[str, Any], metric: str = "wallFrame"
) -> dict[str, Any]:
    analytic_by_round = {
        int(item["expected"]["round"]): median(item["metrics"][metric])
        for item in analytic["runs"]
    }
    tile_by_round = {
        int(item["expected"]["round"]): median(item["metrics"][metric])
        for item in tile["runs"]
    }
    pairs = []
    for round_index in sorted(analytic_by_round):
        av = analytic_by_round[round_index]
        tv = tile_by_round[round_index]
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
    direction = "mixed"
    if all(value < 0.0 for value in deltas):
        direction = "tile-faster"
    elif all(value > 0.0 for value in deltas):
        direction = "analytic-faster"
    elif all(value == 0.0 for value in deltas):
        direction = "equal"
    paired_delta = median(deltas)
    paired_relative = median(relatives)
    significant = (
        len(pairs) == 3
        and abs(paired_delta) >= ABS_MS
        and abs(paired_relative) >= REL_PERCENT
        and direction in ("tile-faster", "analytic-faster")
    )
    winner = "tie"
    if significant:
        winner = "tile" if direction == "tile-faster" else "analytic"
    return {
        "winner": winner,
        "direction": direction,
        "significant": significant,
        "medianPairedDeltaMs": paired_delta,
        "medianPairedRelativePercent": paired_relative,
        "pairs": pairs,
    }


def aggregate_group(runs: list[dict[str, Any]]) -> dict[str, Any]:
    ordered = sorted(runs, key=lambda item: int(item["expected"]["round"]))
    aggregated: dict[str, Any] = {"runCount": len(ordered), "metrics": {}, "runs": ordered}
    for metric in METRICS:
        process_medians = [median(item["metrics"][metric]) for item in ordered]
        pooled = [value for item in ordered for value in item["metrics"][metric]]
        aggregated["metrics"][metric] = {
            "pooled": statistics(pooled),
            "processMedians": process_medians,
            "medianOfProcessMedians": median(process_medians),
        }
    first = ordered[0]["result"]
    if bool(ordered[0]["expected"]["tile"]):
        fields = (
            "tileSize",
            "sliceCount",
            "tilesX",
            "tilesY",
            "logicalCells",
            "nonEmptyCells",
            "lightCount",
            "totalIndices",
            "maximumLightsPerCell",
            "averageLightsPerCell",
            "metadataBytes",
            "indexBytes",
            "lightBytes",
            "residentBytes",
            "buildCount",
            "uploadCount",
            "cacheHitCount",
        )
        aggregated["grid"] = {
            field: first["pointLightStress"]["gridRuntime"][field] for field in fields
        }
    return aggregated


def clean_group(group: dict[str, Any]) -> dict[str, Any]:
    result = {key: value for key, value in group.items() if key != "runs"}
    result["runs"] = [
        {
            "stem": item["expected"]["stem"],
            "round": int(item["expected"]["round"]),
            "result": item["expected"]["result"],
            "capture": item["expected"]["capture"],
        }
        for item in group["runs"]
    ]
    return result


def source_paths(project: Path) -> dict[str, Path]:
    tools = project / "tools"
    return {
        "deferredCpp": project / "DeferRenderPass.cpp",
        "deferredHeader": project / "DeferRenderPass.h",
        "gridCpp": project / "PointLightGridRuntime.cpp",
        "gridHeader": project / "PointLightGridRuntime.h",
        "globalHeader": project / "Global.h",
        "stressGenerator": project / "PointLightStressBenchmark.h",
        "motionCpp": project / "BenchmarkMotionTimeline.cpp",
        "motionHeader": project / "BenchmarkMotionTimeline.h",
        "testDriver": project / "test.cpp",
        "screenVertex": project / "shaders" / "lightVolumeFullscreenVertex.glsl",
        "screenFragment": project / "shaders" / "lightVolumeFullscreenFragment.glsl",
        "gridVertex": project / "shaders" / "pointLightGridVertex.glsl",
        "gridFragment": project / "shaders" / "pointLightGridFragment.glsl",
        "orchestrator": tools / "run_analytic_tile_ab.ps1",
        "analyzer": tools / "analyze_analytic_tile_ab.py",
        "verifier": tools / "verify_analytic_tile_ab.py",
    }


def make_charts(run_dir: Path, cells: list[dict[str, Any]]) -> None:
    chart_dir = run_dir / "charts"
    chart_dir.mkdir(parents=True, exist_ok=True)
    labels = [CELL_LABELS[name] for name in CELL_ORDER]
    x = np.arange(len(labels), dtype=np.float64)

    figure, axes = plt.subplots(2, 1, figsize=(10.2, 8.0), constrained_layout=True)
    for axis, camera, title in zip(
        axes, ("static", "moving"), ("Static camera (CSR reusable)", "Moving camera (CSR rebuilt every frame)")
    ):
        analytic = [
            cell["cameras"][camera]["analytic"]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            for cell in cells
        ]
        tile = [
            cell["cameras"][camera]["tile"]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            for cell in cells
        ]
        width = 0.36
        axis.bar(x - width / 2, analytic, width, label="Analytic Screen", color="#4C78A8")
        axis.bar(x + width / 2, tile, width, label="Tile S1", color="#F58518")
        axis.set_title(title)
        axis.set_ylabel("Wall frame median (ms)")
        axis.set_xticks(x, labels)
        axis.grid(axis="y", alpha=0.25)
        axis.legend()
    figure.savefig(chart_dir / "wall-frame-static-moving.png", dpi=180)
    plt.close(figure)

    static_changes = [
        cell["cameras"]["static"]["comparison"]["medianPairedRelativePercent"] for cell in cells
    ]
    moving_changes = [
        cell["cameras"]["moving"]["comparison"]["medianPairedRelativePercent"] for cell in cells
    ]
    figure, axis = plt.subplots(figsize=(10.2, 4.8), constrained_layout=True)
    width = 0.36
    axis.bar(x - width / 2, static_changes, width, label="Static", color="#54A24B")
    axis.bar(x + width / 2, moving_changes, width, label="Moving", color="#E45756")
    axis.axhline(0.0, color="black", linewidth=0.8)
    axis.axhspan(-3.0, 3.0, color="gray", alpha=0.12, label="±3% tie band")
    axis.set_xticks(x, labels)
    axis.set_ylabel("Tile relative to Analytic (%)")
    axis.set_title("Paired wall-frame change (negative means Tile is faster)")
    axis.grid(axis="y", alpha=0.25)
    axis.legend()
    figure.savefig(chart_dir / "paired-relative-change.png", dpi=180)
    plt.close(figure)

    analytic_gpu = [
        cell["cameras"]["moving"]["analytic"]["metrics"]["screenGpu"]["medianOfProcessMedians"]
        for cell in cells
    ]
    tile_gpu = [
        cell["cameras"]["moving"]["tile"]["metrics"]["gridLightingGpu"]["medianOfProcessMedians"]
        for cell in cells
    ]
    tile_build = [
        cell["cameras"]["moving"]["tile"]["metrics"]["gridBuildCpu"]["medianOfProcessMedians"]
        for cell in cells
    ]
    tile_upload = [
        cell["cameras"]["moving"]["tile"]["metrics"]["gridUploadCpu"]["medianOfProcessMedians"]
        for cell in cells
    ]
    figure, axes = plt.subplots(2, 1, figsize=(10.2, 7.6), constrained_layout=True)
    width = 0.36
    axes[0].bar(x - width / 2, analytic_gpu, width, label="Analytic Screen GPU", color="#4C78A8")
    axes[0].bar(x + width / 2, tile_gpu, width, label="Tile Lighting GPU", color="#F58518")
    axes[0].set_ylabel("GPU zone median (ms)")
    axes[0].set_title("Moving camera: lighting GPU work")
    axes[0].set_xticks(x, labels)
    axes[0].grid(axis="y", alpha=0.25)
    axes[0].legend()
    axes[1].bar(x - width / 2, tile_build, width, label="CSR Build CPU", color="#E45756")
    axes[1].bar(x + width / 2, tile_upload, width, label="CSR Upload CPU", color="#72B7B2")
    axes[1].set_ylabel("CPU zone median (ms)")
    axes[1].set_title("Moving camera: Tile preprocessing (do not add directly to GPU time)")
    axes[1].set_xticks(x, labels)
    axes[1].grid(axis="y", alpha=0.25)
    axes[1].legend()
    figure.savefig(chart_dir / "moving-cost-breakdown.png", dpi=180)
    plt.close(figure)

    indices = [cell["cameras"]["static"]["tile"]["grid"]["totalIndices"] / 1e6 for cell in cells]
    memory = [
        cell["cameras"]["static"]["tile"]["grid"]["residentBytes"] / (1024.0**2)
        for cell in cells
    ]
    figure, axis1 = plt.subplots(figsize=(10.2, 4.8), constrained_layout=True)
    axis2 = axis1.twinx()
    axis1.plot(x, indices, "o-", color="#4C78A8", label="CSR indices")
    axis2.plot(x, memory, "s--", color="#E45756", label="Resident memory")
    axis1.set_xticks(x, labels)
    axis1.set_ylabel("Index references (million)", color="#4C78A8")
    axis2.set_ylabel("Resident memory (MiB)", color="#E45756")
    axis1.set_title("Tile S1 CSR pressure")
    axis1.grid(alpha=0.25)
    lines = axis1.get_lines() + axis2.get_lines()
    axis1.legend(lines, [line.get_label() for line in lines], loc="upper left")
    figure.savefig(chart_dir / "csr-pressure.png", dpi=180)
    plt.close(figure)


def write_report(run_dir: Path, aggregate: dict[str, Any]) -> None:
    decision = aggregate["decision"]
    cells = aggregate["cells"]
    quality = aggregate["quality"]
    verification_path = run_dir / "verification" / "independent-verification.json"
    verification = load(verification_path) if verification_path.exists() else None

    verdict_text = {
        "default-go": "Tile S1 默认路径 Go",
        "conditional-go": "Tile S1 仅条件路径 Go，默认路径 No-Go",
        "no-go": "Tile S1 No-Go",
    }[decision["verdict"]]
    lines = [
        "# Analytic Screen 与 Tile S1 正式 A/B 实验报告",
        "",
        "## 结论",
        "",
        f"**{verdict_text}。** 正式实验包含 {aggregate['runCount']} 个独立进程，每进程 "
        f"{aggregate['warmupFrames']} 帧预热 + {aggregate['sampleFrames']} 帧采样。"
        "主判据为同轮配对的 Wall Frame，而不是单独的 Lighting GPU Zone。",
        "",
        f"- 静止相机：Tile 明确获胜 {decision['staticTileWins']}/{len(cells)} 个锚点；"
        f"Analytic 明确获胜 {decision['staticAnalyticWins']}/{len(cells)} 个锚点。",
        f"- 运动相机：Tile 明确获胜 {decision['movingTileWins']}/{len(cells)} 个锚点；"
        f"Analytic 明确获胜 {decision['movingAnalyticWins']}/{len(cells)} 个锚点。",
        f"- 质量：{quality['passedPairs']}/{quality['pairCount']} 组截图通过冻结门槛；"
        f"最坏 Max={quality['worstMaxChannelLsb']} LSB，Mean={quality['worstMeanChannelLsb']:.6f} LSB，"
        f"P99={quality['worstP99ChannelLsb']:.3f} LSB。",
        "",
        decision["explanation"],
        "",
        "## 端到端结果",
        "",
        "下表中的变化均为 `Tile - Analytic`；负数表示 Tile 更快。只有 3/3 方向一致、"
        "绝对差 ≥0.05 ms 且相对差 ≥3% 才判定赢家。",
        "",
        "| 场景 | 相机 | Analytic Wall | Tile Wall | 配对变化 | 判定 |",
        "|---|---|---:|---:|---:|---|",
    ]
    for cell in cells:
        for camera in ("static", "moving"):
            data = cell["cameras"][camera]
            analytic = data["analytic"]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            tile = data["tile"]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            comparison = data["comparison"]
            winner = {"tile": "Tile", "analytic": "Analytic", "tie": "Tie"}[comparison["winner"]]
            lines.append(
                f"| {CELL_LABELS[cell['name']]} | {'静止' if camera == 'static' else '运动'} | "
                f"{analytic:.4f} ms | {tile:.4f} ms | "
                f"{comparison['medianPairedDeltaMs']:+.4f} ms / "
                f"{comparison['medianPairedRelativePercent']:+.2f}% | {winner} |"
            )

    lines += [
        "",
        "## 为什么静止与运动结果会分叉",
        "",
        "Analytic Screen 每盏灯执行一次解析屏幕包围、Scissor 和矩形 Draw。Tile S1 则把所有灯压入一次"
        "全屏 Draw，并让像素只遍历所在 Tile 的候选灯，因此通常能减少 Draw Call 和 Lighting GPU 工作。",
        "",
        "但 Tile 的 CSR 是 View/Projection 相关数据。相机静止且灯不变时，它只在预热阶段构建一次，正式"
        "采样只承担很小的 Cache Check；相机运动时，矩阵每帧变化，CSR 每帧重建并上传。若 Build/Upload"
        "超过 GPU 侧节省，端到端 Wall Frame 就会变慢。CPU 和 GPU Zone 可能并行，报告不把二者简单相加，"
        "最终只用 Wall Frame 做 Go/No-Go。",
        "",
        "## 正确性与边界",
        "",
        "- 两条路径使用相同灯光数据、精确球体影响判定、BRDF/衰减公式和逐灯累加顺序；Tile 的矩形覆盖只"
        "扩大候选集合，不删除真实受光像素。",
        "- Moving A/B 的 600 个采样相机状态逐帧一致；所有 Scene/Submission Signature 一致。",
        "- Static 的一次性 CSR Build 位于预热阶段，因此静态数据表示稳定运行时上限，不表示首次进入场景成本。",
        "- 该结果属于 OpenGL 3.3、CPU 构表 + TBO 上传实现；不能外推为 GPU Compute Tiled/Clustered Shading。",
        "- 未修改默认路径；若要利用静态收益，需要未来实现可靠的 View/Light Revision 策略或 GPU 构表，再重新实验。",
        "",
        "## 证据与复现",
        "",
        "- `aggregate.json`：完整聚合、配对比较与判定。",
        "- `summary.csv`：每组每项指标的 pooled Median/P95/P99。",
        "- `charts/wall-frame-static-moving.png`：静止/运动端到端对照。",
        "- `charts/paired-relative-change.png`：Tile 相对变化。",
        "- `charts/moving-cost-breakdown.png`：GPU 节省与 CPU 构表/上传分解。",
        "- `charts/representative-heavy-moving-analytic.png`、`...-tile.png`、`...-difference-x96.png`：代表性运行截图和放大误差图。",
        "- `pre-capture-manifest.json` 与 `capture-manifest.json`：冻结协议、二进制/源码哈希及 60 个原始进程证据链。",
        "- 复现入口：`tools/run_analytic_tile_ab.ps1`。",
    ]
    if verification:
        lines += [
            "",
            "## 独立复核",
            "",
            f"独立验证脚本结论：{'PASS' if verification['passed'] else 'FAIL'}；"
            f"重算 {verification['comparisonCount']} 个配对比较，校验 {verification['artifactCount']} 个文件哈希，"
            f"相机逐帧配对 {verification['motionPairCount']} 组。",
        ]
    (run_dir / "REPORT_CN.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    project = Path(__file__).resolve().parent.parent

    pre = load(run_dir / "pre-capture-manifest.json")
    manifest = load(run_dir / "capture-manifest.json")
    protocol_path = run_dir / pre["protocol"]
    if sha256(protocol_path) != pre["protocolSha256"]:
        raise ValueError("frozen protocol hash mismatch")
    if sha256(Path(pre["executable"])) != pre["executableSha256"]:
        raise ValueError("frozen executable hash mismatch")
    for name, path in source_paths(project).items():
        if sha256(path) != pre["sourceHashes"][name]:
            raise ValueError(f"frozen source hash mismatch: {name}")
    if not manifest["valid"]:
        raise ValueError("capture manifest is not valid")
    if manifest["protocolSha256"] != pre["protocolSha256"]:
        raise ValueError("manifest protocol chain mismatch")
    if manifest["preCaptureManifestSha256"] != sha256(run_dir / "pre-capture-manifest.json"):
        raise ValueError("pre-capture manifest chain mismatch")

    expected_runs = pre["expectedRuns"]
    if len(expected_runs) != 60 or int(manifest["completedRunCount"]) != len(expected_runs):
        raise ValueError("formal 60-process capture is incomplete")
    completed = {item["stem"]: item for item in manifest["completedRuns"]}
    runs: list[dict[str, Any]] = []
    for expected in expected_runs:
        record = completed.get(expected["stem"])
        if record is None:
            raise ValueError(f"missing capture record: {expected['stem']}")
        result_path = run_dir / expected["result"]
        capture_path = run_dir / expected["capture"]
        log_path = run_dir / expected["log"]
        for path, field in (
            (result_path, "resultSha256"),
            (capture_path, "captureSha256"),
            (log_path, "logSha256"),
        ):
            if sha256(path) != record[field]:
                raise ValueError(f"artifact hash mismatch: {expected['stem']} / {field}")
        result = load(result_path)
        validate_result(result, expected, int(pre["warmupFrames"]), int(pre["sampleFrames"]))
        runs.append(
            {
                "expected": expected,
                "result": result,
                "capture": capture_path,
                "metrics": {metric: metric_samples(result, metric) for metric in METRICS},
            }
        )

    grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    lookup: dict[tuple[str, str, str, int], dict[str, Any]] = {}
    for run in runs:
        expected = run["expected"]
        technology = "tile" if expected["tile"] else "analytic"
        key = (expected["cell"], expected["camera"], technology)
        grouped[key].append(run)
        lookup[(expected["cell"], expected["camera"], technology, int(expected["round"]))] = run

    quality_pairs: list[dict[str, Any]] = []
    cells: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []
    for cell_name in CELL_ORDER:
        cell_runs = [run for run in runs if run["expected"]["cell"] == cell_name]
        signatures = {
            (
                run["result"]["pointLightStress"]["sceneSignature"],
                run["result"]["pointLightStress"]["submissionSignature"],
            )
            for run in cell_runs
        }
        if len(signatures) != 1:
            raise ValueError(f"scene/submission signature drift: {cell_name}")
        first_expected = cell_runs[0]["expected"]
        cell_entry: dict[str, Any] = {
            "name": cell_name,
            "lightCount": int(first_expected["lightCount"]),
            "radius": float(first_expected["radius"]),
            "sceneSignature": next(iter(signatures))[0],
            "submissionSignature": next(iter(signatures))[1],
            "cameras": {},
        }
        for camera in ("static", "moving"):
            analytic = aggregate_group(grouped[(cell_name, camera, "analytic")])
            tile = aggregate_group(grouped[(cell_name, camera, "tile")])
            comparison = compare_paths(analytic, tile)
            cell_entry["cameras"][camera] = {
                "analytic": clean_group(analytic),
                "tile": clean_group(tile),
                "comparison": comparison,
            }
            for technology, group in (("analytic", analytic), ("tile", tile)):
                for metric, values in group["metrics"].items():
                    summary_rows.append(
                        {
                            "cell": cell_name,
                            "lightCount": cell_entry["lightCount"],
                            "radius": cell_entry["radius"],
                            "camera": camera,
                            "technology": technology,
                            "metric": metric,
                            **values["pooled"],
                            "medianOfProcessMedians": values["medianOfProcessMedians"],
                        }
                    )
            for round_index in range(1, 4):
                arun = lookup[(cell_name, camera, "analytic", round_index)]
                trun = lookup[(cell_name, camera, "tile", round_index)]
                if camera == "moving":
                    if camera_trace(arun["result"]) != camera_trace(trun["result"]):
                        raise ValueError(f"camera timeline mismatch: {cell_name}/round{round_index}")
                quality = image_quality(arun["capture"], trun["capture"], pre["qualityGate"])
                quality_pairs.append(
                    {
                        "cell": cell_name,
                        "camera": camera,
                        "round": round_index,
                        "analyticCapture": arun["expected"]["capture"],
                        "tileCapture": trun["expected"]["capture"],
                        **quality,
                    }
                )
        cells.append(cell_entry)

    if not all(item["passed"] for item in quality_pairs):
        failed = [item for item in quality_pairs if not item["passed"]]
        raise ValueError(f"frozen image-quality gate failed: {failed}")

    static_tile_wins = sum(
        cell["cameras"]["static"]["comparison"]["winner"] == "tile" for cell in cells
    )
    static_analytic_wins = sum(
        cell["cameras"]["static"]["comparison"]["winner"] == "analytic" for cell in cells
    )
    moving_tile_wins = sum(
        cell["cameras"]["moving"]["comparison"]["winner"] == "tile" for cell in cells
    )
    moving_analytic_wins = sum(
        cell["cameras"]["moving"]["comparison"]["winner"] == "analytic" for cell in cells
    )
    if moving_tile_wins == len(cells):
        verdict = "default-go"
        explanation = (
            "Moving Camera 的全部锚点均通过冻结门槛，Tile S1 可作为当前矩阵内的默认路径候选。"
            "正式切换前仍需补充 RenderDoc 事件树与动态灯光 Revision 回归。"
        )
    elif static_tile_wins > 0:
        verdict = "conditional-go"
        explanation = (
            "Tile 的收益依赖 CSR 跨帧复用：静止 View/Light Set 下存在明确收益，但运动相机下不满足默认路径门槛。"
            "因此它是条件优化，不是通用替换；当前默认继续保留 Analytic Screen。"
        )
    else:
        verdict = "no-go"
        explanation = (
            "Tile 在静止相机中也没有任何锚点通过冻结胜负门槛，当前 CPU 构表 + TBO 实现没有启用价值。"
        )

    quality_summary = {
        "gate": pre["qualityGate"],
        "pairCount": len(quality_pairs),
        "passedPairs": sum(item["passed"] for item in quality_pairs),
        "worstMaxChannelLsb": max(item["maxChannelLsb"] for item in quality_pairs),
        "worstMeanChannelLsb": max(item["meanChannelLsb"] for item in quality_pairs),
        "worstP99ChannelLsb": max(item["p99ChannelLsb"] for item in quality_pairs),
        "pairs": quality_pairs,
    }
    aggregate = {
        "schemaVersion": 1,
        "valid": True,
        "experiment": "point-light-analytic-screen-vs-tile-s1",
        "protocolSha256": pre["protocolSha256"],
        "preCaptureManifestSha256": sha256(run_dir / "pre-capture-manifest.json"),
        "captureManifestSha256": sha256(run_dir / "capture-manifest.json"),
        "executableSha256": pre["executableSha256"],
        "runCount": len(runs),
        "warmupFrames": int(pre["warmupFrames"]),
        "sampleFrames": int(pre["sampleFrames"]),
        "rounds": int(pre["rounds"]),
        "winnerRule": pre["winnerThreshold"],
        "quality": quality_summary,
        "cells": cells,
        "decision": {
            "verdict": verdict,
            "staticTileWins": static_tile_wins,
            "staticAnalyticWins": static_analytic_wins,
            "movingTileWins": moving_tile_wins,
            "movingAnalyticWins": moving_analytic_wins,
            "defaultRenderPathChanged": False,
            "explanation": explanation,
        },
    }
    dump(run_dir / "aggregate.json", aggregate)

    with (run_dir / "summary.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0].keys()))
        writer.writeheader()
        writer.writerows(summary_rows)

    make_charts(run_dir, cells)
    representative_analytic = lookup[("heavy", "moving", "analytic", 1)]["capture"]
    representative_tile = lookup[("heavy", "moving", "tile", 1)]["capture"]
    chart_dir = run_dir / "charts"
    analytic_image = np.asarray(Image.open(representative_analytic).convert("RGB"), dtype=np.int16)
    tile_image = np.asarray(Image.open(representative_tile).convert("RGB"), dtype=np.int16)
    Image.fromarray(analytic_image.astype(np.uint8), mode="RGB").save(
        chart_dir / "representative-heavy-moving-analytic.png"
    )
    Image.fromarray(tile_image.astype(np.uint8), mode="RGB").save(
        chart_dir / "representative-heavy-moving-tile.png"
    )
    difference = np.abs(analytic_image - tile_image)
    Image.fromarray(np.clip(difference * 96, 0, 255).astype(np.uint8), mode="RGB").save(
        chart_dir / "representative-heavy-moving-difference-x96.png"
    )
    write_report(run_dir, aggregate)
    print(
        "[analysis] PASS "
        f"runs={len(runs)} quality={quality_summary['passedPairs']}/{quality_summary['pairCount']} "
        f"staticTile={static_tile_wins}/{len(cells)} movingTile={moving_tile_wins}/{len(cells)} "
        f"verdict={verdict}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
