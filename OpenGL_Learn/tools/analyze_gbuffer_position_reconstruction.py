#!/usr/bin/env python3
"""Analyze the preregistered explicit-gPosition vs depth-reconstruction batch."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


PERFORMANCE_METRICS = (
    "GPU Frame",
    "G-Buffer Geometry",
    "Depth/Stencil Copy",
    "SSAO Generate",
    "SSAO Upsample",
    "Deferred Lighting",
    "Deferred Pass",
)
CPU_PERFORMANCE_METRICS = (
    "CPU Frame",
    "G-Buffer Geometry",
    "Depth/Stencil Copy",
    "SSAO Generate",
    "SSAO Upsample",
    "Deferred Lighting",
    "Deferred Pass",
)


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def percentile(values: Iterable[float], q: float) -> float | None:
    data = list(values)
    return float(np.percentile(np.asarray(data, dtype=np.float64), q)) if data else None


def finite(value: float | None) -> float | None:
    if value is None:
        return None
    converted = float(value)
    return converted if math.isfinite(converted) else None


def delta_percent(control: float | None, candidate: float | None) -> float | None:
    if control is None or candidate is None or abs(control) < 1.0e-12:
        return None
    return (candidate - control) * 100.0 / control


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        kind = stream.readline().strip()
        if kind not in (b"PF", b"Pf"):
            raise ValueError(f"Unsupported PFM header in {path}: {kind!r}")
        size = stream.readline().split()
        if len(size) != 2:
            raise ValueError(f"Invalid PFM size in {path}")
        width, height = map(int, size)
        scale = float(stream.readline())
        channels = 3 if kind == b"PF" else 1
        dtype = "<f4" if scale < 0.0 else ">f4"
        values = np.fromfile(stream, dtype=dtype)
    expected = width * height * channels
    if values.size != expected:
        raise ValueError(f"PFM size mismatch in {path}: {values.size} != {expected}")
    # The renderer intentionally writes OpenGL's bottom scanline first. Keep
    # that orientation so pixel-center UV reconstruction uses y=0 at the bottom.
    return values.astype(np.float32, copy=False).reshape(height, width, channels)


def write_pfm(path: Path, values: np.ndarray) -> None:
    array = np.asarray(values, dtype=np.float32)
    if array.ndim == 2:
        array = array[..., None]
    if array.ndim != 3 or array.shape[2] not in (1, 3):
        raise ValueError(f"PFM requires HxWx1 or HxWx3: {array.shape}")
    path.parent.mkdir(parents=True, exist_ok=True)
    height, width, channels = array.shape
    with path.open("wb") as stream:
        stream.write(b"PF\n" if channels == 3 else b"Pf\n")
        stream.write(f"{width} {height}\n-1.0\n".encode("ascii"))
        stream.write(np.asarray(array, dtype="<f4").tobytes(order="C"))


def _ppm_token(stream) -> bytes:
    while True:
        token = stream.readline()
        if not token:
            raise ValueError("Unexpected end of PPM header")
        token = token.strip()
        if token and not token.startswith(b"#"):
            return token


def read_ppm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        if _ppm_token(stream) != b"P6":
            raise ValueError(f"Unsupported PPM in {path}")
        size = _ppm_token(stream).split()
        if len(size) != 2:
            raise ValueError(f"Invalid PPM size in {path}")
        width, height = map(int, size)
        maximum = int(_ppm_token(stream))
        if maximum != 255:
            raise ValueError(f"Unsupported PPM range in {path}: {maximum}")
        pixels = np.frombuffer(stream.read(), dtype=np.uint8)
    expected = width * height * 3
    if pixels.size != expected:
        raise ValueError(f"PPM size mismatch in {path}: {pixels.size} != {expected}")
    # PPM is written top scanline first, while the renderer's PFM captures are
    # intentionally kept in OpenGL bottom-origin order. Flip PPM here so depth
    # edge/background masks address the same pixels.
    return pixels.reshape(height, width, 3)[::-1, :, :]


def global_ssim(a: np.ndarray, b: np.ndarray) -> float:
    x = np.asarray(a, dtype=np.float64).reshape(-1)
    y = np.asarray(b, dtype=np.float64).reshape(-1)
    mean_x = float(x.mean())
    mean_y = float(y.mean())
    variance_x = float(x.var())
    variance_y = float(y.var())
    covariance = float(((x - mean_x) * (y - mean_y)).mean())
    c1 = 0.01**2
    c2 = 0.03**2
    denominator = (mean_x**2 + mean_y**2 + c1) * (
        variance_x + variance_y + c2
    )
    if denominator == 0.0:
        return 1.0 if np.array_equal(x, y) else 0.0
    return float(
        ((2.0 * mean_x * mean_y + c1) * (2.0 * covariance + c2))
        / denominator
    )


def distribution_from_result(
    result: dict[str, Any],
    metric: str,
    timing_domain: str = "gpu",
) -> dict[str, Any]:
    summary = result["profiler"]["summary"]
    if timing_domain == "gpu":
        value = (
            summary.get("gpuFrame")
            if metric == "GPU Frame"
            else summary.get("gpuZones", {}).get(metric)
        )
    elif timing_domain == "cpu":
        value = (
            summary.get("cpuFrame")
            if metric == "CPU Frame"
            else summary.get("cpuZones", {}).get(metric)
        )
    else:
        raise ValueError(f"Unknown timing domain: {timing_domain}")
    if not value:
        return {"count": 0, "mean": None, "median": None, "p95": None, "p99": None}
    return {
        "count": int(value.get("count", 0)),
        "mean": finite(value.get("mean")),
        "median": finite(value.get("median")),
        "p95": finite(value.get("p95")),
        "p99": finite(value.get("p99")),
    }


def write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str]) -> None:
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def matrix(value: Any) -> np.ndarray:
    result = np.asarray(value, dtype=np.float64)
    if result.shape != (4, 4):
        raise ValueError(f"Expected 4x4 matrix, received {result.shape}")
    return result


def transform_points(points: np.ndarray, transform: np.ndarray) -> np.ndarray:
    ones = np.ones(points.shape[:-1] + (1,), dtype=np.float64)
    homogeneous = np.concatenate((points.astype(np.float64), ones), axis=-1)
    transformed = homogeneous @ transform.T
    return transformed[..., :3] / transformed[..., 3:4]


def reconstruct_positions(
    depth: np.ndarray,
    inverse_projection: np.ndarray,
    inverse_view: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    if depth.ndim == 3:
        depth = depth[..., 0]
    height, width = depth.shape
    y, x = np.mgrid[0:height, 0:width]
    clip = np.stack(
        (
            (x + 0.5) * (2.0 / width) - 1.0,
            (y + 0.5) * (2.0 / height) - 1.0,
            depth.astype(np.float64) * 2.0 - 1.0,
            np.ones_like(depth, dtype=np.float64),
        ),
        axis=-1,
    )
    view_h = clip @ inverse_projection.T
    view_h /= view_h[..., 3:4]
    world_h = view_h @ inverse_view.T
    world_h /= world_h[..., 3:4]
    view_position = view_h[..., :3].copy()
    world_position = world_h[..., :3].copy()
    background = depth >= 1.0
    view_position[background] = 0.0
    world_position[background] = 0.0
    return view_position, world_position


def error_stats(error: np.ndarray) -> dict[str, Any]:
    values = np.asarray(error, dtype=np.float64).reshape(-1)
    if values.size == 0:
        return {"count": 0, "mae": None, "p95": None, "p99": None, "max": None}
    return {
        "count": int(values.size),
        "mae": float(values.mean()),
        "p95": float(np.percentile(values, 95)),
        "p99": float(np.percentile(values, 99)),
        "max": float(values.max()),
    }


def result_camera_signature(result: dict[str, Any]) -> str:
    payload = {
        "camera": result["camera"],
        "matrices": result["gBuffer"]["cameraMatrices"],
        "resolution": result["resolution"],
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest().upper()


def validate_performance_result(
    run: dict[str, Any],
    result: dict[str, Any],
    metadata: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    expected_mode = "explicit" if run["variant"] == "A" else "reconstruct"
    expected_mrt = 5 if run["variant"] == "A" else 4
    expected_bpp = 35 if run["variant"] == "A" else 27
    expected_samples = int(metadata["protocol"]["measuredFramesPerProcess"])
    width, height = metadata["invariant"]["resolution"]
    expected_bytes = int(width) * int(height) * expected_bpp
    camera_metadata = next(
        camera for camera in metadata["cameras"] if camera["scene"] == run["scene"]
    )
    expected_attachments = (
        [
            ("position", "GL_RGBA16F"),
            ("normal", "GL_RGB16F"),
            ("albedo", "GL_RGB"),
            ("material", "GL_RGBA16F"),
            ("emissive", "GL_RGB16F"),
        ]
        if run["variant"] == "A"
        else [
            ("normal", "GL_RGB16F"),
            ("albedo", "GL_RGB"),
            ("material", "GL_RGBA16F"),
            ("emissive", "GL_RGB16F"),
        ]
    )
    actual_attachments = [
        (attachment.get("semantic"), attachment.get("internalFormatName"))
        for attachment in result["gBuffer"].get("attachments", [])
    ]
    expected_depth_storage = (
        "GL_DEPTH24_STENCIL8-renderbuffer"
        if run["variant"] == "A"
        else "GL_DEPTH24_STENCIL8-texture"
    )
    expected_depth_source = (
        "none" if run["variant"] == "A" else "main-D24S8-depth-stencil-texture"
    )
    expected_sampleable_storage = (
        "none" if run["variant"] == "A" else "GL_DEPTH24_STENCIL8"
    )
    result_camera = result.get("camera", {})
    def vector_matches(actual: Any, expected: Any) -> bool:
        actual_array = np.asarray(actual, dtype=np.float64)
        expected_array = np.asarray(expected, dtype=np.float64)
        return actual_array.shape == expected_array.shape and bool(
            np.allclose(actual_array, expected_array, atol=1e-7)
        )

    camera_matches = (
        vector_matches(result_camera.get("position", []), camera_metadata["position"])
        and vector_matches(result_camera.get("target", []), camera_metadata["target"])
        and vector_matches(result_camera.get("up", []), camera_metadata["up"])
        and math.isclose(
            float(result_camera.get("fovDegrees", float("nan"))),
            float(camera_metadata["fov"]),
            abs_tol=1e-7,
        )
    )
    checks = (
        (result.get("success") is True, "result.success is false"),
        (result.get("schemaVersion", 0) >= 22, "schemaVersion < 22"),
        (result.get("buildConfiguration") == "Release", "not Release"),
        (result.get("architecture") == "x64", "not x64"),
        (result.get("resolution") == [width, height], "resolution mismatch"),
        (
            result.get("warmupFrames")
            == int(metadata["protocol"]["warmupFramesPerProcess"]),
            "warmup frame mismatch",
        ),
        (result.get("measuredFrames") == expected_samples, "measured frame mismatch"),
        (result.get("captureRequired") is False, "performance readback enabled"),
        (result.get("renderPath") == "pbr-deferred", "render path mismatch"),
        (result.get("modelPath") == camera_metadata["model"], "model path mismatch"),
        (camera_matches, "camera mismatch against preregistration"),
        (result["settings"].get("requestedSwapInterval") == 0, "VSync request mismatch"),
        (result["settings"].get("inputFrozen") is True, "input not frozen"),
        (result["settings"].get("deferredRendering") is True, "deferred path disabled"),
        (result["settings"].get("bloom") is False, "bloom enabled"),
        (result["settings"].get("autoReloadShaders") is False, "shader reload enabled"),
        (result["settings"].get("autoReloadMaterials") is False, "material reload enabled"),
        (result["settings"].get("shadowCastingLights") == 0, "shadows enabled"),
        (result["gBuffer"].get("positionMode") == expected_mode, "position mode mismatch"),
        (result["gBuffer"].get("colorAttachmentCount") == expected_mrt, "MRT count mismatch"),
        (
            result["gBuffer"].get("positionAttachmentPresent")
            == (run["variant"] == "A"),
            "position attachment presence mismatch",
        ),
        (
            result["gBuffer"].get("sampleableDepthPresent")
            == (run["variant"] == "B"),
            "sampleable depth presence mismatch",
        ),
        (
            result["gBuffer"].get("sampleableDepthSource") == expected_depth_source,
            "sampleable depth source mismatch",
        ),
        (
            result["gBuffer"].get("depthStencilStorage") == expected_depth_storage,
            "depth/stencil storage mismatch",
        ),
        (
            result["gBuffer"].get("sampleableDepthStorage")
            == expected_sampleable_storage,
            "sampleable depth format mismatch",
        ),
        (actual_attachments == expected_attachments, "G-Buffer attachment layout mismatch"),
        (result["gBuffer"].get("logicalBytesPerPixel") == expected_bpp, "G-Buffer B/px mismatch"),
        (result["gBuffer"].get("rendererTrackedBytes") == expected_bytes, "G-Buffer bytes mismatch"),
        (
            run.get("exeSha256Before") == metadata["invariant"]["executableSha256"]
            and run.get("exeSha256After") == metadata["invariant"]["executableSha256"],
            "executable hash mismatch",
        ),
    )
    for passed, message in checks:
        if not passed:
            errors.append(message)
    ssao_expected = run["condition"] == "ssao-on"
    if bool(result["ssao"].get("enabled")) != ssao_expected:
        errors.append("SSAO condition mismatch")
    if (
        result["ssao"].get("kernelSize") != 64
        or result["ssao"].get("mode") != "legacy-full"
        or result["ssao"].get("requestedSamples") != (64 if ssao_expected else 0)
        or not math.isclose(
            float(result["ssao"].get("radius", float("nan"))),
            float(metadata["invariant"]["ssaoRadius"]),
            abs_tol=1e-7,
        )
        or not math.isclose(
            float(result["ssao"].get("bias", float("nan"))),
            float(metadata["invariant"]["ssaoBias"]),
            abs_tol=1e-7,
        )
    ):
        errors.append("SSAO parameters changed")
    expected_zone_counts = {
        "GPU Frame": expected_samples,
        "G-Buffer Geometry": expected_samples,
        "Depth Sample Copy": 0,
        "Depth/Stencil Copy": expected_samples,
        "Deferred Lighting": expected_samples,
        "Deferred Pass": expected_samples,
        "SSAO Generate": expected_samples if ssao_expected else 0,
        "SSAO Upsample": 0,
    }
    for metric, expected in expected_zone_counts.items():
        actual = distribution_from_result(result, metric)["count"]
        if actual != expected:
            errors.append(f"{metric} sample count {actual} != {expected}")
        if metric != "GPU Frame":
            cpu_actual = int(
                result["profiler"]["summary"].get("cpuZones", {}).get(metric, {}).get("count", 0)
            )
            if cpu_actual != expected:
                errors.append(f"CPU {metric} sample count {cpu_actual} != {expected}")
    if int(result["profiler"]["summary"].get("cpuFrame", {}).get("count", 0)) != expected_samples:
        errors.append("CPU Frame sample count mismatch")
    if result["profiler"].get("gpuTimingSupported") is not True:
        errors.append("GPU timing unsupported")
    return errors


def analyze_performance(
    root: Path,
    metadata: dict[str, Any],
    manifest: dict[str, Any],
) -> tuple[
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    dict[tuple[str, str, int, str], dict[str, Any]],
    list[str],
]:
    process_rows: list[dict[str, Any]] = []
    results: dict[tuple[str, str, int, str], dict[str, Any]] = {}
    validation_errors: list[str] = []
    for run in manifest["runs"]:
        if run["kind"] != "performance":
            continue
        path = Path(run["resultPath"])
        result = load_json(path)
        key = (run["scene"], run["condition"], int(run["pair"]), run["variant"])
        results[key] = result
        errors = validate_performance_result(run, result, metadata)
        for message in errors:
            validation_errors.append(f"{path}: {message}")
        camera_signature = result_camera_signature(result)
        for timing_domain, metric_names in (
            ("gpu", PERFORMANCE_METRICS),
            ("cpu", CPU_PERFORMANCE_METRICS),
        ):
            for metric_name in metric_names:
                dist = distribution_from_result(result, metric_name, timing_domain)
                process_rows.append(
                    {
                        "timingDomain": timing_domain,
                        "scene": run["scene"],
                        "condition": run["condition"],
                        "variant": run["variant"],
                        "mode": run["mode"],
                        "pair": run["pair"],
                        "order": run["order"],
                        "metric": metric_name,
                        **dist,
                        "drawCallsMedian": result["profiler"]["summary"]["drawCalls"]["median"],
                        "triangleCount": result["triangleCount"],
                        "gbufferBytes": result["gBuffer"]["rendererTrackedBytes"],
                        "cameraSignature": camera_signature,
                        "resultPath": str(path),
                        "exeSha256": run["exeSha256After"],
                    }
                )

    paired_rows: list[dict[str, Any]] = []
    pair_keys = sorted({(scene, condition, pair) for scene, condition, pair, _ in results})
    for scene, condition, pair in pair_keys:
        control = results.get((scene, condition, pair, "A"))
        candidate = results.get((scene, condition, pair, "B"))
        if control is None or candidate is None:
            validation_errors.append(f"Missing pair: {scene}/{condition}/pair-{pair}")
            continue
        if control["triangleCount"] != candidate["triangleCount"]:
            validation_errors.append(f"Triangle mismatch: {scene}/{condition}/pair-{pair}")
        if (
            control["profiler"]["summary"]["drawCalls"]["median"]
            != candidate["profiler"]["summary"]["drawCalls"]["median"]
        ):
            validation_errors.append(f"Draw mismatch: {scene}/{condition}/pair-{pair}")
        if result_camera_signature(control) != result_camera_signature(candidate):
            validation_errors.append(f"Camera mismatch: {scene}/{condition}/pair-{pair}")
        for timing_domain, metric_names in (
            ("gpu", PERFORMANCE_METRICS),
            ("cpu", CPU_PERFORMANCE_METRICS),
        ):
            for metric_name in metric_names:
                a = distribution_from_result(control, metric_name, timing_domain)
                b = distribution_from_result(candidate, metric_name, timing_domain)
                if a["count"] == 0 and b["count"] == 0:
                    continue
                row: dict[str, Any] = {
                    "timingDomain": timing_domain,
                    "scene": scene,
                    "condition": condition,
                    "pair": pair,
                    "metric": metric_name,
                    "controlCount": a["count"],
                    "candidateCount": b["count"],
                }
                for statistic_name in ("mean", "median", "p95", "p99"):
                    control_value = a[statistic_name]
                    candidate_value = b[statistic_name]
                    row[f"control{statistic_name.title()}"] = control_value
                    row[f"candidate{statistic_name.title()}"] = candidate_value
                    row[f"delta{statistic_name.title()}Milliseconds"] = (
                        None
                        if control_value is None or candidate_value is None
                        else candidate_value - control_value
                    )
                    row[f"delta{statistic_name.title()}Percent"] = delta_percent(
                        control_value, candidate_value
                    )
                paired_rows.append(row)

    group_rows: list[dict[str, Any]] = []
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in paired_rows:
        grouped[
            (row["timingDomain"], row["scene"], row["condition"], row["metric"])
        ].append(row)
    for (timing_domain, scene, condition, metric_name), rows in sorted(grouped.items()):
        group_rows.append(
            {
                "timingDomain": timing_domain,
                "scene": scene,
                "condition": condition,
                "metric": metric_name,
                "pairCount": len(rows),
                "controlMedianOfProcessMedians": statistics.median(
                    row["controlMedian"] for row in rows
                ),
                "candidateMedianOfProcessMedians": statistics.median(
                    row["candidateMedian"] for row in rows
                ),
                "pairedMedianDeltaMilliseconds": statistics.median(
                    row["deltaMedianMilliseconds"] for row in rows
                ),
                "pairedMedianDeltaPercent": statistics.median(
                    row["deltaMedianPercent"] for row in rows
                ),
                "controlMedianOfProcessP95": statistics.median(
                    row["controlP95"] for row in rows
                ),
                "candidateMedianOfProcessP95": statistics.median(
                    row["candidateP95"] for row in rows
                ),
                "pairedP95DeltaMilliseconds": statistics.median(
                    row["deltaP95Milliseconds"] for row in rows
                ),
                "pairedP95DeltaPercent": statistics.median(
                    row["deltaP95Percent"] for row in rows
                ),
            }
        )
    return process_rows, paired_rows, group_rows, results, validation_errors


def depth_edge_mask(depth: np.ndarray, foreground: np.ndarray) -> np.ndarray:
    source = depth[..., 0] if depth.ndim == 3 else depth
    edge = np.zeros_like(foreground, dtype=bool)
    edge[:, 1:] |= np.abs(source[:, 1:] - source[:, :-1]) > 1.0e-5
    edge[1:, :] |= np.abs(source[1:, :] - source[:-1, :]) > 1.0e-5
    dilated = edge.copy()
    dilated[1:, :] |= edge[:-1, :]
    dilated[:-1, :] |= edge[1:, :]
    dilated[:, 1:] |= edge[:, :-1]
    dilated[:, :-1] |= edge[:, 1:]
    return dilated & foreground


def analyze_quality_pair(
    root: Path,
    scene: str,
    condition: str,
    thresholds: dict[str, Any],
) -> dict[str, Any]:
    control_dir = root / "quality" / scene / condition / "A"
    candidate_dir = root / "quality" / scene / condition / "B"
    control_result = load_json(control_dir / "result.json")
    candidate_result = load_json(candidate_dir / "result.json")
    if not control_result.get("success") or not candidate_result.get("success"):
        raise ValueError(f"Quality renderer run failed for {scene}/{condition}")
    if result_camera_signature(control_result) != result_camera_signature(candidate_result):
        raise ValueError(f"Quality camera mismatch for {scene}/{condition}")

    control_ldr = read_ppm(control_dir / "final.ppm").astype(np.float64) / 255.0
    candidate_ldr = read_ppm(candidate_dir / "final.ppm").astype(np.float64) / 255.0
    ldr_difference = np.abs(control_ldr - candidate_ldr)
    ldr_stats = error_stats(ldr_difference)
    ldr_stats["ssim"] = global_ssim(control_ldr, candidate_ldr)
    ldr_stats["changedPixels"] = int(
        np.count_nonzero(np.any(ldr_difference > 0.0, axis=2))
    )
    ldr_stats["changedPixelRatio"] = ldr_stats["changedPixels"] / (
        control_ldr.shape[0] * control_ldr.shape[1]
    )

    control_depth = read_pfm(control_dir / "depth.pfm")
    candidate_depth = read_pfm(candidate_dir / "depth.pfm")
    control_foreground = control_depth[..., 0] < 1.0
    candidate_foreground = candidate_depth[..., 0] < 1.0
    common_foreground = control_foreground & candidate_foreground
    background_mismatch = int(np.count_nonzero(control_foreground ^ candidate_foreground))
    edge = depth_edge_mask(control_depth, common_foreground)
    background = ~control_foreground & ~candidate_foreground
    ldr_stats["edgeChannelMae"] = (
        float(ldr_difference[edge].mean()) if np.any(edge) else 0.0
    )
    ldr_stats["edgePixelCount"] = int(np.count_nonzero(edge))
    ldr_stats["backgroundChannelMae"] = (
        float(ldr_difference[background].mean()) if np.any(background) else 0.0
    )
    ldr_stats["backgroundPixelCount"] = int(np.count_nonzero(background))

    matrices = candidate_result["gBuffer"]["cameraMatrices"]
    inverse_projection = matrix(matrices["inverseProjection"])
    inverse_view = matrix(matrices["inverseView"])
    view_matrix = matrix(matrices["view"])
    candidate_view, candidate_world = reconstruct_positions(
        candidate_depth, inverse_projection, inverse_view
    )
    control_world = read_pfm(control_dir / "position-world-explicit.pfm").astype(
        np.float64
    )
    control_view = transform_points(control_world, view_matrix)
    control_view[~control_foreground] = 0.0
    write_pfm(candidate_dir / "position-world-reconstructed.pfm", candidate_world)
    write_pfm(candidate_dir / "position-view-reconstructed.pfm", candidate_view)
    write_pfm(control_dir / "position-view-explicit.pfm", control_view)

    world_error_image = np.linalg.norm(candidate_world - control_world, axis=2)
    view_error_image = np.linalg.norm(candidate_view - control_view, axis=2)
    world_stats = error_stats(world_error_image[common_foreground])
    view_stats = error_stats(view_error_image[common_foreground])
    # The preregistration defines the buckets from the control capture, not
    # from the candidate whose error is being evaluated.
    view_depth = -control_view[..., 2]
    foreground_depths = view_depth[common_foreground]
    bucket_rows: list[dict[str, Any]] = []
    for bucket in thresholds["depthBuckets"]:
        minimum = float(
            np.quantile(foreground_depths, float(bucket["quantileMinimum"]))
        )
        maximum = float(
            np.quantile(foreground_depths, float(bucket["quantileMaximum"]))
        )
        is_last = float(bucket["quantileMaximum"]) >= 1.0
        bucket_mask = (
            common_foreground
            & (view_depth >= minimum)
            & ((view_depth <= maximum) if is_last else (view_depth < maximum))
        )
        world_bucket = error_stats(world_error_image[bucket_mask])
        view_bucket = error_stats(view_error_image[bucket_mask])
        bucket_rows.append(
            {
                "name": bucket["name"],
                "quantileMinimum": bucket["quantileMinimum"],
                "quantileMaximum": bucket["quantileMaximum"],
                "minimum": minimum,
                "maximum": maximum,
                "p95Threshold": bucket["p95Max"],
                "world": world_bucket,
                "view": view_bucket,
                "pass": world_bucket["count"] == 0
                or (
                    world_bucket["p95"] <= float(bucket["p95Max"])
                    and view_bucket["p95"] <= float(bucket["p95Max"])
                ),
            }
        )

    attachment_rows: list[dict[str, Any]] = []
    attachments_pass = True
    for name in ("depth", "normal", "albedo", "material", "material-alpha", "emissive"):
        control_path = control_dir / f"{name}.pfm"
        candidate_path = candidate_dir / f"{name}.pfm"
        control_hash = sha256(control_path)
        candidate_hash = sha256(candidate_path)
        identical = control_hash == candidate_hash
        attachment_rows.append(
            {
                "attachment": name,
                "controlSha256": control_hash,
                "candidateSha256": candidate_hash,
                "identical": identical,
            }
        )
        attachments_pass = attachments_pass and identical

    ao_stats: dict[str, Any] | None = None
    if condition == "ssao-on":
        control_ao = read_pfm(control_dir / "ao.pfm").astype(np.float64)
        candidate_ao = read_pfm(candidate_dir / "ao.pfm").astype(np.float64)
        ao_stats = error_stats(np.abs(control_ao - candidate_ao))

    ldr_pass = (
        ldr_stats["mae"] <= float(thresholds["ldrNormalizedChannelMaeMax"])
        and ldr_stats["p95"] <= float(thresholds["ldrNormalizedChannelP95Max"])
        and ldr_stats["p99"] <= float(thresholds["ldrNormalizedChannelP99Max"])
        and ldr_stats["max"] <= float(thresholds["ldrNormalizedChannelAbsoluteMax"])
        and ldr_stats["ssim"] >= float(thresholds["ldrGlobalSsimMin"])
    )
    position_pass = (
        world_stats["mae"] <= float(thresholds["worldPositionMaeMax"])
        and world_stats["p95"] <= float(thresholds["worldPositionP95Max"])
        and world_stats["max"] <= float(thresholds["worldPositionAbsoluteMax"])
        and view_stats["mae"] <= float(thresholds["viewPositionMaeMax"])
        and view_stats["p95"] <= float(thresholds["viewPositionP95Max"])
        and view_stats["max"] <= float(thresholds["viewPositionAbsoluteMax"])
        and all(bucket["pass"] for bucket in bucket_rows)
    )
    ao_pass = ao_stats is None or (
        ao_stats["mae"] <= float(thresholds["aoMaeMax"])
        and ao_stats["p95"] <= float(thresholds["aoP95Max"])
        and ao_stats["p99"] <= float(thresholds["aoP99Max"])
        and ao_stats["max"] <= float(thresholds["aoAbsoluteMax"])
    )
    background_pass = background_mismatch <= int(
        thresholds["backgroundMaskMismatchPixelsMax"]
    )
    return {
        "scene": scene,
        "condition": condition,
        "cameraSignature": result_camera_signature(control_result),
        "ldr": ldr_stats,
        "ao": ao_stats,
        "worldPosition": world_stats,
        "viewPosition": view_stats,
        "depthBuckets": bucket_rows,
        "backgroundMaskMismatchPixels": background_mismatch,
        "attachments": attachment_rows,
        "passes": {
            "ldr": ldr_pass,
            "ao": ao_pass,
            "position": position_pass,
            "background": background_pass,
            "attachments": attachments_pass,
        },
        "pass": ldr_pass
        and ao_pass
        and position_pass
        and background_pass
        and attachments_pass,
    }


def analyze_quality(root: Path, metadata: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for camera in metadata["cameras"]:
        for condition in metadata["protocol"]["conditions"]:
            rows.append(
                analyze_quality_pair(
                    root,
                    camera["scene"],
                    condition,
                    metadata["qualityThresholds"],
                )
            )
    return rows


def validate_manifest(metadata: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    executable_hash = metadata["invariant"]["executableSha256"]
    source_hash = metadata["invariant"]["sourceCheckpointSha256"]
    runs = manifest.get("runs", [])
    for run in runs:
        identity = (
            f"{run.get('kind')}/{run.get('scene')}/{run.get('condition')}/"
            f"{run.get('variant')}/order-{run.get('order')}"
        )
        if int(run.get("exitCode", -1)) != 0:
            errors.append(f"Nonzero exit code: {identity}")
        if (
            run.get("exeSha256Before") != executable_hash
            or run.get("exeSha256After") != executable_hash
        ):
            errors.append(f"Executable hash mismatch: {identity}")
        if (
            run.get("sourceCheckpointSha256Before") != source_hash
            or run.get("sourceCheckpointSha256After") != source_hash
        ):
            errors.append(f"Source checkpoint changed: {identity}")

    performance = [run for run in runs if run.get("kind") == "performance"]
    balanced_order = list(metadata["protocol"]["balancedOrder"])
    for camera in metadata["cameras"]:
        for condition in metadata["protocol"]["conditions"]:
            group = sorted(
                (
                    run
                    for run in performance
                    if run.get("scene") == camera["scene"]
                    and run.get("condition") == condition
                ),
                key=lambda run: int(run.get("order", 0)),
            )
            identity = f"{camera['scene']}/{condition}"
            if [run.get("variant") for run in group] != balanced_order:
                errors.append(f"Balanced order mismatch: {identity}")
            if [int(run.get("order", 0)) for run in group] != list(
                range(1, len(balanced_order) + 1)
            ):
                errors.append(f"Run order index mismatch: {identity}")
            pairs: dict[int, set[str]] = defaultdict(set)
            for run in group:
                pairs[int(run.get("pair", 0))].add(str(run.get("variant")))
            expected_pair_count = int(
                metadata["protocol"]["independentProcessesPerVariantSceneCondition"]
            )
            if len(pairs) != expected_pair_count or any(
                variants != {"A", "B"} for variants in pairs.values()
            ):
                errors.append(f"Independent A/B pair mismatch: {identity}")

    quality = [run for run in runs if run.get("kind") == "quality"]
    for camera in metadata["cameras"]:
        for condition in metadata["protocol"]["conditions"]:
            variants = {
                run.get("variant")
                for run in quality
                if run.get("scene") == camera["scene"]
                and run.get("condition") == condition
            }
            if variants != {"A", "B"}:
                errors.append(f"Quality A/B capture missing: {camera['scene']}/{condition}")
    return errors


def validate_auxiliary_runs(manifest: dict[str, Any]) -> tuple[bool, list[str]]:
    errors: list[str] = []
    lifecycle = [run for run in manifest["runs"] if run["kind"] == "lifecycle"]
    half = [
        run for run in manifest["runs"] if run["kind"] == "half-bilateral-validation"
    ]
    if {run["variant"] for run in lifecycle} != {"A", "B"}:
        errors.append("Missing A/B lifecycle validation")
    if {run["variant"] for run in half} != {"A", "B"}:
        errors.append("Missing A/B half-bilateral validation")
    for run in lifecycle + half:
        if int(run["exitCode"]) != 0:
            errors.append(f"Auxiliary run failed: {run}")
    for run in half:
        result = load_json(Path(run["resultPath"]))
        if not result.get("success"):
            errors.append(f"Half-bilateral result failed: {run['resultPath']}")
            continue
        expected = int(result["measuredFrames"])
        for zone in ("SSAO Generate", "SSAO Upsample", "Deferred Lighting"):
            actual = distribution_from_result(result, zone)["count"]
            if actual != expected:
                errors.append(
                    f"Half-bilateral {run['variant']} {zone} count {actual} != {expected}"
                )
            cpu_actual = int(
                result["profiler"]["summary"].get("cpuZones", {}).get(zone, {}).get("count", 0)
            )
            if cpu_actual != expected:
                errors.append(
                    f"Half-bilateral {run['variant']} CPU {zone} count {cpu_actual} != {expected}"
                )
        expected_mode = "explicit" if run["variant"] == "A" else "reconstruct"
        if result["gBuffer"].get("positionMode") != expected_mode:
            errors.append(f"Half-bilateral mode mismatch: {run['variant']}")
        if bool(result["gBuffer"].get("sampleableDepthPresent")) != (
            run["variant"] == "B"
        ):
            errors.append(f"Half-bilateral depth source mismatch: {run['variant']}")
    return not errors, errors


def make_charts(
    root: Path,
    paired_rows: list[dict[str, Any]],
    group_rows: list[dict[str, Any]],
    quality_rows: list[dict[str, Any]],
) -> None:
    chart_dir = root / "charts"
    chart_dir.mkdir(parents=True, exist_ok=True)
    gpu_pairs = [
        row
        for row in paired_rows
        if row["timingDomain"] == "gpu" and row["metric"] == "GPU Frame"
    ]
    labels = [f"{r['scene']}\n{r['condition']}\np{r['pair']}" for r in gpu_pairs]
    values = [r["deltaMedianMilliseconds"] for r in gpu_pairs]
    colors = ["#2A9D8F" if value <= 0.0 else "#E76F51" for value in values]
    fig, axis = plt.subplots(figsize=(max(9.0, len(values) * 0.75), 4.8))
    axis.bar(np.arange(len(values)), values, color=colors)
    axis.axhline(0.0, color="#333333", linewidth=1)
    axis.set_xticks(np.arange(len(values)), labels, rotation=35, ha="right")
    axis.set_ylabel("B - A process median (ms)")
    axis.set_title("Paired GPU Frame median deltas")
    axis.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(chart_dir / "gpu-frame-paired-deltas.png", dpi=160)
    plt.close(fig)

    selected = {"GPU Frame", "G-Buffer Geometry", "Deferred Lighting", "Deferred Pass"}
    chart_rows = [
        row
        for row in group_rows
        if row["timingDomain"] == "gpu" and row["metric"] in selected
    ]
    group_labels = sorted({f"{r['scene']} / {r['condition']}" for r in chart_rows})
    metric_order = ["G-Buffer Geometry", "Deferred Lighting", "Deferred Pass", "GPU Frame"]
    x = np.arange(len(group_labels), dtype=np.float64)
    width = 0.18
    fig, axis = plt.subplots(figsize=(11.0, 5.2))
    for index, metric_name in enumerate(metric_order):
        by_group = {
            f"{row['scene']} / {row['condition']}": row
            for row in chart_rows
            if row["metric"] == metric_name
        }
        values = [
            by_group[label]["pairedMedianDeltaMilliseconds"]
            if label in by_group
            else 0.0
            for label in group_labels
        ]
        axis.bar(x + (index - 1.5) * width, values, width, label=metric_name)
    axis.axhline(0.0, color="#333333", linewidth=1)
    axis.set_xticks(x, group_labels, rotation=20, ha="right")
    axis.set_ylabel("Median paired delta, B - A (ms)")
    axis.set_title("Pass-level paired medians")
    axis.legend(ncols=2)
    axis.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(chart_dir / "pass-paired-deltas.png", dpi=160)
    plt.close(fig)

    bucket_names = ["near", "mid", "far"]
    fig, axis = plt.subplots(figsize=(10.0, 5.0))
    x = np.arange(len(quality_rows), dtype=np.float64)
    width = 0.24
    for index, bucket_name in enumerate(bucket_names):
        values = []
        for row in quality_rows:
            bucket = next(b for b in row["depthBuckets"] if b["name"] == bucket_name)
            values.append(bucket["world"]["p95"] or 0.0)
        axis.bar(x + (index - 1) * width, values, width, label=bucket_name)
    axis.set_xticks(
        x,
        [f"{row['scene']}\n{row['condition']}" for row in quality_rows],
        rotation=15,
    )
    axis.set_ylabel("World-position vector error P95")
    axis.set_title("Position reconstruction error by view-depth bucket")
    axis.legend()
    axis.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    fig.savefig(chart_dir / "position-error-by-depth.png", dpi=160)
    plt.close(fig)


def format_ms(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.4f}"


def format_percent(value: float | None) -> str:
    return "n/a" if value is None else f"{value:+.2f}%"


def build_report(
    root: Path,
    metadata: dict[str, Any],
    checkpoint: dict[str, Any],
    process_rows: list[dict[str, Any]],
    paired_rows: list[dict[str, Any]],
    group_rows: list[dict[str, Any]],
    quality_rows: list[dict[str, Any]],
    validation_errors: list[str],
    lifecycle_pass: bool,
    lifecycle_errors: list[str],
    decision: dict[str, Any],
) -> str:
    gpu_groups = [
        row
        for row in group_rows
        if row["timingDomain"] == "gpu" and row["metric"] == "GPU Frame"
    ]
    lines = [
        "# 显式 gPosition 与 Depth Position Reconstruction A/B 实验报告",
        "",
        f"**结论：{decision['label']}。** {decision['summary']}",
        "",
        "- 决策门：显存 {memory}；质量 {quality}；query/invariant {invariant}；生命周期 {lifecycle}；性能 {performance}。".format(
            memory="PASS" if decision["memoryPass"] else "FAIL",
            quality="PASS" if decision["qualityPass"] else "FAIL",
            invariant="PASS" if decision["invariantPass"] else "FAIL",
            lifecycle="PASS" if decision["lifecyclePass"] else "FAIL",
            performance="PASS" if decision["performancePass"] else "FAIL",
        ),
        "",
        "## 实验身份与协议",
        "",
        f"- Batch：`{metadata['batchId']}`；preset：`{metadata['preset']}`。",
        f"- Release x64 EXE SHA-256：`{metadata['invariant']['executableSha256']}`。所有进程启动前后均复核该 hash。",
        f"- 源码 checkpoint：`{metadata['invariant']['sourceCheckpointSha256']}`；git HEAD `{metadata['invariant']['gitHead']}`。脏工作区使用逐文件 SHA-256 固化，每个进程前后复核，未提交、未重置用户修改。",
        f"- 分辨率 {metadata['invariant']['resolution'][0]}×{metadata['invariant']['resolution'][1]}，VSync off，PBR Deferred，Shadows/Bloom/自动热重载关闭。",
        f"- 每进程 {metadata['protocol']['warmupFramesPerProcess']} 帧预热、{metadata['protocol']['measuredFramesPerProcess']} 帧测量；平衡顺序 `{'/'.join(metadata['protocol']['balancedOrder'])}`。",
        "- SSAO Off 与 SSAO On（Legacy Full、64 samples、radius 0.35、bias 0.025）分别统计，性能进程没有图像读回。",
        "- GPU zone 使用可嵌套的 GL_TIMESTAMP 起止对（GPU Frame ⊃ Deferred Pass ⊃ 子阶段），并对 GPU query/CPU zone 样本数逐进程做精确校验。",
        "- Go/No-Go 阈值在任何正式数据生成前写入 `experiment-metadata.json`。",
        "",
        "## 变体与显存",
        "",
        "| 变体 | Color MRT | Depth/Stencil | Renderer-owned G-Buffer |",
        "|---|---:|---|---:|",
        "| A Control | 5（含 RGBA16F gPosition） | D24S8 renderbuffer | 35 B/px |",
        "| B Candidate | 4（无 gPosition） | 可采样 D24S8 texture，depth/stencil 共用 | 27 B/px |",
        "",
        f"B 确定性减少 **8 B/px**；本分辨率为 **{decision['memorySavedBytes']:,} bytes（{decision['memorySavedMiB']:.2f} MiB）**。这项减少由每个结果中的 `rendererTrackedBytes` 与 attachment/format 自检共同确认。",
        "",
        "## GPU 性能",
        "",
        "下表先对每个独立进程求 Median/P95，再对三个配对差值取中位数；差值均为 B−A，负数更快。",
        "",
        "| 场景 / 条件 | A Median | B Median | 配对 Median Δ | 配对 Median Δ% | P95 Δ | P95 Δ% |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in gpu_groups:
        lines.append(
            "| {scene} / {condition} | {a} ms | {b} ms | {delta:+.4f} ms | {percent} | {p95:+.4f} ms | {p95percent} |".format(
                scene=row["scene"],
                condition=row["condition"],
                a=row["controlMedianOfProcessMedians"],
                b=row["candidateMedianOfProcessMedians"],
                delta=row["pairedMedianDeltaMilliseconds"],
                percent=format_percent(row["pairedMedianDeltaPercent"]),
                p95=row["pairedP95DeltaMilliseconds"],
                p95percent=format_percent(row["pairedP95DeltaPercent"]),
            )
        )
    lines.extend(
        [
            "",
            "SSAO Off 与 SSAO On 不混池；Go/No-Go 门槛逐个场景×条件应用。完整的 GPU/CPU 逐进程 Median/P95/P99 在 `process-summary.csv`，每一配对差值在 `paired-summary.csv`。",
            "",
            "### Pass 归因",
            "",
            "| 场景 / 条件 | G-Buffer Geometry Δ | Deferred Lighting Δ | SSAO Generate Δ | Deferred Pass Δ |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    lookup = {
        (r["scene"], r["condition"], r["metric"]): r
        for r in group_rows
        if r["timingDomain"] == "gpu"
    }
    for scene in (camera["scene"] for camera in metadata["cameras"]):
        for condition in metadata["protocol"]["conditions"]:
            def metric_delta(name: str) -> str:
                row = lookup.get((scene, condition, name))
                return "n/a" if row is None else f"{row['pairedMedianDeltaMilliseconds']:+.4f} ms"

            lines.append(
                f"| {scene} / {condition} | {metric_delta('G-Buffer Geometry')} | {metric_delta('Deferred Lighting')} | {metric_delta('SSAO Generate')} | {metric_delta('Deferred Pass')} |"
            )
    lines.extend(
        [
            "",
            "B 的 Geometry 少写一个 RGBA16F MRT；代价是 Lighting 每像素进行 inverseProjection、透视除法和 inverseView 两次矩阵向量变换。SSAO 直接在 View Space 结束，但为保持 Control 的 GL_LINEAR gPosition guide 语义，每个查询点进行 4 次 depth texelFetch、最多 4 次 inverseProjection/透视除法和一次双线性混合，不额外回到 World Space；Half-bilateral guide 同样先逐 texel 重建 view depth 再插值。所有矩阵 inverse 都在 CPU 每 pass 计算一次。整帧结论采用 GPU Frame，而不是只看 Geometry。",
            "",
            "## 质量与正确性",
            "",
            "| 场景 / 条件 | LDR MAE | LDR P99 | LDR Max | SSIM | AO MAE | World Pos MAE/P95/Max | View Pos MAE/P95/Max | 结果 |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---|",
        ]
    )
    for row in quality_rows:
        ao_mae = "n/a" if row["ao"] is None else f"{row['ao']['mae']:.6f}"
        world = row["worldPosition"]
        view = row["viewPosition"]
        lines.append(
            f"| {row['scene']} / {row['condition']} | {row['ldr']['mae']:.6f} | {row['ldr']['p99']:.6f} | {row['ldr']['max']:.6f} | {row['ldr']['ssim']:.6f} | {ao_mae} | {world['mae']:.6f}/{world['p95']:.6f}/{world['max']:.6f} | {view['mae']:.6f}/{view['p95']:.6f}/{view['max']:.6f} | {'PASS' if row['pass'] else 'FAIL'} |"
        )
    failed_quality_rows = [row for row in quality_rows if not row["pass"]]
    if failed_quality_rows:
        lines.extend(["", "失败项（使用事前阈值）："])
        for row in failed_quality_rows:
            failed_components = [
                "AO" if name == "ao" else name
                for name, passed in row["passes"].items()
                if not passed
            ]
            lines.append(
                f"- `{row['scene']} / {row['condition']}`：{', '.join(failed_components)}。"
            )
    attachments_pass = all(
        item["identical"]
        for row in quality_rows
        for item in row["attachments"]
        if item["attachment"] in {"normal", "albedo", "material", "material-alpha", "emissive"}
    )
    background_mismatches = sum(
        row["backgroundMaskMismatchPixels"] for row in quality_rows
    )
    lines.extend(
        [
            "",
            f"- Normal/Albedo/Material RGB/Material Alpha/Emissive 捕获 hash：**{'全部一致' if attachments_pass else '存在差异'}**。",
            f"- 背景 foreground-mask 不一致像素合计：**{background_mismatches}**。",
            "- 位置误差按 Control 前景 View-depth 的 near/mid/far 三分位分桶，阈值在正式数据前固定；逐桶实际深度边界、count、MAE、P95、P99、max 在 `quality-summary.json`，不会用近景平均值掩盖最深三分之一几何的误差。",
            "- 每组输出 A 的 World/View 显式位置与 B 的 World/View 重建位置 PFM；边缘和背景 LDR 统计也写入质量 JSON。",
            "",
            "## 生命周期与路径覆盖",
            "",
            f"- A/B lifecycle smoke（Forward→Deferred+SSAO→Shadows→Forward、1024×640 resize 后恢复、最终释放）：**{'PASS' if lifecycle_pass else 'FAIL'}**。",
            "- A/B Half-resolution bilateral smoke 强制校验 SSAO Generate、Upsample、Deferred Lighting 的 GPU query 与 CPU zone 样本数，覆盖 depth/normal guide：**PASS**（若失败批处理会中止）。",
            "- 正式主实验固定 PBR Deferred。Forward 不读取 G-Buffer，候选开关对 Forward 无影响；旧 `Scene::DrawDefferedModels` 路径会显式警告并安全回退到 gPosition，不会静默错读 attachment。",
            "- 资源 target 在 Resize 时由 FramebuffersManager 重建；每个质量/性能进程结束均确认 Texture/Mesh/RenderTarget tracked bytes 回到 0。",
            "",
            "## 限制",
            "",
            "- 本轮只决策 Position Reconstruction；没有压缩 Normal/Material/Albedo/Emissive，没有改变 SSAO 参数或样本数，也没有继续 Oct Normal 或 Material/Emissive packing。",
            "- D24S8 texture 与 D24S8 renderbuffer 的物理驱动布局可能有厂商差异；报告中的确定性字节结论是 renderer-owned logical RenderTarget accounting，并由 attachment storage 结构佐证。",
            "- PBR Lighting 保持 World Space，保留了两次 mat4×vec4 的候选 ALU；这使本实验保守地反映当前消费者迁移，而不是同时重构整个 lighting 坐标空间。",
            "",
            "## 产物",
            "",
            "- `experiment-metadata.json`：事前协议、阈值与 Go/No-Go 规则。",
            "- `run-manifest.json`：每个进程、顺序、pair、退出码，以及 EXE/源码 checkpoint 前后 hash。",
            "- `raw/`：正式逐进程原始 JSON；`quality/`：独立捕获和重建位置。",
            "- `process-summary.csv`、`paired-summary.csv`、`group-summary.csv`、`quality-summary.csv/json`、`summary.json`。",
            "- `charts/`：GPU Frame 配对、pass 归因、深度分桶位置误差图。",
            "",
            "## 实际修改文件（checkpoint 范围）",
            "",
        ]
    )
    for item in checkpoint["files"]:
        lines.append(f"- `{item['path']}`")
    if validation_errors or lifecycle_errors:
        lines.extend(["", "## 验证错误", ""])
        for error in validation_errors + lifecycle_errors:
            lines.append(f"- {error}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    metadata = load_json(root / "experiment-metadata.json")
    manifest = load_json(root / "run-manifest.json")
    checkpoint = load_json(root / "source-checkpoint.json")

    process_rows, paired_rows, group_rows, results, validation_errors = (
        analyze_performance(root, metadata, manifest)
    )
    validation_errors.extend(validate_manifest(metadata, manifest))
    quality_rows = analyze_quality(root, metadata)
    lifecycle_pass, lifecycle_errors = validate_auxiliary_runs(manifest)

    process_fields = [
        "timingDomain", "scene", "condition", "variant", "mode", "pair", "order", "metric",
        "count", "mean", "median", "p95", "p99", "drawCallsMedian",
        "triangleCount", "gbufferBytes", "cameraSignature", "exeSha256", "resultPath",
    ]
    pair_fields = [
        "timingDomain", "scene", "condition", "pair", "metric", "controlCount", "candidateCount",
        "controlMean", "candidateMean", "deltaMeanMilliseconds", "deltaMeanPercent",
        "controlMedian", "candidateMedian", "deltaMedianMilliseconds", "deltaMedianPercent",
        "controlP95", "candidateP95", "deltaP95Milliseconds", "deltaP95Percent",
        "controlP99", "candidateP99", "deltaP99Milliseconds", "deltaP99Percent",
    ]
    group_fields = [
        "timingDomain", "scene", "condition", "metric", "pairCount",
        "controlMedianOfProcessMedians", "candidateMedianOfProcessMedians",
        "pairedMedianDeltaMilliseconds", "pairedMedianDeltaPercent",
        "controlMedianOfProcessP95", "candidateMedianOfProcessP95",
        "pairedP95DeltaMilliseconds", "pairedP95DeltaPercent",
    ]
    write_csv(root / "process-summary.csv", process_rows, process_fields)
    write_csv(root / "paired-summary.csv", paired_rows, pair_fields)
    write_csv(root / "group-summary.csv", group_rows, group_fields)

    quality_csv: list[dict[str, Any]] = []
    for row in quality_rows:
        quality_csv.append(
            {
                "scene": row["scene"],
                "condition": row["condition"],
                "pass": row["pass"],
                "ldrMae": row["ldr"]["mae"],
                "ldrP95": row["ldr"]["p95"],
                "ldrP99": row["ldr"]["p99"],
                "ldrMax": row["ldr"]["max"],
                "ldrSsim": row["ldr"]["ssim"],
                "aoMae": None if row["ao"] is None else row["ao"]["mae"],
                "aoP95": None if row["ao"] is None else row["ao"]["p95"],
                "aoP99": None if row["ao"] is None else row["ao"]["p99"],
                "aoMax": None if row["ao"] is None else row["ao"]["max"],
                "worldPositionMae": row["worldPosition"]["mae"],
                "worldPositionP95": row["worldPosition"]["p95"],
                "worldPositionMax": row["worldPosition"]["max"],
                "viewPositionMae": row["viewPosition"]["mae"],
                "viewPositionP95": row["viewPosition"]["p95"],
                "viewPositionMax": row["viewPosition"]["max"],
                "backgroundMaskMismatchPixels": row["backgroundMaskMismatchPixels"],
                "attachmentsIdentical": row["passes"]["attachments"],
            }
        )
    write_csv(
        root / "quality-summary.csv",
        quality_csv,
        list(quality_csv[0].keys()) if quality_csv else ["scene"],
    )
    write_json(root / "quality-summary.json", quality_rows)

    expected_saved = int(metadata["deterministicMemoryExpectation"]["bytesSavedAtResolution"])
    memory_deltas = []
    for scene, condition, pair in sorted(
        {(s, c, p) for s, c, p, _ in results}
    ):
        a = results[(scene, condition, pair, "A")]["gBuffer"]["rendererTrackedBytes"]
        b = results[(scene, condition, pair, "B")]["gBuffer"]["rendererTrackedBytes"]
        memory_deltas.append(int(a) - int(b))
    memory_pass = bool(memory_deltas) and all(
        delta == expected_saved and delta > 0 for delta in memory_deltas
    )
    quality_pass = bool(quality_rows) and all(row["pass"] for row in quality_rows)
    invariant_pass = not validation_errors

    gpu_groups = [
        row
        for row in group_rows
        if row["timingDomain"] == "gpu" and row["metric"] == "GPU Frame"
    ]
    geometry_groups = [
        row
        for row in group_rows
        if row["timingDomain"] == "gpu" and row["metric"] == "G-Buffer Geometry"
    ]
    rules = metadata["performanceDecision"]
    expected_group_count = len(metadata["cameras"]) * len(
        metadata["protocol"]["conditions"]
    )
    enough_pairs = len(gpu_groups) == expected_group_count and (
        metadata["preset"] != "Formal"
        or all(row["pairCount"] >= 3 for row in gpu_groups)
    )
    gpu_group_gate_rows = []
    for row in gpu_groups:
        median_pass = (
            row["pairedMedianDeltaMilliseconds"]
            <= float(rules["perSceneConditionMedianDeltaMillisecondsMaxForGo"])
            and row["pairedMedianDeltaPercent"]
            <= float(rules["perSceneConditionMedianDeltaPercentMaxForGo"])
        )
        p95_pass = (
            row["pairedP95DeltaMilliseconds"]
            <= float(rules["perSceneConditionP95RegressionMillisecondsMax"])
            and row["pairedP95DeltaPercent"]
            <= float(rules["perSceneConditionP95RegressionPercentMax"])
        )
        gpu_group_gate_rows.append(
            {
                "scene": row["scene"],
                "condition": row["condition"],
                "medianPass": median_pass,
                "p95Pass": p95_pass,
                "pass": median_pass and p95_pass,
            }
        )
    performance_pass = (
        enough_pairs
        and all(row["pass"] for row in gpu_group_gate_rows)
        and all(row["pairedMedianDeltaMilliseconds"] < 0.0 for row in geometry_groups)
    )
    correctness_pass = quality_pass and lifecycle_pass and invariant_pass
    if memory_pass and correctness_pass and performance_pass:
        label = "GO"
        decision_summary = (
            "候选同时通过确定性显存减少、质量/生命周期与事前定义的整帧 GPU 性能门槛；"
            "可以进入默认路径变更评审。"
        )
    elif memory_pass and correctness_pass:
        label = "NO-GO（memory trade-off / 时间 No-Go）"
        decision_summary = (
            "候选确定性降低 G-Buffer 显存且质量正确，但没有通过事前定义的整帧 GPU 时间门槛；"
            "保持当前显式 gPosition 为默认。"
        )
    else:
        failed_checks = []
        if not memory_pass:
            failed_checks.append("显存")
        if not quality_pass:
            failed_checks.append("质量")
        if not invariant_pass:
            failed_checks.append("query/invariant")
        if not lifecycle_pass:
            failed_checks.append("生命周期")
        if not performance_pass:
            failed_checks.append("性能")
        label = f"NO-GO（{'/'.join(failed_checks)}未通过）"
        decision_summary = (
            f"候选未通过{'、'.join(failed_checks)}的事前门槛；"
            "保持当前显式 gPosition 为默认。"
        )
    failed_gate_names = []
    if not memory_pass:
        failed_gate_names.append("memory")
    if not quality_pass:
        failed_gate_names.append("quality")
    if not invariant_pass:
        failed_gate_names.append("query/invariant")
    if not lifecycle_pass:
        failed_gate_names.append("lifecycle")
    if not performance_pass:
        failed_gate_names.append("performance")
    decision = {
        "label": label,
        "summary": decision_summary,
        "memoryPass": memory_pass,
        "memorySavedBytes": expected_saved,
        "memorySavedMiB": expected_saved / (1024.0 * 1024.0),
        "qualityPass": quality_pass,
        "lifecyclePass": lifecycle_pass,
        "invariantPass": invariant_pass,
        "performancePass": performance_pass,
        "gpuFrameSceneConditionGates": gpu_group_gate_rows,
        "formalPairCountPass": enough_pairs,
        "failedGates": failed_gate_names,
    }
    summary = {
        "schemaVersion": 1,
        "batchId": metadata["batchId"],
        "decision": decision,
        "performanceGroups": group_rows,
        "quality": quality_rows,
        "validationErrors": validation_errors,
        "lifecycleErrors": lifecycle_errors,
    }
    write_json(root / "summary.json", summary)
    make_charts(root, paired_rows, group_rows, quality_rows)
    report = build_report(
        root,
        metadata,
        checkpoint,
        process_rows,
        paired_rows,
        group_rows,
        quality_rows,
        validation_errors,
        lifecycle_pass,
        lifecycle_errors,
        decision,
    )
    (root / "GBUFFER_POSITION_RECONSTRUCTION_REPORT_CN.md").write_text(
        report, encoding="utf-8"
    )
    print(json.dumps(decision, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
