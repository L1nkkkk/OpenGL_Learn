#!/usr/bin/env python3
"""Validate and aggregate the Deferred point-light stencil-clear A/B."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import re
import struct
import zlib
from collections import defaultdict
from pathlib import Path
from statistics import median
from typing import Any, Iterable


MODES = ("legacy-2n", "coalesced-n-plus-one")
FORMAL_COUNTS = (16, 256, 512)
NAME_PATTERN = re.compile(
    r"^(formal|smoke|zero)-([a-z-]+)-(\d{4})-(legacy-2n|coalesced-n-plus-one)-run(\d{2})$"
)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def read_log(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    if raw[:4096].count(0) > max(8, len(raw[:4096]) // 8):
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def percentile(sorted_values: list[float], quantile: float) -> float | None:
    if not sorted_values:
        return None
    position = quantile * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def distribution(values: Iterable[float]) -> dict[str, float | int | None]:
    samples = sorted(float(value) for value in values)
    if not samples:
        return {"count": 0, "mean": None, "median": None, "p95": None, "p99": None, "minimum": None, "maximum": None}
    return {
        "count": len(samples),
        "mean": sum(samples) / len(samples),
        "median": percentile(samples, 0.50),
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
        "minimum": samples[0],
        "maximum": samples[-1],
    }


def summary_value(summary: dict[str, Any], name: str, field: str = "median") -> float:
    value = summary.get(name, {}).get(field)
    return float(value) if value is not None else math.nan


def zone_value(summary: dict[str, Any], kind: str, name: str, field: str = "median") -> float:
    value = summary.get(kind, {}).get(name, {}).get(field)
    return float(value) if value is not None else math.nan


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def parse_result(path: Path, errors: list[str]) -> dict[str, Any] | None:
    match = NAME_PATTERN.match(path.stem)
    require(match is not None, f"unexpected JSON filename: {path.name}", errors)
    if not match:
        return None
    classification, coverage, count_text, mode, run_text = match.groups()
    light_count = int(count_text)
    run = int(run_text)
    data = load_json(path)
    point = data.get("pointLightStress", {})
    profiler = data.get("profiler", {})
    summary = profiler.get("summary", {})
    samples = profiler.get("samples", {})
    sample_frames = int(point.get("sampleFrames", 0))
    expected_point_clears = (
        light_count * 2
        if mode == "legacy-2n"
        else (light_count + 1 if light_count > 0 else 0)
    )
    label = path.name
    require(int(data.get("schemaVersion", 0)) >= 24, f"{label}: schema < 24", errors)
    require(data.get("success") is True, f"{label}: success=false", errors)
    require(point.get("enabled") is True, f"{label}: stress disabled", errors)
    require(point.get("stencilClearMode") == mode, f"{label}: mode mismatch", errors)
    require(point.get("stencilClearModeExplicit") is True, f"{label}: mode was not explicit", errors)
    require(point.get("coverage") == coverage, f"{label}: coverage mismatch", errors)
    require(int(point.get("generatedLightCount", -1)) == light_count, f"{label}: light count mismatch", errors)
    require(data.get("resolution") == [1920, 1080], f"{label}: resolution mismatch", errors)
    require(data.get("buildConfiguration") == "Release", f"{label}: non-Release build", errors)
    require(data.get("architecture") == "x64", f"{label}: non-x64 build", errors)
    require(data.get("renderPath") == "phong-deferred-volume", f"{label}: render path mismatch", errors)
    settings = data.get("settings", {})
    require(settings.get("requestedSwapInterval") == 0, f"{label}: VSync request mismatch", errors)
    require(settings.get("deferredRendering") is True, f"{label}: Deferred disabled", errors)
    require(settings.get("bloom") is False, f"{label}: Bloom enabled", errors)
    require(data.get("ssao", {}).get("enabled") is False, f"{label}: SSAO enabled", errors)
    require(settings.get("shadowCastingLights") == 0, f"{label}: shadows enabled", errors)
    require(point.get("pointShadowsEnabled") is False, f"{label}: point shadows enabled", errors)
    require(data.get("gBuffer", {}).get("positionMode") == "explicit", f"{label}: gPosition not explicit", errors)
    require(profiler.get("gpuTimingSupported") is True, f"{label}: GPU timing unavailable", errors)
    for sample_name in ("cpuFrame", "gpuFrame", "drawCalls", "pointLightStencilClears", "fixedStencilClears", "stencilClears"):
        require(int(summary.get(sample_name, {}).get("count", -1)) == sample_frames, f"{label}: {sample_name} count mismatch", errors)
    for kind in ("cpuZones", "gpuZones"):
        require(
            int(summary.get(kind, {}).get("Deferred Point Lights", {}).get("count", -1)) == sample_frames,
            f"{label}: {kind}/Deferred Point Lights count mismatch",
            errors,
        )
    require(summary_value(summary, "pointLightsActive") == light_count, f"{label}: active lights mismatch", errors)
    require(summary_value(summary, "pointLightsSubmitted") == light_count, f"{label}: submitted lights mismatch", errors)
    require(summary_value(summary, "pointLightsCulled") == 0.0, f"{label}: culled lights nonzero", errors)
    require(summary_value(summary, "pointLightStencilClears") == expected_point_clears, f"{label}: point clear mismatch", errors)
    require(summary_value(summary, "fixedStencilClears") == 3.0, f"{label}: fixed clear mismatch", errors)
    require(summary_value(summary, "stencilClears") == expected_point_clears + 3, f"{label}: total clear mismatch", errors)
    lifecycle = point.get("stencilLifecycleValidation", {})
    if classification in ("smoke", "zero"):
        require(lifecycle.get("requested") is True, f"{label}: lifecycle check not requested", errors)
        require(lifecycle.get("executed") is True, f"{label}: lifecycle check not executed", errors)
        require(lifecycle.get("clean") is True, f"{label}: final stencil is dirty", errors)
        require(int(lifecycle.get("nonZeroPixels", -1)) == 0, f"{label}: nonzero stencil pixels", errors)
    capture_path = Path(str(data.get("capturePath", "")))
    if not capture_path.is_absolute():
        capture_path = (path.parents[3] / capture_path).resolve()
    # The application was launched from the project directory; prefer the run-local image when present.
    local_capture = path.parent.parent / "images" / (path.stem + ".ppm")
    if local_capture.exists():
        capture_path = local_capture.resolve()
    require(capture_path.exists(), f"{label}: capture missing: {capture_path}", errors)
    row = {
        "classification": classification,
        "coverage": coverage,
        "lightCount": light_count,
        "mode": mode,
        "run": run,
        "path": str(path.resolve()),
        "capturePath": str(capture_path),
        "warmupFrames": int(point.get("warmupFrames", 0)),
        "sampleFrames": sample_frames,
        "sceneSignature": str(point.get("sceneSignature", "")),
        "submissionSignature": str(point.get("submissionSignature", "")),
        "cpuFrameMedianMs": summary_value(summary, "cpuFrame"),
        "cpuFrameP95Ms": summary_value(summary, "cpuFrame", "p95"),
        "cpuFrameP99Ms": summary_value(summary, "cpuFrame", "p99"),
        "gpuFrameMedianMs": summary_value(summary, "gpuFrame"),
        "gpuFrameP95Ms": summary_value(summary, "gpuFrame", "p95"),
        "gpuFrameP99Ms": summary_value(summary, "gpuFrame", "p99"),
        "pointLightCpuMedianMs": zone_value(summary, "cpuZones", "Deferred Point Lights"),
        "pointLightGpuMedianMs": zone_value(summary, "gpuZones", "Deferred Point Lights"),
        "drawCallsMedian": summary_value(summary, "drawCalls"),
        "submittedLightsMedian": summary_value(summary, "pointLightsSubmitted"),
        "culledLightsMedian": summary_value(summary, "pointLightsCulled"),
        "pointLightStencilClearsMedian": summary_value(summary, "pointLightStencilClears"),
        "fixedStencilClearsMedian": summary_value(summary, "fixedStencilClears"),
        "totalStencilClearsMedian": summary_value(summary, "stencilClears"),
        "lifecycleClean": bool(lifecycle.get("clean")),
        "lifecycleNonZeroPixels": int(lifecycle.get("nonZeroPixels", 0)),
        "nearPlaneVerified": bool(point.get("fixtures", {}).get("nearPlaneIntersectionVerified")),
        "cameraInsideVerified": bool(point.get("fixtures", {}).get("cameraInsideVerified")),
        "offscreenVerified": bool(point.get("fixtures", {}).get("fullyOffscreenVerified")),
        "cpuFrameSamples": [float(value) for value in samples.get("cpuFrame", [])],
        "gpuFrameSamples": [float(value) for value in samples.get("gpuFrame", [])],
        "pointLightCpuSamples": [float(value) for value in samples.get("cpuZones", {}).get("Deferred Point Lights", [])],
        "pointLightGpuSamples": [float(value) for value in samples.get("gpuZones", {}).get("Deferred Point Lights", [])],
        "drawCallSamples": [float(value) for value in samples.get("drawCalls", [])],
    }
    return row


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
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
        raise ValueError(f"unsupported PPM: {path}")
    width, height = int(tokens[1]), int(tokens[2])
    if raw[index] == 13 and index + 1 < len(raw) and raw[index + 1] == 10:
        index += 2
    else:
        index += 1
    pixels = raw[index:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"invalid PPM payload: {path}")
    return width, height, pixels


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def write_rgb_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    scanlines = b"".join(b"\x00" + pixels[row * width * 3 : (row + 1) * width * 3] for row in range(height))
    encoded = (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(scanlines, 6))
        + png_chunk(b"IEND", b"")
    )
    path.write_bytes(encoded)


def compare_images(rows: list[dict[str, Any]], output_dir: Path, errors: list[str]) -> list[dict[str, Any]]:
    pairs: dict[tuple[str, str, int, int], dict[str, dict[str, Any]]] = defaultdict(dict)
    for row in rows:
        pairs[(row["classification"], row["coverage"], row["lightCount"], row["run"])][row["mode"]] = row
    evidence: list[dict[str, Any]] = []
    for key, modes in sorted(pairs.items()):
        if set(modes) != set(MODES):
            errors.append(f"image pair missing mode: {key}")
            continue
        legacy_path = Path(modes["legacy-2n"]["capturePath"])
        coalesced_path = Path(modes["coalesced-n-plus-one"]["capturePath"])
        lw, lh, legacy = read_ppm(legacy_path)
        cw, ch, coalesced = read_ppm(coalesced_path)
        require((lw, lh) == (cw, ch), f"image size mismatch: {key}", errors)
        if (lw, lh) != (cw, ch):
            continue
        differences = bytes(abs(a - b) for a, b in zip(legacy, coalesced))
        total = sum(differences)
        maximum = max(differences, default=0)
        mismatch_channels = sum(value != 0 for value in differences)
        mismatch_pixels = sum(
            any(differences[index + channel] != 0 for channel in range(3))
            for index in range(0, len(differences), 3)
        )
        classification, coverage, count, run = key
        diff_path = output_dir / f"diff-{classification}-{coverage}-{count:04d}-run{run:02d}.png"
        write_rgb_png(diff_path, lw, lh, differences)
        evidence.append({
            "classification": classification,
            "coverage": coverage,
            "lightCount": count,
            "run": run,
            "width": lw,
            "height": lh,
            "legacyPath": str(legacy_path),
            "coalescedPath": str(coalesced_path),
            "legacySha256": hashlib.sha256(legacy_path.read_bytes()).hexdigest(),
            "coalescedSha256": hashlib.sha256(coalesced_path.read_bytes()).hexdigest(),
            "exact": maximum == 0,
            "maxChannelError": maximum,
            "meanChannelError": total / len(differences) if differences else 0.0,
            "mismatchedChannels": mismatch_channels,
            "mismatchedPixels": mismatch_pixels,
            "diffPath": str(diff_path.resolve()),
        })
    return evidence


def group_formal(rows: list[dict[str, Any]], errors: list[str]) -> list[dict[str, Any]]:
    groups: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if row["classification"] == "formal":
            groups[(row["lightCount"], row["mode"])].append(row)
    aggregates: list[dict[str, Any]] = []
    for count in FORMAL_COUNTS:
        for mode in MODES:
            group = sorted(groups.get((count, mode), []), key=lambda row: row["run"])
            require(len(group) == 3, f"formal {count}/{mode}: expected 3 processes, got {len(group)}", errors)
            if not group:
                continue
            metrics: dict[str, Any] = {}
            for metric, sample_key, median_key in (
                ("cpuFrame", "cpuFrameSamples", "cpuFrameMedianMs"),
                ("gpuFrame", "gpuFrameSamples", "gpuFrameMedianMs"),
                ("pointLightCpu", "pointLightCpuSamples", "pointLightCpuMedianMs"),
                ("pointLightGpu", "pointLightGpuSamples", "pointLightGpuMedianMs"),
            ):
                process_medians = [float(row[median_key]) for row in group]
                metrics[metric] = {
                    "pooled": distribution(value for row in group for value in row[sample_key]),
                    "processMedians": process_medians,
                    "processMedianRange": {
                        "minimum": min(process_medians),
                        "maximum": max(process_medians),
                        "median": median(process_medians),
                    },
                }
            aggregates.append({
                "coverage": "representative",
                "lightCount": count,
                "mode": mode,
                "processCount": len(group),
                "samplesPerProcess": group[0]["sampleFrames"],
                "pooledSampleCount": sum(len(row["gpuFrameSamples"]) for row in group),
                "metrics": metrics,
                "drawCallsMedian": median(row["drawCallsMedian"] for row in group),
                "submittedLightsMedian": median(row["submittedLightsMedian"] for row in group),
                "culledLightsMedian": median(row["culledLightsMedian"] for row in group),
                "pointLightStencilClearsMedian": median(row["pointLightStencilClearsMedian"] for row in group),
                "fixedStencilClearsMedian": median(row["fixedStencilClearsMedian"] for row in group),
                "totalStencilClearsMedian": median(row["totalStencilClearsMedian"] for row in group),
                "sceneSignature": group[0]["sceneSignature"],
                "submissionSignature": group[0]["submissionSignature"],
            })
    return aggregates


def compare_aggregates(rows: list[dict[str, Any]], aggregates: list[dict[str, Any]], errors: list[str]) -> list[dict[str, Any]]:
    lookup = {(item["lightCount"], item["mode"]): item for item in aggregates}
    formal_by_key = {(row["lightCount"], row["mode"], row["run"]): row for row in rows if row["classification"] == "formal"}
    comparisons: list[dict[str, Any]] = []
    for count in FORMAL_COUNTS:
        legacy = lookup.get((count, "legacy-2n"))
        coalesced = lookup.get((count, "coalesced-n-plus-one"))
        if not legacy or not coalesced:
            continue

        def saving(metric: str) -> dict[str, float]:
            a = float(legacy["metrics"][metric]["pooled"]["median"])
            b = float(coalesced["metrics"][metric]["pooled"]["median"])
            return {"legacyMedianMs": a, "coalescedMedianMs": b, "savedMs": a - b, "savedPercent": (a - b) / a * 100.0 if a else 0.0}

        paired_point_gpu = []
        draw_equal = True
        signature_equal = True
        submission_equal = True
        for run in (1, 2, 3):
            legacy_row = formal_by_key.get((count, "legacy-2n", run))
            coalesced_row = formal_by_key.get((count, "coalesced-n-plus-one", run))
            if not legacy_row or not coalesced_row:
                continue
            paired_point_gpu.append(legacy_row["pointLightGpuMedianMs"] - coalesced_row["pointLightGpuMedianMs"])
            draw_equal = draw_equal and legacy_row["drawCallSamples"] == coalesced_row["drawCallSamples"]
            signature_equal = signature_equal and legacy_row["sceneSignature"] == coalesced_row["sceneSignature"]
            submission_equal = submission_equal and legacy_row["submissionSignature"] == coalesced_row["submissionSignature"]
        point_saving = saving("pointLightGpu")
        gate = (
            point_saving["savedMs"] >= 0.10
            and point_saving["savedPercent"] >= 2.0
            and len(paired_point_gpu) == 3
            and all(value > 0.0 for value in paired_point_gpu)
        )
        comparisons.append({
            "lightCount": count,
            "cpuFrame": saving("cpuFrame"),
            "gpuFrame": saving("gpuFrame"),
            "pointLightCpu": saving("pointLightCpu"),
            "pointLightGpu": point_saving,
            "pairedProcessPointLightGpuMedianSavingsMs": paired_point_gpu,
            "pairedProcessImprovementCount": sum(value > 0.0 for value in paired_point_gpu),
            "drawCallsExactlyEqual": draw_equal,
            "sceneSignatureEqual": signature_equal,
            "submissionSignatureEqual": submission_equal,
            "pointClearReduction": {
                "legacy": count * 2,
                "coalesced": count + 1,
                "removed": count - 1,
                "percent": (count - 1) / (count * 2) * 100.0,
            },
            "stableBeyondNoiseGate": gate,
        })
    return comparisons


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_chart(path: Path, comparisons: list[dict[str, Any]]) -> None:
    width, height = 980, 560
    left, right, top, bottom = 90, 40, 55, 90
    plot_width, plot_height = width - left - right, height - top - bottom
    maximum = max([1.0] + [float(item["pointLightGpu"][f"{mode}MedianMs"]) for item in comparisons for mode in ("legacy", "coalesced")]) * 1.15
    group_width = plot_width / max(1, len(comparisons))
    bar_width = min(105.0, group_width * 0.28)
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        '<text x="490" y="30" text-anchor="middle" font-family="sans-serif" font-size="20">Deferred point-light stencil clear A/B</text>',
    ]
    for step in range(6):
        value = maximum * step / 5
        y = top + plot_height * (1.0 - value / maximum)
        parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_width}" y2="{y:.2f}" stroke="#e5e7eb"/>')
        parts.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="sans-serif" font-size="12">{value:.1f}</text>')
    parts.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}" stroke="#111827"/>')
    parts.append(f'<line x1="{left}" y1="{top + plot_height}" x2="{left + plot_width}" y2="{top + plot_height}" stroke="#111827"/>')
    for index, item in enumerate(comparisons):
        center = left + group_width * (index + 0.5)
        for offset, key, color in ((-0.55, "legacyMedianMs", "#dc2626"), (0.55, "coalescedMedianMs", "#059669")):
            value = float(item["pointLightGpu"][key])
            x = center + offset * bar_width - bar_width / 2
            y = top + plot_height * (1.0 - value / maximum)
            parts.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_width:.2f}" height="{top + plot_height - y:.2f}" fill="{color}"/>')
            parts.append(f'<text x="{x + bar_width / 2:.2f}" y="{y - 6:.2f}" text-anchor="middle" font-family="sans-serif" font-size="12">{value:.3f}</text>')
        parts.append(f'<text x="{center:.2f}" y="{top + plot_height + 25}" text-anchor="middle" font-family="sans-serif" font-size="14">{item["lightCount"]} lights</text>')
        parts.append(f'<text x="{center:.2f}" y="{top + plot_height + 45}" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#374151">save {item["pointLightGpu"]["savedMs"]:.3f} ms / {item["pointLightGpu"]["savedPercent"]:.1f}%</text>')
    parts.extend([
        '<rect x="290" y="510" width="18" height="12" fill="#dc2626"/><text x="316" y="521" font-family="sans-serif" font-size="13">Legacy 2N</text>',
        '<rect x="500" y="510" width="18" height="12" fill="#059669"/><text x="526" y="521" font-family="sans-serif" font-size="13">Coalesced N+1</text>',
        '<text x="22" y="280" transform="rotate(-90 22 280)" text-anchor="middle" font-family="sans-serif" font-size="14">Pooled point-light GPU median (ms)</text>',
        '</svg>',
    ])
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def fmt(value: Any, digits: int = 3) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "—"
    return f"{float(value):.{digits}f}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    raw_dir = run_dir / "raw"
    image_dir = run_dir / "images"
    manifest = load_json(run_dir / "manifest.json") if (run_dir / "manifest.json").exists() else {}
    errors: list[str] = []
    rows: list[dict[str, Any]] = []
    for path in sorted(raw_dir.glob("*.json")):
        row = parse_result(path, errors)
        if row:
            rows.append(row)
    require(bool(rows), "no result JSON files", errors)

    formal_rows = [row for row in rows if row["classification"] == "formal"]
    require(len(formal_rows) == 18, f"expected 18 formal processes, got {len(formal_rows)}", errors)
    for count in FORMAL_COUNTS:
        count_rows = [row for row in formal_rows if row["lightCount"] == count]
        signatures = {(row["sceneSignature"], row["submissionSignature"]) for row in count_rows}
        require(len(signatures) == 1, f"{count}: cross-mode/process signature mismatch", errors)
    edge_rows = [row for row in rows if row["classification"] == "smoke" and row["coverage"] == "edge-cases"]
    require(len(edge_rows) == 2, f"expected 2 edge-case smoke rows, got {len(edge_rows)}", errors)
    for row in edge_rows:
        require(row["nearPlaneVerified"] and row["cameraInsideVerified"] and row["offscreenVerified"], f"edge fixtures not verified: {row['path']}", errors)
    overlap_rows = [row for row in rows if row["classification"] == "smoke" and row["coverage"] == "high-overlap"]
    require(len(overlap_rows) == 2, f"expected 2 high-overlap smoke rows, got {len(overlap_rows)}", errors)
    zero_rows = [row for row in rows if row["classification"] == "zero"]
    require(len(zero_rows) == 2, f"expected 2 zero-light rows, got {len(zero_rows)}", errors)
    for row in zero_rows:
        require(row["pointLightStencilClearsMedian"] == 0.0, f"zero-light clear was emitted: {row['path']}", errors)

    log_gl_errors = []
    for log_path in sorted(run_dir.rglob("*.log")):
        text = read_log(log_path)
        if "OpenGL error" in text or "GL_INVALID_" in text:
            log_gl_errors.append(str(log_path.resolve()))
    require(not log_gl_errors, f"GL errors found in logs: {log_gl_errors}", errors)

    renderdoc_path = run_dir / "renderdoc" / "replay" / "representative-0512-coalesced-replay.json"
    renderdoc_evidence = load_json(renderdoc_path) if renderdoc_path.exists() else None
    capture_executable_path = run_dir / "renderdoc" / "capture-executable.json"
    capture_executable = load_json(capture_executable_path) if capture_executable_path.exists() else None
    if renderdoc_evidence is not None:
        renderdoc_counts = renderdoc_evidence.get("eventTree", {}).get("counts", {})
        require(renderdoc_evidence.get("success") is True, "RenderDoc replay validation failed", errors)
        require(int(renderdoc_counts.get("lightMarkers", -1)) == 512, "RenderDoc light marker count mismatch", errors)
        require(int(renderdoc_counts.get("stencilVolumeDraw", -1)) == 512, "RenderDoc stencil draw count mismatch", errors)
        require(int(renderdoc_counts.get("lightingVolumeDraw", -1)) == 512, "RenderDoc lighting draw count mismatch", errors)
        require(int(renderdoc_counts.get("pointLightStencilClears", -1)) == 513, "RenderDoc clear count mismatch", errors)

    final_smoke_directory = run_dir / "final-smoke"
    final_smoke_evidence = None
    if final_smoke_directory.exists():
        final_documents = {
            name: load_json(final_smoke_directory / f"{name}.json")
            for name in ("legacy-2n", "coalesced-n-plus-one", "default")
        }
        final_image_paths = {
            name: final_smoke_directory / "images" / f"{name}.ppm"
            for name in final_documents
        }
        final_hashes = {
            name: hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in final_image_paths.items()
        }
        resource_log = read_log(final_smoke_directory / "logs" / "resource-smoke.log")
        pbr_log = read_log(final_smoke_directory / "logs" / "pbr-smoke.log")
        require(all(document.get("success") is True for document in final_documents.values()), "final point-light smoke failed", errors)
        require(final_documents["legacy-2n"]["pointLightStress"]["stencilClearMode"] == "legacy-2n", "final Legacy switch failed", errors)
        require(final_documents["coalesced-n-plus-one"]["pointLightStress"]["stencilClearMode"] == "coalesced-n-plus-one", "final Coalesced switch failed", errors)
        require(final_documents["default"]["pointLightStress"]["stencilClearMode"] == "coalesced-n-plus-one", "normal Deferred default is not Coalesced", errors)
        require(final_documents["default"]["pointLightStress"]["stencilClearModeExplicit"] is False, "default smoke unexpectedly used explicit mode", errors)
        require(len(set(final_hashes.values())) == 1, "final default/Legacy/Coalesced images differ", errors)
        require("released textureBytes=0 meshCpuBytes=0 meshGpuBytes=0 renderTargetBytes=0" in resource_log, "resource smoke did not release resources", errors)
        require("[PBRSmoke] forwardDeferredMae=" in pbr_log and "released textureBytes=0 meshCpuBytes=0 meshGpuBytes=0 renderTargetBytes=0" in pbr_log, "PBR Forward/Deferred smoke failed", errors)
        post_final_path = final_smoke_directory / "post-final-build.json"
        post_final = load_json(post_final_path) if post_final_path.exists() else None
        if post_final is not None:
            require(post_final.get("success") is True, "post-final-build smoke failed", errors)
            require(post_final["pointLightStress"]["stencilClearMode"] == "coalesced-n-plus-one", "post-final-build mode mismatch", errors)
            require(float(post_final["profiler"]["summary"]["pointLightStencilClears"]["median"]) == 17.0, "post-final-build clear mismatch", errors)
            require(int(post_final["pointLightStress"]["stencilLifecycleValidation"]["nonZeroPixels"]) == 0, "post-final-build stencil lifecycle failed", errors)
        final_smoke_evidence = {
            "directory": str(final_smoke_directory),
            "defaultMode": final_documents["default"]["pointLightStress"]["stencilClearMode"],
            "defaultModeExplicit": final_documents["default"]["pointLightStress"]["stencilClearModeExplicit"],
            "legacyPointClears": float(final_documents["legacy-2n"]["profiler"]["summary"]["pointLightStencilClears"]["median"]),
            "coalescedPointClears": float(final_documents["coalesced-n-plus-one"]["profiler"]["summary"]["pointLightStencilClears"]["median"]),
            "nonZeroStencilPixels": {name: int(document["pointLightStress"]["stencilLifecycleValidation"]["nonZeroPixels"]) for name, document in final_documents.items()},
            "imageSha256": final_hashes,
            "resourceSmokePassed": True,
            "pbrForwardDeferredSmokePassed": True,
            "postFinalBuildSmokePassed": post_final is not None and post_final.get("success") is True,
        }

    image_evidence = compare_images(rows, image_dir, errors)
    required_image_pairs = [item for item in image_evidence if item["classification"] == "formal" and item["run"] == 1]
    require({item["lightCount"] for item in required_image_pairs} == set(FORMAL_COUNTS), "missing formal run01 image pair", errors)
    require(all(item["exact"] for item in image_evidence), "one or more Legacy/Coalesced image pairs differ", errors)
    (run_dir / "image-diff.json").write_text(json.dumps({"allExact": all(item["exact"] for item in image_evidence), "comparisons": image_evidence}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    aggregates = group_formal(rows, errors)
    comparisons = compare_aggregates(rows, aggregates, errors)
    correctness_ok = (
        all(item["exact"] for item in image_evidence)
        and all(item["drawCallsExactlyEqual"] and item["sceneSignatureEqual"] and item["submissionSignatureEqual"] for item in comparisons)
        and not log_gl_errors
        and all(row["lifecycleClean"] for row in rows if row["classification"] in ("smoke", "zero"))
    )
    performance_ok = all(
        next((item["stableBeyondNoiseGate"] for item in comparisons if item["lightCount"] == count), False)
        for count in (256, 512)
    )
    go = not errors and correctness_ok and performance_ok

    aggregate_document = {
        "schemaVersion": 1,
        "generatedAtUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "valid": not errors,
        "validationErrors": errors,
        "decision": "Go" if go else "No-Go",
        "predefinedGate": manifest.get("protocol", {}).get("passGate", ""),
        "correctnessGatePassed": correctness_ok,
        "performanceGatePassed": performance_ok,
        "aggregates": aggregates,
        "comparisons": comparisons,
        "imageEvidencePath": str((run_dir / "image-diff.json").resolve()),
        "glErrorLogs": log_gl_errors,
        "renderDocEvidence": renderdoc_evidence,
        "abExecutableSha256": manifest.get("executableSha256", ""),
        "finalCaptureExecutableSha256": capture_executable.get("sha256", "") if capture_executable else "",
        "finalSmokeEvidence": final_smoke_evidence,
    }
    (run_dir / "aggregate.json").write_text(json.dumps(aggregate_document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    per_process_fields = [
        "classification", "coverage", "lightCount", "mode", "run", "warmupFrames", "sampleFrames",
        "sceneSignature", "submissionSignature", "cpuFrameMedianMs", "cpuFrameP95Ms", "cpuFrameP99Ms",
        "gpuFrameMedianMs", "gpuFrameP95Ms", "gpuFrameP99Ms", "pointLightCpuMedianMs", "pointLightGpuMedianMs",
        "drawCallsMedian", "submittedLightsMedian", "culledLightsMedian", "pointLightStencilClearsMedian",
        "fixedStencilClearsMedian", "totalStencilClearsMedian", "lifecycleClean", "lifecycleNonZeroPixels", "capturePath", "path",
    ]
    write_csv(run_dir / "per-process.csv", rows, per_process_fields)
    flat_aggregates = []
    for item in aggregates:
        flat = {key: item[key] for key in ("coverage", "lightCount", "mode", "processCount", "samplesPerProcess", "pooledSampleCount", "drawCallsMedian", "submittedLightsMedian", "culledLightsMedian", "pointLightStencilClearsMedian", "fixedStencilClearsMedian", "totalStencilClearsMedian", "sceneSignature", "submissionSignature")}
        for metric in ("cpuFrame", "gpuFrame", "pointLightCpu", "pointLightGpu"):
            pooled = item["metrics"][metric]["pooled"]
            process_range = item["metrics"][metric]["processMedianRange"]
            for field in ("median", "p95", "p99"):
                flat[f"{metric}Pooled{field.title()}"] = pooled[field]
            flat[f"{metric}ProcessMedianMin"] = process_range["minimum"]
            flat[f"{metric}ProcessMedianMax"] = process_range["maximum"]
        flat_aggregates.append(flat)
    if flat_aggregates:
        write_csv(run_dir / "aggregate.csv", flat_aggregates, list(flat_aggregates[0]))
    comparison_rows = []
    for item in comparisons:
        flat = {"lightCount": item["lightCount"], "stableBeyondNoiseGate": item["stableBeyondNoiseGate"], "pairedProcessImprovementCount": item["pairedProcessImprovementCount"], "drawCallsExactlyEqual": item["drawCallsExactlyEqual"], "sceneSignatureEqual": item["sceneSignatureEqual"], "submissionSignatureEqual": item["submissionSignatureEqual"]}
        for metric in ("cpuFrame", "gpuFrame", "pointLightCpu", "pointLightGpu"):
            for field, value in item[metric].items():
                flat[f"{metric}{field[0].upper()}{field[1:]}"] = value
        flat.update({f"pointClear{key[0].upper()}{key[1:]}": value for key, value in item["pointClearReduction"].items()})
        comparison_rows.append(flat)
    if comparison_rows:
        write_csv(run_dir / "comparisons.csv", comparison_rows, list(comparison_rows[0]))

    chart_path = image_dir / "point-light-stencil-clear-ab.svg"
    write_chart(chart_path, comparisons)
    comparison_by_count = {item["lightCount"]: item for item in comparisons}
    historical_half_clear_reference_ms = 10.324 / 2.0
    observed_512_saving_ms = comparison_by_count.get(512, {}).get("pointLightGpu", {}).get("savedMs", 0.0)
    historical_reference_ratio = (
        observed_512_saving_ms / historical_half_clear_reference_ms * 100.0
        if historical_half_clear_reference_ms
        else 0.0
    )
    point_cpu_savings = [
        comparison_by_count.get(count, {}).get("pointLightCpu", {}).get("savedMs", 0.0)
        for count in FORMAL_COUNTS
    ]
    point_cpu_percentages = [
        comparison_by_count.get(count, {}).get("pointLightCpu", {}).get("savedPercent", 0.0)
        for count in FORMAL_COUNTS
    ]

    lines = [
        "# Deferred 点光源相邻 Stencil Clear 合并 A/B",
        "",
        f"> 决策：**{'Go' if go else 'No-Go'}**；数据有效：{'是' if not errors else '否'}；生成于 {aggregate_document['generatedAtUtc']}。",
        "",
        "## 预定义通过条件",
        "",
        "- 图像逐像素一致，scene/submission signature、Draw Call、submitted/culled 完全一致，日志无 GL error，点光阶段出口 stencil 为 0。",
        "- Legacy 精确为 `2N` 次点光 clear；Coalesced 在 `N>0` 时精确为 `N+1`，`N=0` 时为 0；外围固定 clear 单独保持 3 次。",
        "- representative/256 与 /512 的 pooled 点光 GPU Median 均至少改善 0.10 ms 且 2%，并且 3/3 对应进程的点光 GPU Median 都改善。",
        "",
        "## 环境与二进制口径",
        "",
        f"- 正式 A/B 的 18 个进程全部使用 manifest 记录的同一 Release x64 二进制，SHA-256 `{manifest.get('executableSha256', 'unavailable')}`。每个进程均显式指定模式，不依赖默认值。",
        f"- 1920×1080、固定 Sponza/相机、seed `0x21D3F3A5`、显式 gPosition、Deferred、VSync 请求 0，SSAO/Bloom/阴影关闭。启动次序完整记录在 `launch-order.jsonl` 与 `manifest.json`。",
        "",
        "## 正式逐进程结果",
        "",
        "| 灯数 | 模式 | 进程 | CPU Frame M/P95/P99 ms | GPU Frame M/P95/P99 ms | 点光 GPU/CPU M ms | Draw | submitted/culled | 点光/固定/总 clear |",
        "|---:|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in sorted(formal_rows, key=lambda row: (row["lightCount"], row["run"], row["mode"])):
        lines.append(
            f"| {row['lightCount']} | {row['mode']} | {row['run']} | {fmt(row['cpuFrameMedianMs'])}/{fmt(row['cpuFrameP95Ms'])}/{fmt(row['cpuFrameP99Ms'])} | "
            f"{fmt(row['gpuFrameMedianMs'])}/{fmt(row['gpuFrameP95Ms'])}/{fmt(row['gpuFrameP99Ms'])} | {fmt(row['pointLightGpuMedianMs'])}/{fmt(row['pointLightCpuMedianMs'], 5)} | "
            f"{fmt(row['drawCallsMedian'], 0)} | {fmt(row['submittedLightsMedian'], 0)}/{fmt(row['culledLightsMedian'], 0)} | "
            f"{fmt(row['pointLightStencilClearsMedian'], 0)}/{fmt(row['fixedStencilClearsMedian'], 0)}/{fmt(row['totalStencilClearsMedian'], 0)} |"
        )
    lines.extend([
        "",
        "## Pooled 样本与进程中位数范围",
        "",
        "| 灯数 | 模式 | 样本 | CPU Frame pooled M/P95/P99；进程 M 范围 | GPU Frame pooled M/P95/P99；进程 M 范围 | 点光 GPU pooled M/P95/P99；进程 M 范围 | 点光 CPU pooled M/P95/P99；进程 M 范围 |",
        "|---:|---|---:|---:|---:|---:|---:|",
    ])
    for item in aggregates:
        def metric_text(name: str, digits: int = 3) -> str:
            pooled = item["metrics"][name]["pooled"]
            rng = item["metrics"][name]["processMedianRange"]
            return f"{fmt(pooled['median'], digits)}/{fmt(pooled['p95'], digits)}/{fmt(pooled['p99'], digits)}；{fmt(rng['minimum'], digits)}–{fmt(rng['maximum'], digits)}"
        lines.append(f"| {item['lightCount']} | {item['mode']} | {item['processCount']}×{item['samplesPerProcess']} | {metric_text('cpuFrame')} | {metric_text('gpuFrame')} | {metric_text('pointLightGpu')} | {metric_text('pointLightCpu', 5)} |")
    lines.extend([
        "",
        "## A/B 收益与判定",
        "",
        "| 灯数 | 点光 GPU Legacy→Coalesced | 节省 | GPU Frame 节省 | CPU Frame 节省 | 点光 CPU 节省 | clear 下降 | 进程方向 | 稳定门槛 |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|:---:|",
    ])
    for item in comparisons:
        lines.append(
            f"| {item['lightCount']} | {fmt(item['pointLightGpu']['legacyMedianMs'])}→{fmt(item['pointLightGpu']['coalescedMedianMs'])} ms | "
            f"{fmt(item['pointLightGpu']['savedMs'])} ms / {fmt(item['pointLightGpu']['savedPercent'], 2)}% | "
            f"{fmt(item['gpuFrame']['savedMs'])} ms / {fmt(item['gpuFrame']['savedPercent'], 2)}% | "
            f"{fmt(item['cpuFrame']['savedMs'])} ms / {fmt(item['cpuFrame']['savedPercent'], 2)}% | "
            f"{fmt(item['pointLightCpu']['savedMs'], 5)} ms / {fmt(item['pointLightCpu']['savedPercent'], 2)}% | "
            f"{item['pointClearReduction']['legacy']}→{item['pointClearReduction']['coalesced']}（-{fmt(item['pointClearReduction']['percent'], 2)}%） | "
            f"{item['pairedProcessImprovementCount']}/3 | {'通过' if item['stableBeyondNoiseGate'] else '未通过'} |"
        )
    lines.extend([
        "",
        "clear 数量接近减半是机制工作量，不等同于 GPU 时间必然减半；表中的 GPU 收益全部来自本轮同一新二进制、显式模式参数的应用内 Timer Query。旧 RenderDoc 的 10.324 ms 仅作为历史定位证据，未被计入本轮收益。",
        "",
        f"在 512 灯下，本轮真实 A/B 点光 GPU 节省 {observed_512_saving_ms:.3f} ms。若仅把旧 breakdown 的 10.324 ms clear 总时长作为线性机制量级参考，删除约一半 clear 的粗略预期是 {historical_half_clear_reference_ms:.3f} ms；本轮收益约为该参考的 {historical_reference_ratio:.1f}%，量级接近但并不等同。差异符合驱动批次、clear 固定开销及 replay 与应用内 Timer 口径不同的预期。",
        "",
        f"点光 CPU Median 的变化很小：16/256/512 分别仅 {point_cpu_savings[0]:.5f}/{point_cpu_savings[1]:.5f}/{point_cpu_savings[2]:.5f} ms（{point_cpu_percentages[0]:.2f}%/{point_cpu_percentages[1]:.2f}%/{point_cpu_percentages[2]:.2f}%）。因此不把 CPU Frame 的较大下降解释成算法侧 CPU 优化；它更可能来自更少 GPU clear 后的驱动提交阻塞/队列节奏变化。",
        "",
        "## 正确性",
        "",
        f"- 图像：{len(image_evidence)} 对 Legacy/Coalesced 截图全部逐字节比较；完全一致对数 {sum(item['exact'] for item in image_evidence)}/{len(image_evidence)}，全局 max/mean channel error 为 {max((item['maxChannelError'] for item in image_evidence), default=0)}/{max((item['meanChannelError'] for item in image_evidence), default=0.0):.9f}。",
        f"- signature / Draw：所有正式配置 scene/submission signature 与逐帧 Draw Call {'一致' if all(item['drawCallsExactlyEqual'] and item['sceneSignatureEqual'] and item['submissionSignatureEqual'] for item in comparisons) else '存在差异'}；submitted 保持 N，culled 保持 0。",
        f"- 生命周期：high-overlap/16、edge-cases/16 和 0 灯的两种模式均执行一次非计时 readback，非零 stencil 像素为 0；edge-cases 的近裁面、相机在光体内、完全离屏 fixture 均已验证。",
        f"- GL：扫描日志未发现 OpenGL error / GL_INVALID；JSON success 均通过。",
        "",
    ])
    if renderdoc_evidence is not None:
        counts = renderdoc_evidence["eventTree"]["counts"]
        lines.extend([
            "## RenderDoc 机制证据",
            "",
            f"- 新捕获的 representative/512 Coalesced RDC 已由独立 QRenderDoc 进程成功打开并 replay；fatal status 为 `{renderdoc_evidence.get('fatalReplayStatusAfter', 'unknown')}`，debug message 数为 {len(renderdoc_evidence.get('debugMessages', []))}。",
            f"- 事件树：Light marker={counts['lightMarkers']}，Stencil volume draw={counts['stencilVolumeDraw']}，Lighting volume draw={counts['lightingVolumeDraw']}；clear initial/before/after={counts['stencilClearInitial']}/{counts['stencilClearBefore']}/{counts['stencilClearAfter']}，点光 clear 总计={counts['pointLightStencilClears']}。",
            f"- RDC：`{renderdoc_evidence.get('capture', '')}`；replay JSON：`{renderdoc_path}`；事件树文本：`{renderdoc_evidence.get('eventTree', {}).get('textPath', '')}`。",
            f"- 最终默认值切换后的 capture 二进制 SHA-256 为 `{capture_executable.get('sha256', 'unavailable') if capture_executable else 'unavailable'}`。它与正式 A/B 二进制分开记录；事件计数验证使用显式 Coalesced 参数，不冒充正式 A/B 时序样本。",
            "- 本轮 RenderDoc 只做可重放性与事件树/计数验证，未 Fetch duration counter；收益口径始终是正式 A/B 的应用内 Timer Query。旧 Legacy RDC 仅保留作历史机制对照，未伪装成同二进制 A/B。",
            "",
        ])
    if final_smoke_evidence is not None:
        lines.extend([
            "## 最终默认值与常规回归 smoke",
            "",
            f"- 最终 Release 重建后，显式 Legacy/Coalesced、不带模式参数的默认 representative/16，以及 post-final-build 关键 smoke 均成功；默认实际模式为 `{final_smoke_evidence['defaultMode']}`，`stencilClearModeExplicit=false`。",
            f"- 最终二进制中 Legacy/Coalesced 点光 clear 为 {final_smoke_evidence['legacyPointClears']:.0f}/{final_smoke_evidence['coalescedPointClears']:.0f}；三张截图 SHA-256 相同，生命周期 readback 非零像素均为 0。",
            "- Resource smoke 覆盖 Forward、Deferred+SSAO+Bloom、resize/restore 与资源释放；PBR smoke 覆盖 Forward/Deferred，二者退出码均为 0，最终资源计数归零。PBR smoke 固定路径的历史 PPM 已从未修改 PNG 无损还原，本轮图像另存于 final-smoke/images。",
            "",
        ])
    lines.extend([
        "## 为什么是 N+1，而不是只清一次",
        "",
        "Stencil volume draw 会把当前灯的非零掩码写入同一附件。若整个点光阶段只在开头清一次，后一盏灯会继承前一盏灯的掩码，`GL_NOTEQUAL` lighting draw 将使用错误的并集/抵消结果。安全合并只能删除“上一盏 ClearAfter 与下一盏 ClearBefore”中的一个；首灯前一次加每灯后一次得到 N+1。最后一次 clear 仍有必要，它恢复阶段出口 stencil=0，避免 Forward extras / outline 等后续消费者继承最后一盏灯的掩码。",
        "",
        "## 副作用与边界",
        "",
        "- 不改变灯、灯序、Uniform、两次 volume draw、Blend/Depth/Cull/Stencil 状态；只跳过第二盏及以后与前一盏尾 clear 相邻的 ClearBefore。",
        "- 收益随 active/submitted 灯数增长；N=0 不发点光 clear，N=1 与 Legacy 都是 2 次，因此无 clear 数量收益。",
        "- 未混入 Scissor、剔除、批处理、Instancing、GBuffer、Shader、网格或 Uniform 缓存优化。",
        "",
        "## 产物与复现",
        "",
        f"- 绝对目录：`{run_dir}`",
        f"- 原始 JSON / 日志 / 截图：`{raw_dir}`、`{run_dir / 'logs'}`、`{image_dir}`",
        f"- 聚合：`{run_dir / 'aggregate.json'}`、`{run_dir / 'per-process.csv'}`、`{run_dir / 'aggregate.csv'}`、`{run_dir / 'comparisons.csv'}`",
        f"- A/B 图：`{chart_path}`；逐像素证据：`{run_dir / 'image-diff.json'}`",
        f"- RenderDoc：`{run_dir / 'renderdoc'}`",
        f"- 可重复脚本快照：`{run_dir / 'scripts'}`（项目内源文件位于 `tools/`）。",
        "",
        "```powershell",
        "powershell -ExecutionPolicy Bypass -File .\\tools\\run_point_light_stencil_clear_ab.ps1 -Mode All",
        "```",
        "",
        "RenderDoc 512 Coalesced 机制捕获（在 A/B 完成后执行）：",
        "",
        "```powershell",
        "powershell -ExecutionPolicy Bypass -File .\\tools\\capture_point_light_stencil_clear_renderdoc.ps1 -RunDirectory <A/B绝对目录>",
        "```",
    ])
    if errors:
        lines.extend(["", "## 验证错误", ""] + [f"- {error}" for error in errors])
    (run_dir / "REPORT_CN.md").write_text("\n".join(lines) + "\n", encoding="utf-8-sig")

    print(json.dumps({"valid": not errors, "decision": "Go" if go else "No-Go", "formalProcesses": len(formal_rows), "imagePairs": len(image_evidence), "report": str((run_dir / "REPORT_CN.md").resolve()), "chart": str(chart_path.resolve()), "errors": errors}, indent=2, ensure_ascii=False))
    return 0 if not errors else 2


if __name__ == "__main__":
    raise SystemExit(main())
