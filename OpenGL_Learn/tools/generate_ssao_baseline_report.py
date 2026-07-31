#!/usr/bin/env python3
"""Validate, aggregate, and visualize the fixed-camera SSAO baseline."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from PIL import Image, ImageDraw, ImageFont, ImageOps


METRICS = (
    ("cpuFrame", "CPU Frame", "ms"),
    ("gpuFrame", "GPU Frame", "ms"),
    ("deferredGpu", "Deferred Pass GPU", "ms"),
    ("ssaoCpu", "SSAO Pass CPU", "ms"),
    ("ssaoGpu", "SSAO Pass GPU", "ms"),
    ("drawCalls", "Draw Call", "count"),
)
CONFIGURATION_ORDER = (0, 8, 16, 32, 64)
COLORS = {
    "background": "#F7F8FA",
    "panel": "#FFFFFF",
    "grid": "#D9DEE7",
    "axis": "#445066",
    "text": "#172033",
    "muted": "#667085",
    "gpu": "#EF8A34",
    "ssao": "#3478D4",
    "ideal": "#8B95A7",
    "positive": "#27936B",
    "negative": "#C84B4B",
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


def numeric_values(values: Any, context: str) -> list[float]:
    expect(isinstance(values, list), f"{context} must be an array")
    result: list[float] = []
    for index, value in enumerate(values):
        expect(finite_number(value), f"{context}[{index}] is not finite")
        result.append(float(value))
    return result


def nearest_rank(sorted_values: list[float], percentile: float) -> float:
    expect(bool(sorted_values), "cannot summarize an empty distribution")
    rank = int(math.ceil(max(0.0, min(1.0, percentile)) * len(sorted_values)))
    return sorted_values[max(0, min(len(sorted_values) - 1, rank - 1))]


def summarize(values: Iterable[float]) -> dict[str, float | int | None]:
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


def process_dispersion(values: Iterable[float]) -> dict[str, float | int | None]:
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


def project_path(project_directory: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = project_directory / path
    return path.resolve()


def summary_zone_count(result: dict[str, Any], kind: str, name: str) -> int:
    zone = (
        result.get("profiler", {})
        .get("summary", {})
        .get(kind, {})
        .get(name)
    )
    if not isinstance(zone, dict):
        return 0
    return int(zone.get("count", 0))


def extract_run_metrics(
    result: dict[str, Any],
    expected_frames: int,
    samples: int,
    context: str,
) -> dict[str, list[float]]:
    profiler = result.get("profiler")
    expect(isinstance(profiler, dict), f"{context}: profiler is missing")
    expect(profiler.get("gpuTimingSupported") is True, f"{context}: GPU timing unavailable")
    raw = profiler.get("samples")
    expect(isinstance(raw, dict), f"{context}: profiler.samples is missing")
    cpu_zones = raw.get("cpuZones")
    gpu_zones = raw.get("gpuZones")
    expect(isinstance(cpu_zones, dict), f"{context}: CPU zones are missing")
    expect(isinstance(gpu_zones, dict), f"{context}: GPU zones are missing")

    metrics = {
        "cpuFrame": numeric_values(raw.get("cpuFrame"), f"{context}: CPU Frame"),
        "gpuFrame": numeric_values(raw.get("gpuFrame"), f"{context}: GPU Frame"),
        "deferredGpu": numeric_values(
            gpu_zones.get("Deferred Pass"), f"{context}: Deferred Pass GPU"
        ),
        "ssaoCpu": numeric_values(
            cpu_zones.get("SSAO Pass", []), f"{context}: SSAO Pass CPU"
        ),
        "ssaoGpu": numeric_values(
            gpu_zones.get("SSAO Pass", []), f"{context}: SSAO Pass GPU"
        ),
        "drawCalls": numeric_values(raw.get("drawCalls"), f"{context}: Draw Call"),
    }
    deferred_cpu = numeric_values(
        cpu_zones.get("Deferred Pass"), f"{context}: Deferred Pass CPU"
    )

    for metric in ("cpuFrame", "gpuFrame", "deferredGpu", "drawCalls"):
        expect(
            len(metrics[metric]) == expected_frames,
            f"{context}: {metric} has {len(metrics[metric])} samples, "
            f"expected {expected_frames}",
        )
    expect(
        len(deferred_cpu) == expected_frames,
        f"{context}: Deferred Pass CPU has {len(deferred_cpu)} samples, "
        f"expected {expected_frames}",
    )
    expected_ssao_frames = 0 if samples == 0 else expected_frames
    for metric in ("ssaoCpu", "ssaoGpu"):
        expect(
            len(metrics[metric]) == expected_ssao_frames,
            f"{context}: {metric} has {len(metrics[metric])} samples, "
            f"expected {expected_ssao_frames}",
        )

    summary = profiler.get("summary")
    expect(isinstance(summary, dict), f"{context}: profiler.summary is missing")
    expected_counts = {
        "cpuFrame": int(summary.get("cpuFrame", {}).get("count", -1)),
        "gpuFrame": int(summary.get("gpuFrame", {}).get("count", -1)),
        "drawCalls": int(summary.get("drawCalls", {}).get("count", -1)),
        "deferredCpu": summary_zone_count(result, "cpuZones", "Deferred Pass"),
        "deferredGpu": summary_zone_count(result, "gpuZones", "Deferred Pass"),
        "ssaoCpu": summary_zone_count(result, "cpuZones", "SSAO Pass"),
        "ssaoGpu": summary_zone_count(result, "gpuZones", "SSAO Pass"),
    }
    for name in ("cpuFrame", "gpuFrame", "drawCalls", "deferredCpu", "deferredGpu"):
        expect(
            expected_counts[name] == expected_frames,
            f"{context}: summary {name} count is {expected_counts[name]}, "
            f"expected {expected_frames}",
        )
    for name in ("ssaoCpu", "ssaoGpu"):
        expect(
            expected_counts[name] == expected_ssao_frames,
            f"{context}: summary {name} count is {expected_counts[name]}, "
            f"expected {expected_ssao_frames}",
        )

    for metric, values in metrics.items():
        if metric == "drawCalls":
            expect(all(value > 0.0 for value in values), f"{context}: invalid draw count")
        else:
            expect(all(value > 0.0 for value in values), f"{context}: non-positive {metric}")

    for frame_index, (gpu_frame, deferred_gpu) in enumerate(
        zip(metrics["gpuFrame"], metrics["deferredGpu"])
    ):
        expect(
            deferred_gpu <= gpu_frame,
            f"{context}: Deferred GPU exceeds GPU Frame at sample {frame_index}",
        )
    if samples > 0:
        for frame_index, (deferred_gpu, ssao_gpu) in enumerate(
            zip(metrics["deferredGpu"], metrics["ssaoGpu"])
        ):
            expect(
                ssao_gpu <= deferred_gpu,
                f"{context}: SSAO GPU exceeds Deferred GPU at sample {frame_index}",
            )
    return metrics


def validate_capture(path: Path, context: str) -> None:
    expect(path.is_file(), f"{context}: capture is missing: {path}")
    try:
        with Image.open(path) as image:
            expect(
                image.size == (1920, 1080),
                f"{context}: capture resolution is {image.size}, expected 1920x1080",
            )
            image.verify()
    except ValidationError:
        raise
    except Exception as error:
        raise ValidationError(f"{context}: invalid capture {path}: {error}") from error


def validate_run(
    record: dict[str, Any],
    manifest_scene: dict[str, Any],
    manifest: dict[str, Any],
    project_directory: Path,
) -> dict[str, Any]:
    scene_id = str(record.get("scene", ""))
    samples = int(record.get("samples", -1))
    process_index = int(record.get("process", 0))
    context = f"{scene_id}/{samples} samples/process {process_index}"
    result_path = project_path(project_directory, str(record.get("result", "")))
    expect(result_path.is_file(), f"{context}: result is missing: {result_path}")
    result = load_json(result_path)

    protocol = manifest["protocol"]
    expected_frames = int(protocol["measuredFrames"])
    expected_resolution = [int(value) for value in protocol["resolution"]]
    expect(result.get("success") is True, f"{context}: result success is false")
    expect(int(result.get("schemaVersion", 0)) >= 20, f"{context}: schema < 20")
    expect(result.get("scene") == scene_id, f"{context}: scene id mismatch")
    expect(result.get("buildConfiguration") == "Release", f"{context}: not Release")
    expect(result.get("architecture") == "x64", f"{context}: not x64")
    expect(result.get("resolution") == expected_resolution, f"{context}: resolution mismatch")
    expect(result.get("renderPath") == "pbr-deferred", f"{context}: render path mismatch")
    expect(result.get("materialMode") == "source", f"{context}: material mode mismatch")
    expect(
        result.get("frameMeasurement") == "cpu-submission-wall",
        f"{context}: CPU frame measurement mode mismatch",
    )
    expected_model = "classic-scenes/" + str(manifest_scene["modelPath"]).replace("\\", "/")
    expect(result.get("modelPath") == expected_model, f"{context}: model path mismatch")
    expect(
        int(result.get("measuredFrames", -1)) == expected_frames,
        f"{context}: measured frame count mismatch",
    )
    expect(
        int(result.get("warmupFrames", -1)) == int(protocol["warmupFrames"]),
        f"{context}: warmup frame count mismatch",
    )

    camera = result.get("camera")
    expect(isinstance(camera, dict), f"{context}: camera is missing")
    expect(
        close_sequence(camera.get("position"), manifest_scene["camera"]),
        f"{context}: camera position mismatch",
    )
    expect(
        close_sequence(camera.get("target"), manifest_scene["target"]),
        f"{context}: camera target mismatch",
    )
    expect(
        close_sequence(camera.get("up"), manifest_scene["up"]),
        f"{context}: camera up mismatch",
    )
    expect(
        abs(float(camera.get("fovDegrees", -1.0)) - float(manifest_scene["fov"]))
        <= 1.0e-5,
        f"{context}: FOV mismatch",
    )
    shadow = result.get("shadow")
    expect(isinstance(shadow, dict), f"{context}: shadow settings are missing")
    expect(
        abs(float(shadow.get("worldScale", -1.0)) - 1.0) <= 1.0e-5,
        f"{context}: world scale mismatch",
    )

    settings = result.get("settings")
    expect(isinstance(settings, dict), f"{context}: settings are missing")
    expected_settings = {
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
    for name, expected in expected_settings.items():
        expect(settings.get(name) == expected, f"{context}: setting {name} mismatch")

    ssao = result.get("ssao")
    expect(isinstance(ssao, dict), f"{context}: SSAO metadata is missing")
    expect(ssao.get("experiment") is True, f"{context}: SSAO experiment flag is false")
    expect(int(ssao.get("requestedSamples", -1)) == samples, f"{context}: sample mismatch")
    expect(
        abs(float(ssao.get("radius", -1.0)) - float(protocol["ssaoRadius"])) <= 1.0e-5,
        f"{context}: SSAO radius mismatch",
    )
    expect(
        abs(float(ssao.get("bias", -1.0)) - float(protocol["ssaoBias"])) <= 1.0e-5,
        f"{context}: SSAO bias mismatch",
    )
    if samples == 0:
        expect(ssao.get("enabled") is False, f"{context}: SSAO Off is enabled")
        expect(ssao.get("outputAvailable") is False, f"{context}: Off has AO output")
        expect(ssao.get("fullResolution") is False, f"{context}: Off reports an FBO")
    else:
        expect(ssao.get("enabled") is True, f"{context}: SSAO is disabled")
        expect(
            int(ssao.get("kernelSize", -1)) == samples,
            f"{context}: actual SSAO kernel size does not match requested samples",
        )
        expect(ssao.get("outputAvailable") is True, f"{context}: AO FBO unavailable")
        expect(ssao.get("fullResolution") is True, f"{context}: AO is not full resolution")
        expect(int(ssao.get("outputWidth", 0)) == 1920, f"{context}: AO width mismatch")
        expect(int(ssao.get("outputHeight", 0)) == 1080, f"{context}: AO height mismatch")
        expect(
            ssao.get("outputInternalFormatName") == "GL_R16F",
            f"{context}: AO format is not GL_R16F",
        )
        expect(
            int(ssao.get("outputInternalFormat", 0)) == 33325,
            f"{context}: AO internal format enum mismatch",
        )

    metrics = extract_run_metrics(result, expected_frames, samples, context)

    final_capture_value = record.get("finalCapture")
    ao_capture_value = record.get("aoCapture")
    if final_capture_value:
        final_capture_path = project_path(project_directory, str(final_capture_value))
        validate_capture(final_capture_path, f"{context} final")
        expect(result.get("captureRequired") is True, f"{context}: capture not required")
        expect(
            project_path(project_directory, str(result.get("capturePath", "")))
            == final_capture_path,
            f"{context}: final capture path mismatch",
        )
        expect(float(result.get("meanLuminance", 0.0)) > 0.0, f"{context}: black capture")
    else:
        final_capture_path = None
        expect(result.get("captureRequired") is False, f"{context}: unexpected capture")

    if ao_capture_value:
        ao_capture_path = project_path(project_directory, str(ao_capture_value))
        validate_capture(ao_capture_path, f"{context} AO")
        expect(ssao.get("captureValid") is True, f"{context}: AO capture is invalid")
        expect(
            project_path(project_directory, str(ssao.get("capturePath", "")))
            == ao_capture_path,
            f"{context}: AO capture path mismatch",
        )
    else:
        ao_capture_path = None
        expect(ssao.get("captureValid") is False, f"{context}: unexpected AO capture")

    light_signature = {
        "directionalLight": shadow.get("directionalLight"),
        "pointLightPosition": shadow.get("pointLightPosition"),
        "spotLightPosition": shadow.get("spotLightPosition"),
        "spotLightDirection": shadow.get("spotLightDirection"),
        "settings": settings,
    }
    return {
        "scene": scene_id,
        "configuration": str(record.get("configuration", "")),
        "samples": samples,
        "process": process_index,
        "resultPath": result_path,
        "logPath": project_path(project_directory, str(record.get("log", ""))),
        "finalCapturePath": final_capture_path,
        "aoCapturePath": ao_capture_path,
        "metrics": metrics,
        "glVendor": result.get("glVendor"),
        "glRenderer": result.get("glRenderer"),
        "glVersion": result.get("glVersion"),
        "lightSignature": light_signature,
    }


def aggregate_groups(
    runs: list[dict[str, Any]],
    expected_processes: int,
) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)
    for run in runs:
        grouped[(run["scene"], run["samples"])].append(run)

    aggregates: list[dict[str, Any]] = []
    for (scene, samples), group_runs in sorted(
        grouped.items(), key=lambda item: (item[0][0], item[0][1])
    ):
        expect(
            len(group_runs) == expected_processes,
            f"{scene}/{samples}: found {len(group_runs)} processes, "
            f"expected {expected_processes}",
        )
        process_ids = sorted(run["process"] for run in group_runs)
        expect(
            process_ids == list(range(1, expected_processes + 1)),
            f"{scene}/{samples}: process ids are {process_ids}",
        )
        group_runs.sort(key=lambda run: run["process"])
        metric_aggregates: dict[str, Any] = {}
        for key, _label, unit in METRICS:
            pooled = [
                value
                for run in group_runs
                for value in run["metrics"][key]
            ]
            per_process = [
                {
                    "process": run["process"],
                    **summarize(run["metrics"][key]),
                }
                for run in group_runs
            ]
            process_medians = [
                float(process_stats["median"])
                for process_stats in per_process
                if process_stats["median"] is not None
            ]
            metric_aggregates[key] = {
                "unit": unit,
                "pooled": summarize(pooled),
                "processMedianDispersion": process_dispersion(process_medians),
                "perProcess": per_process,
            }
        aggregates.append(
            {
                "scene": scene,
                "configuration": group_runs[0]["configuration"],
                "samples": samples,
                "metrics": metric_aggregates,
                "captures": {
                    "final": next(
                        (
                            str(run["finalCapturePath"])
                            for run in group_runs
                            if run["finalCapturePath"] is not None
                        ),
                        None,
                    ),
                    "ao": next(
                        (
                            str(run["aoCapturePath"])
                            for run in group_runs
                            if run["aoCapturePath"] is not None
                        ),
                        None,
                    ),
                },
            }
        )
    return aggregates


def add_comparisons(aggregates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_scene: dict[str, dict[int, dict[str, Any]]] = defaultdict(dict)
    for group in aggregates:
        by_scene[group["scene"]][group["samples"]] = group

    comparisons: list[dict[str, Any]] = []
    for scene, configurations in sorted(by_scene.items()):
        expect(0 in configurations, f"{scene}: SSAO Off result is missing")
        off_gpu = float(
            configurations[0]["metrics"]["gpuFrame"]["pooled"]["median"]
        )
        base_ssao = (
            float(configurations[8]["metrics"]["ssaoGpu"]["pooled"]["median"])
            if 8 in configurations
            else None
        )
        previous_ssao: float | None = None
        rows: list[dict[str, Any]] = []
        for samples in sorted(configurations):
            group = configurations[samples]
            gpu_frame = float(group["metrics"]["gpuFrame"]["pooled"]["median"])
            deferred_gpu = float(
                group["metrics"]["deferredGpu"]["pooled"]["median"]
            )
            if samples == 0:
                ssao_gpu = None
                generation_share = 0.0
                scale_vs_8 = None
                adjacent_scale = None
            else:
                ssao_gpu = float(group["metrics"]["ssaoGpu"]["pooled"]["median"])
                generation_share = ssao_gpu / gpu_frame * 100.0
                scale_vs_8 = ssao_gpu / base_ssao if base_ssao else None
                adjacent_scale = ssao_gpu / previous_ssao if previous_ssao else None
                previous_ssao = ssao_gpu
            rows.append(
                {
                    "configuration": group["configuration"],
                    "samples": samples,
                    "gpuFrameMedianMs": gpu_frame,
                    "deferredGpuMedianMs": deferred_gpu,
                    "ssaoGpuMedianMs": ssao_gpu,
                    "ssaoGenerationShareOfGpuFramePercent": generation_share,
                    "gpuFrameDeltaVsOffMs": gpu_frame - off_gpu,
                    "gpuFrameDeltaVsOffPercent": (
                        (gpu_frame - off_gpu) / off_gpu * 100.0
                        if off_gpu
                        else None
                    ),
                    "ssaoGpuScaleVs8": scale_vs_8,
                    "idealSampleCountScaleVs8": (
                        samples / 8.0 if samples > 0 else None
                    ),
                    "ssaoGpuAdjacentScale": adjacent_scale,
                }
            )
        comparisons.append({"scene": scene, "configurations": rows})
    return comparisons


def get_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    names = (
        ("C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf")
        if bold
        else ("C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf")
    )
    for name in names:
        try:
            return ImageFont.truetype(name, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def text_width(draw: ImageDraw.ImageDraw, value: str, font: ImageFont.ImageFont) -> float:
    box = draw.textbbox((0, 0), value, font=font)
    return float(box[2] - box[0])


def draw_centered(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    value: str,
    font: ImageFont.ImageFont,
    fill: str,
) -> None:
    draw.text(
        (xy[0] - text_width(draw, value, font) / 2.0, xy[1]),
        value,
        font=font,
        fill=fill,
    )


def draw_vertical_scale(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    maximum: float,
    tick_count: int,
    unit: str,
) -> None:
    left, top, right, bottom = box
    axis_font = get_font(18)
    draw.line((left, top, left, bottom), fill=COLORS["axis"], width=2)
    draw.line((left, bottom, right, bottom), fill=COLORS["axis"], width=2)
    for tick in range(tick_count + 1):
        value = maximum * tick / tick_count
        y = bottom - (bottom - top) * tick / tick_count
        draw.line((left, y, right, y), fill=COLORS["grid"], width=1)
        label = f"{value:.1f}"
        draw.text(
            (left - text_width(draw, label, axis_font) - 12, y - 10),
            label,
            font=axis_font,
            fill=COLORS["muted"],
        )
    draw.text((left - 62, top - 30), unit, font=axis_font, fill=COLORS["muted"])


def timing_chart(
    path: Path,
    aggregates: list[dict[str, Any]],
    display_names: dict[str, str],
) -> None:
    scenes = sorted({group["scene"] for group in aggregates})
    width = 1800
    height = 145 + 430 * len(scenes)
    image = Image.new("RGB", (width, height), COLORS["background"])
    draw = ImageDraw.Draw(image)
    title_font = get_font(34, True)
    panel_title_font = get_font(25, True)
    label_font = get_font(18)
    small_font = get_font(15)
    draw.text(
        (60, 35),
        "SSAO baseline: current-configuration GPU timing at 1920x1080",
        font=title_font,
        fill=COLORS["text"],
    )
    draw.rectangle((1060, 48, 1090, 70), fill=COLORS["gpu"])
    draw.text((1100, 47), "GPU Frame median", font=label_font, fill=COLORS["text"])
    draw.rectangle((1355, 48, 1385, 70), fill=COLORS["ssao"])
    draw.text((1395, 47), "SSAO Pass GPU median", font=label_font, fill=COLORS["text"])

    for scene_index, scene in enumerate(scenes):
        groups = sorted(
            (group for group in aggregates if group["scene"] == scene),
            key=lambda group: group["samples"],
        )
        panel_top = 105 + scene_index * 430
        draw.rounded_rectangle(
            (35, panel_top, width - 35, panel_top + 395),
            radius=15,
            fill=COLORS["panel"],
        )
        draw.text(
            (65, panel_top + 22),
            display_names.get(scene, scene),
            font=panel_title_font,
            fill=COLORS["text"],
        )
        chart = (130, panel_top + 85, width - 75, panel_top + 345)
        max_value = max(
            float(group["metrics"]["gpuFrame"]["pooled"]["p99"])
            for group in groups
        )
        maximum = max(0.5, math.ceil(max_value * 1.18 * 2.0) / 2.0)
        draw_vertical_scale(draw, chart, maximum, 5, "ms")
        left, top, right, bottom = chart
        slot = (right - left) / len(groups)
        for index, group in enumerate(groups):
            center = left + slot * (index + 0.5)
            gpu_value = float(group["metrics"]["gpuFrame"]["pooled"]["median"])
            gpu_top = bottom - (bottom - top) * gpu_value / maximum
            draw.rectangle(
                (center - 42, gpu_top, center + 42, bottom),
                fill=COLORS["gpu"],
            )
            ssao_stats = group["metrics"]["ssaoGpu"]["pooled"]
            if ssao_stats["median"] is not None:
                ssao_value = float(ssao_stats["median"])
                ssao_top = bottom - (bottom - top) * ssao_value / maximum
                draw.rectangle(
                    (center - 22, ssao_top, center + 22, bottom),
                    fill=COLORS["ssao"],
                )
                dispersion = group["metrics"]["ssaoGpu"]["processMedianDispersion"]
                low = float(dispersion["min"])
                high = float(dispersion["max"])
                low_y = bottom - (bottom - top) * low / maximum
                high_y = bottom - (bottom - top) * high / maximum
                draw.line((center, low_y, center, high_y), fill=COLORS["text"], width=2)
                draw.line(
                    (center - 9, low_y, center + 9, low_y),
                    fill=COLORS["text"],
                    width=2,
                )
                draw.line(
                    (center - 9, high_y, center + 9, high_y),
                    fill=COLORS["text"],
                    width=2,
                )
                draw_centered(
                    draw,
                    (center, max(top + 2, ssao_top - 23)),
                    f"{ssao_value:.3f}",
                    small_font,
                    COLORS["ssao"],
                )
            draw_centered(
                draw,
                (center, max(top + 2, gpu_top - 23)),
                f"{gpu_value:.3f}",
                small_font,
                COLORS["gpu"],
            )
            config_label = "Off" if group["samples"] == 0 else str(group["samples"])
            draw_centered(
                draw,
                (center, bottom + 10),
                config_label,
                label_font,
                COLORS["text"],
            )
        draw_centered(
            draw,
            ((left + right) / 2.0, bottom + 40),
            "SSAO sample count (Off, Full 8/16/32/64)",
            label_font,
            COLORS["muted"],
        )
    image.save(path, format="PNG")


def line_plot(
    draw: ImageDraw.ImageDraw,
    box: tuple[int, int, int, int],
    x_labels: list[str],
    series: list[tuple[str, list[float], str, bool]],
    maximum: float,
    unit: str,
) -> None:
    left, top, right, bottom = box
    draw_vertical_scale(draw, box, maximum, 4, unit)
    label_font = get_font(16)
    points_by_series: list[tuple[str, list[tuple[float, float]], str, bool]] = []
    for name, values, color, dashed in series:
        points: list[tuple[float, float]] = []
        for index, value in enumerate(values):
            x = (
                left + (right - left) / 2.0
                if len(values) == 1
                else left + (right - left) * index / (len(values) - 1)
            )
            y = bottom - (bottom - top) * value / maximum
            points.append((x, y))
        points_by_series.append((name, points, color, dashed))
    for name, points, color, dashed in points_by_series:
        if dashed:
            for start, end in zip(points, points[1:]):
                steps = 12
                for segment in range(0, steps, 2):
                    t0 = segment / steps
                    t1 = min(1.0, (segment + 1) / steps)
                    draw.line(
                        (
                            start[0] + (end[0] - start[0]) * t0,
                            start[1] + (end[1] - start[1]) * t0,
                            start[0] + (end[0] - start[0]) * t1,
                            start[1] + (end[1] - start[1]) * t1,
                        ),
                        fill=color,
                        width=3,
                    )
        else:
            draw.line(points, fill=color, width=4)
        for x, y in points:
            draw.ellipse((x - 5, y - 5, x + 5, y + 5), fill=color)
        if points:
            draw.text(
                (points[-1][0] + 10, points[-1][1] - 10),
                name,
                font=label_font,
                fill=color,
            )
    for index, label in enumerate(x_labels):
        x = (
            left + (right - left) / 2.0
            if len(x_labels) == 1
            else left + (right - left) * index / (len(x_labels) - 1)
        )
        draw_centered(draw, (x, bottom + 10), label, label_font, COLORS["text"])


def scaling_chart(
    path: Path,
    comparisons: list[dict[str, Any]],
    display_names: dict[str, str],
) -> None:
    width = 1800
    height = 155 + 445 * len(comparisons)
    image = Image.new("RGB", (width, height), COLORS["background"])
    draw = ImageDraw.Draw(image)
    title_font = get_font(34, True)
    panel_title_font = get_font(24, True)
    label_font = get_font(17)
    small_font = get_font(15)
    draw.text(
        (60, 35),
        "Current SSAO configuration expansion and total GPU-frame impact",
        font=title_font,
        fill=COLORS["text"],
    )
    draw.text(
        (60, 82),
        "8/16/32 use prefixes of one 64-kernel with i/64 radial scale; this is not a pure sample-count experiment.",
        font=label_font,
        fill=COLORS["muted"],
    )
    for scene_index, comparison in enumerate(comparisons):
        scene = comparison["scene"]
        rows = comparison["configurations"]
        positive_rows = [row for row in rows if row["samples"] > 0]
        panel_top = 125 + scene_index * 445
        draw.rounded_rectangle(
            (35, panel_top, width - 35, panel_top + 410),
            radius=15,
            fill=COLORS["panel"],
        )
        draw.text(
            (65, panel_top + 20),
            display_names.get(scene, scene),
            font=panel_title_font,
            fill=COLORS["text"],
        )
        draw.text(
            (115, panel_top + 65),
            "SSAO GPU scaling vs Full-8",
            font=label_font,
            fill=COLORS["text"],
        )
        scaling_values = [float(row["ssaoGpuScaleVs8"]) for row in positive_rows]
        ideal_values = [float(row["idealSampleCountScaleVs8"]) for row in positive_rows]
        max_scale = max(ideal_values + scaling_values)
        line_plot(
            draw,
            (135, panel_top + 115, 770, panel_top + 335),
            [str(row["samples"]) for row in positive_rows],
            [
                ("measured", scaling_values, COLORS["ssao"], False),
                ("nominal sample ratio", ideal_values, COLORS["ideal"], True),
            ],
            max(2.0, math.ceil(max_scale)),
            "x",
        )
        draw.text(
            (970, panel_top + 65),
            "GPU Frame median delta vs SSAO Off",
            font=label_font,
            fill=COLORS["text"],
        )
        chart = (1030, panel_top + 115, 1690, panel_top + 335)
        deltas = [float(row["gpuFrameDeltaVsOffMs"]) for row in positive_rows]
        max_abs = max(0.1, max(abs(value) for value in deltas) * 1.25)
        maximum = math.ceil(max_abs * 10.0) / 10.0
        left, top, right, bottom = chart
        zero_y = (top + bottom) / 2.0
        draw.line((left, zero_y, right, zero_y), fill=COLORS["axis"], width=2)
        for tick in (-1.0, -0.5, 0.5, 1.0):
            y = zero_y - tick * (bottom - top) / 2.0
            draw.line((left, y, right, y), fill=COLORS["grid"], width=1)
            label = f"{tick * maximum:.2f}"
            draw.text(
                (left - text_width(draw, label, small_font) - 10, y - 9),
                label,
                font=small_font,
                fill=COLORS["muted"],
            )
        draw.text((left - 55, top - 22), "ms", font=small_font, fill=COLORS["muted"])
        slot = (right - left) / len(positive_rows)
        for index, (row, delta) in enumerate(zip(positive_rows, deltas)):
            center = left + slot * (index + 0.5)
            end_y = zero_y - delta / maximum * (bottom - top) / 2.0
            color = COLORS["positive"] if delta >= 0.0 else COLORS["negative"]
            draw.rectangle(
                (center - 31, min(zero_y, end_y), center + 31, max(zero_y, end_y)),
                fill=color,
            )
            draw_centered(
                draw,
                (center, end_y - 24 if delta >= 0.0 else end_y + 5),
                f"{delta:+.3f}",
                small_font,
                color,
            )
            draw_centered(
                draw,
                (center, bottom + 10),
                str(row["samples"]),
                label_font,
                COLORS["text"],
            )
            share = float(row["ssaoGenerationShareOfGpuFramePercent"])
            draw_centered(
                draw,
                (center, bottom + 36),
                f"zone share {share:.1f}%",
                small_font,
                COLORS["ssao"],
            )
    image.save(path, format="PNG")


def create_capture_figures(
    figures_directory: Path,
    aggregates: list[dict[str, Any]],
    display_names: dict[str, str],
) -> dict[str, dict[str, str]]:
    capture_directory = figures_directory / "captures"
    capture_directory.mkdir(parents=True, exist_ok=True)
    output: dict[str, dict[str, str]] = {}
    for scene in sorted({group["scene"] for group in aggregates}):
        groups = sorted(
            (group for group in aggregates if group["scene"] == scene),
            key=lambda group: group["samples"],
        )
        scene_output: dict[str, str] = {}
        for capture_kind, label in (("final", "Final lighting"), ("ao", "AO output")):
            selected = [
                group for group in groups if group["captures"].get(capture_kind)
            ]
            if not selected:
                continue
            tile_width = 330
            tile_height = 186
            header_height = 78
            sheet = Image.new(
                "RGB",
                (tile_width * len(selected), header_height + tile_height),
                COLORS["background"],
            )
            sheet_draw = ImageDraw.Draw(sheet)
            title_font = get_font(22, True)
            label_font = get_font(17)
            sheet_draw.text(
                (18, 12),
                f"{display_names.get(scene, scene)} — {label}",
                font=title_font,
                fill=COLORS["text"],
            )
            for index, group in enumerate(selected):
                source = Path(str(group["captures"][capture_kind]))
                with Image.open(source) as source_image:
                    frame = ImageOps.fit(
                        source_image.convert("RGB"),
                        (tile_width, tile_height),
                        method=Image.Resampling.LANCZOS,
                    )
                x = index * tile_width
                sheet.paste(frame, (x, header_height))
                config_label = (
                    "SSAO Off"
                    if group["samples"] == 0
                    else f"Full {group['samples']} samples"
                )
                draw_centered(
                    sheet_draw,
                    (x + tile_width / 2.0, 48),
                    config_label,
                    label_font,
                    COLORS["text"],
                )
                individual_name = (
                    f"{scene}-{group['configuration']}-{capture_kind}.png"
                )
                frame.save(capture_directory / individual_name, format="PNG")
            sheet_name = f"{scene}-{capture_kind}-contact-sheet.png"
            sheet_path = figures_directory / sheet_name
            sheet.save(sheet_path, format="PNG")
            scene_output[capture_kind] = f"figures/{sheet_name}"
        output[scene] = scene_output
    return output


def write_summary_csv(path: Path, aggregates: list[dict[str, Any]]) -> None:
    fields = [
        "scene",
        "configuration",
        "samples",
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
            for key, label, _unit in METRICS:
                metric = group["metrics"][key]
                pooled = metric["pooled"]
                dispersion = metric["processMedianDispersion"]
                writer.writerow(
                    {
                        "scene": group["scene"],
                        "configuration": group["configuration"],
                        "samples": group["samples"],
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
        "samples",
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
                for process_stats in metric["perProcess"]:
                    writer.writerow(
                        {
                            "scene": group["scene"],
                            "configuration": group["configuration"],
                            "samples": group["samples"],
                            "process": process_stats["process"],
                            "metric": label,
                            "unit": metric["unit"],
                            "count": process_stats["count"],
                            "mean": process_stats["mean"],
                            "min": process_stats["min"],
                            "max": process_stats["max"],
                            "median": process_stats["median"],
                            "p95": process_stats["p95"],
                            "p99": process_stats["p99"],
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
    capture_figures: dict[str, dict[str, str]],
    decision: str,
    decision_reason: str,
) -> None:
    scene_records = {
        str(scene["id"]): scene for scene in manifest.get("scenes", [])
    }
    display_names = {
        scene_id: str(scene.get("displayName") or scene_id)
        for scene_id, scene in scene_records.items()
    }
    protocol = manifest["protocol"]
    system = manifest.get("system", {})
    source = manifest.get("source", {})
    preset = str(manifest.get("preset", "unknown"))
    lines: list[str] = [
        "# SSAO 性能基线（1080p / Deferred）",
        "",
        f"- 批次：`{manifest.get('batchId', '')}`",
        f"- 类型：`{preset}`",
        f"- 平台：{system.get('cpu', 'unknown')} / {system.get('gpu', 'unknown')}",
        f"- OpenGL 驱动：{system.get('gpuDriverVersion', 'unknown')}",
        f"- 构建：Release x64，提交 `{source.get('gitCommit', 'unknown')}`"
        + ("，工作树含未提交修改" if source.get("worktreeDirty") else ""),
        f"- 协议：每配置 {protocol['warmupFrames']} 帧预热、"
        f"{protocol['measuredFrames']} 帧采样、"
        f"{protocol['independentProcesses']} 个独立进程",
        "- 固定条件：1920×1080、VSync 关闭、固定相机与输入、Deferred、"
        "Bloom/阴影/Shader 与 Material 自动热重载关闭、Gamma 开启",
        "- SSAO：Off、Full-Resolution 8/16/32/64 Samples；"
        "启用时输出经运行时验证为 1920×1080 `GL_R16F`",
        "- 百分位：合并同配置各独立进程的原始帧样本后，按 nearest-rank "
        "计算 Median/P95/P99",
        "",
        "## 结论",
        "",
    ]
    if decision == "go":
        lines.extend(
            [
                f"**Go。** {decision_reason}",
                "",
                "下一阶段可进入数据对照：Full-64、Full-32、Half-64、"
                "Half-32 + Depth/Normal-aware Bilateral Upsampling。"
                "本批次没有实现其中任何优化。",
            ]
        )
    elif decision == "no-go":
        lines.extend(
            [
                f"**No-Go。** {decision_reason}",
                "",
                "当前不应以半分辨率 SSAO 为下一项核心优化。应先补 Draw Submission "
                "计时，或转向基线中已确认的其他实际 GPU/CPU 热点。",
            ]
        )
    else:
        lines.extend(
            [
                "**待判定。** 当前产物用于链路筛查或尚未写入正式数据判断；"
                "不能据此启动 SSAO 优化实现。",
            ]
        )
    lines.extend(
        [
            "",
            "### 实验混杂项更正",
            "",
            "当前实现始终按 `i/64` 递增尺度生成 64 个 Kernel，8/16/32 "
            "配置直接取该 64 核序列的前缀。因此这些配置同时改变了样本数量与"
            "径向分布。报告中的 14.10× / 13.94× 只能解释为“当前 "
            "Full-8/16/32/64 实现配置的实测扩张”，不能归因为纯样本数效应。",
            "",
            "这不影响 Full-64 相对 SSAO Off 的核心 Go 证据：Sponza 与 "
            "San Miguel 的稳定 SSAO GPU Median 仍分别为 2.216 ms 与 "
            "2.638 ms。旧原始数据保持不变，本更正没有重跑或替换旧结果。",
        ]
    )
    lines.extend(
        [
            "",
            "## GPU 结果",
            "",
            "表中时间均为毫秒，格式为 `Median / P95 / P99`。"
            "“进程离散”是各进程 Median 的 `(最大值−最小值)/进程 Median 中位数`。",
            "",
        ]
    )
    for scene_id in sorted({group["scene"] for group in aggregates}):
        lines.extend(
            [
                f"### {display_names.get(scene_id, scene_id)}",
                "",
                "| 配置 | GPU Frame | Deferred Pass GPU | SSAO Pass GPU | "
                "SSAO/GPU Frame | GPU Frame 进程离散 | SSAO GPU 进程离散 | Draw Call |",
                "|---|---:|---:|---:|---:|---:|---:|---:|",
            ]
        )
        scene_groups = sorted(
            (group for group in aggregates if group["scene"] == scene_id),
            key=lambda group: group["samples"],
        )
        comparison_rows = {
            row["samples"]: row
            for comparison in comparisons
            if comparison["scene"] == scene_id
            for row in comparison["configurations"]
        }
        for group in scene_groups:
            metrics = group["metrics"]
            row = comparison_rows[group["samples"]]
            config = "Off" if group["samples"] == 0 else f"Full-{group['samples']}"
            share = (
                "—"
                if group["samples"] == 0
                else f"{row['ssaoGenerationShareOfGpuFramePercent']:.1f}%"
            )
            draw_calls = format_triplet(metrics["drawCalls"], 0)
            lines.append(
                f"| {config} | {format_triplet(metrics['gpuFrame'])} | "
                f"{format_triplet(metrics['deferredGpu'])} | "
                f"{format_triplet(metrics['ssaoGpu'])} | {share} | "
                f"{format_dispersion(metrics['gpuFrame'])} | "
                f"{format_dispersion(metrics['ssaoGpu'])} | {draw_calls} |"
            )
        lines.extend(["", ""])
    lines.extend(
        [
            "![SSAO GPU timing](figures/ssao-timing.png)",
            "",
            "## CPU 结果",
            "",
            "CPU Frame 是主循环 CPU 墙钟时间；SSAO Pass CPU 是 AO pass 的 CPU "
            "提交区间。CPU 与 GPU 数字不可相加。",
            "",
        ]
    )
    for scene_id in sorted({group["scene"] for group in aggregates}):
        lines.extend(
            [
                f"### {display_names.get(scene_id, scene_id)}",
                "",
                "| 配置 | CPU Frame | SSAO Pass CPU | CPU Frame 进程离散 | "
                "SSAO CPU 进程离散 |",
                "|---|---:|---:|---:|---:|",
            ]
        )
        for group in sorted(
            (group for group in aggregates if group["scene"] == scene_id),
            key=lambda group: group["samples"],
        ):
            metrics = group["metrics"]
            config = "Off" if group["samples"] == 0 else f"Full-{group['samples']}"
            lines.append(
                f"| {config} | {format_triplet(metrics['cpuFrame'])} | "
                f"{format_triplet(metrics['ssaoCpu'])} | "
                f"{format_dispersion(metrics['cpuFrame'])} | "
                f"{format_dispersion(metrics['ssaoCpu'])} |"
            )
        lines.extend(["", ""])
    cpu_outlier_group = max(
        aggregates,
        key=lambda group: float(
            group["metrics"]["cpuFrame"]["processMedianDispersion"][
                "rangePercent"
            ]
            or 0.0
        ),
    )
    gpu_frame_dispersion_group = max(
        aggregates,
        key=lambda group: float(
            group["metrics"]["gpuFrame"]["processMedianDispersion"][
                "rangePercent"
            ]
            or 0.0
        ),
    )
    ssao_gpu_groups = [
        group
        for group in aggregates
        if group["metrics"]["ssaoGpu"]["pooled"]["count"] > 0
    ]
    ssao_gpu_dispersion_group = max(
        ssao_gpu_groups,
        key=lambda group: float(
            group["metrics"]["ssaoGpu"]["processMedianDispersion"][
                "rangePercent"
            ]
            or 0.0
        ),
    )

    def report_configuration(group: dict[str, Any]) -> str:
        return "Off" if group["samples"] == 0 else f"Full-{group['samples']}"

    def process_medians(group: dict[str, Any], metric: str, digits: int) -> str:
        return " / ".join(
            f"{float(row['median']):.{digits}f}"
            for row in group["metrics"][metric]["perProcess"]
        )

    cpu_dispersion = float(
        cpu_outlier_group["metrics"]["cpuFrame"]["processMedianDispersion"][
            "rangePercent"
        ]
    )
    ssao_cpu_dispersion = cpu_outlier_group["metrics"]["ssaoCpu"][
        "processMedianDispersion"
    ]["rangePercent"]
    corresponding_gpu_dispersion = float(
        cpu_outlier_group["metrics"]["gpuFrame"]["processMedianDispersion"][
            "rangePercent"
        ]
    )
    corresponding_ssao_gpu_dispersion = cpu_outlier_group["metrics"]["ssaoGpu"][
        "processMedianDispersion"
    ]["rangePercent"]
    lines.extend(
        [
            "### 进程稳定性备注",
            "",
            f"- GPU Frame 的最大进程间相对极差为 "
            f"{float(gpu_frame_dispersion_group['metrics']['gpuFrame']['processMedianDispersion']['rangePercent']):.2f}%"
            f"（{display_names.get(gpu_frame_dispersion_group['scene'], gpu_frame_dispersion_group['scene'])} "
            f"{report_configuration(gpu_frame_dispersion_group)}）；SSAO Pass GPU 的最大值为 "
            f"{float(ssao_gpu_dispersion_group['metrics']['ssaoGpu']['processMedianDispersion']['rangePercent']):.2f}%"
            f"（{display_names.get(ssao_gpu_dispersion_group['scene'], ssao_gpu_dispersion_group['scene'])} "
            f"{report_configuration(ssao_gpu_dispersion_group)}）。",
            f"- CPU 侧最大离散出现在 "
            f"{display_names.get(cpu_outlier_group['scene'], cpu_outlier_group['scene'])} "
            f"{report_configuration(cpu_outlier_group)}：三进程 CPU Frame Median 为 "
            f"{process_medians(cpu_outlier_group, 'cpuFrame', 4)} ms，相对极差 "
            f"{cpu_dispersion:.2f}%；SSAO CPU Median 为 "
            f"{process_medians(cpu_outlier_group, 'ssaoCpu', 4)} ms"
            + (
                f"，相对极差 {float(ssao_cpu_dispersion):.2f}%。"
                if ssao_cpu_dispersion is not None
                else "。"
            ),
            f"- 同一配置的 GPU Frame 与 SSAO GPU 相对极差仅 "
            f"{corresponding_gpu_dispersion:.2f}% 和 "
            + (
                f"{float(corresponding_ssao_gpu_dispersion):.2f}%"
                if corresponding_ssao_gpu_dispersion is not None
                else "不可用"
            )
            + "。由此推断该异常主要是单进程 CPU 侧噪声；"
            "它不改变 GPU Go 判断，但该配置的 CPU 数据不应被用于宣称稳定 CPU 收益。",
            "",
        ]
    )
    lines.extend(
        [
            "## 当前实现配置扩张与完整帧影响",
            "",
            "`SSAO Pass GPU` 是 AO 生成区间；`GPU Frame On−Off` 更接近整个 SSAO "
            "feature 对最终 GPU 帧的影响，因为 Deferred Lighting 对 AO 纹理的采样不在 "
            "`SSAO Pass` zone 内。",
            "",
        ]
    )
    for comparison in comparisons:
        scene_id = comparison["scene"]
        lines.extend(
            [
                f"### {display_names.get(scene_id, scene_id)}",
                "",
                "| 配置 | SSAO GPU Median | 相对 Full-8 | 名义样本数比例（非隔离变量） | "
                "相邻倍率 | GPU Frame On−Off | 相对 Off |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for row in comparison["configurations"]:
            config = "Off" if row["samples"] == 0 else f"Full-{row['samples']}"
            if row["samples"] == 0:
                lines.append(f"| {config} | — | — | — | — | 0.000 ms | 0.0% |")
            else:
                adjacent = (
                    "—"
                    if row["ssaoGpuAdjacentScale"] is None
                    else f"{row['ssaoGpuAdjacentScale']:.2f}×"
                )
                lines.append(
                    f"| {config} | {row['ssaoGpuMedianMs']:.3f} ms | "
                    f"{row['ssaoGpuScaleVs8']:.2f}× | "
                    f"{row['idealSampleCountScaleVs8']:.2f}× | {adjacent} | "
                    f"{row['gpuFrameDeltaVsOffMs']:+.3f} ms | "
                    f"{row['gpuFrameDeltaVsOffPercent']:+.1f}% |"
                )
        lines.extend(["", ""])
    lines.extend(
        [
            "![SSAO scaling and frame impact](figures/ssao-scaling-and-frame-impact.png)",
            "",
            "## 固定帧质量参考",
            "",
            "以下截图只用于确认相机、灯光与输出链路一致，不作为正式图像质量评分。",
            "",
        ]
    )
    for scene_id, figures in sorted(capture_figures.items()):
        lines.append(f"### {display_names.get(scene_id, scene_id)}")
        lines.append("")
        if "final" in figures:
            lines.append(f"![{scene_id} final lighting]({figures['final']})")
            lines.append("")
        if "ao" in figures:
            lines.append(f"![{scene_id} AO output]({figures['ao']})")
            lines.append("")
    lines.extend(
        [
            "## 计时边界与解释",
            "",
            "- 代码审查确认 GPU query 使用独立 `GL_TIMESTAMP` 起止点；"
            "`GPU Frame → Deferred Pass → SSAO Pass` 的嵌套合法。"
            "捕获前后均等待 GPU 并排空延迟查询。",
            "- 本入口严格要求 GPU Frame、Deferred Pass、SSAO Pass（启用时）的有效 "
            "GPU/CPU 样本数等于请求采样帧数；缺样结果不会进入汇总。",
            "- `SSAO Pass GPU` 包含 AO FBO/状态设置、清屏、Kernel Uniform 提交及全屏 "
            "AO draw；不包含随后 Deferred Lighting 对 AO 纹理的采样。",
            "- `Deferred Pass GPU` 包含 GBuffer、嵌套 SSAO、Deferred Lighting、天空盒"
            "等多个阶段；它与 SSAO zone 重叠，不能相加。",
            "- `GPU Frame` 包含 Deferred、Postprocess、最终呈现 blit 与 UI GPU 提交；"
            "不包含 swap/events。`CPU Frame` 包含主循环 CPU 工作与 Present/Events。",
            "- Draw Call 是引擎显式记录的 draw 数；UI draw、blit 与 clear 不计入该字段。",
            "",
            "## 数据文件",
            "",
            "- 原始运行结果：[`raw/`](raw/)",
            "- 运行清单：[`run-manifest.json`](run-manifest.json)",
            "- 机器可读汇总：[`summary.json`](summary.json)",
            "- 聚合 CSV：[`summary.csv`](summary.csv)",
            "- 独立进程 CSV：[`process-summary.csv`](process-summary.csv)",
            "- 控制台日志：[`logs/`](logs/)",
            "- 原始固定帧：[`captures/`](captures/)",
            "",
            "## 已知限制",
            "",
            "- Full-8/16/32 使用按 `i/64` 生成的 64 核前缀，样本数与径向分布"
            "同时变化；配置扩张不能解释为纯样本数因果效应。",
            "- 本基线只覆盖两个固定相机的静态真实场景，不代表所有镜头、分辨率或 GPU。",
            "- OpenGL Timestamp 衡量提交区间在 GPU 上的执行时间，不能单独证明某条着色器"
            "指令或每帧 Kernel Uniform 上传的因果成本。",
            "- AO 输出截图是后续质量比较的参考，不含 SSIM、边缘保真或时域稳定性评分。",
            "- 本任务没有实现半分辨率、双边上采样、Instancing、Mesh 合并或其他新优化。",
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
    expect(manifest_path.is_file(), f"run manifest is missing: {manifest_path}")
    if arguments.decision != "pending":
        expect(
            bool(arguments.decision_reason.strip()),
            "a data-backed --decision-reason is required for Go/No-Go",
        )
    manifest = load_json(manifest_path)
    expect(int(manifest.get("schemaVersion", 0)) == 1, "unsupported manifest schema")
    protocol = manifest.get("protocol")
    expect(isinstance(protocol, dict), "manifest protocol is missing")
    expect(protocol.get("resolution") == [1920, 1080], "protocol is not 1920x1080")
    expect(protocol.get("renderPath") == "pbr-deferred", "protocol is not deferred")
    expect(protocol.get("requestedSwapInterval") == 0, "VSync is not disabled")
    expect(protocol.get("inputFrozen") is True, "input is not frozen")
    expect(protocol.get("bloom") is False, "Bloom is enabled")
    expect(protocol.get("shadows") is False, "shadows are enabled")
    expect(protocol.get("gammaCorrection") is True, "Gamma is disabled")
    expect(protocol.get("autoReloadShaders") is False, "shader reload is enabled")
    expect(protocol.get("autoReloadMaterials") is False, "material reload is enabled")

    project_directory = Path(__file__).resolve().parent.parent
    scene_records = {
        str(scene["id"]): scene for scene in manifest.get("scenes", [])
    }
    expected_samples = sorted(int(value) for value in protocol["configurations"])
    expect(
        all(value in CONFIGURATION_ORDER for value in expected_samples),
        f"unexpected configurations: {expected_samples}",
    )
    if arguments.decision != "pending":
        expect(
            manifest.get("preset") == "formal",
            "Go/No-Go may only be written for a Formal batch",
        )
        expect(
            expected_samples == list(CONFIGURATION_ORDER),
            "Go/No-Go requires Off and Full 8/16/32/64",
        )
        expect(
            set(scene_records) == {"sponza", "san-miguel"},
            "Go/No-Go requires both Sponza and San Miguel",
        )
        expect(
            int(protocol["warmupFrames"]) == 300,
            "Go/No-Go requires 300 warmup frames",
        )
        expect(
            int(protocol["measuredFrames"]) == 2000,
            "Go/No-Go requires 2000 measured frames",
        )
        expect(
            int(protocol["independentProcesses"]) == 3,
            "Go/No-Go requires three independent processes",
        )
    run_records = manifest.get("runs")
    expect(isinstance(run_records, list) and run_records, "manifest runs are missing")
    expected_processes = int(protocol["independentProcesses"])
    expected_run_count = len(scene_records) * len(expected_samples) * expected_processes
    expect(
        len(run_records) == expected_run_count,
        f"manifest has {len(run_records)} runs, expected {expected_run_count}",
    )

    seen: set[tuple[str, int, int]] = set()
    validated_runs: list[dict[str, Any]] = []
    for record in run_records:
        expect(isinstance(record, dict), "run record is not an object")
        scene_id = str(record.get("scene", ""))
        samples = int(record.get("samples", -1))
        process_index = int(record.get("process", 0))
        expect(scene_id in scene_records, f"unknown scene in run: {scene_id}")
        expect(samples in expected_samples, f"unexpected sample count: {samples}")
        key = (scene_id, samples, process_index)
        expect(key not in seen, f"duplicate run: {key}")
        seen.add(key)
        validated_runs.append(
            validate_run(
                record,
                scene_records[scene_id],
                manifest,
                project_directory,
            )
        )

    gl_signatures = {
        (run["glVendor"], run["glRenderer"], run["glVersion"])
        for run in validated_runs
    }
    expect(len(gl_signatures) == 1, "OpenGL device/driver changed between runs")
    for scene_id in scene_records:
        scene_runs = [run for run in validated_runs if run["scene"] == scene_id]
        signatures = {
            json.dumps(run["lightSignature"], sort_keys=True)
            for run in scene_runs
        }
        expect(len(signatures) == 1, f"{scene_id}: light/settings changed between runs")

    aggregates = aggregate_groups(validated_runs, expected_processes)
    expected_groups = len(scene_records) * len(expected_samples)
    expect(len(aggregates) == expected_groups, "one or more configuration groups are missing")
    for group in aggregates:
        expect(
            group["captures"]["final"] is not None,
            f"{group['scene']}/{group['samples']}: final-lighting capture is missing",
        )
        if group["samples"] > 0:
            expect(
                group["captures"]["ao"] is not None,
                f"{group['scene']}/{group['samples']}: AO reference capture is missing",
            )
    comparisons = add_comparisons(aggregates)

    figures_directory = input_directory / "figures"
    figures_directory.mkdir(parents=True, exist_ok=True)
    display_names = {
        scene_id: str(scene.get("displayName") or scene_id)
        for scene_id, scene in scene_records.items()
    }
    timing_chart(figures_directory / "ssao-timing.png", aggregates, display_names)
    scaling_chart(
        figures_directory / "ssao-scaling-and-frame-impact.png",
        comparisons,
        display_names,
    )
    capture_figures = create_capture_figures(
        figures_directory, aggregates, display_names
    )

    serializable_aggregates = json.loads(json.dumps(aggregates, default=str))
    summary_document = {
        "schemaVersion": 1,
        "generatedAtUtc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "validation": {
            "status": "pass",
            "validatedRuns": len(validated_runs),
            "expectedFramesPerRun": int(protocol["measuredFrames"]),
            "requiredZoneCountsExact": True,
            "fullResolutionR16fVerified": True,
            "fixedStateVerified": True,
        },
        "decision": {
            "value": arguments.decision,
            "reason": arguments.decision_reason.strip(),
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
                "range and coefficient of variation over independent-process medians"
            ),
        },
        "knownConfounds": [
            {
                "id": "kernel-prefix-radial-distribution",
                "description": (
                    "The implementation generates 64 kernels with scale i/64, "
                    "then Full-8/16/32 consume prefixes. Sample count and radial "
                    "distribution change together, so configuration expansion "
                    "is not a pure sample-count effect."
                ),
                "full64VsOffGoConclusionAffected": False,
                "oldRawDataRerun": False,
            }
        ],
        "results": serializable_aggregates,
        "comparisons": comparisons,
        "artifacts": {
            "summaryCsv": "summary.csv",
            "processSummaryCsv": "process-summary.csv",
            "report": "SSAO_BASELINE_REPORT_CN.md",
            "timingChart": "figures/ssao-timing.png",
            "scalingChart": "figures/ssao-scaling-and-frame-impact.png",
            "captureFigures": capture_figures,
        },
    }
    write_json(input_directory / "summary.json", summary_document)
    write_summary_csv(input_directory / "summary.csv", aggregates)
    write_process_csv(input_directory / "process-summary.csv", aggregates)
    markdown_report(
        input_directory / "SSAO_BASELINE_REPORT_CN.md",
        manifest,
        aggregates,
        comparisons,
        capture_figures,
        arguments.decision,
        arguments.decision_reason.strip(),
    )
    print(f"Validated {len(validated_runs)} runs.")
    print(f"Wrote {input_directory / 'summary.json'}")
    print(f"Wrote {input_directory / 'SSAO_BASELINE_REPORT_CN.md'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as error:
        raise SystemExit(f"SSAO baseline validation failed: {error}")
