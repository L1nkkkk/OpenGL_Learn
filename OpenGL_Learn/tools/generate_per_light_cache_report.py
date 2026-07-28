#!/usr/bin/env python3
"""Generate a Chinese A/B report bundle for a Per-Light shadow-cache experiment.

The source directory is expected to be produced by Test-ShadowOptimizations.ps1:

    metadata.json
    summary.json
    formal/<scene>/{A1,A2,A3,B1,B2,B3}.json

The generator intentionally recomputes the report statistics from the six run
files.  The benchmark's summary.json is still read and cross-checked, but is not
used as the source of resume-facing performance numbers.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Iterable

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib import font_manager

from shadow_image_compare import compare_images


RUN_NAMES = ("A1", "A2", "A3", "B1", "B2", "B3")
VARIANTS = ("A", "B")
EXPECTED_ORDER = ("A", "B", "B", "A", "A", "B")
MAX_CAPTURE_CHANNEL_DELTA = 255
MAX_CAPTURE_CHANGED_PIXELS = 32
EXPECTED_SCHEMA_VERSION = 17
EXPECTED_MEASURED_FRAMES = 1000
EXPECTED_EXTERNAL_WARMUP_FRAMES = 100
EXPECTED_INTERNAL_WARMUP_FRAMES = 15
DEFAULT_POINT_SHADOW_RESOLUTION = 1024
STATISTICS = ("median", "p95", "p99")

PERFORMANCE_METRICS = (
    {
        "key": "shadowGpu",
        "label": "阴影更新 GPU",
        "chartLabel": "阴影更新 GPU",
        "path": ("profiler", "summary", "gpuZones", "Shadow Map Update"),
        "summaryKey": "shadowMapUpdateGpuMilliseconds",
    },
    {
        "key": "gpuFrame",
        "label": "GPU 帧时间",
        "chartLabel": "GPU 帧时间",
        "path": ("profiler", "summary", "gpuFrame"),
        "summaryKey": "gpuFrameMilliseconds",
    },
    {
        "key": "shadowCpu",
        "label": "阴影阶段 CPU",
        "chartLabel": "阴影阶段 CPU",
        "path": ("profiler", "summary", "cpuZones", "Shadow Maps"),
        "summaryKey": "shadowMapsCpuMilliseconds",
    },
    {
        "key": "wall",
        "label": "整帧 Wall",
        "chartLabel": "整帧 Wall",
        "path": ("profiler", "summary", "wallFrame"),
        "summaryKey": "wallFrameMilliseconds",
    },
)

UPDATE_FIELDS = (
    ("updatedLightsPerFrame", "每帧更新总灯数", "measuredUpdatedLightCount"),
    (
        "directionalUpdatesPerFrame",
        "Directional 更新数/帧",
        "measuredDirectionalLightUpdateCount",
    ),
    ("pointUpdatesPerFrame", "Point 更新数/帧", "measuredPointLightUpdateCount"),
    ("spotUpdatesPerFrame", "Spot 更新数/帧", "measuredSpotLightUpdateCount"),
)

RESOURCE_FIELDS = (
    ("textureBytes", "纹理", "texture"),
    ("meshGpuBytes", "Mesh GPU", "meshGpu"),
    ("renderTargetBytes", "渲染目标", "renderTarget"),
)


class ReportError(RuntimeError):
    """Raised for incomplete or incompatible benchmark inputs."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "从 Per-Light 阴影缓存 A/B 实验目录生成中文 Markdown、JSON、"
            "性能图、更新数图、资源图和截图差异图。"
        )
    )
    parser.add_argument("experiment_dir", type=Path, help="实验输入目录")
    parser.add_argument("output_dir", type=Path, help="报告制品输出目录")
    parser.add_argument(
        "--skip-image-comparison",
        action="store_true",
        help="只生成数据和性能图，不生成 A/B 截图对比（正式报告不建议使用）",
    )
    return parser.parse_args(argv)


