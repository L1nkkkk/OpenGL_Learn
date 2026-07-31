#!/usr/bin/env python3
"""Validate, aggregate, and visualize the half-resolution SSAO experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import numpy as np
from PIL import Image, ImageDraw, ImageFont


CONFIGURATION_ORDER = (
    "legacy-full64",
    "half-raw64",
    "half-bilateral64",
    "legacy-full32",
    "half-bilateral32",
)
CONFIGURATION_LABELS = {
    "legacy-full64": "Full-64 Legacy",
    "half-raw64": "Half-64 Raw",
    "half-bilateral64": "Half-64 Bilateral",
    "legacy-full32": "Full-32 Legacy",
    "half-bilateral32": "Half-32 Bilateral",
}
METRICS = (
    ("cpuFrame", "CPU Frame", "ms"),
    ("gpuFrame", "GPU Frame", "ms"),
    ("deferredGpu", "Deferred Pass GPU", "ms"),
    ("ssaoTotalCpu", "SSAO Pass CPU", "ms"),
    ("ssaoTotalGpu", "SSAO Pass GPU", "ms"),
    ("ssaoGenerateCpu", "SSAO Generate CPU", "ms"),
    ("ssaoGenerateGpu", "SSAO Generate GPU", "ms"),
    ("ssaoUpsampleCpu", "SSAO Upsample CPU", "ms"),
    ("ssaoUpsampleGpu", "SSAO Upsample GPU", "ms"),
    ("drawCalls", "Draw Call", "count"),
)
COLORS = {
    "background": "#F5F7FA",
    "panel": "#FFFFFF",
    "grid": "#D8DEE9",
    "axis": "#465066",
    "text": "#172033",
    "muted": "#667085",
    "frame": "#EF8A34",
    "total": "#3478D4",
    "generate": "#2AA876",
    "upsample": "#9C6ADE",
    "raw": "#D65A5A",
    "bilateral": "#3478D4",
}


class ValidationError(RuntimeError):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as stream:
        value = json.load(stream)
    expect(isinstance(value, dict), f"JSON root must be an object: {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def finite_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def nested_value(root: Any, path: str, default: Any = None) -> Any:
    current = root
    for component in path.split("."):
        if not isinstance(current, dict) or component not in current:
            return default
        current = current[component]
    return current


def first_value(root: Any, paths: Iterable[str], default: Any = None) -> Any:
    missing = object()
    for path in paths:
        value = nested_value(root, path, missing)
        if value is not missing:
            return value
    return default


def project_path(project_directory: Path, value: Any) -> Path:
    expect(isinstance(value, str) and bool(value), "artifact path is empty")
    path = Path(value)
    if not path.is_absolute():
        path = project_directory / path
    return path.resolve()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest().upper()


def numeric_values(value: Any, context: str) -> list[float]:
    expect(isinstance(value, list), f"{context} must be an array")
    result: list[float] = []
    for index, item in enumerate(value):
        expect(finite_number(item), f"{context}[{index}] is not finite")
        result.append(float(item))
    return result


def nearest_rank(ordered: list[float], percentile: float) -> float:
    expect(bool(ordered), "cannot summarize an empty distribution")
    rank = int(math.ceil(max(0.0, min(1.0, percentile)) * len(ordered)))
    return ordered[max(0, min(len(ordered) - 1, rank - 1))]


def summarize(values: Iterable[float]) -> dict[str, Any]:
    data = [float(value) for value in values]
    if not data:
        return {
            "count": 0,
            "mean": None,
            "min": None,
            "max": None,
            "median": None,
            "p95": None,
            "p99": None,
        }
    ordered = sorted(data)
    return {
        "count": len(data),
        "mean": statistics.fmean(data),
        "min": ordered[0],
        "max": ordered[-1],
        "median": nearest_rank(ordered, 0.50),
        "p95": nearest_rank(ordered, 0.95),
        "p99": nearest_rank(ordered, 0.99),
    }


def process_dispersion(values: Iterable[float]) -> dict[str, Any]:
    data = [float(value) for value in values]
    if not data:
        return {
            "count": 0,
            "medianOfProcessMedians": None,
            "min": None,
            "max": None,
            "rangeAbsolute": None,
            "rangePercent": None,
            "coefficientOfVariationPercent": None,
        }
    ordered = sorted(data)
    center = nearest_rank(ordered, 0.50)
    average = statistics.fmean(data)
    spread = ordered[-1] - ordered[0]
    return {
        "count": len(data),
        "medianOfProcessMedians": center,
        "min": ordered[0],
        "max": ordered[-1],
        "rangeAbsolute": spread,
        "rangePercent": spread / abs(center) * 100.0 if center else None,
        "coefficientOfVariationPercent": (
            statistics.pstdev(data) / abs(average) * 100.0
            if len(data) > 1 and average
            else 0.0
        ),
    }


def close_sequence(actual: Any, expected: Any, tolerance: float = 1.0e-5) -> bool:
    if not isinstance(actual, list) or not isinstance(expected, list):
        return False
    if len(actual) != len(expected):
        return False
    return all(
        finite_number(left)
        and finite_number(right)
        and abs(float(left) - float(right)) <= tolerance
        for left, right in zip(actual, expected)
    )


def read_pfm(path: Path) -> np.ndarray:
    expect(path.is_file(), f"PFM is missing: {path}")
    with path.open("rb") as stream:
        magic = stream.readline().decode("ascii", errors="strict").strip()
        expect(magic in {"Pf", "PF"}, f"invalid PFM magic in {path}: {magic!r}")
        channels = 1 if magic == "Pf" else 3

        dimensions_line = stream.readline().decode("ascii", errors="strict").strip()
        while dimensions_line.startswith("#") or not dimensions_line:
            dimensions_line = (
                stream.readline().decode("ascii", errors="strict").strip()
            )
        pieces = dimensions_line.split()
        expect(len(pieces) == 2, f"invalid PFM dimensions in {path}")
        width, height = (int(pieces[0]), int(pieces[1]))
        expect(width > 0 and height > 0, f"invalid PFM size in {path}")

        scale = float(stream.readline().decode("ascii", errors="strict").strip())
        expect(scale != 0.0 and math.isfinite(scale), f"invalid PFM scale in {path}")
        dtype = "<f4" if scale < 0.0 else ">f4"
        pixels = np.fromfile(stream, dtype=dtype)
    expected_count = width * height * channels
    expect(
        pixels.size == expected_count,
        f"{path}: PFM has {pixels.size} floats, expected {expected_count}",
    )
    pixels = pixels.astype(np.float32, copy=False) * abs(scale)
    if channels == 1:
        pixels = pixels.reshape((height, width))
    else:
        pixels = pixels.reshape((height, width, channels))
    # PFM stores the bottom row first; image/report coordinates use top-left.
    pixels = np.flipud(pixels).copy()
    expect(bool(np.isfinite(pixels).all()), f"{path}: PFM contains NaN/Inf")
    return pixels


def pfm_metadata(path: Path, expected_shape: tuple[int, ...]) -> dict[str, Any]:
    pixels = read_pfm(path)
    expect(
        pixels.shape == expected_shape,
        f"{path}: PFM shape {pixels.shape}, expected {expected_shape}",
    )
    return {
        "path": str(path),
        "sha256": sha256_file(path),
        "shape": list(pixels.shape),
        "min": float(np.min(pixels)),
        "max": float(np.max(pixels)),
        "mean": float(np.mean(pixels, dtype=np.float64)),
    }


def validate_float_capture_metadata(
    capture: Any,
    project_directory: Path,
    expected_path: Path | None,
    expected_width: int,
    expected_height: int,
    expected_channels: int,
    context: str,
) -> None:
    expect(isinstance(capture, dict), f"{context}: float-capture metadata missing")
    if expected_path is None:
        expect(capture.get("path") == "", f"{context}: unexpected capture path")
        expect(capture.get("requested") is False, f"{context}: unexpectedly requested")
        expect(capture.get("valid") is False, f"{context}: unexpectedly valid")
        for name in (
            "width",
            "height",
            "channels",
            "finiteValueCount",
            "nonFiniteValueCount",
        ):
            expect(int(capture.get(name, -1)) == 0, f"{context}: {name} is not zero")
        return

    expected_path = expected_path.resolve()
    expect(capture.get("requested") is True, f"{context}: capture not requested")
    expect(capture.get("valid") is True, f"{context}: capture is invalid")
    expect(
        project_path(project_directory, capture.get("path")) == expected_path,
        f"{context}: recorded capture path mismatch",
    )
    expect(
        int(capture.get("width", -1)) == expected_width
        and int(capture.get("height", -1)) == expected_height
        and int(capture.get("channels", -1)) == expected_channels,
        f"{context}: dimensions/channels mismatch",
    )
    expected_values = expected_width * expected_height * expected_channels
    expect(
        int(capture.get("finiteValueCount", -1)) == expected_values
        and int(capture.get("nonFiniteValueCount", -1)) == 0,
        f"{context}: finite-value accounting mismatch",
    )
    for name in ("minimum", "maximum", "mean"):
        expect(finite_number(capture.get(name)), f"{context}: invalid {name}")


def validate_ppm(path: Path, expected_size: tuple[int, int], context: str) -> None:
    expect(path.is_file(), f"{context}: capture is missing: {path}")
    try:
        with Image.open(path) as image:
            expect(
                image.size == expected_size,
                f"{context}: capture size {image.size}, expected {expected_size}",
            )
            image.verify()
    except ValidationError:
        raise
    except Exception as error:
        raise ValidationError(f"{context}: invalid PPM: {error}") from error


def zone_samples(result: dict[str, Any], kind: str, name: str) -> list[float]:
    zones = nested_value(result, f"profiler.samples.{kind}", {})
    expect(isinstance(zones, dict), f"profiler.samples.{kind} is missing")
    return numeric_values(zones.get(name, []), f"{kind}/{name}")


def zone_summary_count(result: dict[str, Any], kind: str, name: str) -> int:
    zone = nested_value(result, f"profiler.summary.{kind}.{name}")
    return int(zone.get("count", 0)) if isinstance(zone, dict) else 0


def validate_nesting(
    total: list[float],
    generate: list[float],
    upsample: list[float],
    context: str,
) -> None:
    expect(len(total) == len(generate), f"{context}: total/generate count mismatch")
    for index, (total_value, generate_value) in enumerate(zip(total, generate)):
        expect(total_value > 0.0, f"{context}: non-positive total at {index}")
        expect(generate_value > 0.0, f"{context}: non-positive generate at {index}")
        expect(
            generate_value <= total_value + 0.005,
            f"{context}: generate exceeds total at {index}",
        )
        if upsample:
            upsample_value = upsample[index]
            expect(upsample_value > 0.0, f"{context}: non-positive upsample")
            expect(
                generate_value + upsample_value <= total_value + 0.010,
                f"{context}: generate+upsample exceeds total at {index}",
            )


def extract_metrics(
    result: dict[str, Any],
    expected_frames: int,
    bilateral: bool,
    context: str,
) -> dict[str, list[float]]:
    expect(
        nested_value(result, "profiler.gpuTimingSupported") is True,
        f"{context}: GPU timestamp timing is unavailable",
    )
    samples = nested_value(result, "profiler.samples")
    expect(isinstance(samples, dict), f"{context}: profiler.samples missing")
    wall_frame = numeric_values(
        samples.get("wallFrame"), f"{context}/Wall Frame"
    )
    metrics = {
        "cpuFrame": numeric_values(samples.get("cpuFrame"), f"{context}/CPU Frame"),
        "gpuFrame": numeric_values(samples.get("gpuFrame"), f"{context}/GPU Frame"),
        "deferredGpu": zone_samples(result, "gpuZones", "Deferred Pass"),
        "drawCalls": numeric_values(samples.get("drawCalls"), f"{context}/Draw Call"),
        "ssaoTotalCpu": zone_samples(result, "cpuZones", "SSAO Pass"),
        "ssaoTotalGpu": zone_samples(result, "gpuZones", "SSAO Pass"),
        "ssaoGenerateCpu": zone_samples(result, "cpuZones", "SSAO Generate"),
        "ssaoGenerateGpu": zone_samples(result, "gpuZones", "SSAO Generate"),
        "ssaoUpsampleCpu": zone_samples(result, "cpuZones", "SSAO Upsample"),
        "ssaoUpsampleGpu": zone_samples(result, "gpuZones", "SSAO Upsample"),
    }
    deferred_cpu = zone_samples(result, "cpuZones", "Deferred Pass")
    deferred_gpu = metrics["deferredGpu"]
    required = (
        "cpuFrame",
        "gpuFrame",
        "deferredGpu",
        "drawCalls",
        "ssaoTotalCpu",
        "ssaoTotalGpu",
        "ssaoGenerateCpu",
        "ssaoGenerateGpu",
    )
    for name in required:
        expect(
            len(metrics[name]) == expected_frames,
            f"{context}: {name} has {len(metrics[name])}, expected {expected_frames}",
        )
    expect(
        len(wall_frame) == expected_frames
        and all(value > 0.0 for value in wall_frame),
        f"{context}: Wall Frame samples are incomplete",
    )
    expect(
        len(deferred_cpu) == expected_frames
        and len(deferred_gpu) == expected_frames,
        f"{context}: Deferred Pass samples are incomplete",
    )
    expected_upsample = expected_frames if bilateral else 0
    for name in ("ssaoUpsampleCpu", "ssaoUpsampleGpu"):
        expect(
            len(metrics[name]) == expected_upsample,
            f"{context}: {name} has {len(metrics[name])}, expected {expected_upsample}",
        )

    summary = nested_value(result, "profiler.summary")
    expect(isinstance(summary, dict), f"{context}: profiler.summary missing")
    for name in ("wallFrame", "cpuFrame", "gpuFrame", "drawCalls"):
        expect(
            int(summary.get(name, {}).get("count", -1)) == expected_frames,
            f"{context}: summary count mismatch for {name}",
        )
    for kind in ("cpuZones", "gpuZones"):
        for zone in ("Deferred Pass", "SSAO Pass", "SSAO Generate"):
            expect(
                zone_summary_count(result, kind, zone) == expected_frames,
                f"{context}: summary count mismatch for {kind}/{zone}",
            )
        expect(
            zone_summary_count(result, kind, "SSAO Upsample")
            == expected_upsample,
            f"{context}: summary count mismatch for {kind}/SSAO Upsample",
        )

    for name, values in metrics.items():
        if name.startswith("ssaoUpsample") and not bilateral:
            continue
        expect(all(value > 0.0 for value in values), f"{context}: invalid {name}")
    expect(
        all(value > 0.0 and float(value).is_integer() for value in metrics["drawCalls"]),
        f"{context}: Draw Call must be positive integral counts",
    )
    for index, (frame, deferred, total) in enumerate(
        zip(metrics["gpuFrame"], deferred_gpu, metrics["ssaoTotalGpu"])
    ):
        expect(deferred <= frame, f"{context}: Deferred GPU > GPU Frame at {index}")
        expect(total <= deferred, f"{context}: SSAO GPU > Deferred GPU at {index}")
    validate_nesting(
        metrics["ssaoTotalGpu"],
        metrics["ssaoGenerateGpu"],
        metrics["ssaoUpsampleGpu"],
        f"{context}/GPU",
    )
    validate_nesting(
        metrics["ssaoTotalCpu"],
        metrics["ssaoGenerateCpu"],
        metrics["ssaoUpsampleCpu"],
        f"{context}/CPU",
    )
    return metrics


def validate_run(
    record: dict[str, Any],
    configuration: dict[str, Any],
    scene_record: dict[str, Any],
    manifest: dict[str, Any],
    project_directory: Path,
) -> dict[str, Any]:
    scene_id = str(record.get("scene", ""))
    configuration_name = str(record.get("configuration", ""))
    process_index = int(record.get("process", 0))
    context = f"{scene_id}/{configuration_name}/process {process_index}"
    result_path = project_path(project_directory, record.get("result"))
    expect(result_path.is_file(), f"{context}: result missing: {result_path}")
    result = load_json(result_path)
    protocol = manifest["protocol"]
    expected_frames = int(protocol["measuredFrames"])

    expect(result.get("success") is True, f"{context}: success is false")
    expect(int(result.get("schemaVersion", 0)) >= 21, f"{context}: schema < 21")
    expect(result.get("scene") == scene_id, f"{context}: scene mismatch")
    expect(result.get("buildConfiguration") == "Release", f"{context}: not Release")
    expect(result.get("architecture") == "x64", f"{context}: not x64")
    expect(result.get("resolution") == [1920, 1080], f"{context}: resolution")
    expect(result.get("renderPath") == "pbr-deferred", f"{context}: render path")
    expect(result.get("materialMode") == "source", f"{context}: material mode")
    expect(
        result.get("frameMeasurement") == "cpu-submission-wall",
        f"{context}: CPU frame measurement mode",
    )
    expect(
        int(result.get("warmupFrames", -1)) == int(protocol["warmupFrames"])
        and int(result.get("measuredFrames", -1)) == expected_frames,
        f"{context}: frame protocol mismatch",
    )
    expected_model = "classic-scenes/" + str(scene_record["modelPath"]).replace(
        "\\", "/"
    )
    expect(result.get("modelPath") == expected_model, f"{context}: model mismatch")

    camera = result.get("camera")
    expect(isinstance(camera, dict), f"{context}: camera missing")
    expect(
        close_sequence(camera.get("position"), scene_record["camera"])
        and close_sequence(camera.get("target"), scene_record["target"])
        and close_sequence(camera.get("up"), scene_record["up"]),
        f"{context}: camera vectors changed",
    )
    expect(
        abs(float(camera.get("fovDegrees", -1.0)) - float(scene_record["fov"]))
        <= 1.0e-5,
        f"{context}: FOV changed",
    )
    settings = result.get("settings")
    expect(isinstance(settings, dict), f"{context}: settings missing")
    required_settings = {
        "requestedSwapInterval": 0,
        "inputFrozen": True,
        "deferredRendering": True,
        "bloom": False,
        "gammaCorrection": True,
        "autoReloadShaders": False,
        "autoReloadMaterials": False,
        "activePointLights": 1,
        "activeDirectionLights": 1,
        "activeSpotLights": 0,
        "shadowCastingLights": 0,
    }
    for name, expected in required_settings.items():
        expect(settings.get(name) == expected, f"{context}: setting {name} changed")

    ssao = result.get("ssao")
    expect(isinstance(ssao, dict), f"{context}: ssao metadata missing")
    expect(ssao.get("experiment") is True, f"{context}: experiment flag false")
    expect(ssao.get("enabled") is True, f"{context}: SSAO disabled")
    expect(ssao.get("mode") == configuration["mode"], f"{context}: mode mismatch")
    expect(
        int(ssao.get("requestedSamples", -1)) == int(configuration["samples"])
        and int(ssao.get("kernelSize", -1)) == int(configuration["samples"]),
        f"{context}: kernel size mismatch",
    )
    expect(
        abs(float(ssao.get("radius", -1.0)) - 0.35) <= 1.0e-5
        and abs(float(ssao.get("bias", -1.0)) - 0.025) <= 1.0e-5,
        f"{context}: radius/bias changed",
    )
    kernel_generation = ssao.get("kernelGeneration")
    expect(
        isinstance(kernel_generation, dict)
        and int(kernel_generation.get("seed", -1)) == 1337
        and int(kernel_generation.get("capacity", -1)) == 64
        and int(kernel_generation.get("radialScaleDenominator", -1)) == 64
        and kernel_generation.get("selection") == "prefix"
        and kernel_generation.get("sampleCountAndRadialDistributionCoupled")
        is (int(configuration["samples"]) < 64),
        f"{context}: kernel-generation metadata changed",
    )

    half = bool(configuration["halfResolution"])
    bilateral = bool(configuration["bilateral"])
    expected_generate = (960, 540) if half else (1920, 1080)
    expected_output = (960, 540) if configuration["mode"] == "half-raw" else (
        1920,
        1080,
    )
    generate_width = int(
        first_value(ssao, ("generate.width", "generate.outputWidth"), -1)
    )
    generate_height = int(
        first_value(ssao, ("generate.height", "generate.outputHeight"), -1)
    )
    generate_format = str(
        first_value(
            ssao,
            (
                "generate.internalFormatName",
                "generate.outputInternalFormatName",
                "generate.format",
            ),
            "",
        )
    )
    output_width = int(first_value(ssao, ("output.width", "output.outputWidth"), -1))
    output_height = int(
        first_value(ssao, ("output.height", "output.outputHeight"), -1)
    )
    output_format = str(
        first_value(
            ssao,
            (
                "output.internalFormatName",
                "output.outputInternalFormatName",
                "output.format",
            ),
            "",
        )
    )
    expect(
        (generate_width, generate_height) == expected_generate,
        f"{context}: generate dimensions {(generate_width, generate_height)}",
    )
    expect(
        (output_width, output_height) == expected_output,
        f"{context}: output dimensions {(output_width, output_height)}",
    )
    expect(
        generate_format == "GL_R16F" and output_format == "GL_R16F",
        f"{context}: AO target is not GL_R16F",
    )
    expect(
        first_value(ssao, ("generate.available",), False) is True
        and first_value(ssao, ("output.available",), False) is True,
        f"{context}: AO attachment is unavailable",
    )
    expect(
        first_value(ssao, ("generate.fullResolution",), None) is (not half)
        and first_value(ssao, ("generate.resolutionPolicy",), "")
        == ("ceil-half" if half else "full"),
        f"{context}: generate resolution metadata mismatch",
    )
    expected_output_full_resolution = configuration["mode"] != "half-raw"
    expected_sampling = {
        "legacy-full": "full-resolution-direct",
        "half-raw": "direct-gl-linear",
        "half-bilateral": "full-resolution-depth-normal-aware-bilateral",
    }[str(configuration["mode"])]
    expect(
        first_value(ssao, ("output.fullResolution",), None)
        is expected_output_full_resolution
        and first_value(ssao, ("output.sampling",), "") == expected_sampling,
        f"{context}: output sampling metadata mismatch",
    )
    upsample_enabled = bool(
        first_value(ssao, ("upsample.enabled", "upsampleEnabled"), False)
    )
    expect(upsample_enabled == bilateral, f"{context}: upsample flag mismatch")
    algorithm = str(first_value(ssao, ("upsample.algorithm", "upsample.name"), ""))
    expect(
        algorithm
        == ("depth-normal-aware-bilateral-2x2" if bilateral else "none"),
        f"{context}: unexpected upsample algorithm",
    )
    expect(
        first_value(ssao, ("upsample.neighborhood",), "")
        == "2x2-bilinear-footprint"
        and first_value(ssao, ("upsample.inputs",), [])
        == ["halfAO", "fullPositionDepth", "fullNormal"]
        and finite_number(first_value(ssao, ("upsample.depthSigma",), None))
        and float(first_value(ssao, ("upsample.depthSigma",), 0.0)) > 0.0
        and finite_number(first_value(ssao, ("upsample.normalPower",), None))
        and float(first_value(ssao, ("upsample.normalPower",), 0.0)) > 0.0,
        f"{context}: bilateral metadata is incomplete",
    )

    metrics = extract_metrics(result, expected_frames, bilateral, context)
    log_path = project_path(project_directory, record.get("log"))
    expect(log_path.is_file(), f"{context}: log missing")

    capture_record = record.get("captures")
    captures: dict[str, str | None] = {
        "finalPpm": None,
        "aoPpm": None,
        "aoPfm": None,
        "rawHalfPfm": None,
        "depthPfm": None,
        "normalPfm": None,
    }
    capture_metadata: dict[str, Any] = {}
    if process_index == 1:
        expect(isinstance(capture_record, dict), f"{context}: captures missing")
        for name in captures:
            value = capture_record.get(name)
            if value:
                captures[name] = str(project_path(project_directory, value))
        final_path = Path(str(captures["finalPpm"]))
        ao_ppm_path = Path(str(captures["aoPpm"]))
        ao_pfm_path = Path(str(captures["aoPfm"]))
        expect(result.get("captureRequired") is True, f"{context}: final capture flag")
        expect(
            project_path(project_directory, result.get("capturePath")) == final_path,
            f"{context}: final capture path mismatch",
        )
        expect(
            project_path(project_directory, ssao.get("capturePath")) == ao_ppm_path
            and ssao.get("captureValid") is True,
            f"{context}: AO LDR capture metadata mismatch",
        )
        expect(
            project_path(
                project_directory,
                first_value(ssao, ("output.ldrCapturePath",), ""),
            )
            == ao_ppm_path
            and first_value(ssao, ("output.ldrCaptureValid",), False) is True,
            f"{context}: AO output LDR metadata mismatch",
        )
        validate_ppm(final_path, (1920, 1080), f"{context}/final")
        validate_ppm(ao_ppm_path, expected_output, f"{context}/AO")
        capture_metadata["finalPpm"] = {
            "path": str(final_path),
            "sha256": sha256_file(final_path),
            "size": [1920, 1080],
        }
        capture_metadata["aoPpm"] = {
            "path": str(ao_ppm_path),
            "sha256": sha256_file(ao_ppm_path),
            "size": list(expected_output),
        }
        capture_metadata["aoPfm"] = pfm_metadata(
            ao_pfm_path, (expected_output[1], expected_output[0])
        )
        ao_pixels = read_pfm(ao_pfm_path)
        expect(
            float(np.min(ao_pixels)) >= -1.0e-4
            and float(np.max(ao_pixels)) <= 1.0001,
            f"{context}: AO PFM is outside [0,1]",
        )
        if bilateral:
            raw_path = Path(str(captures["rawHalfPfm"]))
            capture_metadata["rawHalfPfm"] = pfm_metadata(raw_path, (540, 960))
        else:
            expect(
                captures["rawHalfPfm"] is None,
                f"{context}: unexpected raw-half capture",
            )
        if configuration_name == "legacy-full64":
            depth_path = Path(str(captures["depthPfm"]))
            normal_path = Path(str(captures["normalPfm"]))
            capture_metadata["depthPfm"] = pfm_metadata(depth_path, (1080, 1920))
            capture_metadata["normalPfm"] = pfm_metadata(
                normal_path, (1080, 1920, 3)
            )
            depth_pixels = read_pfm(depth_path)
            expect(
                float(np.min(depth_pixels)) >= -1.0e-5
                and bool(np.any(depth_pixels > 0.0)),
                f"{context}: invalid guide depth",
            )
        else:
            expect(
                captures["depthPfm"] is None and captures["normalPfm"] is None,
                f"{context}: guide captures must only accompany Full-64",
            )
    else:
        expect(capture_record is None, f"{context}: only process 1 may capture")
        expect(
            result.get("captureRequired") is False
            and result.get("capturePath") == ""
            and ssao.get("capturePath") == ""
            and ssao.get("captureValid") is False
            and first_value(ssao, ("output.ldrCapturePath",), "") == ""
            and first_value(ssao, ("output.ldrCaptureValid",), False) is False,
            f"{context}: non-capture process contains capture metadata",
        )

    output_float_path = (
        Path(str(captures["aoPfm"])) if process_index == 1 else None
    )
    generation_float_path = (
        Path(str(captures["rawHalfPfm"]))
        if process_index == 1 and bilateral
        else output_float_path
    )
    depth_float_path = (
        Path(str(captures["depthPfm"]))
        if process_index == 1 and configuration_name == "legacy-full64"
        else None
    )
    normal_float_path = (
        Path(str(captures["normalPfm"]))
        if process_index == 1 and configuration_name == "legacy-full64"
        else None
    )
    validate_float_capture_metadata(
        nested_value(ssao, "generate.floatCapture"),
        project_directory,
        generation_float_path,
        expected_generate[0],
        expected_generate[1],
        1,
        f"{context}/generate",
    )
    validate_float_capture_metadata(
        nested_value(ssao, "output.floatCapture"),
        project_directory,
        output_float_path,
        expected_output[0],
        expected_output[1],
        1,
        f"{context}/output",
    )
    validate_float_capture_metadata(
        nested_value(ssao, "guidance.depthCapture"),
        project_directory,
        depth_float_path,
        1920,
        1080,
        1,
        f"{context}/depth",
    )
    validate_float_capture_metadata(
        nested_value(ssao, "guidance.normalCapture"),
        project_directory,
        normal_float_path,
        1920,
        1080,
        3,
        f"{context}/normal",
    )

    light_signature = {
        "settings": settings,
        "directionalLight": nested_value(result, "shadow.directionalLight"),
        "pointLightPosition": nested_value(result, "shadow.pointLightPosition"),
        "spotLightPosition": nested_value(result, "shadow.spotLightPosition"),
        "spotLightDirection": nested_value(result, "shadow.spotLightDirection"),
        "radius": ssao.get("radius"),
        "bias": ssao.get("bias"),
    }
    return {
        "scene": scene_id,
        "configuration": configuration_name,
        "mode": configuration["mode"],
        "samples": int(configuration["samples"]),
        "process": process_index,
        "metrics": metrics,
        "captures": captures,
        "captureMetadata": capture_metadata,
        "resultPath": str(result_path),
        "logPath": str(log_path),
        "glVendor": result.get("glVendor"),
        "glRenderer": result.get("glRenderer"),
        "glVersion": result.get("glVersion"),
        "lightSignature": light_signature,
    }


def aggregate_runs(
    runs: list[dict[str, Any]], expected_processes: int
) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for run in runs:
        grouped[(run["scene"], run["configuration"])].append(run)
    aggregates: list[dict[str, Any]] = []
    for (scene, configuration), group_runs in grouped.items():
        expect(
            len(group_runs) == expected_processes,
            f"{scene}/{configuration}: process count mismatch",
        )
        group_runs.sort(key=lambda item: item["process"])
        expect(
            [item["process"] for item in group_runs]
            == list(range(1, expected_processes + 1)),
            f"{scene}/{configuration}: process ids are not contiguous",
        )
        aggregate_metrics: dict[str, Any] = {}
        for key, _label, unit in METRICS:
            pooled = [
                value for run in group_runs for value in run["metrics"][key]
            ]
            per_process = [
                {"process": run["process"], **summarize(run["metrics"][key])}
                for run in group_runs
            ]
            medians = [
                float(item["median"])
                for item in per_process
                if item["median"] is not None
            ]
            aggregate_metrics[key] = {
                "unit": unit,
                "pooled": summarize(pooled),
                "processMedianDispersion": process_dispersion(medians),
                "perProcess": per_process,
            }
        capture_run = next(
            (run for run in group_runs if run["process"] == 1), group_runs[0]
        )
        aggregates.append(
            {
                "scene": scene,
                "configuration": configuration,
                "mode": group_runs[0]["mode"],
                "samples": group_runs[0]["samples"],
                "metrics": aggregate_metrics,
                "captures": capture_run["captures"],
                "captureMetadata": capture_run["captureMetadata"],
            }
        )
    order = {name: index for index, name in enumerate(CONFIGURATION_ORDER)}
    aggregates.sort(key=lambda item: (item["scene"], order[item["configuration"]]))
    return aggregates


def group_map(
    aggregates: list[dict[str, Any]],
) -> dict[str, dict[str, dict[str, Any]]]:
    result: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for aggregate in aggregates:
        result[aggregate["scene"]][aggregate["configuration"]] = aggregate
    return result


def add_performance_comparisons(
    aggregates: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    comparisons: list[dict[str, Any]] = []
    for scene, groups in sorted(group_map(aggregates).items()):
        full64 = groups["legacy-full64"]
        raw64 = groups["half-raw64"]
        bilateral64 = groups["half-bilateral64"]
        full32 = groups["legacy-full32"]
        bilateral32 = groups["half-bilateral32"]

        def median(group: dict[str, Any], metric: str) -> float:
            return float(group["metrics"][metric]["pooled"]["median"])

        def process_medians(group: dict[str, Any], metric: str) -> list[float]:
            return [
                float(item["median"])
                for item in group["metrics"][metric]["perProcess"]
            ]

        reference_total = median(full64, "ssaoTotalGpu")
        reference_frame = median(full64, "gpuFrame")
        rows: list[dict[str, Any]] = []
        for configuration in CONFIGURATION_ORDER:
            group = groups[configuration]
            total = median(group, "ssaoTotalGpu")
            frame = median(group, "gpuFrame")
            rows.append(
                {
                    "configuration": configuration,
                    "ssaoTotalGpuMedianMs": total,
                    "ssaoGenerateGpuMedianMs": median(
                        group, "ssaoGenerateGpu"
                    ),
                    "ssaoUpsampleGpuMedianMs": (
                        median(group, "ssaoUpsampleGpu")
                        if group["metrics"]["ssaoUpsampleGpu"]["pooled"]["count"]
                        else None
                    ),
                    "gpuFrameMedianMs": frame,
                    "ssaoTotalGpuShareOfGpuFrameMedianPercent": (
                        total / frame * 100.0
                    ),
                    "ssaoTotalDeltaVsFull64Ms": total - reference_total,
                    "ssaoTotalSavingsVsFull64Percent": (
                        (reference_total - total) / reference_total * 100.0
                    ),
                    "gpuFrameDeltaVsFull64Ms": frame - reference_frame,
                    "drawCallMedian": median(group, "drawCalls"),
                }
            )
        full64_process = process_medians(full64, "ssaoTotalGpu")
        bilateral64_process = process_medians(bilateral64, "ssaoTotalGpu")
        comparisons.append(
            {
                "scene": scene,
                "configurations": rows,
                "mainAb": {
                    "full64TotalGpuMedianMs": reference_total,
                    "halfRaw64TotalGpuMedianMs": median(raw64, "ssaoTotalGpu"),
                    "halfBilateral64TotalGpuMedianMs": median(
                        bilateral64, "ssaoTotalGpu"
                    ),
                    "halfBilateral64AllProcessesLowerThanFull64": all(
                        half < full
                        for half, full in zip(
                            bilateral64_process, full64_process
                        )
                    ),
                },
                "extension32": {
                    "full32TotalGpuMedianMs": median(full32, "ssaoTotalGpu"),
                    "halfBilateral32TotalGpuMedianMs": median(
                        bilateral32, "ssaoTotalGpu"
                    ),
                },
            }
        )
    return comparisons


def gl_linear_reconstruct(source: np.ndarray, width: int, height: int) -> np.ndarray:
    expect(source.ndim == 2, "GL_LINEAR reconstruction expects one channel")
    source_height, source_width = source.shape
    if (source_width, source_height) == (width, height):
        return source.astype(np.float32, copy=True)
    x = (np.arange(width, dtype=np.float64) + 0.5) * source_width / width - 0.5
    x0_unclamped = np.floor(x).astype(np.int64)
    x_weight = (x - x0_unclamped).astype(np.float32)
    x0 = np.clip(x0_unclamped, 0, source_width - 1)
    x1 = np.clip(x0_unclamped + 1, 0, source_width - 1)
    horizontal = (
        source[:, x0] * (1.0 - x_weight)[None, :]
        + source[:, x1] * x_weight[None, :]
    )
    y = (np.arange(height, dtype=np.float64) + 0.5) * source_height / height - 0.5
    y0_unclamped = np.floor(y).astype(np.int64)
    y_weight = (y - y0_unclamped).astype(np.float32)
    y0 = np.clip(y0_unclamped, 0, source_height - 1)
    y1 = np.clip(y0_unclamped + 1, 0, source_height - 1)
    return (
        horizontal[y0, :] * (1.0 - y_weight)[:, None]
        + horizontal[y1, :] * y_weight[:, None]
    ).astype(np.float32, copy=False)


def shifted_overlap(
    height: int, width: int, dy: int, dx: int
) -> tuple[tuple[slice, slice], tuple[slice, slice]]:
    source_y = slice(max(0, -dy), min(height, height - dy))
    source_x = slice(max(0, -dx), min(width, width - dx))
    target_y = slice(max(0, dy), min(height, height + dy))
    target_x = slice(max(0, dx), min(width, width + dx))
    return (source_y, source_x), (target_y, target_x)


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


def build_edge_mask(
    depth: np.ndarray,
    normal: np.ndarray,
    relative_depth_threshold: float = 0.02,
    normal_angle_degrees: float = 25.0,
    dilation_pixels: int = 3,
) -> tuple[np.ndarray, np.ndarray]:
    expect(depth.shape == (1080, 1920), "guide depth shape mismatch")
    expect(normal.shape == (1080, 1920, 3), "guide normal shape mismatch")
    valid = depth > 0.0
    lengths = np.linalg.norm(normal, axis=2)
    expect(bool(np.any(valid)), "guide depth contains no foreground pixels")
    expect(
        bool(np.all(lengths[valid] > 0.5)),
        "guide normal is invalid for one or more foreground pixels",
    )
    normalized = normal / np.maximum(lengths[..., None], 1.0e-8)
    threshold_dot = math.cos(math.radians(normal_angle_degrees))
    edge = np.zeros_like(valid)
    height, width = valid.shape
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            source, target = shifted_overlap(height, width, dy, dx)
            source_valid = valid[source]
            target_valid = valid[target]
            discontinuity = source_valid != target_valid
            both = source_valid & target_valid
            relative_depth = np.zeros_like(depth[source], dtype=np.float32)
            relative_depth[both] = (
                np.abs(depth[source][both] - depth[target][both])
                / np.maximum(
                    np.minimum(depth[source][both], depth[target][both]),
                    1.0e-6,
                )
            )
            normal_dot = np.sum(normalized[source] * normalized[target], axis=2)
            discontinuity |= both & (
                (relative_depth > relative_depth_threshold)
                | (normal_dot < threshold_dot)
            )
            edge[target] |= discontinuity
    return dilate(edge, dilation_pixels), valid


def uniform_mean(image: np.ndarray, window: int = 11) -> np.ndarray:
    expect(window % 2 == 1, "SSIM window must be odd")
    radius = window // 2
    padded = np.pad(image.astype(np.float64), radius, mode="reflect")
    integral = np.pad(padded, ((1, 0), (1, 0)), mode="constant")
    integral = np.cumsum(np.cumsum(integral, axis=0), axis=1)
    result = (
        integral[window:, window:]
        - integral[:-window, window:]
        - integral[window:, :-window]
        + integral[:-window, :-window]
    )
    return result / float(window * window)


def ssim_map(reference: np.ndarray, candidate: np.ndarray) -> np.ndarray:
    reference64 = reference.astype(np.float64)
    candidate64 = candidate.astype(np.float64)
    mu_reference = uniform_mean(reference64)
    mu_candidate = uniform_mean(candidate64)
    variance_reference = uniform_mean(reference64 * reference64) - (
        mu_reference * mu_reference
    )
    variance_candidate = uniform_mean(candidate64 * candidate64) - (
        mu_candidate * mu_candidate
    )
    covariance = uniform_mean(reference64 * candidate64) - (
        mu_reference * mu_candidate
    )
    c1 = 0.01**2
    c2 = 0.03**2
    numerator = (2.0 * mu_reference * mu_candidate + c1) * (
        2.0 * covariance + c2
    )
    denominator = (
        mu_reference * mu_reference + mu_candidate * mu_candidate + c1
    ) * (variance_reference + variance_candidate + c2)
    return np.clip(numerator / np.maximum(denominator, 1.0e-15), -1.0, 1.0)


def region_error_metrics(
    reference: np.ndarray,
    candidate: np.ndarray,
    quality_map: np.ndarray,
    mask: np.ndarray,
) -> dict[str, Any]:
    selected = mask.astype(bool)
    count = int(np.count_nonzero(selected))
    expect(count > 0, "quality region is empty")
    error = candidate - reference
    absolute = np.abs(error[selected]).astype(np.float64)
    squared = np.square(error[selected].astype(np.float64))
    mse = float(np.mean(squared))
    ordered = np.sort(absolute)
    return {
        "count": count,
        "coveragePercent": count / selected.size * 100.0,
        "mae": float(np.mean(absolute)),
        "rmse": math.sqrt(mse),
        "p95AbsoluteError": nearest_rank(ordered.tolist(), 0.95),
        "maxAbsoluteError": float(ordered[-1]),
        "psnrDb": None if mse == 0.0 else 10.0 * math.log10(1.0 / mse),
        "ssim": float(np.mean(quality_map[selected])),
    }


def compare_ao(
    reference: np.ndarray,
    candidate: np.ndarray,
    foreground: np.ndarray,
    edge: np.ndarray,
) -> dict[str, Any]:
    expect(reference.shape == candidate.shape, "AO comparison shape mismatch")
    quality = ssim_map(reference, candidate)
    all_pixels = np.ones(reference.shape, dtype=bool)
    return {
        "global": region_error_metrics(
            reference, candidate, quality, all_pixels
        ),
        "foreground": region_error_metrics(
            reference, candidate, quality, foreground
        ),
        "edge": region_error_metrics(reference, candidate, quality, edge),
    }


def load_rgb(path: str) -> np.ndarray:
    with Image.open(path) as image:
        pixels = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
    expect(pixels.shape == (1080, 1920, 3), f"final capture shape {pixels.shape}")
    return pixels


def compare_ldr(
    reference: np.ndarray,
    candidate: np.ndarray,
    edge: np.ndarray,
) -> dict[str, Any]:
    error = candidate.astype(np.float64) - reference.astype(np.float64)
    global_mse = float(np.mean(np.square(error)))
    luminance_reference = (
        0.2126 * reference[..., 0]
        + 0.7152 * reference[..., 1]
        + 0.0722 * reference[..., 2]
    )
    luminance_candidate = (
        0.2126 * candidate[..., 0]
        + 0.7152 * candidate[..., 1]
        + 0.0722 * candidate[..., 2]
    )
    quality = ssim_map(luminance_reference, luminance_candidate)
    edge_rgb = np.repeat(edge[..., None], 3, axis=2)
    edge_mse = float(np.mean(np.square(error[edge_rgb])))
    return {
        "global": {
            "psnrDb": (
                None
                if global_mse == 0.0
                else 10.0 * math.log10(1.0 / global_mse)
            ),
            "ssimLuminance": float(np.mean(quality)),
        },
        "edge": {
            "psnrDb": (
                None
                if edge_mse == 0.0
                else 10.0 * math.log10(1.0 / edge_mse)
            ),
            "ssimLuminance": float(np.mean(quality[edge])),
        },
    }


def window_sum_map(values: np.ndarray, height: int, width: int) -> np.ndarray:
    integral = np.pad(values.astype(np.float64), ((1, 0), (1, 0)))
    integral = np.cumsum(np.cumsum(integral, axis=0), axis=1)
    return (
        integral[height:, width:]
        - integral[:-height, width:]
        - integral[height:, :-width]
        + integral[:-height, :-width]
    )


def rectangles_overlap(
    first: tuple[int, int, int, int], second: tuple[int, int, int, int]
) -> bool:
    ax, ay, aw, ah = first
    bx, by, bw, bh = second
    return not (
        ax + aw <= bx or bx + bw <= ax or ay + ah <= by or by + bh <= ay
    )


def choose_native_crops(
    reference: np.ndarray,
    raw: np.ndarray,
    edge: np.ndarray,
    foreground: np.ndarray,
    crop_width: int = 256,
    crop_height: int = 192,
) -> list[dict[str, Any]]:
    edge_score = window_sum_map(
        np.abs(raw - reference) * edge.astype(np.float32),
        crop_height,
        crop_width,
    )
    edge_count = window_sum_map(
        edge.astype(np.float32), crop_height, crop_width
    )
    edge_score[edge_count < 16.0] = -1.0
    expect(
        bool(np.any(edge_score >= 0.0)),
        "no quality crop contains enough depth/normal edge pixels",
    )
    edge_index = int(np.argmax(edge_score))
    edge_y, edge_x = np.unravel_index(edge_index, edge_score.shape)
    edge_crop = (int(edge_x), int(edge_y), crop_width, crop_height)

    contact_values = (
        np.maximum(0.0, 1.0 - reference) * foreground.astype(np.float32)
    )
    contact_score = window_sum_map(contact_values, crop_height, crop_width)
    foreground_count = window_sum_map(
        foreground.astype(np.float32), crop_height, crop_width
    )
    contact_score[
        foreground_count < crop_width * crop_height * 0.25
    ] = -1.0
    # Deterministic descending search; avoid duplicating the edge crop.
    flattened = np.argsort(contact_score.ravel())[::-1]
    contact_crop = edge_crop
    for flat_index in flattened:
        if contact_score.ravel()[int(flat_index)] < 0.0:
            break
        y, x = np.unravel_index(int(flat_index), contact_score.shape)
        candidate = (int(x), int(y), crop_width, crop_height)
        if not rectangles_overlap(candidate, edge_crop):
            contact_crop = candidate
            break
    expect(
        not rectangles_overlap(contact_crop, edge_crop),
        "could not select a non-overlapping contact-shadow crop",
    )
    return [
        {
            "name": "edge",
            "x": edge_crop[0],
            "y": edge_crop[1],
            "width": crop_width,
            "height": crop_height,
            "selection": (
                "maximum window sum of edgeMask * abs(HalfRaw64-Full64)"
            ),
        },
        {
            "name": "contact",
            "x": contact_crop[0],
            "y": contact_crop[1],
            "width": crop_width,
            "height": crop_height,
            "selection": (
                "maximum non-overlapping Full64 foreground occlusion window"
            ),
        },
    ]


def get_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = (
        ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf")
        if bold
        else ("C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf")
    )
    for candidate in candidates:
        try:
            return ImageFont.truetype(candidate, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def gray_image(values: np.ndarray) -> Image.Image:
    pixels = np.rint(np.clip(values, 0.0, 1.0) * 255.0).astype(np.uint8)
    return Image.fromarray(pixels, mode="L").convert("RGB")


def heat_image(values: np.ndarray, scale: float) -> Image.Image:
    normalized = np.clip(values / max(scale, 1.0e-8), 0.0, 1.0)
    red = np.clip(2.2 * normalized, 0.0, 1.0)
    green = np.clip(2.2 * (1.0 - np.abs(normalized - 0.5) * 2.0), 0.0, 1.0)
    blue = np.clip(2.2 * (1.0 - normalized), 0.0, 1.0)
    rgb = np.stack((red, green * 0.75, blue * 0.65), axis=2)
    return Image.fromarray(np.rint(rgb * 255.0).astype(np.uint8), mode="RGB")


def mask_image(values: np.ndarray) -> Image.Image:
    pixels = np.zeros((*values.shape, 3), dtype=np.uint8)
    pixels[values] = (20, 220, 220)
    return Image.fromarray(pixels, mode="RGB")


def create_quality_crop_figures(
    figures_directory: Path,
    scene: str,
    reference: np.ndarray,
    raw: np.ndarray,
    bilateral: np.ndarray,
    edge: np.ndarray,
    crops: list[dict[str, Any]],
    final_reference: np.ndarray,
    final_raw: np.ndarray,
    final_bilateral: np.ndarray,
) -> dict[str, str]:
    output_directory = figures_directory / "quality-crops"
    output_directory.mkdir(parents=True, exist_ok=True)
    scale_factor = 2
    crop_width = int(crops[0]["width"])
    crop_height = int(crops[0]["height"])
    tile_width = crop_width * scale_factor
    tile_height = crop_height * scale_factor
    labels = (
        "Full-64 reference",
        "Half-64 raw",
        "Half-64 bilateral",
        "|Raw - reference|",
        "|Bilateral - reference|",
        "Depth/normal edge mask",
    )
    label_height = 45
    crop_header = 42
    width = tile_width * len(labels)
    height = 50 + len(crops) * (crop_header + label_height + tile_height)
    sheet = Image.new("RGB", (width, height), COLORS["background"])
    draw = ImageDraw.Draw(sheet)
    title_font = get_font(25, True)
    label_font = get_font(17)
    draw.text(
        (18, 10),
        f"{scene}: native AO crops (nearest-neighbor 2x)",
        font=title_font,
        fill=COLORS["text"],
    )
    raw_error = np.abs(raw - reference)
    bilateral_error = np.abs(bilateral - reference)
    heat_scale = max(
        1.0e-4,
        float(np.percentile(raw_error[edge], 99.0)),
    )
    for crop_index, crop in enumerate(crops):
        x = int(crop["x"])
        y = int(crop["y"])
        w = int(crop["width"])
        h = int(crop["height"])
        top = 50 + crop_index * (crop_header + label_height + tile_height)
        draw.text(
            (18, top + 8),
            f"{crop['name']} crop: x={x}, y={y}, {w}x{h}",
            font=label_font,
            fill=COLORS["text"],
        )
        slices = np.s_[y : y + h, x : x + w]
        images = (
            gray_image(reference[slices]),
            gray_image(raw[slices]),
            gray_image(bilateral[slices]),
            heat_image(raw_error[slices], heat_scale),
            heat_image(bilateral_error[slices], heat_scale),
            mask_image(edge[slices]),
        )
        for column, (label, image) in enumerate(zip(labels, images)):
            left = column * tile_width
            label_y = top + crop_header
            text_box = draw.textbbox((0, 0), label, font=label_font)
            text_width = text_box[2] - text_box[0]
            draw.text(
                (left + (tile_width - text_width) / 2.0, label_y + 9),
                label,
                font=label_font,
                fill=COLORS["text"],
            )
            enlarged = image.resize(
                (tile_width, tile_height), Image.Resampling.NEAREST
            )
            sheet.paste(enlarged, (left, label_y + label_height))
    ao_name = f"{scene}-ao-native-crops.png"
    sheet.save(output_directory / ao_name, format="PNG")

    final_labels = ("Full-64 reference", "Half-64 raw", "Half-64 bilateral")
    final_width = tile_width * len(final_labels)
    final_height = 50 + len(crops) * (crop_header + label_height + tile_height)
    final_sheet = Image.new("RGB", (final_width, final_height), COLORS["background"])
    final_draw = ImageDraw.Draw(final_sheet)
    final_draw.text(
        (18, 10),
        f"{scene}: final-lighting crops (nearest-neighbor 2x)",
        font=title_font,
        fill=COLORS["text"],
    )
    for crop_index, crop in enumerate(crops):
        x = int(crop["x"])
        y = int(crop["y"])
        w = int(crop["width"])
        h = int(crop["height"])
        top = 50 + crop_index * (crop_header + label_height + tile_height)
        final_draw.text(
            (18, top + 8),
            f"{crop['name']} crop: x={x}, y={y}, {w}x{h}",
            font=label_font,
            fill=COLORS["text"],
        )
        slices = np.s_[y : y + h, x : x + w, :]
        images = (
            Image.fromarray(
                np.rint(np.clip(final_reference[slices], 0.0, 1.0) * 255.0).astype(
                    np.uint8
                ),
                mode="RGB",
            ),
            Image.fromarray(
                np.rint(np.clip(final_raw[slices], 0.0, 1.0) * 255.0).astype(
                    np.uint8
                ),
                mode="RGB",
            ),
            Image.fromarray(
                np.rint(
                    np.clip(final_bilateral[slices], 0.0, 1.0) * 255.0
                ).astype(np.uint8),
                mode="RGB",
            ),
        )
        for column, (label, image) in enumerate(zip(final_labels, images)):
            left = column * tile_width
            label_y = top + crop_header
            text_box = final_draw.textbbox((0, 0), label, font=label_font)
            text_width = text_box[2] - text_box[0]
            final_draw.text(
                (left + (tile_width - text_width) / 2.0, label_y + 9),
                label,
                font=label_font,
                fill=COLORS["text"],
            )
            final_sheet.paste(
                image.resize(
                    (tile_width, tile_height), Image.Resampling.NEAREST
                ),
                (left, label_y + label_height),
            )
    final_name = f"{scene}-final-native-crops.png"
    final_sheet.save(output_directory / final_name, format="PNG")
    return {
        "aoCrops": f"figures/quality-crops/{ao_name}",
        "finalCrops": f"figures/quality-crops/{final_name}",
    }


def calculate_quality(
    aggregates: list[dict[str, Any]],
    figures_directory: Path,
) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    for scene, groups in sorted(group_map(aggregates).items()):
        full64 = read_pfm(Path(groups["legacy-full64"]["captures"]["aoPfm"]))
        raw64_low = read_pfm(Path(groups["half-raw64"]["captures"]["aoPfm"]))
        bilateral64_raw_low = read_pfm(
            Path(groups["half-bilateral64"]["captures"]["rawHalfPfm"])
        )
        expect(
            np.array_equal(raw64_low, bilateral64_raw_low),
            f"{scene}: Half-64 Generate changed between raw and bilateral modes",
        )
        raw64 = gl_linear_reconstruct(raw64_low, 1920, 1080)
        bilateral64 = read_pfm(
            Path(groups["half-bilateral64"]["captures"]["aoPfm"])
        )
        full32 = read_pfm(Path(groups["legacy-full32"]["captures"]["aoPfm"]))
        bilateral32 = read_pfm(
            Path(groups["half-bilateral32"]["captures"]["aoPfm"])
        )
        depth = read_pfm(Path(groups["legacy-full64"]["captures"]["depthPfm"]))
        normal = read_pfm(Path(groups["legacy-full64"]["captures"]["normalPfm"]))
        for name, image in (
            ("Full64", full64),
            ("HalfRaw64 reconstructed", raw64),
            ("HalfBilateral64", bilateral64),
            ("Full32", full32),
            ("HalfBilateral32", bilateral32),
        ):
            expect(image.shape == (1080, 1920), f"{scene}: {name} shape")
            expect(
                float(np.min(image)) >= -1.0e-4
                and float(np.max(image)) <= 1.0001,
                f"{scene}: {name} is outside [0,1]",
            )
        edge, foreground = build_edge_mask(depth, normal)
        raw_metrics = compare_ao(full64, raw64, foreground, edge)
        bilateral_metrics = compare_ao(full64, bilateral64, foreground, edge)
        extension_metrics = compare_ao(full32, bilateral32, foreground, edge)
        crops = choose_native_crops(full64, raw64, edge, foreground)

        final_reference = load_rgb(
            str(groups["legacy-full64"]["captures"]["finalPpm"])
        )
        final_raw = load_rgb(str(groups["half-raw64"]["captures"]["finalPpm"]))
        final_bilateral = load_rgb(
            str(groups["half-bilateral64"]["captures"]["finalPpm"])
        )
        ldr_raw = compare_ldr(final_reference, final_raw, edge)
        ldr_bilateral = compare_ldr(final_reference, final_bilateral, edge)
        figures = create_quality_crop_figures(
            figures_directory,
            scene,
            full64,
            raw64,
            bilateral64,
            edge,
            crops,
            final_reference,
            final_raw,
            final_bilateral,
        )
        edge_mask_name = f"{scene}-edge-mask.png"
        mask_image(edge).save(figures_directory / edge_mask_name, format="PNG")
        raw_edge_mae = float(raw_metrics["edge"]["mae"])
        bilateral_edge_mae = float(bilateral_metrics["edge"]["mae"])
        results.append(
            {
                "scene": scene,
                "reference": "legacy-full64",
                "half64GenerationEquivalence": {
                    "floatValuesExactlyEqual": True,
                    "maximumAbsoluteDifference": 0.0,
                    "description": (
                        "Half-raw64 AO PFM equals Half-bilateral64 raw Generate "
                        "PFM before upsampling"
                    ),
                },
                "edgeMask": {
                    "definition": {
                        "validityDiscontinuity": True,
                        "relativeLinearDepthThreshold": 0.02,
                        "normalAngleDegrees": 25.0,
                        "dilationPixels": 3,
                    },
                    "pixelCount": int(np.count_nonzero(edge)),
                    "coveragePercent": float(np.mean(edge) * 100.0),
                    "foregroundPixelCount": int(np.count_nonzero(foreground)),
                    "figure": f"figures/{edge_mask_name}",
                },
                "comparisons": {
                    "half-raw64-vs-legacy-full64": raw_metrics,
                    "half-bilateral64-vs-legacy-full64": bilateral_metrics,
                    "half-bilateral32-vs-legacy-full32": extension_metrics,
                },
                "edgeMaeReductionRawToBilateral64Percent": (
                    (raw_edge_mae - bilateral_edge_mae)
                    / raw_edge_mae
                    * 100.0
                    if raw_edge_mae
                    else None
                ),
                "bilateral64EdgeErrorLowerThanRaw64": (
                    bilateral_edge_mae < raw_edge_mae
                    and float(
                        bilateral_metrics["edge"]["p95AbsoluteError"]
                    )
                    < float(raw_metrics["edge"]["p95AbsoluteError"])
                ),
                "finalLdr8BitSupplementary": {
                    "half-raw64-vs-legacy-full64": ldr_raw,
                    "half-bilateral64-vs-legacy-full64": ldr_bilateral,
                },
                "crops": crops,
                "figures": figures,
            }
        )
    return results


def get_display_names(manifest: dict[str, Any]) -> dict[str, str]:
    return {
        str(scene["id"]): str(scene.get("displayName") or scene["id"])
        for scene in manifest.get("scenes", [])
    }


def draw_vertical_scale(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    maximum: float,
    ticks: int = 5,
    unit_label: str = "ms",
) -> None:
    left, top, right, bottom = box
    font = get_font(16)
    draw.line((left, top, left, bottom), fill=COLORS["axis"], width=2)
    draw.line((left, bottom, right, bottom), fill=COLORS["axis"], width=2)
    for tick in range(ticks + 1):
        value = maximum * tick / ticks
        y = bottom - (bottom - top) * tick / ticks
        draw.line((left, y, right, y), fill=COLORS["grid"], width=1)
        draw.text(
            (left - 75, y - 9),
            f"{value:.2f}",
            font=font,
            fill=COLORS["muted"],
        )
    draw.text(
        (left - 55, top - 27),
        unit_label,
        font=font,
        fill=COLORS["muted"],
    )


def performance_chart(
    path: Path,
    aggregates: list[dict[str, Any]],
    display_names: dict[str, str],
) -> None:
    scenes = sorted({item["scene"] for item in aggregates})
    width = 1900
    height = 135 + 455 * len(scenes)
    image = Image.new("RGB", (width, height), COLORS["background"])
    draw = ImageDraw.Draw(image)
    title_font = get_font(32, True)
    panel_font = get_font(23, True)
    label_font = get_font(15)
    draw.text(
        (55, 28),
        "Half-resolution SSAO: GPU timing at 1920x1080",
        font=title_font,
        fill=COLORS["text"],
    )
    legends = (
        ("GPU Frame", COLORS["frame"]),
        ("SSAO total", COLORS["total"]),
        ("Generate", COLORS["generate"]),
        ("Upsample", COLORS["upsample"]),
    )
    legend_x = 1030
    for label, color in legends:
        draw.rectangle((legend_x, 42, legend_x + 24, 61), fill=color)
        draw.text((legend_x + 32, 39), label, font=label_font, fill=COLORS["text"])
        legend_x += 195

    order = {name: index for index, name in enumerate(CONFIGURATION_ORDER)}
    for scene_index, scene in enumerate(scenes):
        groups = sorted(
            (item for item in aggregates if item["scene"] == scene),
            key=lambda item: order[item["configuration"]],
        )
        panel_top = 100 + scene_index * 455
        draw.rounded_rectangle(
            (28, panel_top, width - 28, panel_top + 420),
            radius=14,
            fill=COLORS["panel"],
        )
        draw.text(
            (58, panel_top + 18),
            display_names.get(scene, scene),
            font=panel_font,
            fill=COLORS["text"],
        )
        chart = (125, panel_top + 80, width - 65, panel_top + 340)
        maximum = max(
            float(item["metrics"]["gpuFrame"]["pooled"]["p99"])
            for item in groups
        )
        maximum = max(0.25, math.ceil(maximum * 1.15 * 4.0) / 4.0)
        draw_vertical_scale(draw, chart, maximum)
        left, top, right, bottom = chart
        slot = (right - left) / len(groups)
        for group_index, group in enumerate(groups):
            center = left + slot * (group_index + 0.5)
            values = (
                (
                    float(group["metrics"]["gpuFrame"]["pooled"]["median"]),
                    COLORS["frame"],
                ),
                (
                    float(group["metrics"]["ssaoTotalGpu"]["pooled"]["median"]),
                    COLORS["total"],
                ),
                (
                    float(
                        group["metrics"]["ssaoGenerateGpu"]["pooled"]["median"]
                    ),
                    COLORS["generate"],
                ),
                (
                    (
                        float(
                            group["metrics"]["ssaoUpsampleGpu"]["pooled"][
                                "median"
                            ]
                        )
                        if group["metrics"]["ssaoUpsampleGpu"]["pooled"]["count"]
                        else 0.0
                    ),
                    COLORS["upsample"],
                ),
            )
            bar_width = 32
            for bar_index, (value, color) in enumerate(values):
                x0 = center + (bar_index - 2.0) * (bar_width + 6)
                y = bottom - (bottom - top) * value / maximum
                if value > 0.0:
                    draw.rectangle(
                        (x0, y, x0 + bar_width, bottom), fill=color
                    )
            total = values[1][0]
            draw.text(
                (center - 38, bottom - (bottom - top) * total / maximum - 22),
                f"{total:.3f}",
                font=label_font,
                fill=COLORS["total"],
            )
            label = CONFIGURATION_LABELS[group["configuration"]].replace(
                " ", "\n"
            )
            lines = label.splitlines()
            for line_index, line in enumerate(lines):
                text_box = draw.textbbox((0, 0), line, font=label_font)
                draw.text(
                    (
                        center - (text_box[2] - text_box[0]) / 2.0,
                        bottom + 8 + line_index * 18,
                    ),
                    line,
                    font=label_font,
                    fill=COLORS["text"],
                )
    image.save(path, format="PNG")


def quality_chart(
    path: Path,
    quality: list[dict[str, Any]],
    display_names: dict[str, str],
) -> None:
    width, height = 1350, 650
    image = Image.new("RGB", (width, height), COLORS["background"])
    draw = ImageDraw.Draw(image)
    title_font = get_font(30, True)
    label_font = get_font(18)
    draw.text(
        (50, 28),
        "Float AO error on depth/normal edge mask",
        font=title_font,
        fill=COLORS["text"],
    )
    draw.text(
        (50, 72),
        "Reference: Full-64 Legacy R16F readback; lower MAE is better.",
        font=label_font,
        fill=COLORS["muted"],
    )
    chart = (145, 135, 1280, 535)
    maximum = max(
        max(
            float(
                item["comparisons"]["half-raw64-vs-legacy-full64"]["edge"][
                    "mae"
                ]
            ),
            float(
                item["comparisons"][
                    "half-bilateral64-vs-legacy-full64"
                ]["edge"]["mae"]
            ),
        )
        for item in quality
    )
    maximum = max(0.001, maximum * 1.25)
    draw_vertical_scale(
        draw,
        chart,
        maximum,
        unit_label="absolute AO error",
    )
    left, top, right, bottom = chart
    slot = (right - left) / len(quality)
    for index, item in enumerate(quality):
        center = left + slot * (index + 0.5)
        raw = float(
            item["comparisons"]["half-raw64-vs-legacy-full64"]["edge"]["mae"]
        )
        bilateral = float(
            item["comparisons"]["half-bilateral64-vs-legacy-full64"]["edge"][
                "mae"
            ]
        )
        for offset, value, color in (
            (-55, raw, COLORS["raw"]),
            (15, bilateral, COLORS["bilateral"]),
        ):
            y = bottom - (bottom - top) * value / maximum
            draw.rectangle((center + offset, y, center + offset + 40, bottom), fill=color)
            draw.text(
                (center + offset - 5, y - 24),
                f"{value:.5f}",
                font=label_font,
                fill=color,
            )
        label = display_names.get(item["scene"], item["scene"])
        text_box = draw.textbbox((0, 0), label, font=label_font)
        draw.text(
            (center - (text_box[2] - text_box[0]) / 2.0, bottom + 18),
            label,
            font=label_font,
            fill=COLORS["text"],
        )
        reduction = item["edgeMaeReductionRawToBilateral64Percent"]
        draw.text(
            (center - 95, bottom + 50),
            f"MAE reduction: {float(reduction):.1f}%",
            font=label_font,
            fill=COLORS["text"],
        )
    draw.rectangle((820, 88, 844, 106), fill=COLORS["raw"])
    draw.text((852, 84), "Half-64 raw", font=label_font, fill=COLORS["text"])
    draw.rectangle((1035, 88, 1059, 106), fill=COLORS["bilateral"])
    draw.text((1067, 84), "Half-64 bilateral", font=label_font, fill=COLORS["text"])
    image.save(path, format="PNG")


def write_summary_csv(path: Path, aggregates: list[dict[str, Any]]) -> None:
    fields = [
        "scene",
        "configuration",
        "mode",
        "samples",
        "ssaoTotalGpuShareOfGpuFrameMedianPercent",
        "metric",
        "unit",
        "pooledCount",
        "pooledMean",
        "pooledMedian",
        "pooledP95",
        "pooledP99",
        "processCount",
        "processMedianMin",
        "processMedianMax",
        "processMedianRangeAbsolute",
        "processMedianRangePercent",
        "processMedianCvPercent",
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for group in aggregates:
            ssao_share = (
                float(
                    group["metrics"]["ssaoTotalGpu"]["pooled"]["median"]
                )
                / float(group["metrics"]["gpuFrame"]["pooled"]["median"])
                * 100.0
            )
            for key, label, _unit in METRICS:
                metric = group["metrics"][key]
                pooled = metric["pooled"]
                dispersion = metric["processMedianDispersion"]
                writer.writerow(
                    {
                        "scene": group["scene"],
                        "configuration": group["configuration"],
                        "mode": group["mode"],
                        "samples": group["samples"],
                        "ssaoTotalGpuShareOfGpuFrameMedianPercent": ssao_share,
                        "metric": label,
                        "unit": metric["unit"],
                        "pooledCount": pooled["count"],
                        "pooledMean": pooled["mean"],
                        "pooledMedian": pooled["median"],
                        "pooledP95": pooled["p95"],
                        "pooledP99": pooled["p99"],
                        "processCount": dispersion["count"],
                        "processMedianMin": dispersion["min"],
                        "processMedianMax": dispersion["max"],
                        "processMedianRangeAbsolute": dispersion["rangeAbsolute"],
                        "processMedianRangePercent": dispersion["rangePercent"],
                        "processMedianCvPercent": dispersion[
                            "coefficientOfVariationPercent"
                        ],
                    }
                )


def write_process_csv(path: Path, aggregates: list[dict[str, Any]]) -> None:
    fields = [
        "scene",
        "configuration",
        "process",
        "metric",
        "unit",
        "count",
        "mean",
        "min",
        "max",
        "median",
        "p95",
        "p99",
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for group in aggregates:
            for key, label, _unit in METRICS:
                metric = group["metrics"][key]
                for process in metric["perProcess"]:
                    writer.writerow(
                        {
                            "scene": group["scene"],
                            "configuration": group["configuration"],
                            "process": process["process"],
                            "metric": label,
                            "unit": metric["unit"],
                            "count": process["count"],
                            "mean": process["mean"],
                            "min": process["min"],
                            "max": process["max"],
                            "median": process["median"],
                            "p95": process["p95"],
                            "p99": process["p99"],
                        }
                    )


def write_quality_csv(path: Path, quality: list[dict[str, Any]]) -> None:
    fields = [
        "scene",
        "comparison",
        "region",
        "count",
        "coveragePercent",
        "mae",
        "rmse",
        "p95AbsoluteError",
        "maxAbsoluteError",
        "psnrDb",
        "ssim",
    ]
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for scene in quality:
            for comparison, regions in scene["comparisons"].items():
                for region, metric in regions.items():
                    writer.writerow(
                        {
                            "scene": scene["scene"],
                            "comparison": comparison,
                            "region": region,
                            **metric,
                        }
                    )


def format_triplet(metric: dict[str, Any], digits: int = 3) -> str:
    pooled = metric["pooled"]
    if pooled["median"] is None:
        return "—"
    return (
        f"{float(pooled['median']):.{digits}f} / "
        f"{float(pooled['p95']):.{digits}f} / "
        f"{float(pooled['p99']):.{digits}f}"
    )


def format_dispersion(metric: dict[str, Any]) -> str:
    value = metric["processMedianDispersion"]["rangePercent"]
    return "—" if value is None else f"{float(value):.2f}%"


def markdown_report(
    path: Path,
    manifest: dict[str, Any],
    aggregates: list[dict[str, Any]],
    comparisons: list[dict[str, Any]],
    quality: list[dict[str, Any]],
    decision: str,
    decision_reason: str,
    go_evidence: dict[str, Any],
) -> None:
    display_names = get_display_names(manifest)
    protocol = manifest["protocol"]
    system = manifest.get("system", {})
    source = manifest.get("source", {})
    groups = group_map(aggregates)
    lines: list[str] = [
        "# Half-Resolution SSAO + Depth/Normal-aware Bilateral Upsampling",
        "",
        f"- 批次：`{manifest.get('batchId', '')}`（`{manifest.get('preset', '')}`）",
        f"- 平台：{system.get('cpu', 'unknown')} / {system.get('gpu', 'unknown')}",
        f"- 构建：Release x64，提交 `{source.get('gitCommit', 'unknown')}`"
        + ("，dirty worktree" if source.get("worktreeDirty") else ""),
        f"- Release executable SHA-256：`{source.get('releaseExecutableSha256', '')}`",
        f"- 协议：每配置 {protocol['warmupFrames']} 帧预热、"
        f"{protocol['measuredFrames']} 帧采样、"
        f"{protocol['independentProcesses']} 个独立进程；顺序为正序/逆序/轮转",
        "- 固定条件：1920×1080、VSync/Bloom/阴影/自动热重载关闭、固定相机与输入；"
        "64-sample 主 A/B 保持 radius、bias、kernel、灯光与其他状态一致。",
        "",
        "## Go / No-Go",
        "",
    ]
    if decision == "go":
        lines.extend([f"**Go。** {decision_reason}", ""])
    elif decision == "no-go":
        lines.extend([f"**No-Go。** {decision_reason}", ""])
    else:
        lines.extend(
            [
                "**Pending。** 数据可以用于检查链路，但尚未附加人工、数据驱动的最终判断。",
                "",
            ]
        )
    lines.extend(
        [
            f"- 两场景每个进程 Half-64 Bilateral SSAO total GPU 均低于 Full-64："
            f"{'是' if go_evidence['allScenesEveryProcessFaster'] else '否'}。",
            f"- 两场景 Bilateral 的 edge MAE 与 edge P95 均低于 Half raw："
            f"{'是' if go_evidence['allScenesEdgeErrorLower'] else '否'}。",
            "- 上述是 Go 的必要证据，不是预设百分比门槛；“有意义”的幅度由下列真实数据解释。",
            "",
            "## GPU 性能",
            "",
            f"时间格式均为 `Median / P95 / P99`（ms）。进程离散是 "
            f"{protocol['independentProcesses']} 个独立进程 Median 的相对极差。",
            "`SSAO Pass` 包含 Generate 与可选 Upsample；嵌套 zone 不可相加到 GPU Frame。",
            "",
        ]
    )
    order = {name: index for index, name in enumerate(CONFIGURATION_ORDER)}
    for scene in sorted(groups):
        lines.extend(
            [
                f"### {display_names.get(scene, scene)}",
                "",
                "| 配置 | GPU Frame | Deferred Pass GPU | SSAO total GPU | "
                "SSAO / GPU Frame (Median) | Generate GPU | Upsample GPU | "
                "total 进程离散 | Draw Call |",
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for configuration, group in sorted(
            groups[scene].items(), key=lambda item: order[item[0]]
        ):
            metrics = group["metrics"]
            gpu_frame_median = float(metrics["gpuFrame"]["pooled"]["median"])
            ssao_total_median = float(
                metrics["ssaoTotalGpu"]["pooled"]["median"]
            )
            lines.append(
                f"| {CONFIGURATION_LABELS[configuration]} | "
                f"{format_triplet(metrics['gpuFrame'])} | "
                f"{format_triplet(metrics['deferredGpu'])} | "
                f"{format_triplet(metrics['ssaoTotalGpu'])} | "
                f"{ssao_total_median / gpu_frame_median * 100.0:.2f}% | "
                f"{format_triplet(metrics['ssaoGenerateGpu'])} | "
                f"{format_triplet(metrics['ssaoUpsampleGpu'])} | "
                f"{format_dispersion(metrics['ssaoTotalGpu'])} | "
                f"{format_triplet(metrics['drawCalls'], 0)} |"
            )
        scene_comparison = next(
            item for item in comparisons if item["scene"] == scene
        )
        lines.extend(
            [
                "",
                f"- Full-64 → Half-64 raw：SSAO total GPU Median "
                f"{scene_comparison['mainAb']['full64TotalGpuMedianMs']:.3f} → "
                f"{scene_comparison['mainAb']['halfRaw64TotalGpuMedianMs']:.3f} ms。",
                f"- Full-64 → Half-64 bilateral：SSAO total GPU Median "
                f"{scene_comparison['mainAb']['full64TotalGpuMedianMs']:.3f} → "
                f"{scene_comparison['mainAb']['halfBilateral64TotalGpuMedianMs']:.3f} ms。",
                "",
            ]
        )
    lines.extend(
        [
            "![GPU timing](figures/ssao-half-resolution-timing.png)",
            "",
            "## CPU 性能",
            "",
            "CPU Frame 是主循环墙钟时间，可能因 GPU back-pressure 增长；不能把它解释为 SSAO CPU 工作。"
            "CPU 与 GPU 时间也不能相加。",
            "",
        ]
    )
    for scene in sorted(groups):
        lines.extend(
            [
                f"### {display_names.get(scene, scene)}",
                "",
                "| 配置 | CPU Frame | SSAO total CPU | Generate CPU | Upsample CPU | "
                "CPU Frame 进程离散 |",
                "|---|---:|---:|---:|---:|---:|",
            ]
        )
        for configuration, group in sorted(
            groups[scene].items(), key=lambda item: order[item[0]]
        ):
            metrics = group["metrics"]
            lines.append(
                f"| {CONFIGURATION_LABELS[configuration]} | "
                f"{format_triplet(metrics['cpuFrame'])} | "
                f"{format_triplet(metrics['ssaoTotalCpu'])} | "
                f"{format_triplet(metrics['ssaoGenerateCpu'])} | "
                f"{format_triplet(metrics['ssaoUpsampleCpu'])} | "
                f"{format_dispersion(metrics['cpuFrame'])} |"
            )
        lines.extend(["", ""])
    lines.extend(
        [
            "## Float AO 质量",
            "",
            "正式误差使用测量区间外读取的 R16F/float PFM。Full-64 Legacy 是工程参考，"
            "不是物理 ground truth。Half raw 按 OpenGL `GL_LINEAR` 的 normalized pixel-center "
            "映射并使用 clamp-to-edge 在报告端精确重建到 1920×1080。",
            "",
            "Edge mask 来自 Full-64 同帧 GBuffer：前景有效性变化、相邻线性深度相对差大于 2%，"
            "或法线夹角大于 25°，随后膨胀 3 像素。全局、前景与 edge 指标分别报告，"
            "避免白色背景支配结论。SSIM 是 11×11 uniform-window local SSIM。",
            "",
            "变量隔离校验：每个场景的 Half-64 raw AO 与 Half-64 bilateral 的 "
            "Generate 原始 half-resolution float 值逐元素完全一致；Bilateral 的质量差异"
            "因此来自上采样阶段，而不是 Generate 输入变化。",
            "",
            "| 场景 | 模式 | Global MAE | Foreground MAE | Edge MAE | Edge P95 | "
            "Edge PSNR | Edge SSIM |",
            "|---|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for item in quality:
        for comparison, label in (
            ("half-raw64-vs-legacy-full64", "Half-64 raw"),
            ("half-bilateral64-vs-legacy-full64", "Half-64 bilateral"),
            ("half-bilateral32-vs-legacy-full32", "Half-32 bilateral vs Full-32"),
        ):
            metrics = item["comparisons"][comparison]
            edge_psnr = metrics["edge"]["psnrDb"]
            lines.append(
                f"| {display_names.get(item['scene'], item['scene'])} | {label} | "
                f"{metrics['global']['mae']:.6f} | "
                f"{metrics['foreground']['mae']:.6f} | "
                f"{metrics['edge']['mae']:.6f} | "
                f"{metrics['edge']['p95AbsoluteError']:.6f} | "
                f"{'∞' if edge_psnr is None else f'{edge_psnr:.2f} dB'} | "
                f"{metrics['edge']['ssim']:.6f} |"
            )
    lines.extend(
        [
            "",
            "![Edge error](figures/ssao-edge-quality.png)",
            "",
            "### 原生像素放大 crop",
            "",
            "每个场景的 edge crop 由 `edgeMask × |HalfRaw64−Full64|` 最大窗口确定；"
            "contact crop 由 Full-64 前景遮蔽量确定并避免与 edge crop 重叠。所有模式使用同一坐标，"
            "采用 nearest-neighbor 2× 放大；两张误差图使用相同色标。",
            "",
        ]
    )
    for item in quality:
        display = display_names.get(item["scene"], item["scene"])
        lines.extend(
            [
                f"#### {display}",
                "",
                f"![{display} float AO crops]({item['figures']['aoCrops']})",
                "",
                f"![{display} final crops]({item['figures']['finalCrops']})",
                "",
            ]
        )
    lines.extend(
        [
            "最终画面来自 8-bit PPM；其 PSNR/SSIM 仅作补充展示，不替代 float AO edge 指标"
            "或上述目视边缘检查。",
            "",
            "## 计时完整性与变量隔离",
            "",
            "- 每个 enabled 配置严格要求 CPU/GPU `SSAO Pass` 与 `SSAO Generate` "
            "各等于 measured frame 数；仅 bilateral 要求 `SSAO Upsample` 等量，其他模式必须为 0。",
            "- GPU zone 使用独立 `GL_TIMESTAMP` 起止点，`GPU Frame → Deferred Pass → "
            "SSAO Pass → Generate/Upsample` 的嵌套合法；缺失 query 的进程会被拒绝。",
            "- Full-64 与 Half-64 raw 的 Draw Call 应一致；bilateral 因独立全分辨率上采样"
            "必然多一个 draw，这是模式本身的唯一预期提交差异。",
            "- 同一批次只使用一个 Release executable；运行清单记录 SHA-256，并检查实验期间未变化。",
            "",
            "## 上一轮 kernel 混杂项更正",
            "",
            "当前实现一次生成 64 个 kernel 向量，其径向尺度按 `i/64` 计算；8/16/32 配置直接"
            "取这个 64 核序列的前缀。因此上一轮 14.10× / 13.94× 只能表述为“当前实现配置"
            "的实测扩张”，不能归因为纯样本数效应。该问题不影响 Full-64 相对 Off 的 "
            "2.216 / 2.638 ms 稳定瓶颈结论；本轮主 A/B 固定为 64 samples，Full-32 与 "
            "Half-32 也只在同一 32-sample kernel 前缀内比较分辨率。",
            "",
            "## 数据与检查点",
            "",
            "- 原始 JSON：[`raw/`](raw/)",
            "- 汇总 JSON：[`summary.json`](summary.json)",
            "- 性能 CSV：[`summary.csv`](summary.csv)",
            "- 独立进程 CSV：[`process-summary.csv`](process-summary.csv)",
            "- Float AO 质量 CSV：[`quality-summary.csv`](quality-summary.csv)",
            "- 捕获：[`captures/`](captures/)",
            "- 日志：[`logs/`](logs/)",
            "- 可恢复源码检查点：[`source-checkpoint/`](source-checkpoint/)",
            "",
            "## RenderDoc 验收状态",
            "",
            "当前机器未安装可调用的 RenderDoc，工作区也没有本轮 `.rdc`，因此本报告不声称"
            "已经完成 GPU 帧捕获。Full-64 与 Half-64 Bilateral 的可执行人工捕获命令、"
            "attachment 尺寸/格式、事件顺序、输入纹理和 Draw 验收项见"
            "[`SSAO_RENDERDOC_CAPTURE.md`](../../../../docs/SSAO_RENDERDOC_CAPTURE.md)。"
            "RenderDoc 运行只作诊断证据，不混入性能采样。",
            "",
            "## 已知限制",
            "",
            "- 质量指标来自两个固定相机的单个确定性帧，不覆盖运动时的时域稳定性或闪烁。",
            "- Full-64 是参考实现而非真实环境光遮蔽 ground truth；更接近参考不等于物理正确。",
            "- PFM float AO 用于正式误差；PPM 与最终 LDR 只适合展示。",
            "- 32 与 64 之间仍有 kernel 前缀径向分布混杂，报告不把二者差异归因于纯样本数。",
        ]
    )
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(lines))
        stream.write("\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument(
        "--decision", choices=("pending", "go", "no-go"), default="pending"
    )
    parser.add_argument("--decision-reason", default="")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    input_directory = arguments.input.resolve()
    manifest_path = input_directory / "run-manifest.json"
    expect(manifest_path.is_file(), f"run manifest missing: {manifest_path}")
    manifest = load_json(manifest_path)
    expect(int(manifest.get("schemaVersion", 0)) == 1, "unsupported manifest schema")
    protocol = manifest.get("protocol")
    expect(isinstance(protocol, dict), "manifest protocol missing")
    expect(protocol.get("resolution") == [1920, 1080], "protocol is not 1080p")
    expect(protocol.get("renderPath") == "pbr-deferred", "protocol is not Deferred")
    expect(protocol.get("requestedSwapInterval") == 0, "VSync is not disabled")
    expect(protocol.get("inputFrozen") is True, "input is not frozen")
    expect(protocol.get("bloom") is False, "Bloom is enabled")
    expect(protocol.get("shadows") is False, "shadows are enabled")
    expect(protocol.get("autoReloadShaders") is False, "shader reload enabled")
    expect(protocol.get("autoReloadMaterials") is False, "material reload enabled")
    configuration_records = protocol.get("configurations")
    expect(isinstance(configuration_records, list), "configurations missing")
    configuration_map = {
        str(item["name"]): item
        for item in configuration_records
        if isinstance(item, dict)
    }
    expect(
        tuple(configuration_map) == CONFIGURATION_ORDER,
        f"configuration matrix/order mismatch: {tuple(configuration_map)}",
    )
    scene_records = {
        str(item["id"]): item for item in manifest.get("scenes", [])
    }
    expect(bool(scene_records), "scene records missing")
    expected_processes = int(protocol["independentProcesses"])
    run_records = manifest.get("runs")
    expect(isinstance(run_records, list), "run records missing")
    expect(
        len(run_records)
        == len(scene_records) * len(CONFIGURATION_ORDER) * expected_processes,
        "run count does not match the matrix",
    )
    for process_index in range(1, expected_processes + 1):
        if process_index % 3 == 2:
            expected_order = list(reversed(CONFIGURATION_ORDER))
        elif process_index % 3 == 0:
            expected_order = list(CONFIGURATION_ORDER[2:] + CONFIGURATION_ORDER[:2])
        else:
            expected_order = list(CONFIGURATION_ORDER)
        for scene_id in scene_records:
            observed_order = [
                str(record.get("configuration", ""))
                for record in run_records
                if isinstance(record, dict)
                and int(record.get("process", 0)) == process_index
                and str(record.get("scene", "")) == scene_id
            ]
            expect(
                observed_order == expected_order,
                f"{scene_id}/process {process_index}: execution order mismatch",
            )
    if arguments.decision != "pending":
        expect(
            bool(arguments.decision_reason.strip()),
            "Go/No-Go requires a data-backed --decision-reason",
        )
        expect(manifest.get("preset") == "formal", "decision requires Formal")
        expect(set(scene_records) == {"sponza", "san-miguel"}, "two scenes required")
        expect(
            int(protocol["warmupFrames"]) == 300
            and int(protocol["measuredFrames"]) == 2000
            and expected_processes == 3,
            "decision requires 300+2000x3",
        )

    project_directory = Path(__file__).resolve().parent.parent
    seen: set[tuple[str, str, int]] = set()
    validated_runs: list[dict[str, Any]] = []
    for record in run_records:
        expect(isinstance(record, dict), "run record must be an object")
        scene = str(record.get("scene", ""))
        configuration = str(record.get("configuration", ""))
        process = int(record.get("process", 0))
        expect(scene in scene_records, f"unknown scene: {scene}")
        expect(configuration in configuration_map, f"unknown configuration")
        key = (scene, configuration, process)
        expect(key not in seen, f"duplicate run: {key}")
        seen.add(key)
        validated_runs.append(
            validate_run(
                record,
                configuration_map[configuration],
                scene_records[scene],
                manifest,
                project_directory,
            )
        )

    gl_signatures = {
        (run["glVendor"], run["glRenderer"], run["glVersion"])
        for run in validated_runs
    }
    expect(len(gl_signatures) == 1, "OpenGL device/driver changed between runs")
    for scene in scene_records:
        signatures = {
            json.dumps(run["lightSignature"], sort_keys=True)
            for run in validated_runs
            if run["scene"] == scene
        }
        expect(len(signatures) == 1, f"{scene}: fixed state changed between runs")

    aggregates = aggregate_runs(validated_runs, expected_processes)
    groups = group_map(aggregates)
    for scene in scene_records:
        expect(
            set(groups[scene]) == set(CONFIGURATION_ORDER),
            f"{scene}: one or more configurations are missing",
        )
        full_draw = float(
            groups[scene]["legacy-full64"]["metrics"]["drawCalls"]["pooled"][
                "median"
            ]
        )
        raw_draw = float(
            groups[scene]["half-raw64"]["metrics"]["drawCalls"]["pooled"]["median"]
        )
        bilateral_draw = float(
            groups[scene]["half-bilateral64"]["metrics"]["drawCalls"]["pooled"][
                "median"
            ]
        )
        full32_draw = float(
            groups[scene]["legacy-full32"]["metrics"]["drawCalls"]["pooled"][
                "median"
            ]
        )
        bilateral32_draw = float(
            groups[scene]["half-bilateral32"]["metrics"]["drawCalls"]["pooled"][
                "median"
            ]
        )
        expect(full_draw == raw_draw, f"{scene}: pure-resolution Draw Call changed")
        expect(
            bilateral_draw == raw_draw + 1.0,
            f"{scene}: bilateral must add exactly one Draw Call",
        )
        expect(
            bilateral32_draw == full32_draw + 1.0,
            f"{scene}: Half-32 bilateral must add exactly one Draw Call",
        )

    figures_directory = input_directory / "figures"
    figures_directory.mkdir(parents=True, exist_ok=True)
    comparisons = add_performance_comparisons(aggregates)
    quality = calculate_quality(aggregates, figures_directory)
    performance_evidence = {
        item["scene"]: bool(
            item["mainAb"]["halfBilateral64AllProcessesLowerThanFull64"]
        )
        for item in comparisons
    }
    quality_evidence = {
        item["scene"]: bool(item["bilateral64EdgeErrorLowerThanRaw64"])
        for item in quality
    }
    go_evidence = {
        "perSceneEveryProcessFaster": performance_evidence,
        "perSceneEdgeErrorLower": quality_evidence,
        "allScenesEveryProcessFaster": all(performance_evidence.values()),
        "allScenesEdgeErrorLower": all(quality_evidence.values()),
    }
    if arguments.decision == "go":
        expect(
            go_evidence["allScenesEveryProcessFaster"],
            "Go rejected: bilateral total is not lower in every process/two scenes",
        )
        expect(
            go_evidence["allScenesEdgeErrorLower"],
            "Go rejected: bilateral does not lower edge MAE and P95 in both scenes",
        )

    display_names = get_display_names(manifest)
    performance_chart(
        figures_directory / "ssao-half-resolution-timing.png",
        aggregates,
        display_names,
    )
    quality_chart(
        figures_directory / "ssao-edge-quality.png", quality, display_names
    )
    write_summary_csv(input_directory / "summary.csv", aggregates)
    write_process_csv(input_directory / "process-summary.csv", aggregates)
    write_quality_csv(input_directory / "quality-summary.csv", quality)
    markdown_report(
        input_directory / "SSAO_HALF_RESOLUTION_REPORT_CN.md",
        manifest,
        aggregates,
        comparisons,
        quality,
        arguments.decision,
        arguments.decision_reason.strip(),
        go_evidence,
    )

    serializable_aggregates = json.loads(json.dumps(aggregates))
    summary = {
        "schemaVersion": 1,
        "generatedAtUtc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "validation": {
            "status": "pass",
            "validatedRuns": len(validated_runs),
            "expectedFramesPerRun": int(protocol["measuredFrames"]),
            "requiredZoneCountsExact": True,
            "nestedTimestampBoundsVerified": True,
            "fixedStateVerified": True,
            "floatCapturesVerified": True,
        },
        "decision": {
            "value": arguments.decision,
            "reason": arguments.decision_reason.strip(),
            "goEvidence": go_evidence,
        },
        "batch": {
            "id": manifest.get("batchId"),
            "preset": manifest.get("preset"),
            "protocol": protocol,
            "source": manifest.get("source"),
            "system": manifest.get("system"),
            "scenes": manifest.get("scenes"),
            "openGl": {
                "vendor": validated_runs[0]["glVendor"],
                "renderer": validated_runs[0]["glRenderer"],
                "version": validated_runs[0]["glVersion"],
            },
        },
        "aggregation": {
            "framePercentiles": "nearest-rank over pooled raw frame samples",
            "processDispersion": (
                "range and coefficient of variation over process medians"
            ),
            "ssim": "11x11 uniform-window local SSIM, data range 1",
            "halfRawReconstruction": (
                "exact GL_LINEAR normalized pixel-center mapping, clamp-to-edge"
            ),
        },
        "performance": {
            "results": serializable_aggregates,
            "comparisons": comparisons,
        },
        "quality": quality,
        "methodologyCaveat": protocol.get("kernelMethodCaveat"),
        "artifacts": {
            "performanceCsv": "summary.csv",
            "processCsv": "process-summary.csv",
            "qualityCsv": "quality-summary.csv",
            "report": "SSAO_HALF_RESOLUTION_REPORT_CN.md",
            "performanceChart": "figures/ssao-half-resolution-timing.png",
            "qualityChart": "figures/ssao-edge-quality.png",
        },
    }
    write_json(input_directory / "summary.json", summary)
    print(f"Validated {len(validated_runs)} runs.")
    print(f"Wrote {input_directory / 'summary.json'}")
    print(f"Wrote {input_directory / 'SSAO_HALF_RESOLUTION_REPORT_CN.md'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as error:
        raise SystemExit(f"SSAO half-resolution validation failed: {error}")
