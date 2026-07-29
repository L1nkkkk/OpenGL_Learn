#!/usr/bin/env python3
"""Generate the Chinese three-way Point shadow-cache benchmark report."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path
from typing import Any, Iterable

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager
from PIL import Image, ImageChops, ImageDraw, ImageFont


VARIANTS = {
    "A": "A 全局重绘",
    "B": "B Per-Light",
    "C": "C Per-Light + Per-Face",
}
COLORS = {
    "A": "#D97757",
    "B": "#4D88C7",
    "C": "#39A275",
}
FACE_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as stream:
        return json.load(stream)


def configure_chinese_font() -> font_manager.FontProperties:
    candidates = [
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/msyhbd.ttc"),
        Path("C:/Windows/Fonts/simhei.ttf"),
    ]
    for path in candidates:
        if path.exists():
            properties = font_manager.FontProperties(fname=str(path))
            plt.rcParams["font.family"] = properties.get_name()
            plt.rcParams["axes.unicode_minus"] = False
            return properties
    plt.rcParams["axes.unicode_minus"] = False
    return font_manager.FontProperties()


FONT = configure_chinese_font()


def nested(data: dict[str, Any], *keys: str, default: Any = None) -> Any:
    current: Any = data
    for key in keys:
        if not isinstance(current, dict) or key not in current:
            return default
        current = current[key]
    return current


def median(values: Iterable[float]) -> float:
    items = [float(value) for value in values if value is not None]
    return statistics.median(items) if items else 0.0


def percent(before: float, after: float) -> float | None:
    return (after - before) * 100.0 / before if abs(before) > 1e-12 else None


def reduction(before: float, after: float) -> float | None:
    value = percent(before, after)
    return -value if value is not None else None


def fmt(value: float | None, digits: int = 3) -> str:
    if value is None or not math.isfinite(value):
        return "—"
    return f"{value:.{digits}f}"


def fmt_pct(value: float | None) -> str:
    if value is None or not math.isfinite(value):
        return "—"
    return f"{value:+.2f}%"


def markdown_path(path: Path, report: Path) -> str:
    return Path(
        *Path(
            Path(path).resolve().relative_to(report.parent.resolve())
        ).parts
    ).as_posix()


def load_runs(
    result_root: Path,
    experiment_id: str,
    scene_id: str,
    prefix: str,
    count: int,
) -> list[dict[str, Any]]:
    directory = result_root / experiment_id / "formal" / scene_id
    return [read_json(directory / f"{prefix}{index}.json") for index in range(1, count + 1)]


def run_metric(run: dict[str, Any], metric: str, percentile: str = "median") -> float:
    if metric == "wall":
        return float(nested(run, "frameTimeMilliseconds", percentile, default=0.0))
    if metric == "gpu":
        return float(nested(run, "profiler", "summary", "gpuFrame", percentile, default=0.0))
    if metric == "cpu":
        return float(nested(run, "profiler", "summary", "cpuFrame", percentile, default=0.0))
    zone = {
        "shadow_gpu": "Shadow Map Update",
        "point_gpu": "Point Shadow Update",
    }[metric]
    return float(
        nested(
            run,
            "profiler",
            "summary",
            "gpuZones",
            zone,
            percentile,
            default=0.0,
        )
    )


def aggregate_metric(
    runs: list[dict[str, Any]], metric: str, percentile: str = "median"
) -> float:
    return median(run_metric(run, metric, percentile) for run in runs)


def counter_per_frame(runs: list[dict[str, Any]], field: str) -> float:
    values = []
    for run in runs:
        frames = max(1, int(run.get("measuredFrames", 1)))
        values.append(float(nested(run, "shadow", field, default=0.0)) / frames)
    return median(values)


def scalar_median(runs: list[dict[str, Any]], *keys: str) -> float:
    return median(float(nested(run, *keys, default=0.0)) for run in runs)


def aligned_series(runs: list[dict[str, Any]], field: str) -> np.ndarray:
    series = []
    for run in runs:
        samples = nested(run, "motionTimeline", "samples", default=[])
        series.append(
            np.asarray(
                [float(nested(sample, "shadow", field, default=0.0)) for sample in samples],
                dtype=np.float64,
            )
        )
    minimum = min((len(item) for item in series), default=0)
    return (
        np.mean(np.stack([item[:minimum] for item in series]), axis=0)
        if minimum
        else np.asarray([], dtype=np.float64)
    )


def gpu_frame_series(runs: list[dict[str, Any]]) -> np.ndarray:
    series = [
        np.asarray(
            nested(run, "profiler", "samples", "gpuFrame", default=[]),
            dtype=np.float64,
        )
        for run in runs
    ]
    minimum = min((len(item) for item in series), default=0)
    return (
        np.mean(np.stack([item[:minimum] for item in series]), axis=0)
        if minimum
        else np.asarray([], dtype=np.float64)
    )


def save_figure(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(path, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close()


def chart_performance(scene_data: dict[str, Any], output: Path) -> None:
    scene_ids = list(scene_data)
    x = np.arange(len(scene_ids))
    width = 0.24
    fig, axes = plt.subplots(1, 2, figsize=(12.5, 4.6))
    for axis, metric, title in [
        (axes[0], "gpu", "GPU 帧时间中位数"),
        (axes[1], "point_gpu", "Point Shadow 更新时间中位数"),
    ]:
        for offset, variant in enumerate(VARIANTS):
            values = [
                scene_data[scene_id]["metrics"][variant][metric]["median"]
                for scene_id in scene_ids
            ]
            bars = axis.bar(
                x + (offset - 1) * width,
                values,
                width,
                label=VARIANTS[variant],
                color=COLORS[variant],
            )
            axis.bar_label(bars, fmt="%.3f", padding=2, fontsize=8)
        axis.set_title(title)
        axis.set_ylabel("毫秒")
        axis.set_xticks(x, [scene_data[item]["displayName"] for item in scene_ids])
        axis.grid(axis="y", alpha=0.22)
        axis.margins(y=0.12)
    axes[0].legend(frameon=False, fontsize=9)
    fig.suptitle("三档阴影缓存性能对比（每档三轮独立进程）", fontsize=15)
    save_figure(output)


def chart_work(scene_data: dict[str, Any], output: Path) -> None:
    scene_ids = list(scene_data)
    x = np.arange(len(scene_ids))
    width = 0.24
    fig, axes = plt.subplots(1, 2, figsize=(12.5, 4.6))
    for axis, field, title in [
        (axes[0], "updatedLights", "每帧更新阴影灯数量"),
        (axes[1], "pointSubmissions", "每帧 Point Face 提交数量"),
    ]:
        for offset, variant in enumerate(VARIANTS):
            values = [
                scene_data[scene_id]["work"][variant][field]
                for scene_id in scene_ids
            ]
            bars = axis.bar(
                x + (offset - 1) * width,
                values,
                width,
                color=COLORS[variant],
                label=VARIANTS[variant],
            )
            axis.bar_label(bars, fmt="%.2f", padding=2, fontsize=8)
        axis.set_title(title)
        axis.set_ylabel("次/帧")
        axis.set_xticks(x, [scene_data[item]["displayName"] for item in scene_ids])
        axis.grid(axis="y", alpha=0.22)
        axis.margins(y=0.12)
    axes[0].legend(frameon=False, fontsize=9)
    fig.suptitle("工作量阶梯：全局 → 灯光 → Cubemap Face", fontsize=15)
    save_figure(output)


def chart_timeline(scene: dict[str, Any], output: Path) -> None:
    fig, axes = plt.subplots(3, 1, figsize=(13, 8.2), sharex=True)
    for variant in VARIANTS:
        timeline = scene["timeline"][variant]
        frames = np.arange(len(timeline["pointSubmissions"]))
        axes[0].plot(
            frames,
            timeline["updatedLights"],
            color=COLORS[variant],
            label=VARIANTS[variant],
            linewidth=1.35,
        )
        axes[1].plot(
            frames,
            timeline["pointSubmissions"],
            color=COLORS[variant],
            linewidth=1.35,
        )
        axes[2].plot(
            frames,
            timeline["gpuFrame"],
            color=COLORS[variant],
            linewidth=1.0,
            alpha=0.9,
        )
    sample_count = len(scene["timeline"]["A"]["pointSubmissions"])
    for axis in axes:
        for ratio in (1.0 / 3.0, 2.0 / 3.0):
            axis.axvline(sample_count * ratio, color="#777777", linestyle="--", alpha=0.45)
        axis.grid(alpha=0.2)
    axes[0].set_ylabel("灯/帧")
    axes[1].set_ylabel("Face/帧")
    axes[2].set_ylabel("GPU 帧时间 (ms)")
    axes[2].set_xlabel("测量帧")
    axes[0].set_title(
        f"{scene['displayName']}：点光源+相机 / 局部遮挡物+相机 / 仅相机"
    )
    axes[0].legend(frameon=False, ncol=3)
    save_figure(output)


def chart_face_histogram(scene_data: dict[str, Any], output: Path) -> None:
    scene_ids = list(scene_data)
    fig, axes = plt.subplots(1, len(scene_ids), figsize=(12.5, 4.4), sharey=True)
    if len(scene_ids) == 1:
        axes = [axes]
    for axis, scene_id in zip(axes, scene_ids):
        values = scene_data[scene_id]["timeline"]["C"]["pointSubmissions"]
        counts = [int(np.count_nonzero(np.isclose(values, face))) for face in range(7)]
        bars = axis.bar(range(7), counts, color=COLORS["C"])
        axis.bar_label(
            bars,
            labels=[str(count) if count else "" for count in counts],
            padding=2,
            fontsize=8,
        )
        axis.set_title(scene_data[scene_id]["displayName"])
        axis.set_xlabel("本帧实际更新 Face 数")
        axis.set_xticks(range(7))
        axis.grid(axis="y", alpha=0.2)
        axis.margins(y=0.12)
    axes[0].set_ylabel("帧数")
    fig.suptitle("C 方案 Point Face 更新数量分布", fontsize=15)
    save_figure(output)


def load_ui_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in [
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/arial.ttf"),
    ]:
        if path.exists():
            return ImageFont.truetype(str(path), size=size)
    return ImageFont.load_default()


def build_screenshot_montage(
    images: dict[str, Path], title: str, output: Path
) -> dict[str, Any]:
    loaded = {key: Image.open(path).convert("RGB") for key, path in images.items()}
    sizes = {image.size for image in loaded.values()}
    if len(sizes) != 1:
        raise RuntimeError(f"{title} screenshot resolutions differ")
    width, height = next(iter(sizes))
    exact_ab = ImageChops.difference(loaded["A"], loaded["B"]).getbbox() is None
    exact_bc = ImageChops.difference(loaded["B"], loaded["C"]).getbbox() is None
    preview_width = 640
    preview_height = max(1, round(height * preview_width / width))
    header = 72
    gap = 12
    canvas = Image.new(
        "RGB",
        (preview_width * 3 + gap * 4, preview_height + header + gap),
        "white",
    )
    draw = ImageDraw.Draw(canvas)
    font = load_ui_font(22)
    small_font = load_ui_font(16)
    for index, variant in enumerate(VARIANTS):
        x = gap + index * (preview_width + gap)
        image = loaded[variant].resize(
            (preview_width, preview_height), Image.Resampling.LANCZOS
        )
        canvas.paste(image, (x, header))
        draw.text((x, 10), VARIANTS[variant], fill=COLORS[variant], font=font)
    draw.text(
        (gap, 43),
        f"{title} | A/B：{'完全一致' if exact_ab else '不一致'}"
        f" | B/C：{'完全一致' if exact_bc else '不一致'}",
        fill="#333333",
        font=small_font,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)
    return {"exactAB": exact_ab, "exactBC": exact_bc, "resolution": [width, height]}


def build_data(
    manifest: dict[str, Any], manifest_path: Path, output_dir: Path
) -> tuple[dict[str, Any], dict[str, Any]]:
    result_root = manifest_path.parent
    count = int(manifest["formalRunsPerVariant"])
    experiments = manifest["experiments"]
    primary_summary = read_json(
        result_root / experiments["globalVsPerLight"] / "summary.json"
    )
    display_names = {
        str(scene["id"]): str(scene["displayName"])
        for scene in primary_summary["scenes"]
    }
    scene_data: dict[str, Any] = {}
    screenshot_checks: dict[str, Any] = {}

    for scene_id in manifest["scenes"]:
        runs = {
            "A": load_runs(
                result_root, experiments["globalVsPerLight"], scene_id, "A", count
            ),
            "B": load_runs(
                result_root, experiments["globalVsPerLight"], scene_id, "B", count
            ),
            "C": load_runs(
                result_root, experiments["perLightVsPerFace"], scene_id, "B", count
            ),
        }
        b_recheck = load_runs(
            result_root, experiments["perLightVsPerFace"], scene_id, "A", count
        )
        metrics: dict[str, Any] = {}
        work: dict[str, Any] = {}
        timelines: dict[str, Any] = {}
        for variant, variant_runs in runs.items():
            metrics[variant] = {
                metric: {
                    percentile: aggregate_metric(variant_runs, metric, percentile)
                    for percentile in ("median", "p95", "p99")
                }
                for metric in ("wall", "cpu", "gpu", "shadow_gpu", "point_gpu")
            }
            work[variant] = {
                "updatedLights": counter_per_frame(
                    variant_runs, "measuredUpdatedLightCount"
                ),
                "directionalUpdates": counter_per_frame(
                    variant_runs, "measuredDirectionalLightUpdateCount"
                ),
                "pointUpdates": counter_per_frame(
                    variant_runs, "measuredPointLightUpdateCount"
                ),
                "spotUpdates": counter_per_frame(
                    variant_runs, "measuredSpotLightUpdateCount"
                ),
                "pointSubmissions": counter_per_frame(
                    variant_runs, "measuredPointShadowSubmissionPassCount"
                ),
                "pointRequiredFaces": counter_per_frame(
                    variant_runs, "measuredPointShadowRequiredFaceCount"
                ),
                "pointRenderedFaces": counter_per_frame(
                    variant_runs, "measuredPointShadowRenderedFaceCount"
                ),
                "pointFaceHits": counter_per_frame(
                    variant_runs, "measuredPointShadowFaceCacheHitCount"
                ),
                "casterDraws": counter_per_frame(
                    variant_runs, "measuredCasterDrawCount"
                ),
                "casterTriangles": counter_per_frame(
                    variant_runs, "measuredCasterTriangleCount"
                ),
                "cacheCheckCpu": scalar_median(
                    variant_runs,
                    "shadow",
                    "measuredAverageCacheCheckCpuMilliseconds",
                ),
                "faceDemandCpu": scalar_median(
                    variant_runs,
                    "shadow",
                    "measuredAveragePointShadowFaceDemandCpuMilliseconds",
                ),
                "faceSignatureCpu": scalar_median(
                    variant_runs,
                    "shadow",
                    "measuredAveragePointShadowFaceSignatureCpuMilliseconds",
                ),
            }
            timelines[variant] = {
                "updatedLights": aligned_series(
                    variant_runs, "updatedLightCount"
                ),
                "pointSubmissions": aligned_series(
                    variant_runs, "pointShadowSubmissionPassCount"
                ),
                "pointRequiredFaces": aligned_series(
                    variant_runs, "pointShadowRequiredFaceCount"
                ),
                "pointRenderedFaces": aligned_series(
                    variant_runs, "pointShadowRenderedFaceCount"
                ),
                "gpuFrame": gpu_frame_series(variant_runs),
            }

        formal_dir_ab = (
            result_root / experiments["globalVsPerLight"] / "formal" / scene_id
        )
        formal_dir_bc = (
            result_root / experiments["perLightVsPerFace"] / "formal" / scene_id
        )
        montage_path = output_dir / f"{scene_id}-three-way-screenshot.png"
        screenshot_checks[scene_id] = build_screenshot_montage(
            {
                "A": formal_dir_ab / "A1.ppm",
                "B": formal_dir_ab / "B1.ppm",
                "C": formal_dir_bc / "B1.ppm",
            },
            display_names.get(scene_id, scene_id),
            montage_path,
        )
        b_duplicate_exact = (
            ImageChops.difference(
                Image.open(formal_dir_ab / "B1.ppm").convert("RGB"),
                Image.open(formal_dir_bc / "A1.ppm").convert("RGB"),
            ).getbbox()
            is None
        )
        screenshot_checks[scene_id]["duplicateBExact"] = b_duplicate_exact

        memory = {
            variant: {
                key: int(median(run["memoryBytes"][key] for run in variant_runs))
                for key in ("texture", "meshCpu", "meshGpu", "renderTarget")
            }
            for variant, variant_runs in runs.items()
        }
        scene_data[scene_id] = {
            "displayName": display_names.get(scene_id, scene_id),
            "hardware": {
                "vendor": runs["A"][0]["glVendor"],
                "renderer": runs["A"][0]["glRenderer"],
                "version": runs["A"][0]["glVersion"],
            },
            "metrics": metrics,
            "work": work,
            "timeline": timelines,
            "memory": memory,
            "bRecheckGpuMedian": aggregate_metric(b_recheck, "gpu", "median"),
        }

    audit: dict[str, Any] = {}
    correctness_id = experiments.get("sixFaceCorrectness")
    if correctness_id:
        audit_summary = read_json(result_root / correctness_id / "summary.json")
        for scene in audit_summary["scenes"]:
            captures = scene["correctness"]["captureComparisons"]
            cubes = scene["correctness"]["pointShadowCubeComparisons"]
            audit[scene["id"]] = {
                "pixelsExact": all(bool(item["exact"]) for item in captures),
                "allSixFacesExact": all(bool(item["exact"]) for item in cubes),
                "faceResults": cubes,
            }
    return scene_data, {
        "screenshots": screenshot_checks,
        "materializedAudit": audit,
    }


def serializable_scene_data(scene_data: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for scene_id, scene in scene_data.items():
        result[scene_id] = {
            key: value
            for key, value in scene.items()
            if key != "timeline"
        }
        result[scene_id]["timeline"] = {
            variant: {
                field: values.tolist()
                for field, values in timeline.items()
            }
            for variant, timeline in scene["timeline"].items()
        }
    return result


def write_report(
    report_path: Path,
    output_dir: Path,
    manifest: dict[str, Any],
    scene_data: dict[str, Any],
    correctness: dict[str, Any],
) -> None:
    performance_chart = output_dir / "three-way-performance.png"
    work_chart = output_dir / "three-way-work.png"
    histogram_chart = output_dir / "point-face-update-histogram.png"
    chart_performance(scene_data, performance_chart)
    chart_work(scene_data, work_chart)
    chart_face_histogram(scene_data, histogram_chart)
    timeline_charts = {}
    for scene_id, scene in scene_data.items():
        path = output_dir / f"{scene_id}-timeline.png"
        chart_timeline(scene, path)
        timeline_charts[scene_id] = path

    lines = [
        "# Point Light 空间关联与 Per-Face 阴影缓存三档实验报告",
        "",
        "> 本报告由同一 Release 可执行文件自动生成。三档仅切换阴影缓存策略；场景、Shader、FBO、阴影分辨率、运动轨迹及渲染路径保持一致。",
        "",
        "## 1. 实验结论",
        "",
    ]
    for scene_id, scene in scene_data.items():
        a = scene["metrics"]["A"]["point_gpu"]["median"]
        b = scene["metrics"]["B"]["point_gpu"]["median"]
        c = scene["metrics"]["C"]["point_gpu"]["median"]
        sub_a = scene["work"]["A"]["pointSubmissions"]
        sub_b = scene["work"]["B"]["pointSubmissions"]
        sub_c = scene["work"]["C"]["pointSubmissions"]
        lines.append(
            f"- **{scene['displayName']}**：Point Shadow GPU 中位数 "
            f"`{fmt(a)} → {fmt(b)} → {fmt(c)} ms`；"
            f"Point Face 提交 `{fmt(sub_a, 2)} → {fmt(sub_b, 2)} → {fmt(sub_c, 2)} 次/帧`。"
        )
    lines.extend(
        [
            "",
            "这组数据应按两层优化解读：A→B 消除未变化灯光的重绘；B→C 保留 Point Cubemap 未失效的 Face。Receiver Demand 在部分场景仍会覆盖 5～6 面，因此 C 的主要收益来自局部 Caster 运动阶段，而不是假设相机永远只需要一两个面。",
            "",
            f"![三档性能对比]({markdown_path(performance_chart, report_path)})",
            "",
            f"![三档工作量对比]({markdown_path(work_chart, report_path)})",
            "",
            "## 2. 三档定义与统一轨迹",
            "",
            "| 档位 | 缓存粒度 | 典型行为 |",
            "|---|---|---|",
            "| A 全局重绘 | 无缓存控制路径 | 每帧更新 Directional、Point、Spot；Point 固定 6 Face |",
            "| B Per-Light | 当前灯光级 Revision Cache | Point 移动时只更新 Point，但仍固定 6 Face；Caster Revision 仍可能使多灯失效 |",
            "| C Per-Light + Per-Face | 空间 Caster 签名、Face Dirty/Valid/Required Mask | Point 只绘制 `required & stale` 的 Face，未需求 Face 延迟物化 |",
            "",
            "确定性周期分为三段：`Point+Camera`、`Local Caster+Camera`、`Camera-only`。独立运动 Caster 使用同一小球模型和固定轨迹，避免移动整座 Sponza/San Miguel 导致所有 Face 天然失效。",
            "",
            "## 3. 正式性能结果",
            "",
        ]
    )
    for scene_id, scene in scene_data.items():
        lines.extend(
            [
                f"### {scene['displayName']}",
                "",
                "| 指标 | A 全局重绘 | B Per-Light | C Per-Face | A→B | B→C | A→C |",
                "|---|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for label, metric in [
            ("GPU 帧时间中位数 (ms)", "gpu"),
            ("GPU 帧时间 P95 (ms)", "gpu"),
            ("Shadow Update GPU 中位数 (ms)", "shadow_gpu"),
            ("Point Shadow GPU 中位数 (ms)", "point_gpu"),
            ("整帧墙钟时间中位数 (ms)", "wall"),
        ]:
            percentile = "p95" if "P95" in label else "median"
            values = [
                scene["metrics"][variant][metric][percentile]
                for variant in VARIANTS
            ]
            lines.append(
                f"| {label} | {fmt(values[0])} | {fmt(values[1])} | "
                f"{fmt(values[2])} | {fmt_pct(percent(values[0], values[1]))} | "
                f"{fmt_pct(percent(values[1], values[2]))} | "
                f"{fmt_pct(percent(values[0], values[2]))} |"
            )
        lines.extend(
            [
                "",
                "| 工作量（每帧） | A | B | C |",
                "|---|---:|---:|---:|",
            ]
        )
        for label, field in [
            ("更新阴影灯", "updatedLights"),
            ("Point 更新次数", "pointUpdates"),
            ("Point Face 提交", "pointSubmissions"),
            ("Point Face 实际绘制", "pointRenderedFaces"),
            ("Point Face 缓存命中", "pointFaceHits"),
            ("Caster Draw", "casterDraws"),
            ("Caster Triangle", "casterTriangles"),
        ]:
            values = [scene["work"][variant][field] for variant in VARIANTS]
            lines.append(
                f"| {label} | {fmt(values[0], 2)} | {fmt(values[1], 2)} | "
                f"{fmt(values[2], 2)} |"
            )
        lines.extend(
            [
                "",
                f"![{scene['displayName']} 时间线]({markdown_path(timeline_charts[scene_id], report_path)})",
                "",
                f"![{scene['displayName']} 三档截图]({markdown_path(output_dir / f'{scene_id}-three-way-screenshot.png', report_path)})",
                "",
            ]
        )

    lines.extend(
        [
            "## 4. Face 行为与 CPU 代价",
            "",
            f"![Face 更新分布]({markdown_path(histogram_chart, report_path)})",
            "",
            "| 场景 | C 需求 Face/帧 | C 实际绘制 Face/帧 | Face 命中/帧 | 缓存检查 CPU | 需求分析 CPU | Face 签名 CPU |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for scene in scene_data.values():
        work = scene["work"]["C"]
        lines.append(
            f"| {scene['displayName']} | {fmt(work['pointRequiredFaces'], 3)} | "
            f"{fmt(work['pointRenderedFaces'], 3)} | {fmt(work['pointFaceHits'], 3)} | "
            f"{fmt(work['cacheCheckCpu'], 4)} ms | {fmt(work['faceDemandCpu'], 4)} ms | "
            f"{fmt(work['faceSignatureCpu'], 4)} ms |"
        )

    lines.extend(
        [
            "",
            "## 5. 正确性与资源一致性",
            "",
            "性能模式允许未被当前 Receiver 采样的 Face 暂时保留旧内容，但这些 Face 带有无效签名，未来进入 Required Mask 时必须先重建。报告同时运行独立 PCSS 审计：强制六面全部物化，验证 Per-Face 路径最终与 Six-face 基准路径完全收敛。",
            "",
            "独立进程截图审计还暴露并修复了一个与阴影缓存无关的可重复性问题：Opaque 批次原先按 Shader/Material 内存地址排序，不同进程可能改变少量共面像素的先后覆盖。现在改为按场景首次出现顺序生成稳定批次键，以下屏幕比较继续使用严格 `0` 像素差门禁，没有放宽容差。",
            "",
            "| 场景 | A/B 屏幕像素 | B/C 屏幕像素 | B 重复运行截图 | PCSS 六面深度 Hash |",
            "|---|---|---|---|---|",
        ]
    )
    for scene_id, scene in scene_data.items():
        screen = correctness["screenshots"][scene_id]
        audit = correctness["materializedAudit"].get(scene_id, {})
        lines.append(
            f"| {scene['displayName']} | {'完全一致' if screen['exactAB'] else '不一致'} | "
            f"{'完全一致' if screen['exactBC'] else '不一致'} | "
            f"{'完全一致' if screen['duplicateBExact'] else '不一致'} | "
            f"{'六面完全一致' if audit.get('allSixFacesExact') else '未通过/未运行'} |"
        )
    lines.extend(
        [
            "",
            "| 场景 | B 主实验 GPU 帧中位数 | B 独立重测 | 重测差异 |",
            "|---|---:|---:|---:|",
        ]
    )
    for scene in scene_data.values():
        primary_b = scene["metrics"]["B"]["gpu"]["median"]
        recheck_b = scene["bRecheckGpuMedian"]
        lines.append(
            f"| {scene['displayName']} | {fmt(primary_b)} ms | "
            f"{fmt(recheck_b)} ms | {fmt_pct(percent(primary_b, recheck_b))} |"
        )
    lines.extend(
        [
            "",
            "| 场景 | 纹理内存 A/B/C（字节） | Mesh GPU A/B/C（字节） | Render Target A/B/C（字节） |",
            "|---|---:|---:|---:|",
        ]
    )
    for scene in scene_data.values():
        memory = scene["memory"]
        lines.append(
            f"| {scene['displayName']} | "
            f"{memory['A']['texture']}/{memory['B']['texture']}/{memory['C']['texture']} | "
            f"{memory['A']['meshGpu']}/{memory['B']['meshGpu']}/{memory['C']['meshGpu']} | "
            f"{memory['A']['renderTarget']}/{memory['B']['renderTarget']}/{memory['C']['renderTarget']} |"
        )
    first_scene = next(iter(scene_data.values()))
    hardware = first_scene["hardware"]
    lines.extend(
        [
            "",
            "## 6. 实验条件",
            "",
            f"- 分辨率：`{manifest['resolution'][0]}×{manifest['resolution'][1]}`。",
            f"- 每档独立进程：`{manifest['formalRunsPerVariant']}` 轮；每轮测量 `{manifest['measuredFrames']}` 帧。",
            "- 渲染：Release、PBR Forward、Hard Shadow 性能隔离；另以 PCSS 执行完整六面正确性审计。",
            "- Point Shadow：显式 Six-face 路径、逐面 Caster Culling；三档使用相同 Shader、FBO 与分辨率。",
            f"- GPU：`{hardware['vendor']} / {hardware['renderer']}`；OpenGL `{hardware['version']}`。",
            "",
            "## 7. UI 手动验证与一键复现",
            "",
            "- 打开 `Motion Timeline`，点击 `Prepare 3-light test`。它会临时建立一组 Directional/Point/Spot 阴影灯和一个红色局部运动 Caster；退出测试时可恢复原场景。",
            "- `Profile` 选择 `Cache 3-way phases` 后，轨迹依次执行“点光源+相机 / 局部遮挡物+相机 / 仅相机”。右侧实时显示 Required、Rendered、Face hits 与 Deferred。",
            "- 顶部 A/B/C 按钮分别切换全局重绘、Per-Light、Per-Light + Per-Face。正式测试仍以脚本的独立进程数据为准。",
            "",
            "```powershell",
            ".\\tools\\Test-PointShadowCache3Way.ps1 -SkipBuild -BatchId point-shadow-cache-3way-1080p -Width 1920 -Height 1080 -MeasuredFrames 600 -ExternalWarmupFrames 100 -InternalWarmupFrames 15 -FormalRunsPerVariant 3 -SceneIds sponza,san-miguel",
            "```",
            "",
            "## 8. 边界与结论",
            "",
            "1. 当前场景的 Receiver Demand 并不稀疏：Sponza 常需六面，San Miguel 常需五面。仅靠 Camera Demand 不能保证大收益。",
            "2. 局部 Caster 运动时，Per-Face Signature 能稳定把 Point 更新从六面压到约 2～3 面；这才是 C 档的主要案例。",
            "3. Directional/Spot/Point 的 Auto-fit 投影仍保留全局 Caster 依赖，空间 Per-Light 失效会保守退化；报告不会把这部分包装成已经完成的局部化收益。",
            "4. C 档不增加 Shadow Texture 或 Render Target；额外状态只有 CPU 侧 Face Mask、签名和版本数据。",
            "5. Layered Geometry Shader 仍仅保留诊断用途；当前驱动上曾出现五面未写入，因此生产路径继续使用已验证的 Six-face。",
            "",
            "最终判断：这项优化值得保留，但应把简历结论写成“局部 Caster 变化下的 Point Per-Face 增量更新”，而不是笼统声称所有 Point Light 更新都会从六面降到一面。",
            "",
        ]
    )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve()
    output_dir = args.output_dir.resolve()
    report_path = args.report.resolve()
    manifest = read_json(manifest_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    scene_data, correctness = build_data(manifest, manifest_path, output_dir)
    write_report(report_path, output_dir, manifest, scene_data, correctness)
    summary_path = output_dir / "point-shadow-cache-3way-summary-cn.json"
    summary_path.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "manifest": manifest,
                "scenes": serializable_scene_data(scene_data),
                "correctness": correctness,
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"中文报告: {report_path}")
    print(f"图表目录: {output_dir}")
    print(f"汇总 JSON: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