def read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ReportError(f"缺少必需文件：{path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ReportError(f"无法读取 JSON {path}：{exc}") from exc
    if not isinstance(value, dict):
        raise ReportError(f"JSON 顶层必须是对象：{path}")
    return value


def nested(value: Any, path: Iterable[str], label: str) -> Any:
    current = value
    traversed: list[str] = []
    for key in path:
        traversed.append(key)
        if not isinstance(current, dict) or key not in current:
            raise ReportError(f"缺少字段 {label}.{'.'.join(traversed)}")
        current = current[key]
    return current


def number(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ReportError(f"{label} 必须是数值")
    result = float(value)
    if not math.isfinite(result):
        raise ReportError(f"{label} 必须是有限数值")
    return result


def integer(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ReportError(f"{label} 必须是整数")
    return value


def boolean(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise ReportError(f"{label} 必须是 Boolean")
    return value


def require_equal(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise ReportError(f"{label} 应为 {expected!r}，实际为 {actual!r}")


def require_environment_value(
    environment: dict[str, Any],
    name: str,
    expected: str,
    label: str,
) -> None:
    actual = environment.get(name)
    if str(actual).strip().lower() != expected:
        raise ReportError(
            f"{label}.{name} 应为 {expected!r}，实际为 {actual!r}"
        )


def validate_metadata(
    metadata: dict[str, Any],
    summary: dict[str, Any],
) -> tuple[dict[str, Any], int]:
    require_equal(metadata.get("schemaVersion"), 1, "metadata.schemaVersion")
    require_equal(summary.get("schemaVersion"), 4, "summary.schemaVersion")
    require_equal(metadata.get("resolution"), [1920, 1080], "metadata.resolution")
    require_equal(
        metadata.get("configuration"),
        "Release x64",
        "metadata.configuration",
    )
    require_equal(
        metadata.get("order"),
        list(EXPECTED_ORDER),
        "metadata.order",
    )
    require_equal(
        metadata.get("measuredFrames"),
        EXPECTED_MEASURED_FRAMES,
        "metadata.measuredFrames",
    )
    require_equal(
        metadata.get("externalWarmupFrames"),
        EXPECTED_EXTERNAL_WARMUP_FRAMES,
        "metadata.externalWarmupFrames",
    )
    require_equal(
        metadata.get("externalWarmupPerformed"),
        True,
        "metadata.externalWarmupPerformed",
    )
    require_equal(
        metadata.get("internalWarmupFrames"),
        EXPECTED_INTERNAL_WARMUP_FRAMES,
        "metadata.internalWarmupFrames",
    )
    require_equal(
        metadata.get("formalRunsPerVariant"),
        3,
        "metadata.formalRunsPerVariant",
    )
    require_equal(
        metadata.get("pixelCorrectnessThreshold"),
        {
            "maximumChannelDelta": MAX_CAPTURE_CHANNEL_DELTA,
            "maximumChangedPixels": MAX_CAPTURE_CHANGED_PIXELS,
        },
        "metadata.pixelCorrectnessThreshold",
    )

    settings = metadata.get("settings")
    if not isinstance(settings, dict):
        raise ReportError("metadata.settings 必须是对象")
    expected_settings = {
        "workload": "move-point",
        "lights": "all",
        "mode": "hard",
        "sampling": "stable",
        "renderPath": "pbr-forward",
    }
    for key, expected in expected_settings.items():
        require_equal(settings.get(key), expected, f"metadata.settings.{key}")

    configured_scenes = settings.get("sceneIds")
    if (
        not isinstance(configured_scenes, list)
        or set(configured_scenes) != {"sponza", "san-miguel"}
        or len(configured_scenes) != 2
    ):
        raise ReportError(
            "metadata.settings.sceneIds 必须且只能包含 sponza、san-miguel"
        )

    requested_shadow_resolution = integer(
        settings.get("shadowResolution"),
        "metadata.settings.shadowResolution",
    )
    if requested_shadow_resolution < 0:
        raise ReportError("metadata.settings.shadowResolution 不得为负数")
    point_shadow_resolution = (
        requested_shadow_resolution
        if requested_shadow_resolution > 0
        else DEFAULT_POINT_SHADOW_RESOLUTION
    )

    environments: dict[str, dict[str, Any]] = {}
    for variant, key in (
        ("A", "variantAEnvironment"),
        ("B", "variantBEnvironment"),
    ):
        environment = settings.get(key)
        if not isinstance(environment, dict):
            raise ReportError(f"metadata.settings.{key} 必须是对象")
        environments[variant] = environment
        common = {
            "OPENGL_LEARN_SHADOW_CASTER_CULLING": "1",
            "OPENGL_LEARN_POINT_SHADOW_RENDER_PATH": "six-face",
            "OPENGL_LEARN_POINT_SHADOW_FACE_CULLING": "1",
            "OPENGL_LEARN_SHADOW_SPOT_CASTER_DEPTH_FIT": "0",
        }
        for name, expected in common.items():
            require_environment_value(environment, name, expected, key)
    require_environment_value(
        environments["A"],
        "OPENGL_LEARN_SHADOW_CACHE",
        "none",
        "variantAEnvironment",
    )
    require_environment_value(
        environments["B"],
        "OPENGL_LEARN_SHADOW_CACHE",
        "revision",
        "variantBEnvironment",
    )
    require_environment_value(
        environments["A"],
        "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE",
        "0",
        "variantAEnvironment",
    )
    require_environment_value(
        environments["B"],
        "OPENGL_LEARN_SHADOW_PER_LIGHT_CACHE",
        "1",
        "variantBEnvironment",
    )
    return settings, point_shadow_resolution


def rounded(value: float) -> float:
    return round(value, 6)


def mean(values: list[float]) -> float:
    if not values:
        raise ReportError("无法计算空数组的均值")
    return sum(values) / len(values)


def delta_percent(before: float, after: float) -> float | None:
    if before == 0.0:
        return None
    return rounded((after - before) * 100.0 / before)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def configure_chinese_font() -> str:
    preferred = (
        "Microsoft YaHei",
        "Microsoft YaHei UI",
        "SimHei",
        "DengXian",
        "Noto Sans CJK SC",
        "Source Han Sans CN",
        "Arial Unicode MS",
    )
    available = {entry.name for entry in font_manager.fontManager.ttflist}
    selected = next((name for name in preferred if name in available), None)
    if selected is None:
        raise ReportError(
            "未找到可用于中文图表的字体；请安装 Microsoft YaHei、"
            "Noto Sans CJK SC 或 Source Han Sans CN"
        )
    matplotlib.rcParams["font.family"] = "sans-serif"
    matplotlib.rcParams["font.sans-serif"] = [selected, "DejaVu Sans"]
    matplotlib.rcParams["axes.unicode_minus"] = False
    matplotlib.rcParams["figure.facecolor"] = "white"
    matplotlib.rcParams["axes.facecolor"] = "#fbfdff"
    return selected


def scene_ids(metadata: dict[str, Any], summary: dict[str, Any], formal: Path) -> list[str]:
    discovered = sorted(path.name for path in formal.iterdir() if path.is_dir())
    settings = metadata.get("settings")
    configured = settings.get("sceneIds") if isinstance(settings, dict) else None
    if isinstance(configured, list) and all(isinstance(item, str) for item in configured):
        missing = [item for item in configured if item not in discovered]
        if missing:
            raise ReportError(f"metadata.json 中的场景缺少 formal 数据：{missing}")
        return list(configured)

    summary_scenes = summary.get("scenes")
    if isinstance(summary_scenes, list):
        ordered = [
            item["id"]
            for item in summary_scenes
            if isinstance(item, dict)
            and isinstance(item.get("id"), str)
            and item["id"] in discovered
        ]
        if ordered:
            return ordered
    if not discovered:
        raise ReportError(f"没有发现正式场景目录：{formal}")
    return discovered


def display_names(summary: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    scenes = summary.get("scenes")
    if not isinstance(scenes, list):
        return result
    for scene in scenes:
        if not isinstance(scene, dict) or not isinstance(scene.get("id"), str):
            continue
        display = scene.get("displayName")
        result[scene["id"]] = display if isinstance(display, str) else scene["id"]
    return result


def load_runs(scene_dir: Path) -> dict[str, dict[str, Any]]:
    runs: dict[str, dict[str, Any]] = {}
    for name in RUN_NAMES:
        path = scene_dir / f"{name}.json"
        run = read_json(path)
        if run.get("success") is not True:
            raise ReportError(f"正式运行未成功：{path}")
        runs[name] = run
    return runs


def validate_sample_list(value: Any, expected_count: int, label: str) -> None:
    if not isinstance(value, list) or len(value) != expected_count:
        actual = len(value) if isinstance(value, list) else type(value).__name__
        raise ReportError(
            f"{label} 应包含 {expected_count} 个样本，实际为 {actual}"
        )


def validate_distribution_count(
    run: dict[str, Any],
    path: tuple[str, ...],
    expected_count: int,
    label: str,
) -> None:
    count = integer(
        nested(run, (*path, "count"), label),
        f"{label}.count",
    )
    if count != expected_count:
        raise ReportError(
            f"{label}.count 应为 {expected_count}，实际为 {count}"
        )


def validate_run_set(
    scene_id: str,
    runs: dict[str, dict[str, Any]],
    metadata: dict[str, Any],
    settings: dict[str, Any],
) -> None:
    expected_resolution = metadata["resolution"]
    expected_frames = metadata["measuredFrames"]
    expected_warmup = metadata["internalWarmupFrames"]
    expected_variant_labels = {
        "A": settings.get("variantALabel"),
        "B": settings.get("variantBLabel"),
    }
    if not all(
        isinstance(label, str) and label
        for label in expected_variant_labels.values()
    ):
        raise ReportError("metadata 必须记录非空的 A/B variant label")

    for run_name in RUN_NAMES:
        run = runs[run_name]
        variant = run_name[0]
        label = f"{scene_id}/{run_name}"
        require_equal(
            run.get("schemaVersion"),
            EXPECTED_SCHEMA_VERSION,
            f"{label}.schemaVersion",
        )
        require_equal(run.get("success"), True, f"{label}.success")
        require_equal(run.get("resolution"), expected_resolution, f"{label}.resolution")
        require_equal(
            run.get("measuredFrames"),
            expected_frames,
            f"{label}.measuredFrames",
        )
        require_equal(
            run.get("warmupFrames"),
            expected_warmup,
            f"{label}.warmupFrames",
        )
        require_equal(
            run.get("frameMeasurement"),
            "gpu-synchronized-wall",
            f"{label}.frameMeasurement",
        )
        require_equal(
            run.get("renderPath"),
            settings["renderPath"],
            f"{label}.renderPath",
        )
        require_equal(
            run.get("scene"),
            f"{scene_id}-{run_name}",
            f"{label}.scene",
        )
        if not isinstance(run.get("capturePath"), str) or not run["capturePath"]:
            raise ReportError(f"{label}.capturePath 必须是非空字符串")

        shadow = run.get("shadow")
        if not isinstance(shadow, dict):
            raise ReportError(f"{label}.shadow 必须是对象")
        expected_shadow_values = {
            "experiment": True,
            "mode": settings["mode"],
            "sampling": settings["sampling"],
            "lights": settings["lights"],
            "workload": settings["workload"],
            "variant": expected_variant_labels[variant],
            "requestedResolution": settings["shadowResolution"],
            "legacyCacheSignatureEnabled": False,
            "cacheDisabled": variant == "A",
            "perLightCacheEnabled": variant == "B",
            "casterCullingEnabled": True,
            "pointShadowRenderPolicy": "six-face",
            "pointShadowFaceCullingEnabled": True,
            "spotCasterDepthFitEnabled": False,
        }
        for key, expected in expected_shadow_values.items():
            require_equal(shadow.get(key), expected, f"{label}.shadow.{key}")

        profiler = run.get("profiler")
        if not isinstance(profiler, dict):
            raise ReportError(f"{label}.profiler 必须是对象")
        require_equal(
            profiler.get("gpuTimingSupported"),
            True,
            f"{label}.profiler.gpuTimingSupported",
        )
        require_equal(
            nested(
                run,
                ("frameTimeMilliseconds", "sampleCount"),
                label,
            ),
            expected_frames,
            f"{label}.frameTimeMilliseconds.sampleCount",
        )

        distribution_paths = (
            ("profiler", "summary", "wallFrame"),
            ("profiler", "summary", "cpuFrame"),
            ("profiler", "summary", "gpuFrame"),
            ("profiler", "summary", "cpuZones", "Shadow Maps"),
            ("profiler", "summary", "gpuZones", "Shadow Map Update"),
        )
        for path in distribution_paths:
            validate_distribution_count(
                run,
                path,
                expected_frames,
                f"{label}.{'.'.join(path)}",
            )

        sample_paths = (
            ("profiler", "samples", "wallFrame"),
            ("profiler", "samples", "cpuFrame"),
            ("profiler", "samples", "gpuFrame"),
            ("profiler", "samples", "cpuZones", "Shadow Maps"),
            ("profiler", "samples", "gpuZones", "Shadow Map Update"),
        )
        for path in sample_paths:
            validate_sample_list(
                nested(run, path, label),
                expected_frames,
                f"{label}.{'.'.join(path)}",
            )


def aggregate_values(
    runs: dict[str, dict[str, Any]],
    getter: Any,
) -> dict[str, Any]:
    values: dict[str, list[float]] = {}
    for variant in VARIANTS:
        values[variant] = [
            rounded(getter(runs[f"{variant}{index}"], f"{variant}{index}"))
            for index in range(1, 4)
        ]
    averages = {variant: rounded(mean(values[variant])) for variant in VARIANTS}
    return {
        "A": {
            "values": values["A"],
            "mean": averages["A"],
            "range": [min(values["A"]), max(values["A"])],
        },
        "B": {
            "values": values["B"],
            "mean": averages["B"],
            "range": [min(values["B"]), max(values["B"])],
        },
        "deltaPercent": delta_percent(averages["A"], averages["B"]),
        "pairedDeltaPercent": [
            delta_percent(values["A"][index], values["B"][index])
            for index in range(3)
        ],
    }


def metric_result(
    runs: dict[str, dict[str, Any]],
    descriptor: dict[str, Any],
    statistic: str,
) -> dict[str, Any]:
    path = (*descriptor["path"], statistic)

    def getter(run: dict[str, Any], run_name: str) -> float:
        return number(
            nested(run, path, f"run {run_name}"),
            f"run {run_name}.{'.'.join(path)}",
        )

    result = aggregate_values(runs, getter)
    result.update(
        {
            "label": descriptor["label"],
            "unit": "ms",
            "statistic": statistic,
            "sourcePath": ".".join(path),
        }
    )
    return result


def update_result(
    runs: dict[str, dict[str, Any]], key: str, label: str, field: str
) -> tuple[str, dict[str, Any]]:
    def getter(run: dict[str, Any], run_name: str) -> float:
        frames = integer(run.get("measuredFrames"), f"run {run_name}.measuredFrames")
        if frames <= 0:
            raise ReportError(f"run {run_name}.measuredFrames 必须大于零")
        count = integer(
            nested(run, ("shadow", field), f"run {run_name}"),
            f"run {run_name}.shadow.{field}",
        )
        return count / frames

    result = aggregate_values(runs, getter)
    result.update({"label": label, "unit": "updates/frame", "sourceField": field})
    return key, result


def resource_result(
    runs: dict[str, dict[str, Any]], key: str, label: str, field: str
) -> tuple[str, dict[str, Any]]:
    values: dict[str, list[int]] = {}
    for variant in VARIANTS:
        values[variant] = []
        for index in range(1, 4):
            run_name = f"{variant}{index}"
            value = integer(
                nested(
                    runs[run_name],
                    ("memoryBytes", field),
                    f"run {run_name}",
                ),
                f"run {run_name}.memoryBytes.{field}",
            )
            values[variant].append(value)
    combined = values["A"] + values["B"]
    result = {
        "label": label,
        "unit": "bytes",
        "sourceField": f"memoryBytes.{field}",
        "A": {
            "valuesBytes": values["A"],
            "meanBytes": rounded(mean([float(value) for value in values["A"]])),
            "meanMiB": rounded(mean([float(value) for value in values["A"]]) / 2**20),
        },
        "B": {
            "valuesBytes": values["B"],
            "meanBytes": rounded(mean([float(value) for value in values["B"]])),
            "meanMiB": rounded(mean([float(value) for value in values["B"]]) / 2**20),
        },
        "pairedExact": [
            values["A"][index] == values["B"][index] for index in range(3)
        ],
        "allSixRunsExact": len(set(combined)) == 1,
    }
    return key, result


def point_path_evidence(runs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    fields = {
        "layeredUpdatesPerFrame": "measuredPointShadowLayeredUpdateCount",
        "sixFaceUpdatesPerFrame": "measuredPointShadowSixFaceUpdateCount",
        "submissionPassesPerFrame": "measuredPointShadowSubmissionPassCount",
    }
    result: dict[str, Any] = {}
    for key, field in fields.items():
        _, aggregate = update_result(runs, key, key, field)
        result[key] = aggregate

    policies: dict[str, list[str]] = {}
    update_accounting: dict[str, list[bool]] = {}
    submission_accounting: dict[str, list[bool]] = {}
    for variant in VARIANTS:
        policies[variant] = []
        update_accounting[variant] = []
        submission_accounting[variant] = []
        for index in range(1, 4):
            run_name = f"{variant}{index}"
            run = runs[run_name]
            policy = nested(
                run, ("shadow", "pointShadowRenderPolicy"), f"run {run_name}"
            )
            policies[variant].append(str(policy))
            layered = integer(
                nested(
                    run,
                    ("shadow", "measuredPointShadowLayeredUpdateCount"),
                    f"run {run_name}",
                ),
                f"run {run_name}.shadow.measuredPointShadowLayeredUpdateCount",
            )
            six_face = integer(
                nested(
                    run,
                    ("shadow", "measuredPointShadowSixFaceUpdateCount"),
                    f"run {run_name}",
                ),
                f"run {run_name}.shadow.measuredPointShadowSixFaceUpdateCount",
            )
            submissions = integer(
                nested(
                    run,
                    ("shadow", "measuredPointShadowSubmissionPassCount"),
                    f"run {run_name}",
                ),
                f"run {run_name}.shadow.measuredPointShadowSubmissionPassCount",
            )
            point_updates = integer(
                nested(
                    run,
                    ("shadow", "measuredPointLightUpdateCount"),
                    f"run {run_name}",
                ),
                f"run {run_name}.shadow.measuredPointLightUpdateCount",
            )
            update_accounting[variant].append(
                layered + six_face == point_updates
            )
            submission_accounting[variant].append(
                submissions == layered + six_face * 6
            )
    result["renderPolicy"] = policies
    result["updateAccounting"] = update_accounting
    result["submissionAccounting"] = submission_accounting
    result["allUpdateAccountingValid"] = all(
        valid for values in update_accounting.values() for valid in values
    )
    result["allSubmissionAccountingValid"] = all(
        valid for values in submission_accounting.values() for valid in values
    )
    return result


def point_cube_evidence(
    runs: dict[str, dict[str, Any]],
    expected_resolution: int,
) -> dict[str, Any]:
    if expected_resolution <= 0:
        raise ReportError("点阴影 Cubemap 分辨率必须大于零")
    expected_dimensions = [expected_resolution, expected_resolution]
    expected_samples_per_face = expected_resolution * expected_resolution
    expected_face_names = ("+X", "-X", "+Y", "-Y", "+Z", "-Z")
    per_run: dict[str, dict[str, Any]] = {}
    all_faces_valid = True
    for run_name in RUN_NAMES:
        evidence = nested(
            runs[run_name],
            ("shadow", "pointShadowCubeEvidence"),
            f"run {run_name}",
        )
        if not isinstance(evidence, dict):
            raise ReportError(
                f"run {run_name}.shadow.pointShadowCubeEvidence 必须是对象"
            )
        faces = evidence.get("faces")
        resolution = evidence.get("resolution")
        sample_count = evidence.get("sampleCountPerFace")
        valid = evidence.get("valid") is True
        resolution_valid = (
            resolution == expected_dimensions
            and all(
                isinstance(value, int)
                and not isinstance(value, bool)
                and value > 0
                for value in resolution
            )
        ) if isinstance(resolution, list) and len(resolution) == 2 else False
        sample_count_valid = (
            isinstance(sample_count, int)
            and not isinstance(sample_count, bool)
            and sample_count == expected_samples_per_face
        )
        if not isinstance(faces, list) or len(faces) != 6:
            raise ReportError(
                f"run {run_name} 必须记录六个点阴影 Cubemap 面"
            )
        hashes: list[str] = []
        non_far_counts: list[int] = []
        depth_ranges: list[list[float]] = []
        face_validity: list[bool] = []
        for face_index, face in enumerate(faces):
            if not isinstance(face, dict):
                raise ReportError(
                    f"run {run_name} face {face_index} 必须是对象"
                )
            face_hash = face.get("bitwiseHash")
            non_far = face.get("nonFarSampleCount")
            minimum_depth = face.get("minDepth")
            maximum_depth = face.get("maxDepth")
            minimum_valid = (
                isinstance(minimum_depth, (int, float))
                and not isinstance(minimum_depth, bool)
                and math.isfinite(float(minimum_depth))
                and 0.0 <= float(minimum_depth) <= 1.0
            )
            maximum_valid = (
                isinstance(maximum_depth, (int, float))
                and not isinstance(maximum_depth, bool)
                and math.isfinite(float(maximum_depth))
                and 0.0 <= float(maximum_depth) <= 1.0
            )
            face_valid = (
                face.get("valid") is True
                and face.get("index") == face_index
                and face.get("name") == expected_face_names[face_index]
                and isinstance(face_hash, str)
                and re.fullmatch(r"0x[0-9a-fA-F]{16}", face_hash) is not None
                and isinstance(non_far, int)
                and not isinstance(maximum_depth, bool)
                and not isinstance(non_far, bool)
                and 0 <= non_far <= expected_samples_per_face
                and minimum_valid
                and maximum_valid
                and float(minimum_depth) <= float(maximum_depth)
            )
            hashes.append(str(face_hash))
            non_far_counts.append(
                int(non_far)
                if isinstance(non_far, int) and not isinstance(non_far, bool)
                else -1
            )
            depth_ranges.append(
                [
                    float(minimum_depth) if minimum_valid else math.nan,
                    float(maximum_depth) if maximum_valid else math.nan,
                ]
            )
            face_validity.append(face_valid)
        run_valid = (
            valid
            and resolution_valid
            and sample_count_valid
            and all(face_validity)
        )
        all_faces_valid = all_faces_valid and run_valid
        per_run[run_name] = {
            "valid": run_valid,
            "resolution": resolution,
            "resolutionMatchesMetadata": resolution_valid,
            "sampleCountPerFace": sample_count,
            "hashes": hashes,
            "nonFarSampleCounts": non_far_counts,
            "depthRanges": depth_ranges,
            "facesValid": face_validity,
        }

    paired: list[dict[str, Any]] = []
    for index in range(1, 4):
        before = per_run[f"A{index}"]
        after = per_run[f"B{index}"]
        paired.append(
            {
                "pair": index,
                "resolution": before["resolution"],
                "allSixFaceHashesExact": (
                    before["valid"]
                    and after["valid"]
                    and before["resolution"] == after["resolution"]
                    and before["hashes"] == after["hashes"]
                    and before["nonFarSampleCounts"]
                    == after["nonFarSampleCounts"]
                    and before["depthRanges"] == after["depthRanges"]
                ),
                "minimumNonFarSamplesA": min(
                    before["nonFarSampleCounts"]
                ),
                "minimumNonFarSamplesB": min(
                    after["nonFarSampleCounts"]
                ),
            }
        )
    return {
        "runs": per_run,
        "paired": paired,
        "allFacesValid": all_faces_valid,
        "allPairedContentsExact": all(
            item["allSixFaceHashesExact"] for item in paired
        ),
    }


def cache_safety_evidence(runs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    fields = {
        "resourceFailures": "measuredShadowResourceFailureCount",
        "conservativeFallbacks": "measuredConservativeShadowFallbackCount",
        "emptyShadowClears": "measuredEmptyShadowClearCount",
    }
    values: dict[str, dict[str, list[int]]] = {}
    for key, field in fields.items():
        values[key] = {}
        for variant in VARIANTS:
            values[key][variant] = [
                integer(
                    nested(
                        runs[f"{variant}{index}"],
                        ("shadow", field),
                        f"run {variant}{index}",
                    ),
                    f"run {variant}{index}.shadow.{field}",
                )
                for index in range(1, 4)
            ]
    return {
        "values": values,
        "noResourceFailures": all(
            value == 0
            for variant_values in values["resourceFailures"].values()
            for value in variant_values
        ),
        "noConservativeFallbacks": all(
            value == 0
            for variant_values in values["conservativeFallbacks"].values()
            for value in variant_values
        ),
        "noUnexpectedEmptyClears": all(
            value == 0
            for variant_values in values["emptyShadowClears"].values()
            for value in variant_values
        ),
    }


def summarize_scene(
    scene_id: str,
    display_name: str,
    runs: dict[str, dict[str, Any]],
    expected_point_shadow_resolution: int,
) -> dict[str, Any]:
    metrics = {
        descriptor["key"]: {
            statistic: metric_result(runs, descriptor, statistic)
            for statistic in STATISTICS
        }
        for descriptor in PERFORMANCE_METRICS
    }
    updates = dict(
        update_result(runs, key, label, field)
        for key, label, field in UPDATE_FIELDS
    )
    resources = dict(
        resource_result(runs, key, label, field)
        for key, label, field in RESOURCE_FIELDS
    )
    return {
        "id": scene_id,
        "displayName": display_name,
        "runConditions": {
            "resolution": {
                variant: [
                    runs[f"{variant}{index}"].get("resolution")
                    for index in range(1, 4)
                ]
                for variant in VARIANTS
            },
            "measuredFrames": {
                variant: [
                    runs[f"{variant}{index}"].get("measuredFrames")
                    for index in range(1, 4)
                ]
                for variant in VARIANTS
            },
            "frameMeasurement": {
                variant: [
                    runs[f"{variant}{index}"].get("frameMeasurement")
                    for index in range(1, 4)
                ]
                for variant in VARIANTS
            },
        },
        "metrics": metrics,
        "updates": updates,
        "resources": resources,
        "pointShadowPath": point_path_evidence(runs),
        "pointShadowCube": point_cube_evidence(
            runs,
            expected_point_shadow_resolution,
        ),
        "cacheSafety": cache_safety_evidence(runs),
        "runFiles": {
            name: {
                "path": f"formal/{scene_id}/{name}.json",
                "sha256": sha256(Path(runs[name]["_sourcePath"])),
            }
            for name in RUN_NAMES
        },
    }


def resolve_capture(experiment_dir: Path, raw_path: Any, label: str) -> Path:
    if not isinstance(raw_path, str) or not raw_path:
        raise ReportError(f"{label} 缺少 capturePath")
    candidate = Path(raw_path)
    if candidate.is_absolute() and candidate.is_file():
        return candidate.resolve()
    search_roots = [experiment_dir, *experiment_dir.parents, Path.cwd()]
    for root in search_roots:
        resolved = (root / candidate).resolve()
        if resolved.is_file():
            return resolved
    raise ReportError(f"{label} 的截图不存在：{raw_path}")


def make_capture_comparisons(
    experiment_dir: Path,
    output_dir: Path,
    scene_id: str,
    runs: dict[str, dict[str, Any]],
    expected_resolution: list[int],
) -> list[dict[str, Any]]:
    captures_dir = output_dir / "captures"
    captures_dir.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    for index in range(1, 4):
        pair = f"pair{index}"
        before = resolve_capture(
            experiment_dir,
            runs[f"A{index}"].get("capturePath"),
            f"{scene_id}/A{index}",
        )
        after = resolve_capture(
            experiment_dir,
            runs[f"B{index}"].get("capturePath"),
            f"{scene_id}/B{index}",
        )
        metrics_path = captures_dir / f"{scene_id}-{pair}-metrics.json"
        difference_path = captures_dir / f"{scene_id}-{pair}-difference.png"
        comparison_path = captures_dir / f"{scene_id}-{pair}-comparison.png"
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                compare_images(
                    before,
                    after,
                    metrics_path,
                    difference_path,
                    comparison_path,
                    "A Before / No Cache",
                    "B After / Per-Light Cache",
                )
        except (OSError, ValueError) as exc:
            raise ReportError(f"{scene_id} 第 {index} 组截图比较失败：{exc}") from exc
        raw_metrics = read_json(metrics_path)
        require_equal(
            raw_metrics.get("schemaVersion"),
            1,
            f"{scene_id} 第 {index} 组截图 metrics.schemaVersion",
        )
        require_equal(
            raw_metrics.get("resolution"),
            expected_resolution,
            f"{scene_id} 第 {index} 组截图分辨率",
        )
        pixel_count = expected_resolution[0] * expected_resolution[1]
        maximum_channel_delta = integer(
            raw_metrics.get("maximumChannelDelta"),
            f"{scene_id} 第 {index} 组 maximumChannelDelta",
        )
        exact_changed_pixel_count = integer(
            raw_metrics.get("exactChangedPixelCount"),
            f"{scene_id} 第 {index} 组 exactChangedPixelCount",
        )
        if not 0 <= maximum_channel_delta <= 255:
            raise ReportError(
                f"{scene_id} 第 {index} 组 maximumChannelDelta 超出 [0, 255]"
            )
        if not 0 <= exact_changed_pixel_count <= pixel_count:
            raise ReportError(
                f"{scene_id} 第 {index} 组 exactChangedPixelCount 超出图像像素数"
            )
        numeric_metrics = {
            key: number(raw_metrics.get(key), f"{scene_id} 第 {index} 组 {key}")
            for key in (
                "rgbMeanAbsoluteDifference",
                "rgbRootMeanSquareDifference",
                "exactChangedPixelRatio",
                "changedPixelRatio",
                "luminanceMeanAbsoluteDifference",
            )
        }
        if any(not 0.0 <= value <= 1.0 for value in numeric_metrics.values()):
            raise ReportError(f"{scene_id} 第 {index} 组截图差异比率超出 [0, 1]")
        exact = (
            exact_changed_pixel_count == 0
            and maximum_channel_delta == 0
            and numeric_metrics["rgbRootMeanSquareDifference"] == 0.0
        )
        within_tolerance = (
            maximum_channel_delta <= MAX_CAPTURE_CHANNEL_DELTA
            and exact_changed_pixel_count <= MAX_CAPTURE_CHANGED_PIXELS
        )
        results.append(
            {
                "pair": index,
                "source": {
                    "A": str(before),
                    "B": str(after),
                    "sha256A": sha256(before),
                    "sha256B": sha256(after),
                },
                "resolution": raw_metrics.get("resolution"),
                **numeric_metrics,
                "maximumChannelDelta": maximum_channel_delta,
                "exactChangedPixelCount": exact_changed_pixel_count,
                "exact": exact,
                "withinTolerance": within_tolerance,
                "artifacts": {
                    "metrics": metrics_path.relative_to(output_dir).as_posix(),
                    "difference": difference_path.relative_to(output_dir).as_posix(),
                    "comparison": comparison_path.relative_to(output_dir).as_posix(),
                },
            }
        )
    return results


def add_bar_labels(axis: Any, bars: Any, decimals: int = 3) -> None:
    for bar in bars:
        height = float(bar.get_height())
        axis.annotate(
            f"{height:.{decimals}f}",
            xy=(bar.get_x() + bar.get_width() / 2, height),
            xytext=(0, 4),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=8,
        )


def plot_performance(
    scene: dict[str, Any],
    output_dir: Path,
    variant_labels: dict[str, str],
) -> str:
    labels = [descriptor["chartLabel"] for descriptor in PERFORMANCE_METRICS]
    x_positions = list(range(len(labels)))
    width = 0.34
    statistic_labels = {
        "median": "Median",
        "p95": "P95",
        "p99": "P99",
    }
    figure, axes = plt.subplots(1, len(STATISTICS), figsize=(20, 6.8))
    if len(STATISTICS) == 1:
        axes = [axes]
    for axis, statistic in zip(axes, STATISTICS):
        a_values = [
            scene["metrics"][descriptor["key"]][statistic]["A"]["mean"]
            for descriptor in PERFORMANCE_METRICS
        ]
        b_values = [
            scene["metrics"][descriptor["key"]][statistic]["B"]["mean"]
            for descriptor in PERFORMANCE_METRICS
        ]
        bars_a = axis.bar(
            [value - width / 2 for value in x_positions],
            a_values,
            width,
            color="#64748b",
            label=f"A 优化前（{variant_labels['A']}）",
        )
        bars_b = axis.bar(
            [value + width / 2 for value in x_positions],
            b_values,
            width,
            color="#0ea5a4",
            label=f"B 优化后（{variant_labels['B']}）",
        )
        all_run_values: list[float] = []
        for metric_index, descriptor in enumerate(PERFORMANCE_METRICS):
            metric = scene["metrics"][descriptor["key"]][statistic]
            all_run_values.extend(metric["A"]["values"])
            all_run_values.extend(metric["B"]["values"])
            axis.scatter(
                [
                    metric_index - width / 2 - 0.045,
                    metric_index - width / 2,
                    metric_index - width / 2 + 0.045,
                ],
                metric["A"]["values"],
                s=18,
                color="#111827",
                zorder=5,
            )
            axis.scatter(
                [
                    metric_index + width / 2 - 0.045,
                    metric_index + width / 2,
                    metric_index + width / 2 + 0.045,
                ],
                metric["B"]["values"],
                s=18,
                color="#064e3b",
                zorder=5,
            )
            change = metric["deltaPercent"]
            if change is not None:
                prefix = "↓" if change < 0 else "↑"
                axis.annotate(
                    f"{prefix}{abs(change):.1f}%",
                    xy=(
                        metric_index,
                        max(a_values[metric_index], b_values[metric_index]),
                    ),
                    xytext=(0, 22),
                    textcoords="offset points",
                    ha="center",
                    fontsize=8.5,
                    color="#065f46" if change <= 0 else "#b91c1c",
                    fontweight="bold",
                )
        add_bar_labels(axis, bars_a)
        add_bar_labels(axis, bars_b)
        axis.set_title(statistic_labels[statistic], fontsize=13, fontweight="bold")
        axis.set_ylabel("耗时（ms，越低越好）")
        axis.set_xticks(x_positions, labels, rotation=12)
        axis.grid(axis="y", linestyle="--", alpha=0.28)
        maximum = max(a_values + b_values + all_run_values + [0.001])
        axis.set_ylim(0, maximum * 1.28)
    axes[0].legend(loc="upper left", fontsize=8)
    figure.suptitle(
        f"{scene['displayName']}：Per-Light 阴影缓存 Median / P95 / P99 A/B 对比",
        fontsize=15,
        fontweight="bold",
    )
    figure.text(
        0.995,
        0.01,
        "柱：三个运行制品统计量的算术平均；点：每个运行制品的统计量",
        ha="right",
        va="bottom",
        fontsize=8.5,
        color="#475569",
    )
    figure.tight_layout(rect=(0, 0.035, 1, 0.94))
    path = output_dir / f"performance-{scene['id']}.png"
    figure.savefig(path, dpi=180, bbox_inches="tight")
    plt.close(figure)
    return path.relative_to(output_dir).as_posix()


def plot_updated_lights(
    scenes: list[dict[str, Any]],
    output_dir: Path,
    variant_labels: dict[str, str],
) -> str:
    x_positions = list(range(len(scenes)))
    width = 0.34
    a_values = [
        scene["updates"]["updatedLightsPerFrame"]["A"]["mean"] for scene in scenes
    ]
    b_values = [
        scene["updates"]["updatedLightsPerFrame"]["B"]["mean"] for scene in scenes
    ]
    figure, axis = plt.subplots(figsize=(9.5, 5.8))
    bars_a = axis.bar(
        [value - width / 2 for value in x_positions],
        a_values,
        width,
        color="#64748b",
        label=f"A 优化前（{variant_labels['A']}）",
    )
    bars_b = axis.bar(
        [value + width / 2 for value in x_positions],
        b_values,
        width,
        color="#0ea5a4",
        label=f"B 优化后（{variant_labels['B']}）",
    )
    for scene_index, scene in enumerate(scenes):
        metric = scene["updates"]["updatedLightsPerFrame"]
        axis.scatter(
            [scene_index - width / 2 - 0.035, scene_index - width / 2, scene_index - width / 2 + 0.035],
            metric["A"]["values"],
            s=22,
            color="#111827",
            zorder=5,
        )
        axis.scatter(
            [scene_index + width / 2 - 0.035, scene_index + width / 2, scene_index + width / 2 + 0.035],
            metric["B"]["values"],
            s=22,
            color="#064e3b",
            zorder=5,
        )
    add_bar_labels(axis, bars_a, decimals=1)
    add_bar_labels(axis, bars_b, decimals=1)
    axis.set_title(
        "单点光源移动时的阴影灯更新数：3 → 1",
        fontsize=15,
        fontweight="bold",
        pad=16,
    )
    axis.set_ylabel("每帧更新的阴影灯数量（越低越好）")
    axis.set_xticks(x_positions, [scene["displayName"] for scene in scenes])
    axis.set_ylim(0, max(a_values + b_values + [1.0]) * 1.28)
    axis.grid(axis="y", linestyle="--", alpha=0.28)
    axis.legend(loc="upper right")
    figure.tight_layout()
    path = output_dir / "updated-lights-per-frame.png"
    figure.savefig(path, dpi=180, bbox_inches="tight")
    plt.close(figure)
    return path.relative_to(output_dir).as_posix()


def plot_resources(
    scenes: list[dict[str, Any]],
    output_dir: Path,
    variant_labels: dict[str, str],
) -> str:
    figure, axes = plt.subplots(1, len(RESOURCE_FIELDS), figsize=(15, 5.2))
    if len(RESOURCE_FIELDS) == 1:
        axes = [axes]
    x_positions = list(range(len(scenes)))
    width = 0.34
    for axis, (key, label, _) in zip(axes, RESOURCE_FIELDS):
        a_values = [scene["resources"][key]["A"]["meanMiB"] for scene in scenes]
        b_values = [scene["resources"][key]["B"]["meanMiB"] for scene in scenes]
        bars_a = axis.bar(
            [value - width / 2 for value in x_positions],
            a_values,
            width,
            color="#64748b",
            label=f"A 优化前（{variant_labels['A']}）",
        )
        bars_b = axis.bar(
            [value + width / 2 for value in x_positions],
            b_values,
            width,
            color="#0ea5a4",
            label=f"B 优化后（{variant_labels['B']}）",
        )
        add_bar_labels(axis, bars_a, decimals=2)
        add_bar_labels(axis, bars_b, decimals=2)
        axis.set_title(label, fontweight="bold")
        axis.set_ylabel("Renderer-owned 资源（MiB）")
        axis.set_xticks(
            x_positions,
            [scene["displayName"] for scene in scenes],
            rotation=12,
        )
        axis.set_ylim(0, max(a_values + b_values + [1.0]) * 1.12)
        axis.grid(axis="y", linestyle="--", alpha=0.28)
    axes[0].legend(loc="upper left", fontsize=8)
    figure.suptitle(
        "Renderer-owned 资源占用 A/B 对比",
        fontsize=15,
        fontweight="bold",
    )
    figure.tight_layout(rect=(0, 0, 1, 0.94))
    path = output_dir / "renderer-owned-resources.png"
    figure.savefig(path, dpi=180, bbox_inches="tight")
    plt.close(figure)
    return path.relative_to(output_dir).as_posix()


def format_ms(value: float) -> str:
    return f"{value:.4f}"


def format_triplet(values: list[float], suffix: str = "") -> str:
    return " / ".join(f"{value:.4f}{suffix}" for value in values)


def format_delta(value: float | None) -> str:
    if value is None:
        return "N/A"
    return f"{value:+.2f}%"


def bool_mark(value: bool) -> str:
    return "通过" if value else "未通过"


def require_close(
    actual: Any,
    expected: float,
    label: str,
    tolerance: float = 1.0e-6,
) -> None:
    actual_value = number(actual, label)
    if not math.isclose(actual_value, expected, rel_tol=0.0, abs_tol=tolerance):
        raise ReportError(
            f"{label} 与原始运行重算值不一致：summary={actual_value!r}，"
            f"重算={expected!r}"
        )


def require_number_list_close(
    actual: Any,
    expected: list[float],
    label: str,
    tolerance: float = 1.0e-9,
) -> None:
    if not isinstance(actual, list) or len(actual) != len(expected):
        raise ReportError(
            f"{label} 应包含 {len(expected)} 个值"
        )
    for index, expected_value in enumerate(expected):
        require_close(
            actual[index],
            expected_value,
            f"{label}[{index}]",
            tolerance,
        )


def cross_check_summary(
    summary: dict[str, Any],
    scenes: list[dict[str, Any]],
    image_comparison_enabled: bool,
) -> bool:
    expected_definition = {
        "formalProcessesPerVariant": 3,
        "formalProcessLabels": {
            "A": ["A1", "A2", "A3"],
            "B": ["B1", "B2", "B3"],
        },
        "processStatisticAggregation": "arithmetic-mean",
        "processValuesRetained": True,
        "percentiles": list(STATISTICS),
        "percentileMethod": "linear-interpolation-q-times-n-minus-one",
    }
    require_equal(
        summary.get("statisticsDefinition"),
        expected_definition,
        "summary.statisticsDefinition",
    )
    raw_scenes = summary.get("scenes")
    if not isinstance(raw_scenes, list):
        raise ReportError("summary.scenes 必须是数组")
    summary_scenes = {
        item.get("id"): item
        for item in raw_scenes
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    expected_scene_ids = [scene["id"] for scene in scenes]
    if len(summary_scenes) != len(raw_scenes) or set(summary_scenes) != set(
        expected_scene_ids
    ):
        raise ReportError("summary.scenes 与正式运行场景集合不一致")

    update_summary_keys = {
        "updatedLightsPerFrame": "updatedLightsPerFrame",
        "directionalUpdatesPerFrame": "directionalLightUpdatesPerFrame",
        "pointUpdatesPerFrame": "pointLightUpdatesPerFrame",
        "spotUpdatesPerFrame": "spotLightUpdatesPerFrame",
    }
    expected_face_names = ("+X", "-X", "+Y", "-Y", "+Z", "-Z")

    for scene in scenes:
        scene_id = scene["id"]
        summary_scene = summary_scenes[scene_id]
        require_equal(
            summary_scene.get("displayName"),
            scene["displayName"],
            f"summary.scenes[{scene_id}].displayName",
        )
        for descriptor in PERFORMANCE_METRICS:
            for statistic in STATISTICS:
                label = (
                    f"summary.scenes[{scene_id}].perProcessStatistics."
                    f"{descriptor['summaryKey']}.{statistic}"
                )
                summary_metric = nested(
                    summary_scene,
                    (
                        "perProcessStatistics",
                        descriptor["summaryKey"],
                        statistic,
                    ),
                    f"summary.scenes[{scene_id}]",
                )
                if not isinstance(summary_metric, dict):
                    raise ReportError(f"{label} 必须是对象")
                rebuilt = scene["metrics"][descriptor["key"]][statistic]
                for variant in VARIANTS:
                    require_number_list_close(
                        summary_metric.get(f"{variant}Values"),
                        rebuilt[variant]["values"],
                        f"{label}.{variant}Values",
                    )
                    require_close(
                        summary_metric.get(variant),
                        rebuilt[variant]["mean"],
                        f"{label}.{variant}",
                    )
                require_close(
                    summary_metric.get("delta"),
                    rebuilt["B"]["mean"] - rebuilt["A"]["mean"],
                    f"{label}.delta",
                    2.0e-6,
                )
                if rebuilt["deltaPercent"] is None:
                    require_equal(
                        summary_metric.get("deltaPercent"),
                        None,
                        f"{label}.deltaPercent",
                    )
                else:
                    require_close(
                        summary_metric.get("deltaPercent"),
                        rebuilt["deltaPercent"],
                        f"{label}.deltaPercent",
                        1.0e-3,
                    )

        for update_key, summary_key in update_summary_keys.items():
            summary_update = nested(
                summary_scene,
                ("average", summary_key),
                f"summary.scenes[{scene_id}]",
            )
            if not isinstance(summary_update, dict):
                raise ReportError(
                    f"summary.scenes[{scene_id}].average.{summary_key} 必须是对象"
                )
            for variant in VARIANTS:
                require_close(
                    summary_update.get(variant),
                    scene["updates"][update_key][variant]["mean"],
                    (
                        f"summary.scenes[{scene_id}].average."
                        f"{summary_key}.{variant}"
                    ),
                )

        correctness = summary_scene.get("correctness")
        if not isinstance(correctness, dict):
            raise ReportError(f"summary.scenes[{scene_id}].correctness 必须是对象")
        require_equal(
            correctness.get("pixelThreshold"),
            {
                "maximumChannelDelta": MAX_CAPTURE_CHANNEL_DELTA,
                "maximumChangedPixels": MAX_CAPTURE_CHANGED_PIXELS,
            },
            f"summary.scenes[{scene_id}].correctness.pixelThreshold",
        )

        if image_comparison_enabled:
            summary_captures = correctness.get("captureComparisons")
            if not isinstance(summary_captures, list) or len(summary_captures) != 3:
                raise ReportError(
                    f"summary.scenes[{scene_id}].correctness.captureComparisons "
                    "必须包含三组"
                )
            for rebuilt, recorded in zip(scene["captures"], summary_captures):
                label = (
                    f"summary.scenes[{scene_id}].correctness.captureComparisons"
                    f"[{rebuilt['pair'] - 1}]"
                )
                if not isinstance(recorded, dict):
                    raise ReportError(f"{label} 必须是对象")
                require_equal(recorded.get("pair"), rebuilt["pair"], f"{label}.pair")
                require_equal(
                    recorded.get("resolution"),
                    rebuilt["resolution"],
                    f"{label}.resolution",
                )
                require_equal(recorded.get("exact"), rebuilt["exact"], f"{label}.exact")
                require_equal(
                    recorded.get("maximumChannelDelta"),
                    rebuilt["maximumChannelDelta"],
                    f"{label}.maximumChannelDelta",
                )
                require_equal(
                    recorded.get("changedPixelCount"),
                    rebuilt["exactChangedPixelCount"],
                    f"{label}.changedPixelCount",
                )
                require_equal(
                    recorded.get("withinTolerance"),
                    rebuilt["withinTolerance"],
                    f"{label}.withinTolerance",
                )
                require_close(
                    recorded.get("normalizedMeanAbsoluteDifference"),
                    rebuilt["rgbMeanAbsoluteDifference"],
                    f"{label}.normalizedMeanAbsoluteDifference",
                    1.0e-12,
                )

        summary_cubes = correctness.get("pointShadowCubeComparisons")
        if not isinstance(summary_cubes, list) or len(summary_cubes) != 3:
            raise ReportError(
                f"summary.scenes[{scene_id}].correctness."
                "pointShadowCubeComparisons 必须包含三组"
            )
        for pair_index, (rebuilt_pair, recorded_pair) in enumerate(
            zip(scene["pointShadowCube"]["paired"], summary_cubes),
            start=1,
        ):
            label = (
                f"summary.scenes[{scene_id}].correctness."
                f"pointShadowCubeComparisons[{pair_index - 1}]"
            )
            if not isinstance(recorded_pair, dict):
                raise ReportError(f"{label} 必须是对象")
            require_equal(recorded_pair.get("pair"), pair_index, f"{label}.pair")
            require_equal(
                recorded_pair.get("resolution"),
                rebuilt_pair["resolution"],
                f"{label}.resolution",
            )
            require_equal(
                recorded_pair.get("exact"),
                rebuilt_pair["allSixFaceHashesExact"],
                f"{label}.exact",
            )
            faces = recorded_pair.get("faces")
            if not isinstance(faces, list) or len(faces) != 6:
                raise ReportError(f"{label}.faces 必须包含六个面")
            before = scene["pointShadowCube"]["runs"][f"A{pair_index}"]
            after = scene["pointShadowCube"]["runs"][f"B{pair_index}"]
            for face_index, face in enumerate(faces):
                face_label = f"{label}.faces[{face_index}]"
                if not isinstance(face, dict):
                    raise ReportError(f"{face_label} 必须是对象")
                expected_face = {
                    "index": face_index,
                    "name": expected_face_names[face_index],
                    "hashA": before["hashes"][face_index],
                    "hashB": after["hashes"][face_index],
                    "nonFarSamplesA": before["nonFarSampleCounts"][face_index],
                    "nonFarSamplesB": after["nonFarSampleCounts"][face_index],
                    "exact": (
                        before["hashes"][face_index] == after["hashes"][face_index]
                        and before["nonFarSampleCounts"][face_index]
                        == after["nonFarSampleCounts"][face_index]
                    ),
                }
                require_equal(face, expected_face, face_label)

        summary_resources = correctness.get("rendererOwnedResources")
        if not isinstance(summary_resources, dict):
            raise ReportError(
                f"summary.scenes[{scene_id}].correctness."
                "rendererOwnedResources 必须是对象"
            )
        for resource_key, _, summary_key in RESOURCE_FIELDS:
            recorded = summary_resources.get(summary_key)
            if not isinstance(recorded, dict):
                raise ReportError(
                    f"summary.scenes[{scene_id}].correctness."
                    f"rendererOwnedResources.{summary_key} 必须是对象"
                )
            rebuilt = scene["resources"][resource_key]
            require_equal(
                recorded.get("values"),
                rebuilt["A"]["valuesBytes"] + rebuilt["B"]["valuesBytes"],
                (
                    f"summary.scenes[{scene_id}].correctness."
                    f"rendererOwnedResources.{summary_key}.values"
                ),
            )
            require_equal(
                recorded.get("exact"),
                rebuilt["allSixRunsExact"],
                (
                    f"summary.scenes[{scene_id}].correctness."
                    f"rendererOwnedResources.{summary_key}.exact"
                ),
            )
    return True


def build_checks(
    metadata: dict[str, Any],
    scenes: list[dict[str, Any]],
    image_comparison_enabled: bool,
    summary_cross_checked: bool,
) -> dict[str, Any]:
    resolution = metadata.get("resolution")
    configuration = str(metadata.get("configuration", "")).lower()
    executables = metadata.get("executables")
    executable_hashes_match = False
    if isinstance(executables, dict):
        a_executable = executables.get("A")
        b_executable = executables.get("B")
        if isinstance(a_executable, dict) and isinstance(b_executable, dict):
            a_hash = a_executable.get("sha256")
            b_hash = b_executable.get("sha256")
            executable_hashes_match = (
                isinstance(a_hash, str) and bool(a_hash) and a_hash == b_hash
            )
    input_checks = {
        "metadataContractValidated": True,
        "resolution1920x1080": resolution == [1920, 1080],
        "runResolutionMatchesMetadata": all(
            run_resolution == resolution
            for scene in scenes
            for values in scene["runConditions"]["resolution"].values()
            for run_resolution in values
        ),
        "releaseX64": "release" in configuration and "x64" in configuration,
        "sixHarnessInvocationArtifactsComplete": all(
            set(scene["runFiles"]) == set(RUN_NAMES)
            and len(scene["runFiles"]) == len(RUN_NAMES)
            for scene in scenes
        ),
        "runSchemaSuccessAndSamplesValidated": True,
        "measuredFrames1000": (
            metadata.get("measuredFrames") == EXPECTED_MEASURED_FRAMES
        ),
        "runFramesMatchMetadata": all(
            measured_frames == metadata.get("measuredFrames")
            for scene in scenes
            for values in scene["runConditions"]["measuredFrames"].values()
            for measured_frames in values
        ),
        "gpuSynchronizedTiming": all(
            measurement == "gpu-synchronized-wall"
            for scene in scenes
            for values in scene["runConditions"]["frameMeasurement"].values()
            for measurement in values
        ),
        "externalWarmup100Performed": (
            metadata.get("externalWarmupFrames")
            == EXPECTED_EXTERNAL_WARMUP_FRAMES
            and metadata.get("externalWarmupPerformed") is True
        ),
        "internalWarmup15": (
            metadata.get("internalWarmupFrames")
            == EXPECTED_INTERNAL_WARMUP_FRAMES
        ),
        "sameExecutableForAB": executable_hashes_match,
        "abOrder": metadata.get("order") == list(EXPECTED_ORDER),
        "requiredScenesOnly": (
            {scene["id"] for scene in scenes} == {"sponza", "san-miguel"}
            and len(scenes) == 2
        ),
        "summaryKeyStatisticsCrossChecked": summary_cross_checked,
    }
    evidence_checks = {
        "updatesThreeToOne": all(
            scene["updates"]["updatedLightsPerFrame"]["A"]["values"]
            == [3.0, 3.0, 3.0]
            and scene["updates"]["updatedLightsPerFrame"]["B"]["values"]
            == [1.0, 1.0, 1.0]
            for scene in scenes
        ),
        "variantAUpdatesDirectionalPointSpot": all(
            scene["updates"]["directionalUpdatesPerFrame"]["A"]["values"]
            == [1.0, 1.0, 1.0]
            and scene["updates"]["pointUpdatesPerFrame"]["A"]["values"]
            == [1.0, 1.0, 1.0]
            and scene["updates"]["spotUpdatesPerFrame"]["A"]["values"]
            == [1.0, 1.0, 1.0]
            for scene in scenes
        ),
        "variantBOnlyUpdatesPoint": all(
            scene["updates"]["directionalUpdatesPerFrame"]["B"]["values"]
            == [0.0, 0.0, 0.0]
            and scene["updates"]["pointUpdatesPerFrame"]["B"]["values"]
            == [1.0, 1.0, 1.0]
            and scene["updates"]["spotUpdatesPerFrame"]["B"]["values"]
            == [0.0, 0.0, 0.0]
            for scene in scenes
        ),
        "allPairedShadowGpuStatisticsImprove": all(
            value is not None and value < 0
            for scene in scenes
            for statistic in STATISTICS
            for value in scene["metrics"]["shadowGpu"][statistic][
                "pairedDeltaPercent"
            ]
        ),
        "rendererOwnedResourcesExact": all(
            resource["allSixRunsExact"]
            for scene in scenes
            for resource in scene["resources"].values()
        ),
        "pointUpdateAccounting": all(
            scene["pointShadowPath"]["allUpdateAccountingValid"]
            for scene in scenes
        ),
        "pointSubmissionAccounting": all(
            scene["pointShadowPath"]["allSubmissionAccountingValid"]
            for scene in scenes
        ),
        "pointCubeAllFacesValid": all(
            scene["pointShadowCube"]["allFacesValid"]
            for scene in scenes
        ),
        "pointCubeResolutionMatchesMetadata": all(
            run["resolutionMatchesMetadata"]
            for scene in scenes
            for run in scene["pointShadowCube"]["runs"].values()
        ),
        "pointCubeAllPairedContentsExact": all(
            scene["pointShadowCube"]["allPairedContentsExact"]
            for scene in scenes
        ),
        "noShadowResourceFailures": all(
            scene["cacheSafety"]["noResourceFailures"]
            for scene in scenes
        ),
        "noConservativeFallbacks": all(
            scene["cacheSafety"]["noConservativeFallbacks"]
            for scene in scenes
        ),
        "noUnexpectedEmptyClears": all(
            scene["cacheSafety"]["noUnexpectedEmptyClears"]
            for scene in scenes
        ),
        "imageComparisonPerformed": image_comparison_enabled,
        "allCapturePairsWithinPixelTolerance": (
            image_comparison_enabled
            and all(
                capture["withinTolerance"]
                for scene in scenes
                for capture in scene.get("captures", [])
            )
        ),
    }
    return {
        "inputChecks": input_checks,
        "evidenceChecks": evidence_checks,
        "formalInputEligible": all(input_checks.values()),
        "allAutomatedChecksPass": all(input_checks.values())
        and all(evidence_checks.values()),
    }


def markdown_report(result: dict[str, Any]) -> str:
    experiment = result["experiment"]
    scenes = result["scenes"]
    checks = result["checks"]
    artifacts = result["artifacts"]
    variant_labels = experiment["variantLabels"]
    statistic_labels = {
        "median": "Median",
        "p95": "P95",
        "p99": "P99",
    }
    resolution = experiment.get("resolution")
    resolution_text = (
        f"{resolution[0]}×{resolution[1]}"
        if isinstance(resolution, list) and len(resolution) == 2
        else str(resolution)
    )

    lines = [
        "# Per-Light 阴影缓存 A/B 实验报告",
        "",
        "> 本文档由每个场景六个正式调用制品（A1–A3、B1–B3）的原始 JSON 自动生成。实验脚本按轮启动新的可执行程序；但 JSON 未记录 PID，因此本报告只能验证六个调用制品完整，不能仅凭 JSON 独立证明进程身份。柱为三个运行制品统计量的算术平均，点为各制品结果；性能百分比均以 A 为基准，负值表示耗时下降。",
        "",
        "> **比较边界：** A 是无缓存、每帧重绘全部启用阴影灯的控制路径，B 是当前 Per-Light Revision Cache。两者使用同一可执行文件、相同 Shader/FBO/分辨率和 Six-face 点阴影，仅改变缓存策略。",
        "",
    ]
    if checks["allAutomatedChecksPass"]:
        lines.extend(
            [
                "> **结论状态：** 自动化输入、更新数、截图容差（变化像素 ≤32；Exact 状态另列）、点阴影更新/提交核算与 Renderer-owned 资源检查均通过。",
                "",
            ]
        )
    elif not checks["formalInputEligible"]:
        lines.extend(
            [
                f"> **数据资格警告：** 当前实验为 {resolution_text}，或其他正式输入条件不完整；本报告只能作为预验证证据，不能把其中的性能百分比直接写入简历。",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "> **验收警告：** 正式输入条件满足，但至少一项正确性或稳定性自动检查未通过；修复并重跑前不得发布性能结论。",
                "",
            ]
        )

    lines.extend(
        [
            "## 实验条件",
            "",
            "| 项目 | 取值 |",
            "|---|---|",
            f"| 实验 ID | `{experiment['id']}` |",
            f"| 配置 | {experiment.get('configuration')} |",
            f"| 分辨率 | {resolution_text} |",
            f"| A（优化前） | `{variant_labels['A']}` |",
            f"| B（优化后） | `{variant_labels['B']}` |",
            f"| 正式进程 | A × 3，B × 3 |",
            f"| 外部预热 | {experiment.get('externalWarmupFrames')} 帧，已执行 |",
            f"| 进程内预热 | {experiment.get('internalWarmupFrames')} 帧 |",
            f"| 每进程测量帧数 | {experiment.get('measuredFrames')} |",
            f"| 运行顺序 | {' → '.join(experiment.get('order') or [])} |",
            f"| 工作负载 | `{experiment.get('workload')}` |",
            f"| 阴影模式 / 渲染路径 | `{experiment.get('shadowMode')}` / `{experiment.get('renderPath')}` |",
            "",
            "## 性能参数对比",
            "",
        ]
    )
    for scene in scenes:
        lines.extend(
            [
                f"### {scene['displayName']}",
                "",
                f"![{scene['displayName']} 性能参数 A/B 对比]({scene['artifacts']['performanceChart']})",
                "",
                "| 参数 | 统计量 | A 三组（ms） | A 均值（ms） | B 三组（ms） | B 均值（ms） | B 相对 A |",
                "|---|---|---:|---:|---:|---:|---:|",
            ]
        )
        for descriptor in PERFORMANCE_METRICS:
            for statistic in STATISTICS:
                metric = scene["metrics"][descriptor["key"]][statistic]
                lines.append(
                    f"| {metric['label']} | {statistic_labels[statistic]} | "
                    f"{format_triplet(metric['A']['values'])} | "
                    f"{format_ms(metric['A']['mean'])} | "
                    f"{format_triplet(metric['B']['values'])} | "
                    f"{format_ms(metric['B']['mean'])} | "
                    f"{format_delta(metric['deltaPercent'])} |"
                )
        lines.extend(
            [
                "",
                "阴影更新 GPU 的三组配对变化："
                + "；".join(
                    f"{statistic_labels[statistic]} "
                    + " / ".join(
                        format_delta(value)
                        for value in scene["metrics"]["shadowGpu"][statistic][
                            "pairedDeltaPercent"
                        ]
                    )
                    for statistic in STATISTICS
                )
                + "。三种统计量的每一组配对都必须改善。",
                "",
            ]
        )

    lines.extend(
        [
            "",
            "## 阴影增量更新结果",
            "",
            f"![每帧阴影灯更新数 A/B 对比]({artifacts['updatedLightsChart']})",
            "",
            "| 场景 | 版本 | 总更新/帧 | Directional/帧 | Point/帧 | Spot/帧 |",
            "|---|---|---:|---:|---:|---:|",
        ]
    )
    for scene in scenes:
        for variant in VARIANTS:
            lines.append(
                f"| {scene['displayName']} | {variant} | "
                f"{scene['updates']['updatedLightsPerFrame'][variant]['mean']:.1f} | "
                f"{scene['updates']['directionalUpdatesPerFrame'][variant]['mean']:.1f} | "
                f"{scene['updates']['pointUpdatesPerFrame'][variant]['mean']:.1f} | "
                f"{scene['updates']['spotUpdatesPerFrame'][variant]['mean']:.1f} |"
            )
    lines.extend(
        [
            "",
            "单点光源移动时，A 每帧重绘 Directional / Point / Spot，B 仅重绘 Point Shadow；三组运行制品均保持 `3 → 1`。",
            "",
            "### 缓存安全与降级遥测",
            "",
            "| 场景 | 版本 | 资源/Shader 失败 | 保守降级 | 意外空场景清屏 |",
            "|---|---|---:|---:|---:|",
        ]
    )
    for scene in scenes:
        safety = scene["cacheSafety"]["values"]
        for variant in VARIANTS:
            lines.append(
                f"| {scene['displayName']} | {variant} | "
                f"{' / '.join(str(value) for value in safety['resourceFailures'][variant])} | "
                f"{' / '.join(str(value) for value in safety['conservativeFallbacks'][variant])} | "
                f"{' / '.join(str(value) for value in safety['emptyShadowClears'][variant])} |"
            )
    lines.extend(
        [
            "",
            "正式 `move-point` 实验要求上述三项在六轮测量窗口内全部为 0；失效矩阵另行验证空场景清屏与渲染目标替换。",
            "",
            "### 点阴影六面路径核验",
            "",
            "| 场景 | 版本 | 路径策略 | Layered 更新/帧 | Six-face 更新/帧 | 提交 Pass/帧 | 更新核算 | 提交核算 |",
            "|---|---|---|---:|---:|---:|---|---|",
        ]
    )
    for scene in scenes:
        point_path = scene["pointShadowPath"]
        for variant in VARIANTS:
            policies = point_path["renderPolicy"][variant]
            policy = policies[0] if len(set(policies)) == 1 else " / ".join(policies)
            update_valid = all(point_path["updateAccounting"][variant])
            submission_valid = all(point_path["submissionAccounting"][variant])
            lines.append(
                f"| {scene['displayName']} | {variant} | `{policy}` | "
                f"{point_path['layeredUpdatesPerFrame'][variant]['mean']:.1f} | "
                f"{point_path['sixFaceUpdatesPerFrame'][variant]['mean']:.1f} | "
                f"{point_path['submissionPassesPerFrame'][variant]['mean']:.1f} | "
                f"{bool_mark(update_valid)} | {bool_mark(submission_valid)} |"
            )
    lines.extend(
        [
            "",
            "核算规则为：`point updates = layered + six-face`，`submission = layered + 6 × six-face`；两条规则同时闭合，用于证明点光源更新计数与 Cubemap 六面提交没有因缓存优化而漏记或漏画。",
            "",
            "| 场景 | A/B 配对 | 六面逐位 Hash 一致 | A 最小 nonFar 样本 | B 最小 nonFar 样本 |",
            "|---|---:|---|---:|---:|",
        ]
    )
    for scene in scenes:
        for pair in scene["pointShadowCube"]["paired"]:
            lines.append(
                f"| {scene['displayName']} | A{pair['pair']} / B{pair['pair']} | "
                f"{bool_mark(pair['allSixFaceHashesExact'])} | "
                f"{pair['minimumNonFarSamplesA']} | "
                f"{pair['minimumNonFarSamplesB']} |"
            )
    lines.extend(
        [
            "",
            "逐面证据在性能采样结束后读取六个深度面；分辨率必须为正且与本实验有效点阴影分辨率一致，Hash 必须是严格的 64 位十六进制格式，A/B 的 Hash、nonFar 样本数与深度范围逐面一致。某一面 nonFar 为 0 可表示有效清空面，不单独判为漏画；是否执行六面由提交核算共同证明。",
            "",
            "## Renderer-owned 资源占用",
            "",
            f"![Renderer-owned 资源 A/B 对比]({artifacts['resourcesChart']})",
            "",
            "| 场景 | 资源 | A 三轮（bytes） | B 三轮（bytes） | 六轮完全一致 |",
            "|---|---|---:|---:|---|",
        ]
    )
    for scene in scenes:
        for key, _, _ in RESOURCE_FIELDS:
            resource = scene["resources"][key]
            a_bytes = " / ".join(str(value) for value in resource["A"]["valuesBytes"])
            b_bytes = " / ".join(str(value) for value in resource["B"]["valuesBytes"])
            lines.append(
                f"| {scene['displayName']} | {resource['label']} | {a_bytes} | "
                f"{b_bytes} | {bool_mark(resource['allSixRunsExact'])} |"
            )

    lines.extend(
        [
            "",
            "## 画面一致性与截图证据",
            "",
        ]
    )
    if not checks["evidenceChecks"]["imageComparisonPerformed"]:
        lines.extend(
            [
                "> 本次生成使用了 `--skip-image-comparison`，未生成截图证据；正式报告必须去掉该选项后重新生成。",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "| 场景 | 配对 | 分辨率 | 变化像素数 | 最大通道差 | RGB MAD | 容差（≤32 像素） | Exact（零差异） |",
                "|---|---:|---|---:|---:|---:|---|---|",
            ]
        )
        for scene in scenes:
            for capture in scene["captures"]:
                resolution_value = capture.get("resolution")
                capture_resolution = (
                    f"{resolution_value[0]}×{resolution_value[1]}"
                    if isinstance(resolution_value, list)
                    and len(resolution_value) == 2
                    else str(resolution_value)
                )
                lines.append(
                    f"| {scene['displayName']} | A{capture['pair']} / B{capture['pair']} | "
                    f"{capture_resolution} | "
                    f"{capture['exactChangedPixelCount']} | "
                    f"{capture['maximumChannelDelta']}/255 | "
                    f"{capture['rgbMeanAbsoluteDifference']:.8f} | "
                    f"{bool_mark(capture['withinTolerance'])} | "
                    f"{bool_mark(capture['exact'])} |"
                )
        lines.extend(
            [
                "",
                "截图验收预设为：最大通道差 ≤255 且变化像素数 ≤32。容差通过只表示差异落在预设范围内，不等同于 Exact；每组的 Exact、变化像素数和最大通道差均如实列出。",
                "",
            ]
        )
        for scene in scenes:
            representative = scene["captures"][0]["artifacts"]
            lines.extend(
                [
                    f"### {scene['displayName']}：代表性 A1/B1 截图",
                    "",
                    f"![{scene['displayName']} 优化前后截图]({representative['comparison']})",
                    "",
                    f"![{scene['displayName']} 像素差异热力图]({representative['difference']})",
                    "",
                    "并排图用于核对构图、阴影覆盖与点光源位置；差异热力图用于定位任何像素变化。三组配对的数值结果以上表为准。",
                    "",
                ]
            )

    lines.extend(
        [
            "## 自动化验收摘要",
            "",
            "| 检查项 | 结果 |",
            "|---|---|",
        ]
    )
    check_labels = {
        "metadataContractValidated": "metadata 正式实验参数契约通过",
        "resolution1920x1080": "1920×1080",
        "runResolutionMatchesMetadata": "六轮分辨率与 metadata 一致",
        "releaseX64": "Release x64",
        "sixHarnessInvocationArtifactsComplete": "每场景 A1–A3/B1–B3 六个调用制品完整",
        "runSchemaSuccessAndSamplesValidated": "schema 17、成功状态、GPU 计时、样本数与区域计数通过",
        "measuredFrames1000": "每进程 1,000 测量帧",
        "runFramesMatchMetadata": "六轮测量帧数与 metadata 一致",
        "gpuSynchronizedTiming": "六轮均使用 GPU 同步 Wall 计时",
        "externalWarmup100Performed": "外部预热 100 帧且已执行",
        "internalWarmup15": "进程内预热 15 帧",
        "sameExecutableForAB": "A/B 使用同一可执行文件",
        "abOrder": "固定 A/B/B/A/A/B 顺序",
        "requiredScenesOnly": "场景恰为 Sponza 与 San Miguel",
        "summaryKeyStatisticsCrossChecked": "summary 关键统计量与原始六轮数据重算一致",
        "updatesThreeToOne": "每轮更新数 3 → 1",
        "variantAUpdatesDirectionalPointSpot": "A 每帧 Directional/Point/Spot = 1/1/1",
        "variantBOnlyUpdatesPoint": "B 每帧 Directional/Point/Spot = 0/1/0",
        "allPairedShadowGpuStatisticsImprove": "Shadow GPU Median/P95/P99 的所有配对均改善",
        "rendererOwnedResourcesExact": "Renderer-owned 资源逐字节一致",
        "pointUpdateAccounting": "点阴影更新计数核算闭合",
        "pointSubmissionAccounting": "点阴影六面提交核算通过",
        "pointCubeAllFacesValid": "六轮点阴影 Cubemap 的六个面均有效且已写入",
        "pointCubeResolutionMatchesMetadata": "点阴影 Cubemap 分辨率为正且匹配有效配置",
        "pointCubeAllPairedContentsExact": "三组 A/B 点阴影六面内容逐位一致",
        "noShadowResourceFailures": "六轮测量期内无阴影资源或 Shader 失败",
        "noConservativeFallbacks": "六轮测量期内未触发保守降级",
        "noUnexpectedEmptyClears": "move-point 六轮无意外空场景清屏",
        "imageComparisonPerformed": "已执行截图比较",
        "allCapturePairsWithinPixelTolerance": "所有截图配对满足 ≤32 变化像素容差（Exact 另列）",
    }
    for group in ("inputChecks", "evidenceChecks"):
        for key, value in checks[group].items():
            lines.append(f"| {check_labels[key]} | {bool_mark(value)} |")
    lines.extend(
        [
            "",
            f"机器可读数据见 [`{artifacts['json']}`]({artifacts['json']})。",
            "",
        ]
    )
    return "\n".join(lines)


def build_report(experiment_dir: Path, output_dir: Path, skip_images: bool) -> dict[str, Any]:
    metadata_path = experiment_dir / "metadata.json"
    summary_path = experiment_dir / "summary.json"
    metadata = read_json(metadata_path)
    summary = read_json(summary_path)
    experiment_id = metadata.get("experimentId")
    if not isinstance(experiment_id, str) or not experiment_id:
        raise ReportError("metadata.json 缺少 experimentId")
    if summary.get("experimentId") != experiment_id:
        raise ReportError("metadata.json 与 summary.json 的 experimentId 不一致")
    settings, effective_point_shadow_resolution = validate_metadata(
        metadata,
        summary,
    )

    formal = experiment_dir / "formal"
    if not formal.is_dir():
        raise ReportError(f"缺少正式运行目录：{formal}")
    names = display_names(summary)
    run_sets: dict[str, dict[str, dict[str, Any]]] = {}
    scenes: list[dict[str, Any]] = []
    for scene_id in scene_ids(metadata, summary, formal):
        runs = load_runs(formal / scene_id)
        validate_run_set(scene_id, runs, metadata, settings)
        for run_name, run in runs.items():
            run["_sourcePath"] = str((formal / scene_id / f"{run_name}.json").resolve())
        run_sets[scene_id] = runs
        scenes.append(
            summarize_scene(
                scene_id,
                names.get(scene_id, scene_id),
                runs,
                effective_point_shadow_resolution,
            )
        )

    variant_labels = {
        "A": str(settings.get("variantALabel") or "no-cache"),
        "B": str(settings.get("variantBLabel") or "per-light-revision"),
    }
    result: dict[str, Any] = {
        "schemaVersion": 1,
        "experiment": {
            "id": experiment_id,
            "sourceDirectory": str(experiment_dir),
            "metadataSha256": sha256(metadata_path),
            "summarySha256": sha256(summary_path),
            "configuration": metadata.get("configuration"),
            "resolution": metadata.get("resolution"),
            "order": metadata.get("order"),
            "internalWarmupFrames": metadata.get("internalWarmupFrames"),
            "externalWarmupFrames": metadata.get("externalWarmupFrames"),
            "measuredFrames": metadata.get("measuredFrames"),
            "workload": settings.get("workload"),
            "lights": settings.get("lights"),
            "shadowMode": settings.get("mode"),
            "renderPath": settings.get("renderPath"),
            "shadowResolution": settings.get("shadowResolution"),
            "effectivePointShadowResolution": effective_point_shadow_resolution,
            "variantLabels": variant_labels,
            "aggregation": "arithmetic mean of three per-process statistics",
        },
        "scenes": scenes,
        "artifacts": {},
        "generator": {
            "script": Path(__file__).name,
            "matplotlibBackend": matplotlib.get_backend(),
            "chineseFont": configure_chinese_font(),
        },
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    for scene in scenes:
        if not skip_images:
            scene["captures"] = make_capture_comparisons(
                experiment_dir,
                output_dir,
                scene["id"],
                run_sets[scene["id"]],
                metadata["resolution"],
            )
        scene.setdefault("captures", [])
        scene["artifacts"] = {
            "performanceChart": plot_performance(
                scene, output_dir, variant_labels
            )
        }
    result["artifacts"]["updatedLightsChart"] = plot_updated_lights(
        scenes, output_dir, variant_labels
    )
    result["artifacts"]["resourcesChart"] = plot_resources(
        scenes, output_dir, variant_labels
    )
    result["artifacts"]["json"] = "report-data.json"
    result["artifacts"]["markdown"] = "report.md"
    summary_cross_checked = cross_check_summary(
        summary,
        scenes,
        not skip_images,
    )
    result["checks"] = build_checks(
        metadata,
        scenes,
        not skip_images,
        summary_cross_checked,
    )
    return result


def write_report(result: dict[str, Any], output_dir: Path) -> None:
    json_path = output_dir / result["artifacts"]["json"]
    markdown_path = output_dir / result["artifacts"]["markdown"]
    json_path.write_text(
        json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    markdown_path.write_text(markdown_report(result), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(argv)
    experiment_dir = arguments.experiment_dir.resolve()
    output_dir = arguments.output_dir.resolve()
    try:
        result = build_report(
            experiment_dir,
            output_dir,
            arguments.skip_image_comparison,
        )
        write_report(result, output_dir)
    except (ReportError, OSError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2
    print(f"中文报告：{output_dir / 'report.md'}")
    print(f"机器数据：{output_dir / 'report-data.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
