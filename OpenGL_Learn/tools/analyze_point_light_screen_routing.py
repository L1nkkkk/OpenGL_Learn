#!/usr/bin/env python3
"""Validate and aggregate the staged point-light screen-routing experiment."""

from __future__ import annotations

import argparse
import csv
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


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def percentile(values: Iterable[float], q: float) -> float | None:
    data = sorted(float(value) for value in values)
    if not data:
        return None
    position = q * (len(data) - 1)
    low = math.floor(position)
    high = math.ceil(position)
    weight = position - low
    return data[low] * (1.0 - weight) + data[high] * weight


def distribution(values: Iterable[float]) -> dict[str, float | int | None]:
    data = sorted(float(value) for value in values)
    if not data:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "p95": None,
            "p99": None,
            "minimum": None,
            "maximum": None,
        }
    return {
        "count": len(data),
        "mean": sum(data) / len(data),
        "median": percentile(data, 0.50),
        "p95": percentile(data, 0.95),
        "p99": percentile(data, 0.99),
        "minimum": data[0],
        "maximum": data[-1],
    }


def read_log(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    if raw[:4096].count(0) > max(8, len(raw[:4096]) // 8):
        return raw.decode("utf-16-le", errors="replace")
    return raw.decode("utf-8", errors="replace")


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    cursor = 0
    tokens: list[bytes] = []
    whitespace = b" \t\r\n"
    while len(tokens) < 4:
        while cursor < len(raw) and raw[cursor] in whitespace:
            cursor += 1
        if raw[cursor : cursor + 1] == b"#":
            cursor = raw.index(b"\n", cursor) + 1
            continue
        end = cursor
        while end < len(raw) and raw[end] not in whitespace:
            end += 1
        tokens.append(raw[cursor:end])
        cursor = end
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError(f"unsupported PPM: {path}")
    if raw[cursor : cursor + 2] == b"\r\n":
        cursor += 2
    else:
        cursor += 1
    width, height = int(tokens[1]), int(tokens[2])
    pixels = raw[cursor:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"PPM size mismatch: {path}")
    return width, height, pixels


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
        ">I", zlib.crc32(kind + payload) & 0xFFFFFFFF
    )


def write_png_rgb(path: Path, width: int, height: int, pixels: bytes) -> None:
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)
        rows.extend(pixels[y * stride : (y + 1) * stride])
    payload = b"\x89PNG\r\n\x1a\n"
    payload += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    payload += png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    payload += png_chunk(b"IEND", b"")
    path.write_bytes(payload)


def compare_images(control: Path, candidate: Path, diff_path: Path) -> dict[str, Any]:
    width, height, first = read_ppm(control)
    width2, height2, second = read_ppm(candidate)
    if (width, height) != (width2, height2):
        raise ValueError("image dimensions differ")
    histogram = [0] * 256
    maximum = 0
    total = 0
    different_pixels = 0
    diff = bytearray(len(first))
    for offset in range(0, len(first), 3):
        pixel_different = False
        for channel in range(3):
            error = abs(first[offset + channel] - second[offset + channel])
            histogram[error] += 1
            total += error
            maximum = max(maximum, error)
            pixel_different = pixel_different or error != 0
            diff[offset + channel] = min(255, error * 16)
        if pixel_different:
            different_pixels += 1
    sample_count = len(first)
    p95_target = math.ceil(sample_count * 0.95)
    cumulative = 0
    p95 = 0
    for value, count in enumerate(histogram):
        cumulative += count
        if cumulative >= p95_target:
            p95 = value
            break
    write_png_rgb(diff_path, width, height, bytes(diff))
    return {
        "control": str(control),
        "candidate": str(candidate),
        "controlSha256": hashlib.sha256(control.read_bytes()).hexdigest().upper(),
        "candidateSha256": hashlib.sha256(candidate.read_bytes()).hexdigest().upper(),
        "width": width,
        "height": height,
        "maxChannelError": maximum,
        "meanChannelError": total / sample_count,
        "p95ChannelError": p95,
        "differentPixels": different_pixels,
        "differentPixelRatio": different_pixels / (width * height),
        "exact": maximum == 0,
        "diff": str(diff_path),
    }


