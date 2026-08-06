#!/usr/bin/env python3
"""Analyze the frozen actual Tile16/Cluster16 runtime boundary experiment."""

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
from matplotlib.colors import TwoSlopeNorm
from PIL import Image


ABS_THRESHOLD_MS = 0.05
REL_THRESHOLD_PERCENT = 3.0
COUNTS = (32, 64, 128, 256, 512)
CACHED_RADII = (1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0)
REBUILD_RADII = (1.5, 3.0, 6.0, 12.0)
MODES = ("tile16", "cluster16")
METRICS = {
    "wallFrame": ("sample", "wallFrame"),
    "cpuFrame": ("sample", "cpuFrame"),
    "gpuFrame": ("sample", "gpuFrame"),
    "gridLightingCpu": ("cpu", "Point Light Grid Lighting CPU"),
    "gridLightingGpu": ("gpu", "Point Light Grid Lighting GPU"),
    "cacheCheckCpu": ("cpu", "Point Light Grid Cache Check"),
    "gridBuildCpu": ("cpu", "Point Light Grid Build"),
    "gridUploadCpu": ("cpu", "Point Light Grid Upload"),
    "gridBoundsCpu": ("cpu", "Point Light Grid Bounds"),
    "gridCountCpu": ("cpu", "Point Light Grid Count"),
    "gridPrefixCpu": ("cpu", "Point Light Grid Prefix"),
    "gridFillCpu": ("cpu", "Point Light Grid Fill"),
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def percentiles(values: Iterable[float]) -> dict[str, float | int]:
    data = np.asarray(list(values), dtype=np.float64)
    if data.size == 0:
        return {"count": 0, "mean": 0.0, "median": 0.0, "p95": 0.0, "p99": 0.0}
    return {
        "count": int(data.size),
        "mean": float(np.mean(data)),
        "median": float(np.median(data)),
        "p95": float(np.percentile(data, 95)),
        "p99": float(np.percentile(data, 99)),
    }


def metric_samples(result: dict[str, Any], metric: str) -> list[float]:
    kind, name = METRICS[metric]
    samples = result["profiler"]["samples"]
    if kind == "sample":
        return [float(value) for value in samples[name]]
    zone_group = "cpuZones" if kind == "cpu" else "gpuZones"
    return [float(value) for value in samples[zone_group].get(name, [])]


def classify(pairs: list[dict[str, float]]) -> dict[str, Any]:
    deltas = [item["deltaMs"] for item in pairs]
    relatives = [item["relativePercent"] for item in pairs]
    delta = float(np.median(deltas))
    relative = float(np.median(relatives))
    direction = "mixed"
    if all(value < 0.0 for value in deltas):
        direction = "cluster-faster"
    elif all(value > 0.0 for value in deltas):
        direction = "tile-faster"
    significant = abs(delta) >= ABS_THRESHOLD_MS and abs(relative) >= REL_THRESHOLD_PERCENT
    if direction == "cluster-faster" and significant:
        winner = "cluster16"
    elif direction == "tile-faster" and significant:
        winner = "tile16"
    else:
        winner = "tie"
    return {
        "winner": winner,
        "direction": direction,
        "significant": significant,
        "medianPairedDeltaMs": delta,
        "medianPairedRelativePercent": relative,
        "pairedDirectionAgreement": f"{sum(math.copysign(1.0, value) == math.copysign(1.0, delta) for value in deltas)}/{len(deltas)}" if delta else f"0/{len(deltas)}",
        "pairs": pairs,
    }


def validate_result(
    result: dict[str, Any], expected: dict[str, Any], warmup: int, samples: int
) -> None:
    point = result["pointLightStress"]
    grid = point["gridRuntime"]
    mode = expected["renderMode"]
    regime = expected["regime"]
    count = int(expected["lightCount"])
    radius = float(expected["radius"])
    checks = {
        "success": bool(result["success"]),
        "release": result["buildConfiguration"] == "Release",
        "x64": result["architecture"] == "x64",
        "resolution": result["resolution"] == [1920, 1080],
        "warmup": int(result["warmupFrames"]) == warmup,
        "samples": int(result["measuredFrames"]) == samples,
        "gpuTiming": bool(result["profiler"]["gpuTimingSupported"]),
        "mode": point["renderMode"] == mode and bool(point["renderModeExplicit"]),
        "regime": point["gridUpdateMode"] == regime and bool(point["gridUpdateModeExplicit"]),
        "offscreen": not bool(point["offscreenCulling"]) and bool(point["offscreenCullingExplicit"]),
        "count": int(point["generatedLightCount"]) == count,
        "radius": abs(float(point["volumeRadius"]) - radius) <= 1.0e-4,
        "grid": bool(grid["valid"]) and not bool(grid["overflow"]) and not grid["error"],
        "gridCount": int(grid["lightCount"]) == count,
        "clusterFlag": bool(grid["clustered"]) == (mode == "cluster16"),
        "tileSize": int(grid["tileSize"]) == 16,
        "slices": int(grid["sliceCount"]) == (16 if mode == "cluster16" else 1),
        "draws": int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) == 1,
        "noStencil": int(result["profiler"]["summary"]["pointLightStencilDraws"]["median"]) == 0,
        "noVolume": int(result["profiler"]["summary"]["pointLightLightingVolumeDraws"]["median"]) == 0,
    }
    for metric in ("wallFrame", "cpuFrame", "gpuFrame", "gridLightingCpu", "gridLightingGpu", "cacheCheckCpu", "gridBuildCpu", "gridUploadCpu"):
        checks[f"zone:{metric}"] = len(metric_samples(result, metric)) == samples
    if regime == "rebuild":
        for metric in ("gridBoundsCpu", "gridCountCpu", "gridPrefixCpu", "gridFillCpu"):
            checks[f"zone:{metric}"] = len(metric_samples(result, metric)) == samples
        checks["rebuildCounts"] = int(grid["buildCount"]) == warmup + samples and int(grid["uploadCount"]) == warmup + samples
        checks["cacheHits"] = int(grid["cacheHitCount"]) == 0
    else:
        checks["buildOnce"] = int(grid["buildCount"]) == 1 and int(grid["uploadCount"]) == 1
        checks["steadyCacheHits"] = int(grid["cacheHitCount"]) == warmup + samples - 1
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        raise ValueError(f"{expected['stem']} failed checks: {failed}")


