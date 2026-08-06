#!/usr/bin/env python3
"""Validate and summarize the deterministic Legacy point-light baseline."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import re
import struct
import zlib
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


THRESHOLD_MS = 1000.0 / 60.0
NAME_PATTERN = re.compile(
    r"^(formal|smoke)-([a-z-]+)-(\d{4})-run(\d+)$"
)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def percentile(sorted_values: list[float], quantile: float) -> float | None:
    if not sorted_values:
        return None
    position = quantile * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def distribution(values: Iterable[float]) -> dict[str, float | int | None]:
    samples = [float(value) for value in values]
    samples.sort()
    if not samples:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "p95": None,
            "p99": None,
        }
    return {
        "count": len(samples),
        "mean": sum(samples) / len(samples),
        "median": percentile(samples, 0.50),
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
    }


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def number(summary: dict[str, Any], key: str, field: str = "median") -> float:
    value = summary.get(key, {}).get(field)
    return float(value) if value is not None else math.nan


def zone_number(
    summary: dict[str, Any], zone: str, field: str = "median"
) -> float:
    value = summary.get("gpuZones", {}).get(zone, {}).get(field)
    return float(value) if value is not None else math.nan


def parse_result(path: Path) -> tuple[dict[str, Any], list[str]]:
    errors: list[str] = []
    match = NAME_PATTERN.match(path.stem)
    require(match is not None, f"unexpected result filename: {path.name}", errors)
    data = load_json(path)
    if match is None:
        return {}, errors

    classification, coverage, count_text, run_text = match.groups()
    count = int(count_text)
    run = int(run_text)
    point = data.get("pointLightStress", {})
    profiler = data.get("profiler", {})
    summary = profiler.get("summary", {})
    samples = profiler.get("samples", {})
    sample_frames = int(point.get("sampleFrames", 0))

    require(int(data.get("schemaVersion", 0)) >= 23, f"{path.name}: schema < 23", errors)
    require(data.get("success") is True, f"{path.name}: success is false", errors)
    require(point.get("enabled") is True, f"{path.name}: point mode disabled", errors)
    require(point.get("coverage") == coverage, f"{path.name}: coverage mismatch", errors)
    require(int(point.get("generatedLightCount", -1)) == count, f"{path.name}: light count mismatch", errors)
    require(data.get("resolution") == [1920, 1080], f"{path.name}: resolution mismatch", errors)
    require(data.get("buildConfiguration") == "Release", f"{path.name}: not Release", errors)
    require(data.get("renderPath") == "phong-deferred-volume", f"{path.name}: wrong render path", errors)
    require(data.get("settings", {}).get("requestedSwapInterval") == 0, f"{path.name}: swap interval mismatch", errors)
    require(data.get("settings", {}).get("bloom") is False, f"{path.name}: bloom enabled", errors)
    require(data.get("ssao", {}).get("enabled") is False, f"{path.name}: SSAO enabled", errors)
    require(data.get("settings", {}).get("shadowCastingLights") == 0, f"{path.name}: shadows enabled", errors)
    require(point.get("pointShadowsEnabled") is False, f"{path.name}: point shadows enabled", errors)
    require(point.get("lightMarkersEnabled") is False, f"{path.name}: light markers enabled", errors)
    require(profiler.get("gpuTimingSupported") is True, f"{path.name}: GPU timing unavailable", errors)
    for sample_name in ("wallFrame", "cpuFrame", "gpuFrame", "drawCalls"):
        require(
            int(summary.get(sample_name, {}).get("count", -1)) == sample_frames,
            f"{path.name}: {sample_name} sample count mismatch",
            errors,
        )
    for zone_name in ("Deferred Pass", "Deferred Lighting", "Deferred Point Lights"):
        require(
            int(summary.get("gpuZones", {}).get(zone_name, {}).get("count", -1))
            == sample_frames,
            f"{path.name}: {zone_name} GPU sample count mismatch",
            errors,
        )
    require(number(summary, "pointLightsTotal") == count, f"{path.name}: total lights counter mismatch", errors)
    require(number(summary, "pointLightsSubmitted") == count, f"{path.name}: submitted lights counter mismatch", errors)
    require(number(summary, "pointLightsCulled") == 0.0, f"{path.name}: Legacy culling counter is nonzero", errors)
    require(number(summary, "pointLightStencilClears") == count * 2, f"{path.name}: point stencil clears mismatch", errors)
    require(number(summary, "stencilClears") == count * 2 + 3, f"{path.name}: total stencil clears mismatch", errors)
    require(float(data.get("meanLuminance", 0.0)) > 0.005, f"{path.name}: black capture", errors)
    require(float(data.get("nonBlackRatio", 0.0)) > 0.01, f"{path.name}: insufficient non-black pixels", errors)

    row = {
        "classification": classification,
        "coverage": coverage,
        "lightCount": count,
        "run": run,
        "path": str(path.resolve()),
        "warmupFrames": int(point.get("warmupFrames", 0)),
        "sampleFrames": sample_frames,
        "sceneSignature": point.get("sceneSignature", ""),
        "submissionSignature": point.get("submissionSignature", ""),
        "wallMedianMs": number(summary, "wallFrame"),
        "wallP95Ms": number(summary, "wallFrame", "p95"),
        "wallP99Ms": number(summary, "wallFrame", "p99"),
        "cpuMedianMs": number(summary, "cpuFrame"),
        "cpuP95Ms": number(summary, "cpuFrame", "p95"),
        "cpuP99Ms": number(summary, "cpuFrame", "p99"),
        "gpuMedianMs": number(summary, "gpuFrame"),
        "gpuP95Ms": number(summary, "gpuFrame", "p95"),
        "gpuP99Ms": number(summary, "gpuFrame", "p99"),
        "deferredGpuMedianMs": zone_number(summary, "Deferred Pass"),
        "deferredLightingGpuMedianMs": zone_number(summary, "Deferred Lighting"),
        "pointLightGpuMedianMs": zone_number(summary, "Deferred Point Lights"),
        "drawCallsMedian": number(summary, "drawCalls"),
        "submittedLightsMedian": number(summary, "pointLightsSubmitted"),
        "culledLightsMedian": number(summary, "pointLightsCulled"),
        "stencilClearsMedian": number(summary, "stencilClears"),
        "pointLightStencilClearsMedian": number(summary, "pointLightStencilClears"),
        "meanLuminance": float(data.get("meanLuminance", 0.0)),
        "nonBlackRatio": float(data.get("nonBlackRatio", 0.0)),
        "capturePath": data.get("capturePath", ""),
        "nearPlaneVerified": bool(point.get("fixtures", {}).get("nearPlaneIntersectionVerified")),
        "cameraInsideVerified": bool(point.get("fixtures", {}).get("cameraInsideVerified")),
        "offscreenVerified": bool(point.get("fixtures", {}).get("fullyOffscreenVerified")),
        "cpuSamples": [float(value) for value in samples.get("cpuFrame", [])],
        "gpuSamples": [float(value) for value in samples.get("gpuFrame", [])],
        "pointGpuSamples": [
            float(value)
            for value in samples.get("gpuZones", {}).get("Deferred Point Lights", [])
        ],
        "deferredGpuSamples": [
            float(value)
            for value in samples.get("gpuZones", {}).get("Deferred Pass", [])
        ],
    }
    return row, errors


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def ppm_to_png(source: Path, target: Path) -> None:
    raw = source.read_bytes()
    tokens: list[bytes] = []
    index = 0
    while len(tokens) < 4:
        while index < len(raw) and chr(raw[index]).isspace():
            index += 1
        if index < len(raw) and raw[index] == ord("#"):
            while index < len(raw) and raw[index] not in (10, 13):
                index += 1
            continue
        start = index
        while index < len(raw) and not chr(raw[index]).isspace():
            index += 1
        tokens.append(raw[start:index])
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError(f"unsupported PPM: {source}")
    width, height = int(tokens[1]), int(tokens[2])
    if index >= len(raw) or not chr(raw[index]).isspace():
        raise ValueError(f"missing PPM raster separator: {source}")
    if raw[index] == 13 and index + 1 < len(raw) and raw[index + 1] == 10:
        index += 2
    else:
        index += 1
    pixels = raw[index:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"invalid PPM payload: {source}")
    scanlines = b"".join(
        b"\x00" + pixels[row * width * 3 : (row + 1) * width * 3]
        for row in range(height)
    )
    encoded = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(scanlines, 6))
        + png_chunk(b"IEND", b"")
    )
    target.write_bytes(encoded)


def write_svg(path: Path, aggregates: list[dict[str, Any]]) -> None:
    points = sorted(
        [row for row in aggregates if row["coverage"] == "representative"],
        key=lambda row: row["lightCount"],
    )
    width, height = 900, 520
    left, right, top, bottom = 80, 30, 40, 70
    plot_width = width - left - right
    plot_height = height - top - bottom
    max_value = max(
        [THRESHOLD_MS]
        + [
            float(row[key])
            for row in points
            for key in ("cpuMedianMs", "gpuMedianMs", "pointLightGpuMedianMs")
        ]
    )
    max_value = max(20.0, max_value * 1.15)
    counts = [row["lightCount"] for row in points]

    def x_position(index: int) -> float:
        return left + (plot_width * index / max(1, len(points) - 1))

    def y_position(value: float) -> float:
        return top + plot_height * (1.0 - value / max_value)

    series = [
        ("CPU Frame", "#2563eb", "cpuMedianMs"),
        ("GPU Frame", "#dc2626", "gpuMedianMs"),
        ("Point Lights GPU", "#059669", "pointLightGpuMedianMs"),
    ]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="450" y="24" text-anchor="middle" font-family="sans-serif" font-size="18">Legacy Point-Light-Heavy（进程合并 Median）</text>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}" stroke="#111"/>',
        f'<line x1="{left}" y1="{top + plot_height}" x2="{left + plot_width}" y2="{top + plot_height}" stroke="#111"/>',
    ]
    for step in range(0, 6):
        value = max_value * step / 5
        y = y_position(value)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#e5e7eb"/>')
        parts.append(f'<text x="{left - 8}" y="{y + 4:.2f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.1f}</text>')
    threshold_y = y_position(THRESHOLD_MS)
    parts.append(f'<line x1="{left}" y1="{threshold_y:.2f}" x2="{left + plot_width}" y2="{threshold_y:.2f}" stroke="#7c3aed" stroke-dasharray="7 5"/>')
    parts.append(f'<text x="{left + plot_width - 4}" y="{threshold_y - 6:.2f}" text-anchor="end" font-family="sans-serif" font-size="12" fill="#7c3aed">60 FPS = 16.67 ms</text>')
    for index, count in enumerate(counts):
        x = x_position(index)
        parts.append(f'<text x="{x:.2f}" y="{top + plot_height + 24}" text-anchor="middle" font-family="sans-serif" font-size="13">{count}</text>')
    for series_index, (label, color, key) in enumerate(series):
        coordinates = " ".join(
            f"{x_position(index):.2f},{y_position(float(row[key])):.2f}"
            for index, row in enumerate(points)
        )
        parts.append(f'<polyline points="{coordinates}" fill="none" stroke="{color}" stroke-width="3"/>')
        for index, row in enumerate(points):
            parts.append(f'<circle cx="{x_position(index):.2f}" cy="{y_position(float(row[key])):.2f}" r="4" fill="{color}"/>')
        legend_x = left + 12 + series_index * 210
        parts.append(f'<line x1="{legend_x}" y1="{height - 24}" x2="{legend_x + 28}" y2="{height - 24}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{legend_x + 36}" y="{height - 19}" font-family="sans-serif" font-size="13">{label}</text>')
    parts.append(f'<text x="{left + plot_width / 2}" y="{height - 42}" text-anchor="middle" font-family="sans-serif" font-size="13">点光源数量</text>')
    parts.append(f'<text x="18" y="{top + plot_height / 2}" transform="rotate(-90 18 {top + plot_height / 2})" text-anchor="middle" font-family="sans-serif" font-size="13">毫秒</text>')
    parts.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def fmt(value: Any, digits: int = 3) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "—"
    return f"{float(value):.{digits}f}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--chart", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    manifest_path = run_dir / "manifest.json"
    manifest = load_json(manifest_path) if manifest_path.exists() else {}
    result_paths = sorted(run_dir.glob("formal-*-run*.json")) + sorted(
        run_dir.glob("smoke-*-run*.json")
    )
    if not result_paths:
        raise SystemExit(f"no benchmark JSON found in {run_dir}")

    rows: list[dict[str, Any]] = []
    errors: list[str] = []
    source_data: dict[str, dict[str, Any]] = {}
    for path in result_paths:
        row, result_errors = parse_result(path)
        errors.extend(result_errors)
        if row:
            rows.append(row)
            source_data[str(path.resolve())] = load_json(path)

    signatures: dict[tuple[str, str, int], set[tuple[str, str]]] = defaultdict(set)
    for row in rows:
        signatures[(row["classification"], row["coverage"], row["lightCount"])].add(
            (row["sceneSignature"], row["submissionSignature"])
        )
    for key, values in signatures.items():
        require(len(values) == 1, f"signature mismatch for {key}: {sorted(values)}", errors)

    formal_groups: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row["classification"] == "formal":
            formal_groups[(row["coverage"], row["lightCount"])].append(row)

    aggregates: list[dict[str, Any]] = []
    for (coverage, count), group in sorted(formal_groups.items()):
        cpu = distribution(value for row in group for value in row["cpuSamples"])
        gpu = distribution(value for row in group for value in row["gpuSamples"])
        point_gpu = distribution(value for row in group for value in row["pointGpuSamples"])
        deferred_gpu = distribution(value for row in group for value in row["deferredGpuSamples"])
        stable_runs = sum(
            1
            for row in group
            if row["cpuMedianMs"] > THRESHOLD_MS
            or row["gpuMedianMs"] > THRESHOLD_MS
        )
        aggregates.append(
            {
                "coverage": coverage,
                "lightCount": count,
                "processCount": len(group),
                "pooledSampleCount": cpu["count"],
                "cpuMedianMs": cpu["median"],
                "cpuP95Ms": cpu["p95"],
                "cpuP99Ms": cpu["p99"],
                "gpuMedianMs": gpu["median"],
                "gpuP95Ms": gpu["p95"],
                "gpuP99Ms": gpu["p99"],
                "deferredGpuMedianMs": deferred_gpu["median"],
                "pointLightGpuMedianMs": point_gpu["median"],
                "drawCallsMedian": distribution(row["drawCallsMedian"] for row in group)["median"],
                "submittedLightsMedian": distribution(row["submittedLightsMedian"] for row in group)["median"],
                "culledLightsMedian": distribution(row["culledLightsMedian"] for row in group)["median"],
                "stencilClearsMedian": distribution(row["stencilClearsMedian"] for row in group)["median"],
                "stableBelow60ProcessCount": stable_runs,
                "stableBelow60": len(group) >= 3 and stable_runs == len(group),
                "processCpuMedianMinMs": min(row["cpuMedianMs"] for row in group),
                "processCpuMedianMaxMs": max(row["cpuMedianMs"] for row in group),
                "processGpuMedianMinMs": min(row["gpuMedianMs"] for row in group),
                "processGpuMedianMaxMs": max(row["gpuMedianMs"] for row in group),
            }
        )

    representative = sorted(
        [row for row in aggregates if row["coverage"] == "representative"],
        key=lambda row: row["lightCount"],
    )
    first_stable = next((row for row in representative if row["stableBelow60"]), None)

    per_process_fields = [
        "classification", "coverage", "lightCount", "run", "warmupFrames",
        "sampleFrames", "sceneSignature", "submissionSignature", "wallMedianMs",
        "wallP95Ms", "wallP99Ms", "cpuMedianMs", "cpuP95Ms", "cpuP99Ms",
        "gpuMedianMs", "gpuP95Ms", "gpuP99Ms", "deferredGpuMedianMs",
        "deferredLightingGpuMedianMs", "pointLightGpuMedianMs", "drawCallsMedian",
        "submittedLightsMedian", "culledLightsMedian", "stencilClearsMedian",
        "pointLightStencilClearsMedian", "meanLuminance", "nonBlackRatio",
        "capturePath", "path",
    ]
    aggregate_fields = list(aggregates[0].keys()) if aggregates else []
    write_csv(run_dir / "per-process.csv", rows, per_process_fields)
    if aggregates:
        write_csv(run_dir / "aggregate.csv", aggregates, aggregate_fields)

    aggregate_json = {
        "schemaVersion": 1,
        "generatedAtUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "valid": not errors,
        "validationErrors": errors,
        "threshold60FpsMs": THRESHOLD_MS,
        "firstStableBelow60Fps": (
            first_stable["lightCount"] if first_stable else None
        ),
        "definition": "all independent processes have CPU or GPU frame median > 16.67 ms",
        "aggregates": aggregates,
    }
    (run_dir / "aggregate.json").write_text(
        json.dumps(aggregate_json, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    write_svg(args.chart.resolve(), aggregates)

    image_dir = args.chart.resolve().parent
    png_paths: list[Path] = []
    for ppm_path in sorted(image_dir.glob("*.ppm")):
        png_path = ppm_path.with_suffix(".png")
        ppm_to_png(ppm_path, png_path)
        png_paths.append(png_path)

    first_data = source_data[rows[0]["path"]]
    machine = manifest.get("machine", {})
    largest_formal = max(representative, key=lambda item: item["lightCount"], default=None)
    representative_smoke = next(
        (
            row
            for row in rows
            if row["classification"] == "smoke"
            and row["coverage"] == "representative"
        ),
        None,
    )
    high_overlap_smoke = next(
        (
            row
            for row in rows
            if row["classification"] == "smoke"
            and row["coverage"] == "high-overlap"
        ),
        None,
    )
    if largest_formal and largest_formal["gpuMedianMs"] > 0.0:
        point_gpu_share = (
            largest_formal["pointLightGpuMedianMs"]
            / largest_formal["gpuMedianMs"]
            * 100.0
        )
        largest_observation = (
            f"- 最大正式配置 {largest_formal['lightCount']} 灯的 GPU Frame Median 为 "
            f"{fmt(largest_formal['gpuMedianMs'])} ms，其中点光源阶段 Median 为 "
            f"{fmt(largest_formal['pointLightGpuMedianMs'])} ms（两个 Median 的比值约 "
            f"{point_gpu_share:.1f}%）；这把 Legacy 点光源阶段列为首要候选瓶颈，但不代表任何优化收益。"
        )
    else:
        largest_observation = "- 尚无正式 GPU 样本可用于定位候选瓶颈。"
    if representative_smoke and high_overlap_smoke:
        overlap_ratio = (
            high_overlap_smoke["pointLightGpuMedianMs"]
            / representative_smoke["pointLightGpuMedianMs"]
        )
        overlap_observation = (
            f"- 同为 16 灯、{fmt(representative_smoke['drawCallsMedian'], 0)} Draw、"
            f"{fmt(representative_smoke['stencilClearsMedian'], 0)} 次 stencil clear 的 smoke 中，"
            f"high-overlap 点光源 GPU Median 为 {fmt(high_overlap_smoke['pointLightGpuMedianMs'])} ms，"
            f"representative 为 {fmt(representative_smoke['pointLightGpuMedianMs'])} ms（约 "
            f"{overlap_ratio:.2f} 倍）；因此光体积像素覆盖与 overdraw 是有实测依据的候选调查项。"
        )
    else:
        overlap_observation = "- 尚无成对覆盖形态 smoke 可用于判断 fill-rate/overdraw 候选。"
    report_lines = [
        "# 可复现多点光源压力基准（Legacy Deferred）",
        "",
        f"> 状态：{'通过' if not errors else '验证失败'}；生成时间：{aggregate_json['generatedAtUtc']}。这是基准建设，不包含渲染优化。",
        "",
        "## 场景与隔离变量",
        "",
        "- 场景：项目自带 Crytek Sponza，模型归一化半径 15；固定相机 `(-6, -1.5, 0)` 看向 `(6, -0.8, 0)`，FOV 55°。",
        "- 路径：`phong-deferred-volume`，沿用逐灯 stencil volume + lighting volume 的 Legacy 顺序。",
        "- 固定条件：Release x64、1920×1080、请求 VSync 关闭、显式 gPosition、Bloom/SSAO/所有阴影关闭。点光源调试球和完整编辑器 UI 在基准模式关闭。",
        f"- 默认 framebuffer：sample buffers={first_data.get('settings', {}).get('windowSampleBuffers')}，samples={first_data.get('settings', {}).get('windowSamples')}；HDR/后处理、天空盒和场景透明材质仍沿用项目现状。",
        "- 生成器：固定 xorshift32 v1，seed `0x21D3F3A5`；灯光顺序、位置、8 色固定调色板、强度与衰减均进入 scene/submission 签名。",
        "- representative 半径约 3.686；high-overlap 半径约 9.813。edge-cases 使用 representative 衰减，并显式包含近裁面相交、相机位于光体内和完全离屏灯。",
        "",
        "## 环境与协议",
        "",
        f"- CPU：{machine.get('cpu', '未记录')}。",
        f"- GPU：{first_data.get('glRenderer', '未记录')}；驱动：{machine.get('gpuDriver', '未记录')}。",
        f"- OpenGL：{first_data.get('glVersion', '未记录')}（{first_data.get('glVendor', '未记录')}）。",
        f"- OS：{machine.get('os', '未记录')}。",
        f"- 正式协议：每配置 {representative[0]['processCount'] if representative else 0} 个独立进程；每进程预热 {next((row['warmupFrames'] for row in rows if row['classification'] == 'formal'), 0)} 帧、采样 {next((row['sampleFrames'] for row in rows if row['classification'] == 'formal'), 0)} 帧。百分位按进程原始样本合并后线性插值计算；每进程值仍单独保留。",
        "",
        "## 每进程正式结果",
        "",
        "| 灯数 | 进程 | CPU Median/P95/P99 ms | GPU Median/P95/P99 ms | Deferred GPU ms | 点光源 GPU ms | Draw | 提交/剔除 | Stencil clear |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in sorted(
        [item for item in rows if item["classification"] == "formal"],
        key=lambda item: (item["lightCount"], item["run"]),
    ):
        report_lines.append(
            f"| {row['lightCount']} | {row['run']} | {fmt(row['cpuMedianMs'])}/{fmt(row['cpuP95Ms'])}/{fmt(row['cpuP99Ms'])} | "
            f"{fmt(row['gpuMedianMs'])}/{fmt(row['gpuP95Ms'])}/{fmt(row['gpuP99Ms'])} | {fmt(row['deferredGpuMedianMs'])} | "
            f"{fmt(row['pointLightGpuMedianMs'])} | {fmt(row['drawCallsMedian'], 0)} | "
            f"{fmt(row['submittedLightsMedian'], 0)}/{fmt(row['culledLightsMedian'], 0)} | {fmt(row['stencilClearsMedian'], 0)} |"
        )
    report_lines.extend(
        [
            "",
            "## 跨进程聚合与 60 FPS 阈值",
            "",
            "| 灯数 | 进程×样本 | CPU Median/P95/P99 ms | GPU Median/P95/P99 ms | Deferred GPU ms | 点光源 GPU ms | 3/3 稳定低于 60 FPS |",
            "|---:|---:|---:|---:|---:|---:|:---:|",
        ]
    )
    for row in representative:
        report_lines.append(
            f"| {row['lightCount']} | {row['processCount']}×{int(row['pooledSampleCount'] / max(1, row['processCount']))} | "
            f"{fmt(row['cpuMedianMs'])}/{fmt(row['cpuP95Ms'])}/{fmt(row['cpuP99Ms'])} | "
            f"{fmt(row['gpuMedianMs'])}/{fmt(row['gpuP95Ms'])}/{fmt(row['gpuP99Ms'])} | "
            f"{fmt(row['deferredGpuMedianMs'])} | {fmt(row['pointLightGpuMedianMs'])} | "
            f"{'是' if row['stableBelow60'] else '否'}（{row['stableBelow60ProcessCount']}/{row['processCount']}） |"
        )
    if first_stable:
        report_lines.extend(
            [
                "",
                f"首次稳定低于 60 FPS 的配置是 **representative / {first_stable['lightCount']} 灯**：所有独立进程的 CPU 或 GPU Frame Median 均超过 16.67 ms。",
            ]
        )
    else:
        report_lines.extend(
            [
                "",
                "在已完成的正式配置中，尚未出现所有独立进程均稳定低于 60 FPS 的点。",
            ]
        )
    report_lines.extend(
        [
            "",
            "## 正确性与复现性",
            "",
            "- 所有结果均要求 `success=true`、GL error-free、GPU/CPU/zone 样本数完整、截图非黑屏。",
            "- 同一覆盖形态和灯数的 scene/submission 签名必须跨进程完全一致；不同灯数不要求截图逐像素一致。",
            "- Legacy 路径没有点光源视锥/Scissor 剔除，因此完全离屏灯仍真实提交，正式结果中的剔除数应为 0。",
            "",
            "| 分类/形态 | 灯数 | Scene signature | Submission signature | 进程一致 |",
            "|---|---:|---|---|:---:|",
        ]
    )
    for key in sorted(signatures):
        classification, coverage, light_count = key
        scene_signature, submission_signature = next(iter(signatures[key]))
        process_count = sum(
            1
            for row in rows
            if (
                row["classification"],
                row["coverage"],
                row["lightCount"],
            )
            == key
        )
        report_lines.append(
            f"| {classification}/{coverage} | {light_count} | `{scene_signature}` | "
            f"`{submission_signature}` | 是（{process_count}/{process_count}） |"
        )
    report_lines.extend(
        [
            "",
            "| Smoke | 非黑屏 | 近裁面 | 相机在光体内 | 完全离屏 |",
            "|---|:---:|:---:|:---:|:---:|",
        ]
    )
    for row in sorted(
        [item for item in rows if item["classification"] == "smoke"],
        key=lambda item: item["coverage"],
    ):
        report_lines.append(
            f"| {row['coverage']} / {row['lightCount']} | 是 | "
            f"{'是' if row['nearPlaneVerified'] else '不适用'} | "
            f"{'是' if row['cameraInsideVerified'] else '不适用'} | "
            f"{'是' if row['offscreenVerified'] else '不适用'} |"
        )
    report_lines.extend(
        [
            "",
            "## 产物",
            "",
            f"- 原始 JSON、日志、CSV 和聚合 JSON：`{run_dir}`。",
            f"- 曲线图：`{args.chart.resolve()}`。",
            f"- 固定相机截图：`{image_dir}`（PPM 原始证据与 PNG 便览）。",
            "",
            "## 复现命令",
            "",
            "在项目目录 `OpenGL_Learn` 中运行以下命令，会重建 Release 并执行 16/64/256/512 各 3 个正式进程及三种覆盖形态 smoke：",
            "",
            "```powershell",
            "powershell -ExecutionPolicy Bypass -File .\\tools\\run_point_light_heavy.ps1 -Mode All",
            "```",
            "",
            "## 已知限制与候选瓶颈",
            "",
            "- 点光源 stencil 与 lighting 在 Legacy 循环内逐灯交错。为避免为每盏灯插入大量 timestamp query 并污染基线，本次只导出完整 `Deferred Point Lights` GPU 区间；`Deferred Lighting` 包含方向光全屏与点光源阶段，未伪造 stencil/lighting 子阶段数据。",
            "- CPU Frame 是提交侧范围，GPU Frame 是 timestamp 范围；两者与 wall frame 语义不同，阈值判断只使用用户要求的 CPU/GPU Frame Median。",
            "- 基准显式调用 `glfwSwapInterval(0)` 并在 JSON 记录 `requestedSwapInterval=0`；OpenGL/GLFW 没有可移植的驱动强制 VSync 回读接口，因此控制面板级覆盖无法在进程内独立确认。",
            "- 从真实计数看，点光源阶段每灯固定 2 个 draw，point-light stencil clear 固定为每灯 2 次，总 stencil clear 为 `2N+3`；这只是 Legacy 工作量证据，不代表任何优化收益。",
            largest_observation,
            overlap_observation,
            "- 结合线性增长的每灯 draw/clear 计数，可把逐灯状态/Uniform 更新与模板清理列为后续候选调查项；本任务未实现 Scissor、灯光剔除、批处理、减少 clear 或 Stencil 策略切换。",
        ]
    )
    if errors:
        report_lines.extend(["", "## 验证错误", ""] + [f"- {error}" for error in errors])
    args.report.resolve().write_text("\n".join(report_lines) + "\n", encoding="utf-8")

    print(json.dumps({
        "valid": not errors,
        "resultCount": len(rows),
        "formalAggregateCount": len(aggregates),
        "firstStableBelow60Fps": first_stable["lightCount"] if first_stable else None,
        "report": str(args.report.resolve()),
        "chart": str(args.chart.resolve()),
        "pngCount": len(png_paths),
        "errors": errors,
    }, ensure_ascii=False, indent=2))
    return 0 if not errors else 2


if __name__ == "__main__":
    raise SystemExit(main())