def zone_samples(data: dict[str, Any], kind: str, name: str) -> list[float]:
    return [
        float(value)
        for value in data.get("profiler", {}).get("samples", {}).get(kind, {}).get(name, [])
    ]


def scalar_samples(data: dict[str, Any], name: str) -> list[float]:
    return [
        float(value)
        for value in data.get("profiler", {}).get("samples", {}).get(name, [])
    ]


def mechanism_median(data: dict[str, Any], name: str) -> float:
    value = data.get("profiler", {}).get("summary", {}).get(name, {}).get("median")
    return float(value) if value is not None else math.nan


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True)
    args = parser.parse_args()
    run_dir = Path(args.run_dir).resolve()
    manifest = load_json(run_dir / "manifest.json")
    launch = manifest.get("launchOrder", [])
    errors: list[str] = []
    records: list[dict[str, Any]] = []
    forbidden_log = re.compile(
        r"GL_INVALID|OpenGL error|ERROR::SHADER|shader compilation failed|program link failed",
        re.IGNORECASE,
    )

    for entry in launch:
        json_path = Path(entry["json"])
        log_path = Path(entry["log"])
        image_path = Path(entry["capture"])
        data = load_json(json_path)
        point = data.get("pointLightStress", {})
        label = json_path.name
        expected_samples = int(entry["sampleFrames"])
        checks = [
            (data.get("success") is True, "success=false"),
            (int(data.get("schemaVersion", 0)) >= 25, "schema < 25"),
            (data.get("buildConfiguration") == "Release", "non-Release"),
            (data.get("architecture") == "x64", "non-x64"),
            (point.get("renderMode") == entry["renderMode"], "render mode mismatch"),
            (point.get("renderModeExplicit") is True, "render mode not explicit"),
            (point.get("stencilClearMode") == "coalesced-n-plus-one", "clear mode mismatch"),
            (point.get("stencilClearModeExplicit") is True, "clear mode not explicit"),
            (point.get("coverage") == entry["coverage"], "coverage mismatch"),
            (int(point.get("generatedLightCount", -1)) == int(entry["lightCount"]), "count mismatch"),
            (data.get("gBuffer", {}).get("positionMode") == "explicit", "not explicit gPosition"),
            (data.get("settings", {}).get("requestedSwapInterval") == 0, "VSync request mismatch"),
            (data.get("settings", {}).get("bloom") is False, "Bloom enabled"),
            (data.get("ssao", {}).get("enabled") is False, "SSAO enabled"),
            (data.get("settings", {}).get("shadowCastingLights") == 0, "shadows enabled"),
            (image_path.is_file(), "capture missing"),
        ]
        if entry["classification"] == "formal":
            checks.extend(
                [
                    (len(scalar_samples(data, "cpuFrame")) == expected_samples, "CPU samples incomplete"),
                    (len(scalar_samples(data, "gpuFrame")) == expected_samples, "GPU samples incomplete"),
                    (
                        len(zone_samples(data, "gpuZones", "Deferred Point Lights")) == expected_samples,
                        "point-light GPU samples incomplete",
                    ),
                    (
                        len(zone_samples(data, "cpuZones", "Deferred Point Lights")) == expected_samples,
                        "point-light CPU samples incomplete",
                    ),
                ]
            )
        if entry.get("lifecycleCheck"):
            lifecycle = point.get("stencilLifecycleValidation", {})
            checks.append((lifecycle.get("clean") is True, "stencil exit dirty"))
        if entry.get("boundsTelemetry") and entry["renderMode"] != "coalesced-volume":
            telemetry = point.get("boundsTelemetry", {})
            checks.extend(
                [
                    (telemetry.get("executed") is True, "bounds telemetry not executed"),
                    (
                        len(telemetry.get("records", [])) == int(entry["lightCount"]),
                        "bounds telemetry count mismatch",
                    ),
                ]
            )
        for condition, message in checks:
            if not condition:
                errors.append(f"{label}: {message}")
        if forbidden_log.search(read_log(log_path)):
            errors.append(f"{label}: GL/shader error found in log")
        records.append({"entry": entry, "data": data})

    quality = [record for record in records if record["entry"]["classification"] == "quality"]
    image_index: dict[tuple[str, str], Path] = {}
    quality_data: dict[tuple[str, str], dict[str, Any]] = {}
    for record in quality:
        entry = record["entry"]
        if entry["phase"] != "quality" or (entry["width"], entry["height"]) != (1920, 1080):
            continue
        key = (entry["coverage"], entry["renderMode"])
        image_index[key] = Path(entry["capture"])
        quality_data[key] = record["data"]

    image_diffs: list[dict[str, Any]] = []
    for coverage in ("small-local", "medium-local", "representative", "high-overlap", "edge-cases"):
        pairs = (
            ("phase-b", "coalesced-volume", "scissored-volume"),
            ("semantic-control", "coalesced-volume", "analytic-fullscreen"),
            ("oracle-volume", "analytic-fullscreen", "analytic-volume"),
            ("oracle-screen", "analytic-fullscreen", "analytic-screen"),
            ("switching", "analytic-volume", "analytic-screen"),
        )
        for family, control, candidate in pairs:
            if (coverage, control) not in image_index or (coverage, candidate) not in image_index:
                continue
            diff_path = run_dir / "images" / f"diff-{coverage}-{family}.png"
            result = compare_images(
                image_index[(coverage, control)],
                image_index[(coverage, candidate)],
                diff_path,
            )
            result.update({"coverage": coverage, "family": family})
            image_diffs.append(result)
            # The old mesh-defined Volume control intentionally differs from the
            # ideal-sphere oracle.  Record that isolated semantic delta without
            # treating it as a correctness failure for the unified candidates.
            if family != "semantic-control" and not result["exact"]:
                errors.append(
                    f"{coverage}/{family}: image mismatch max={result['maxChannelError']}"
                )

    culling_images: dict[tuple[int, str], Path] = {}
    for record in records:
        entry = record["entry"]
        if entry["classification"] == "formal" and entry["phase"] == "A-offscreen-culling":
            culling_images[(int(entry["run"]), entry["culling"])] = Path(entry["capture"])
    for run in sorted({key[0] for key in culling_images}):
        if (run, "off") not in culling_images or (run, "on") not in culling_images:
            continue
        result = compare_images(
            culling_images[(run, "off")],
            culling_images[(run, "on")],
            run_dir / "images" / f"diff-edge-cases-offscreen-culling-run{run:02d}.png",
        )
        result.update({"coverage": "edge-cases", "family": f"offscreen-culling-run{run:02d}"})
        image_diffs.append(result)
        if not result["exact"]:
            errors.append(
                f"edge-cases/offscreen-culling-run{run:02d}: image mismatch "
                f"max={result['maxChannelError']}"
            )
    with (run_dir / "image-diff.json").open("w", encoding="utf-8") as handle:
        json.dump({"comparisons": image_diffs}, handle, ensure_ascii=False, indent=2)

    bounds_summary: dict[str, Any] = {}
    for coverage in ("small-local", "medium-local", "representative", "high-overlap", "edge-cases"):
        data = quality_data.get((coverage, "analytic-screen"))
        if not data:
            continue
        telemetry = data["pointLightStress"]["boundsTelemetry"]["records"]
        classifications: dict[str, int] = defaultdict(int)
        reasons: dict[str, int] = defaultdict(int)
        for item in telemetry:
            classifications[item["classification"]] += 1
            reasons[item["fallbackReason"]] += 1
            x, y, width, height = item["pixelRect"]
            if x < 0 or y < 0 or width < 0 or height < 0 or x + width > 1920 or y + height > 1080:
                errors.append(f"{coverage}: out-of-viewport rect for light {item['sourceIndex']}")
        route_signatures: list[tuple[list[int], list[float]]] = []
        for mode in ("scissored-volume", "analytic-volume", "analytic-screen", "analytic-fullscreen"):
            mode_data = quality_data.get((coverage, mode))
            if not mode_data:
                continue
            mode_records = mode_data["pointLightStress"]["boundsTelemetry"].get("records", [])
            route_signatures.append(
                (
                    [int(item["stableLightId"]) for item in mode_records],
                    [float(item["radius"]) for item in mode_records],
                )
            )
        stable_ids_consistent = bool(route_signatures) and all(
            signature == route_signatures[0] for signature in route_signatures[1:]
        )
        if not stable_ids_consistent:
            errors.append(f"{coverage}: stable light ids/radii differ across routed modes")
        bounds_summary[coverage] = {
            "classifications": dict(classifications),
            "fallbackReasons": dict(reasons),
            "coverageRatio": distribution(item["coverageRatio"] for item in telemetry),
            "stableLightIdsAndRadiiConsistent": stable_ids_consistent,
            "records": telemetry,
        }
    with (run_dir / "bounds-telemetry.json").open("w", encoding="utf-8") as handle:
        json.dump(bounds_summary, handle, ensure_ascii=False, indent=2)

    aspect_summary: list[dict[str, Any]] = []
    for record in quality:
        entry, data = record["entry"], record["data"]
        if entry["phase"] != "bounds-aspect":
            continue
        width, height = int(entry["width"]), int(entry["height"])
        telemetry = data["pointLightStress"]["boundsTelemetry"]["records"]
        for item in telemetry:
            x, y, rect_width, rect_height = item["pixelRect"]
            if (
                x < 0
                or y < 0
                or rect_width < 0
                or rect_height < 0
                or x + rect_width > width
                or y + rect_height > height
            ):
                errors.append(
                    f"{width}x{height}: out-of-viewport rect for light {item['sourceIndex']}"
                )
        aspect_summary.append(
            {
                "width": width,
                "height": height,
                "recordCount": len(telemetry),
                "coverageRatio": distribution(item["coverageRatio"] for item in telemetry),
                "stableLightIds": [int(item["stableLightId"]) for item in telemetry],
                "radii": [float(item["radius"]) for item in telemetry],
            }
        )
    if aspect_summary:
        reference_ids = aspect_summary[0]["stableLightIds"]
        reference_radii = aspect_summary[0]["radii"]
        for item in aspect_summary[1:]:
            if item["stableLightIds"] != reference_ids or item["radii"] != reference_radii:
                errors.append(f"{item['width']}x{item['height']}: ids/radii changed after resize")
    with (run_dir / "bounds-aspect-telemetry.json").open("w", encoding="utf-8") as handle:
        json.dump(aspect_summary, handle, ensure_ascii=False, indent=2)

    formal = [record for record in records if record["entry"]["classification"] == "formal"]
    metric_names = (
        "cpuFrame",
        "gpuFrame",
        "pointCpu",
        "pointGpu",
        "boundsCpu",
        "selectorCpu",
        "volumeCpu",
        "screenCpu",
        "volumeGpu",
        "screenGpu",
    )
    process_rows: list[dict[str, Any]] = []
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in formal:
        entry, data = record["entry"], record["data"]
        metrics = {
            "cpuFrame": scalar_samples(data, "cpuFrame"),
            "gpuFrame": scalar_samples(data, "gpuFrame"),
            "pointCpu": zone_samples(data, "cpuZones", "Deferred Point Lights"),
            "pointGpu": zone_samples(data, "gpuZones", "Deferred Point Lights"),
            "boundsCpu": zone_samples(data, "cpuZones", "Point Light Bounds"),
            "selectorCpu": zone_samples(data, "cpuZones", "Point Light Selector"),
            "volumeCpu": zone_samples(data, "cpuZones", "Point Light Volume CPU"),
            "screenCpu": zone_samples(data, "cpuZones", "Point Light Screen CPU"),
            "volumeGpu": zone_samples(data, "gpuZones", "Point Light Volume GPU"),
            "screenGpu": zone_samples(data, "gpuZones", "Point Light Screen GPU"),
        }
        row: dict[str, Any] = {
            "phase": entry["phase"],
            "coverage": entry["coverage"],
            "lightCount": entry["lightCount"],
            "renderMode": entry["renderMode"],
            "culling": entry["culling"],
            "run": entry["run"],
            "json": entry["json"],
            "sceneSignature": data["pointLightStress"].get("sceneSignature"),
            "submissionSignature": data["pointLightStress"].get("submissionSignature"),
        }
        for name in metric_names:
            stats = distribution(metrics[name])
            for field in ("median", "p95", "p99"):
                row[f"{name}_{field}"] = stats[field]
        for name in (
            "drawCalls",
            "pointLightsSubmitted",
            "pointLightsCulled",
            "pointLightStencilClears",
            "fixedStencilClears",
            "stencilClears",
            "pointLightBoundsRect",
            "pointLightBoundsOutside",
            "pointLightBoundsFullscreenFallback",
            "pointLightVolumeCount",
            "pointLightScreenCount",
            "pointLightStencilDraws",
            "pointLightLightingVolumeDraws",
            "pointLightScreenDraws",
            "pointLightRectPixelArea",
            "pointLightStencilClearPixelArea",
        ):
            row[name] = mechanism_median(data, name)
        row["_metrics"] = metrics
        process_rows.append(row)
        grouped[(entry["phase"], entry["coverage"], entry["lightCount"], entry["renderMode"], entry["culling"])].append(row)

    aggregate_rows: list[dict[str, Any]] = []
    aggregate_lookup: dict[tuple[Any, ...], dict[str, Any]] = {}
    for key, rows in sorted(grouped.items()):
        phase, coverage, count, mode, culling = key
        aggregate: dict[str, Any] = {
            "phase": phase,
            "coverage": coverage,
            "lightCount": count,
            "renderMode": mode,
            "culling": culling,
            "processCount": len(rows),
            "samplesPerProcess": int(records[0]["entry"]["sampleFrames"]) if records else 0,
            "metrics": {},
            "mechanism": {},
        }
        for name in metric_names:
            pooled = [value for row in rows for value in row["_metrics"][name]]
            process_medians = [row[f"{name}_median"] for row in rows if row[f"{name}_median"] is not None]
            aggregate["metrics"][name] = {
                "pooled": distribution(pooled),
                "processMedianRange": {
                    "minimum": min(process_medians) if process_medians else None,
                    "maximum": max(process_medians) if process_medians else None,
                },
            }
        for name in (
            "drawCalls",
            "pointLightsSubmitted",
            "pointLightsCulled",
            "pointLightStencilClears",
            "fixedStencilClears",
            "stencilClears",
            "pointLightBoundsRect",
            "pointLightBoundsOutside",
            "pointLightBoundsFullscreenFallback",
            "pointLightVolumeCount",
            "pointLightScreenCount",
            "pointLightStencilDraws",
            "pointLightLightingVolumeDraws",
            "pointLightScreenDraws",
            "pointLightRectPixelArea",
            "pointLightStencilClearPixelArea",
        ):
            aggregate["mechanism"][name] = median(row[name] for row in rows)
        aggregate_rows.append(aggregate)
        aggregate_lookup[key] = aggregate

    comparison_specs: list[tuple[str, str, int, str, str, str, str]] = [
        ("A-bounds-overhead", "representative", 16, "coalesced-volume", "off", "bounds-volume", "off"),
        ("A-offscreen-culling", "edge-cases", 16, "bounds-volume", "off", "bounds-volume", "on"),
    ]
    comparison_specs.extend(
        ("B-scissor", "representative", count, "coalesced-volume", "off", "scissored-volume", "off")
        for count in (16, 256, 512)
    )
    comparison_specs.append(
        (
            "C0-semantic-unification",
            "representative",
            256,
            "scissored-volume",
            "off",
            "analytic-volume",
            "off",
        )
    )
    comparison_specs.extend(
        ("C-fixed-path", coverage, 256, "analytic-volume", "off", "analytic-screen", "off")
        for coverage in ("small-local", "medium-local", "representative", "high-overlap")
    )
    comparisons: list[dict[str, Any]] = []
    for phase, coverage, count, control_mode, control_cull, candidate_mode, candidate_cull in comparison_specs:
        control_key = (phase, coverage, count, control_mode, control_cull)
        candidate_key = (phase, coverage, count, candidate_mode, candidate_cull)
        if control_key not in aggregate_lookup or candidate_key not in aggregate_lookup:
            continue
        control = aggregate_lookup[control_key]
        candidate = aggregate_lookup[candidate_key]
        control_rows = grouped[control_key]
        candidate_rows = grouped[candidate_key]
        control_by_run = {row["run"]: row for row in control_rows}
        candidate_by_run = {row["run"]: row for row in candidate_rows}
        directions = []
        for run in sorted(set(control_by_run) & set(candidate_by_run)):
            directions.append(candidate_by_run[run]["pointGpu_median"] < control_by_run[run]["pointGpu_median"])
        control_gpu = control["metrics"]["pointGpu"]["pooled"]["median"]
        candidate_gpu = candidate["metrics"]["pointGpu"]["pooled"]["median"]
        improvement = control_gpu - candidate_gpu
        scene_signature_match = all(
            control_by_run[run]["sceneSignature"] == candidate_by_run[run]["sceneSignature"]
            for run in sorted(set(control_by_run) & set(candidate_by_run))
        )
        submission_signature_match = all(
            control_by_run[run]["submissionSignature"] == candidate_by_run[run]["submissionSignature"]
            for run in sorted(set(control_by_run) & set(candidate_by_run))
        )
        if not scene_signature_match:
            errors.append(f"{phase}/{coverage}/{count}: scene signature mismatch")
        if phase != "A-offscreen-culling" and not submission_signature_match:
            errors.append(f"{phase}/{coverage}/{count}: submission signature mismatch")
        comparisons.append(
            {
                "phase": phase,
                "coverage": coverage,
                "lightCount": count,
                "controlMode": control_mode,
                "controlCulling": control_cull,
                "candidateMode": candidate_mode,
                "candidateCulling": candidate_cull,
                "controlPointGpuMedianMs": control_gpu,
                "candidatePointGpuMedianMs": candidate_gpu,
                "pointGpuImprovementMs": improvement,
                "pointGpuImprovementPercent": improvement / control_gpu * 100.0,
                "controlPointCpuMedianMs": control["metrics"]["pointCpu"]["pooled"]["median"],
                "candidatePointCpuMedianMs": candidate["metrics"]["pointCpu"]["pooled"]["median"],
                "controlDrawCalls": control["mechanism"]["drawCalls"],
                "candidateDrawCalls": candidate["mechanism"]["drawCalls"],
                "controlSubmittedLights": control["mechanism"]["pointLightsSubmitted"],
                "candidateSubmittedLights": candidate["mechanism"]["pointLightsSubmitted"],
                "sceneSignatureMatch": scene_signature_match,
                "submissionSignatureMatch": submission_signature_match,
                "sameDirectionCount": sum(directions),
                "pairedProcessCount": len(directions),
                "allProcessesImprove": bool(directions) and all(directions),
            }
        )

    exact_phase_b = all(item["exact"] for item in image_diffs if item["family"] == "phase-b") and any(
        item["family"] == "phase-b" for item in image_diffs
    )
    exact_analytic = all(
        item["exact"] for item in image_diffs if item["family"] in ("oracle-volume", "oracle-screen", "switching")
    ) and any(item["family"] == "oracle-screen" for item in image_diffs)
    scissor_key = [item for item in comparisons if item["phase"] == "B-scissor" and item["lightCount"] in (256, 512)]
    screen_key = [item for item in comparisons if item["phase"] == "C-fixed-path"]
    scissor_go = exact_phase_b and bool(scissor_key) and all(
        item["allProcessesImprove"] and item["pointGpuImprovementPercent"] >= 3.0 for item in scissor_key
    )
    screen_go = exact_analytic and len(screen_key) == 4 and all(
        item["allProcessesImprove"] and item["pointGpuImprovementPercent"] >= 3.0 for item in screen_key
    )
    crossover_exists = False
    if len(screen_key) == 4:
        by_coverage = {item["coverage"]: item for item in screen_key}
        crossover_exists = (
            by_coverage["small-local"]["pointGpuImprovementMs"] > 0.0
            and by_coverage["high-overlap"]["pointGpuImprovementMs"] < 0.0
        )
    decisions = {
        "scissor": "Go" if scissor_go else "No-Go",
        "screen": "Go" if screen_go else "No-Go",
        "adaptive": "Not-Implemented/No-Go" if not crossover_exists else "Requires-Mixed-Coverage-Gate",
        "exactPhaseB": exact_phase_b,
        "exactAnalyticPaths": exact_analytic,
        "stableCrossoverExists": crossover_exists,
        "adaptiveGainVsBestFixedMs": None,
        "adaptiveGainVsBestFixedPercent": None,
    }

    serializable_process_rows = []
    for row in process_rows:
        serializable_process_rows.append({key: value for key, value in row.items() if key != "_metrics"})
    aggregate_payload = {
        "schemaVersion": 1,
        "runDirectory": str(run_dir),
        "executableSha256": manifest.get("executableSha256"),
        "errors": errors,
        "valid": not errors,
        "processes": serializable_process_rows,
        "aggregates": aggregate_rows,
        "comparisons": comparisons,
        "bounds": bounds_summary,
        "boundsAspects": aspect_summary,
        "imageDiffs": image_diffs,
        "decisions": decisions,
    }
    with (run_dir / "aggregate.json").open("w", encoding="utf-8") as handle:
        json.dump(aggregate_payload, handle, ensure_ascii=False, indent=2)

    process_fields = sorted({key for row in serializable_process_rows for key in row})
    with (run_dir / "per-process.csv").open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=process_fields)
        writer.writeheader()
        writer.writerows(serializable_process_rows)
    aggregate_csv: list[dict[str, Any]] = []
    for item in aggregate_rows:
        row = {key: item[key] for key in ("phase", "coverage", "lightCount", "renderMode", "culling", "processCount")}
        for metric in ("cpuFrame", "gpuFrame", "pointCpu", "pointGpu", "boundsCpu", "selectorCpu", "volumeGpu", "screenGpu"):
            pooled = item["metrics"][metric]["pooled"]
            row[f"{metric}Median"] = pooled["median"]
            row[f"{metric}P95"] = pooled["p95"]
            row[f"{metric}P99"] = pooled["p99"]
            row[f"{metric}ProcessMedianMin"] = item["metrics"][metric]["processMedianRange"]["minimum"]
            row[f"{metric}ProcessMedianMax"] = item["metrics"][metric]["processMedianRange"]["maximum"]
        row.update(item["mechanism"])
        aggregate_csv.append(row)
    aggregate_fields = sorted({key for row in aggregate_csv for key in row})
    with (run_dir / "aggregate.csv").open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=aggregate_fields)
        writer.writeheader()
        writer.writerows(aggregate_csv)
    comparison_fields = sorted({key for row in comparisons for key in row})
    with (run_dir / "comparisons.csv").open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=comparison_fields)
        if comparison_fields:
            writer.writeheader()
            writer.writerows(comparisons)

    if comparisons:
        width = 1180
        height = 90 + len(comparisons) * 48
        maximum = max(max(item["controlPointGpuMedianMs"], item["candidatePointGpuMedianMs"]) for item in comparisons)
        lines = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
            '<rect width="100%" height="100%" fill="#0b1020"/>',
            '<style>text{font-family:Segoe UI,Arial,sans-serif;fill:#e8edf7}.label{font-size:13px}.value{font-size:12px;fill:#b8c2d8}</style>',
            '<text x="24" y="30" font-size="20">Point-light fixed-path A/B — pooled GPU median</text>',
        ]
        for index, item in enumerate(comparisons):
            y = 58 + index * 48
            label = f"{item['phase']} {item['coverage']}/{item['lightCount']}"
            control_width = 650 * item["controlPointGpuMedianMs"] / maximum
            candidate_width = 650 * item["candidatePointGpuMedianMs"] / maximum
            lines.extend(
                [
                    f'<text class="label" x="24" y="{y + 13}">{label}</text>',
                    f'<rect x="300" y="{y}" width="{control_width:.2f}" height="15" fill="#d97757"/>',
                    f'<rect x="300" y="{y + 19}" width="{candidate_width:.2f}" height="15" fill="#43b5a0"/>',
                    f'<text class="value" x="{310 + max(control_width, candidate_width):.2f}" y="{y + 13}">{item["controlPointGpuMedianMs"]:.3f} → {item["candidatePointGpuMedianMs"]:.3f} ms ({item["pointGpuImprovementPercent"]:.2f}%)</text>',
                ]
            )
        lines.append("</svg>")
        (run_dir / "images" / "point-light-screen-routing-ab.svg").write_text("\n".join(lines), encoding="utf-8")

    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 2
    print(json.dumps(decisions, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