def make_heatmap(
    rows: list[dict[str, Any]], regime: str, metric: str, radii: tuple[float, ...], output: Path, title: str
) -> None:
    lookup = {(int(row["lightCount"]), float(row["radius"])): row for row in rows if row["regime"] == regime}
    values = np.zeros((len(COUNTS), len(radii)), dtype=np.float64)
    labels: list[list[str]] = []
    for yi, count in enumerate(COUNTS):
        label_row = []
        for xi, radius in enumerate(radii):
            classification = lookup[(count, radius)]["classifications"][metric]
            values[yi, xi] = classification["medianPairedDeltaMs"]
            symbol = {"cluster16": "C", "tile16": "T", "tie": "="}[classification["winner"]]
            label_row.append(f"{values[yi, xi]:+.3f}\n{symbol}")
        labels.append(label_row)
    maximum = max(float(np.max(np.abs(values))), 0.05)
    figure, axis = plt.subplots(figsize=(max(8.0, len(radii) * 1.05), 4.8), constrained_layout=True)
    image = axis.imshow(values, cmap="RdBu_r", norm=TwoSlopeNorm(vmin=-maximum, vcenter=0.0, vmax=maximum), aspect="auto")
    axis.set_xticks(range(len(radii)), [str(value).rstrip("0").rstrip(".") for value in radii])
    axis.set_yticks(range(len(COUNTS)), [str(value) for value in COUNTS])
    axis.set_xlabel("Effective light radius R")
    axis.set_ylabel("Point-light count N")
    axis.set_title(title + "\nΔ = Cluster16 − Tile16 (ms); C/T/= uses frozen 0.05 ms + 3% + 3/3 rule")
    threshold = maximum * 0.52
    for yi in range(len(COUNTS)):
        for xi in range(len(radii)):
            axis.text(xi, yi, labels[yi][xi], ha="center", va="center", fontsize=8, color="white" if abs(values[yi, xi]) > threshold else "black")
    figure.colorbar(image, ax=axis, label="paired median delta (ms)")
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, dpi=180)
    plt.close(figure)


def write_report(run_dir: Path, aggregate: dict[str, Any]) -> None:
    cells = aggregate["cells"]
    cached = [cell for cell in cells if cell["regime"] == "cached"]
    rebuild = [cell for cell in cells if cell["regime"] == "rebuild"]
    def winner_counts(items: list[dict[str, Any]], metric: str) -> dict[str, int]:
        result = {"cluster16": 0, "tile16": 0, "tie": 0}
        for item in items:
            result[item["classifications"][metric]["winner"]] += 1
        return result
    cached_wall = winner_counts(cached, "wallFrame")
    cached_gpu = winner_counts(cached, "gpuFrame")
    cached_light = winner_counts(cached, "gridLightingGpu")
    rebuild_wall = winner_counts(rebuild, "wallFrame")
    rebuild_light = winner_counts(rebuild, "gridLightingGpu")

    lines = [
        "# Tile16 / Cluster16 实际运行时性能边界报告",
        "",
        "## 结论",
        "",
        f"本实验不是离线候选数估算，而是在同一 OpenGL 3.3 Deferred Lighting 路径中实际执行 Tile16 与 Cluster16。正式矩阵共 {aggregate['runCount']} 个独立进程，每个进程 300 帧预热、600 帧采样；所有配对截图逐字节一致。",
        "",
        f"- Cached/静态列表：Wall Frame 判定 Cluster/Tile/Tie = {cached_wall['cluster16']}/{cached_wall['tile16']}/{cached_wall['tie']}；GPU Frame = {cached_gpu['cluster16']}/{cached_gpu['tile16']}/{cached_gpu['tie']}；仅点光 Lighting GPU = {cached_light['cluster16']}/{cached_light['tile16']}/{cached_light['tie']}。",
        f"- Rebuild/每帧重建：Wall Frame 判定 Cluster/Tile/Tie = {rebuild_wall['cluster16']}/{rebuild_wall['tile16']}/{rebuild_wall['tie']}；仅点光 Lighting GPU = {rebuild_light['cluster16']}/{rebuild_light['tile16']}/{rebuild_light['tie']}。",
        "- 主结论必须看 Wall/GPU Frame 与 Lighting GPU 两层；CPU 与 GPU 并行，未把二者错误相加。",
        "- 胜负规则在采集前冻结：3/3 独立配对同方向，同时绝对差至少 0.05 ms、相对差至少 3%；否则为 Tie。",
        "",
        "## 边界表：Cached（Δ = Cluster16 − Tile16）",
        "",
        "| N | R | Wall Δ / 判定 | GPU Frame Δ / 判定 | Lighting GPU Δ / 判定 | Tile indices | Cluster indices |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for cell in cached:
        def desc(metric: str) -> str:
            item = cell["classifications"][metric]
            return f"{item['medianPairedDeltaMs']:+.4f} ms / {item['winner']}"
        lines.append(
            f"| {cell['lightCount']} | {cell['radius']:g} | {desc('wallFrame')} | {desc('gpuFrame')} | {desc('gridLightingGpu')} | {cell['modes']['tile16']['grid']['totalIndices']:,} | {cell['modes']['cluster16']['grid']['totalIndices']:,} |"
        )
    lines += [
        "",
        "## 边界表：Rebuild Every Frame",
        "",
        "| N | R | Wall Δ / 判定 | Build CPU Δ | Upload CPU Δ | Lighting GPU Δ / 判定 |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for cell in rebuild:
        wall = cell["classifications"]["wallFrame"]
        lighting = cell["classifications"]["gridLightingGpu"]
        build = cell["classifications"]["gridBuildCpu"]
        upload = cell["classifications"]["gridUploadCpu"]
        lines.append(
            f"| {cell['lightCount']} | {cell['radius']:g} | {wall['medianPairedDeltaMs']:+.4f} ms / {wall['winner']} | {build['medianPairedDeltaMs']:+.4f} ms | {upload['medianPairedDeltaMs']:+.4f} ms | {lighting['medianPairedDeltaMs']:+.4f} ms / {lighting['winner']} |"
        )
    lines += [
        "",
        "## 如何读这个边界",
        "",
        "Cluster16 通过对数 Z 切片减少每个像素遍历的候选灯，但把逻辑网格从 8,160 个 Tile 扩大到 130,560 个 Cluster。列表可缓存时，这笔构建成本被摊销，候选减少可能转化为 GPU Lighting 收益；列表每帧重建时，额外 metadata、count/prefix/fill 与上传成本可能远大于像素阶段节省。",
        "",
        "因此这里没有“Cluster 永远优于 Tile”的结论。边界由 N、有效半径 R、列表更新频率共同决定；图中的 Tie 是按预冻结门槛判定，并不等于数值完全相同。",
        "",
        "## 正确性与公平性",
        "",
        f"- 正式配对：{aggregate['imageParity']['exactPairs']}/{aggregate['imageParity']['pairCount']} 张 Tile/Cluster 配对截图逐字节一致。",
        "- 两条路径共享同一灯数据、G-Buffer、材质/光照公式、精确逐像素球体谓词、TBO 格式与一个全屏 Draw；唯一变量是候选列表是否追加 16 个对数 Z 切片。",
        "- CSR 无固定容量截断；超出纹理缓冲区/uint32 容量会使进程失败，而不是漏灯。",
        "- Cached 中 Build/Upload zone 的采样是稳态 cache-hit 空作用域；一次性首帧构建发生在 300 帧预热内，不冒充每帧成本。Rebuild 子阶段才表示真实每帧 Bounds/Count/Prefix/Fill。",
        "",
        "## 图表",
        "",
        "- `charts/cached-wall-delta.png`：Cached 端到端 Wall 边界。",
        "- `charts/cached-gpu-frame-delta.png`：Cached 整帧 GPU 边界。",
        "- `charts/cached-lighting-gpu-delta.png`：Cached 点光像素阶段边界。",
        "- `charts/rebuild-wall-delta.png`：每帧重建时的端到端边界。",
        "- `charts/csr-index-ratio.png`：Cluster/Tile CSR index 数量比。",
        "",
        "## 可复现入口",
        "",
        "- `pre-capture-manifest.json`：采集前冻结的矩阵、协议/二进制/源码哈希。",
        "- `capture-manifest.json`：每个进程的结果、截图与日志哈希。",
        "- `aggregate.json` / `summary.csv`：完整聚合数据。",
        "- `verification/independent-verification.json`：独立验证器输出。",
        "- `tools/run_tile_cluster_runtime_boundary.ps1`：正式复现实验入口。",
        "",
    ]
    (run_dir / "REPORT_CN.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    pre = load_json(run_dir / "pre-capture-manifest.json")
    manifest = load_json(run_dir / "capture-manifest.json")
    protocol_path = run_dir / pre["protocol"]
    if sha256_file(protocol_path) != pre["protocolSha256"]:
        raise ValueError("frozen protocol hash mismatch")
    if sha256_file(Path(pre["executable"])) != pre["executableSha256"]:
        raise ValueError("formal executable changed after freeze")
    if not manifest["valid"] or manifest["completedRunCount"] != manifest["expectedRunCount"]:
        raise ValueError("capture manifest is incomplete")
    expected_runs = pre["expectedRuns"]
    completed = {item["stem"]: item for item in manifest["completedRuns"]}
    if len(expected_runs) != 420 or len(completed) != 420:
        raise ValueError(f"formal run count must be 420, got {len(expected_runs)}/{len(completed)}")

    runs: list[dict[str, Any]] = []
    for expected in expected_runs:
        stem = expected["stem"]
        done = completed[stem]
        result_path = run_dir / expected["result"]
        capture_path = run_dir / expected["capture"]
        log_path = run_dir / expected["log"]
        if sha256_file(result_path) != done["resultSha256"] or sha256_file(capture_path) != done["captureSha256"] or sha256_file(log_path) != done["logSha256"]:
            raise ValueError(f"artifact hash mismatch: {stem}")
        result = load_json(result_path)
        validate_result(result, expected, int(pre["warmupFrames"]), int(pre["sampleFrames"]))
        metric_data = {metric: metric_samples(result, metric) for metric in METRICS}
        grid = result["pointLightStress"]["gridRuntime"]
        runs.append({"expected": expected, "result": result, "metrics": metric_data, "grid": grid, "capture": capture_path})

    grouped: dict[tuple[str, int, float, str], list[dict[str, Any]]] = defaultdict(list)
    by_pair: dict[tuple[str, int, float, int, str], dict[str, Any]] = {}
    for run in runs:
        e = run["expected"]
        key = (e["regime"], int(e["lightCount"]), float(e["radius"]), e["renderMode"])
        grouped[key].append(run)
        by_pair[(e["regime"], int(e["lightCount"]), float(e["radius"]), int(e["round"]), e["renderMode"])] = run

    image_pairs = 0
    image_exact = 0
    cells: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []
    for regime, radii in (("cached", CACHED_RADII), ("rebuild", REBUILD_RADII)):
        for count in COUNTS:
            for radius in radii:
                mode_aggregates: dict[str, Any] = {}
                for mode in MODES:
                    mode_runs = grouped[(regime, count, radius, mode)]
                    if len(mode_runs) != 3:
                        raise ValueError(f"expected 3 processes: {regime}/{count}/{radius}/{mode}")
                    metric_stats: dict[str, Any] = {}
                    for metric in METRICS:
                        pooled = [value for run in mode_runs for value in run["metrics"][metric]]
                        process_medians = [float(np.median(run["metrics"][metric])) if run["metrics"][metric] else 0.0 for run in mode_runs]
                        metric_stats[metric] = {"pooled": percentiles(pooled), "processMedians": process_medians, "medianOfProcessMedians": float(np.median(process_medians))}
                    first_grid = mode_runs[0]["grid"]
                    for run in mode_runs[1:]:
                        for field in ("inputSignature", "csrSignature", "logicalCells", "totalIndices", "residentBytes", "maximumLightsPerCell"):
                            if run["grid"][field] != first_grid[field]:
                                raise ValueError(f"grid telemetry drift: {regime}/{count}/{radius}/{mode}/{field}")
                    mode_aggregates[mode] = {
                        "metrics": metric_stats,
                        "grid": {field: first_grid[field] for field in ("inputSignature", "csrSignature", "logicalCells", "nonEmptyCells", "totalIndices", "maximumLightsPerCell", "averageLightsPerCell", "metadataBytes", "indexBytes", "lightBytes", "residentBytes")},
                    }
                    for metric, stats in metric_stats.items():
                        summary_rows.append({
                            "regime": regime, "lightCount": count, "radius": radius, "mode": mode, "metric": metric,
                            **stats["pooled"], "medianOfProcessMedians": stats["medianOfProcessMedians"],
                        })

                classifications: dict[str, Any] = {}
                for metric in METRICS:
                    pairs = []
                    for round_index in (1, 2, 3):
                        tile_run = by_pair[(regime, count, radius, round_index, "tile16")]
                        cluster_run = by_pair[(regime, count, radius, round_index, "cluster16")]
                        tile_median = float(np.median(tile_run["metrics"][metric])) if tile_run["metrics"][metric] else 0.0
                        cluster_median = float(np.median(cluster_run["metrics"][metric])) if cluster_run["metrics"][metric] else 0.0
                        delta = cluster_median - tile_median
                        relative = delta / tile_median * 100.0 if tile_median else 0.0
                        pairs.append({"round": round_index, "tileMedianMs": tile_median, "clusterMedianMs": cluster_median, "deltaMs": delta, "relativePercent": relative})
                        if metric == "wallFrame":
                            image_pairs += 1
                            if tile_run["capture"].read_bytes() == cluster_run["capture"].read_bytes():
                                image_exact += 1
                            else:
                                raise ValueError(f"Tile/Cluster image mismatch: {regime}/{count}/{radius}/round{round_index}")
                    classifications[metric] = classify(pairs)
                cells.append({"regime": regime, "lightCount": count, "radius": radius, "modes": mode_aggregates, "classifications": classifications})

    aggregate = {
        "schemaVersion": 1,
        "valid": True,
        "experiment": "actual-tile16-vs-cluster16-runtime-boundary",
        "protocolSha256": pre["protocolSha256"],
        "preCaptureManifestSha256": sha256_file(run_dir / "pre-capture-manifest.json"),
        "captureManifestSha256": sha256_file(run_dir / "capture-manifest.json"),
        "executableSha256": pre["executableSha256"],
        "runCount": len(runs),
        "independentProcessesPerModeCell": 3,
        "warmupFrames": pre["warmupFrames"],
        "sampleFrames": pre["sampleFrames"],
        "winnerRule": {"absoluteMilliseconds": ABS_THRESHOLD_MS, "relativePercent": REL_THRESHOLD_PERCENT, "sameDirectionPairs": "3/3"},
        "imageParity": {"pairCount": image_pairs, "exactPairs": image_exact, "allExact": image_pairs == image_exact},
        "cells": cells,
    }
    dump_json(run_dir / "aggregate.json", aggregate)
    with (run_dir / "summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)

    chart_dir = run_dir / "charts"
    make_heatmap(cells, "cached", "wallFrame", CACHED_RADII, chart_dir / "cached-wall-delta.png", "Cached: end-to-end Wall Frame")
    make_heatmap(cells, "cached", "gpuFrame", CACHED_RADII, chart_dir / "cached-gpu-frame-delta.png", "Cached: whole GPU Frame")
    make_heatmap(cells, "cached", "gridLightingGpu", CACHED_RADII, chart_dir / "cached-lighting-gpu-delta.png", "Cached: point-light Lighting GPU")
    make_heatmap(cells, "rebuild", "wallFrame", REBUILD_RADII, chart_dir / "rebuild-wall-delta.png", "Rebuild every frame: end-to-end Wall Frame")

    ratio = np.zeros((len(COUNTS), len(CACHED_RADII)), dtype=np.float64)
    lookup = {(cell["lightCount"], cell["radius"]): cell for cell in cells if cell["regime"] == "cached"}
    for yi, count in enumerate(COUNTS):
        for xi, radius in enumerate(CACHED_RADII):
            cell = lookup[(count, radius)]
            tile = cell["modes"]["tile16"]["grid"]["totalIndices"]
            cluster = cell["modes"]["cluster16"]["grid"]["totalIndices"]
            ratio[yi, xi] = cluster / tile if tile else 0.0
    figure, axis = plt.subplots(figsize=(11, 4.8), constrained_layout=True)
    image = axis.imshow(ratio, cmap="viridis", aspect="auto")
    axis.set_xticks(range(len(CACHED_RADII)), [str(value).rstrip("0").rstrip(".") for value in CACHED_RADII])
    axis.set_yticks(range(len(COUNTS)), [str(value) for value in COUNTS])
    axis.set_xlabel("Effective light radius R")
    axis.set_ylabel("Point-light count N")
    axis.set_title("Cluster16 / Tile16 total CSR index references")
    for yi in range(len(COUNTS)):
        for xi in range(len(CACHED_RADII)):
            axis.text(xi, yi, f"{ratio[yi, xi]:.1f}×", ha="center", va="center", fontsize=8, color="white" if ratio[yi, xi] > np.max(ratio) * 0.55 else "black")
    figure.colorbar(image, ax=axis, label="index-reference ratio")
    figure.savefig(chart_dir / "csr-index-ratio.png", dpi=180)
    plt.close(figure)

    representative = by_pair[("cached", 256, 3.0, 1, "tile16")]["capture"]
    image = Image.open(representative).convert("RGB")
    image.save(chart_dir / "representative-runtime.png")
    write_report(run_dir, aggregate)
    print(f"[analysis] PASS runs={len(runs)} exactImagePairs={image_exact}/{image_pairs} cells={len(cells)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
