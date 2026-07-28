#!/usr/bin/env python3
"""Generate a Chinese report for deterministic shadow motion timelines."""

from __future__ import annotations

import argparse
import contextlib
import csv
import io
import json
import math
import statistics
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib import font_manager

from shadow_image_compare import compare_images


class ReportError(RuntimeError):
    pass


STATISTICS = {
    "median": 0.50,
    "p95": 0.95,
    "p99": 0.99,
}

METRICS = {
    "wallMilliseconds": ("帧时间", "ms", True),
    "shadowGpuMilliseconds": ("Shadow Update GPU", "ms", True),
    "shadowCpuMilliseconds": ("Shadow Update CPU", "ms", True),
    "updatedLights": ("更新灯数", "盏/帧", True),
    "pointSubmissions": ("Point 提交次数", "次/帧", True),
    "lightCacheHits": ("Per-Light Cache Hit", "次/帧", False),
}

PROFILE_LABELS = {
    "point": "Point 连续运动",
    "caster": "Caster 连续运动",
    "camera": "Camera-only",
    "mixed": "Point + Caster + Camera",
}

SCENE_LABELS = {
    "sponza": "Sponza",
    "san-miguel": "San Miguel",
}


def read_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"无法读取 JSON：{path}: {error}") from error


def configure_chinese_font() -> str:
    preferred = (
        "Microsoft YaHei",
        "Microsoft YaHei UI",
        "SimHei",
        "DengXian",
        "Noto Sans CJK SC",
        "Source Han Sans CN",
    )
    available = {entry.name for entry in font_manager.fontManager.ttflist}
    selected = next((name for name in preferred if name in available), None)
    if selected is None:
        raise ReportError("未找到可用于中文图表的字体")
    matplotlib.rcParams["font.family"] = "sans-serif"
    matplotlib.rcParams["font.sans-serif"] = [selected, "DejaVu Sans"]
    matplotlib.rcParams["axes.unicode_minus"] = False
    matplotlib.rcParams["figure.facecolor"] = "white"
    matplotlib.rcParams["axes.facecolor"] = "#fbfdff"
    return selected


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = quantile * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def averaged_process_statistics(
    run_series: list[list[float]],
) -> dict[str, float]:
    if not run_series:
        raise ReportError("缺少正式运行数据")
    result: dict[str, float] = {}
    for name, quantile in STATISTICS.items():
        process_values = [
            percentile(series, quantile) for series in run_series
        ]
        result[name] = statistics.fmean(process_values)
    return result


def process_statistics(values: list[float]) -> dict[str, float]:
    return {
        name: percentile(values, quantile)
        for name, quantile in STATISTICS.items()
    }


def delta_percent(before: float, after: float) -> float | None:
    if abs(before) < 1e-12:
        return None
    return (after - before) * 100.0 / before


def load_run(path: Path) -> dict[str, Any]:
    run = read_json(path)
    if not run.get("success"):
        raise ReportError(f"运行未通过：{path}")
    timeline = run.get("motionTimeline")
    if not isinstance(timeline, dict) or not timeline.get("enabled"):
        raise ReportError(f"运行没有有效运动时间轴：{path}")
    samples = timeline.get("samples")
    if not isinstance(samples, list) or not samples:
        raise ReportError(f"运行没有逐帧时间轴样本：{path}")
    if len(samples) != int(run.get("measuredFrames", -1)):
        raise ReportError(f"时间轴样本数与 measuredFrames 不一致：{path}")
    return run


def extract_run_series(run: dict[str, Any]) -> dict[str, list[float]]:
    timeline = run["motionTimeline"]
    samples = timeline["samples"]
    sample_count = len(samples)
    gpu_zones = run["profiler"]["samples"]["gpuZones"]
    shadow_gpu = list(gpu_zones.get("Shadow Map Update", []))
    update_frames = sum(
        int(sample["shadow"]["updateCount"]) for sample in samples
    )
    if not shadow_gpu and update_frames == 0:
        shadow_gpu = [0.0] * sample_count
    if len(shadow_gpu) != sample_count:
        raise ReportError(
            "Shadow Map Update GPU 样本无法与连续时间轴逐帧对齐"
        )
    wall = [float(value) for value in run["profiler"]["samples"]["wallFrame"]]
    if len(wall) != sample_count:
        raise ReportError("Wall Frame 样本无法与时间轴对齐")
    base = timeline["baseState"]
    scene_radius = float(timeline["sceneRadius"])
    if scene_radius <= 0.0:
        raise ReportError("sceneRadius 必须大于零")

    return {
        "frame": [float(sample["measurementFrame"]) for sample in samples],
        "wallMilliseconds": wall,
        "shadowGpuMilliseconds": [float(value) for value in shadow_gpu],
        "shadowCpuMilliseconds": [
            float(sample["shadow"]["updateCpuMilliseconds"])
            for sample in samples
        ],
        "updatedLights": [
            float(sample["shadow"]["updatedLightCount"]) for sample in samples
        ],
        "pointSubmissions": [
            float(sample["shadow"]["pointShadowSubmissionPassCount"])
            for sample in samples
        ],
        "lightCacheHits": [
            float(sample["shadow"]["lightCacheHitCount"]) for sample in samples
        ],
        "pointMotion": [
            (
                float(sample["pointPosition"][0])
                - float(base["pointPosition"][0])
            )
            / scene_radius
            for sample in samples
        ],
        "casterMotion": [
            (
                float(sample["casterPosition"][0])
                - float(base["casterPosition"][0])
            )
            / scene_radius
            for sample in samples
        ],
        "cameraMotion": [
            (
                float(sample["cameraPosition"][0])
                - float(base["cameraPosition"][0])
            )
            / scene_radius
            for sample in samples
        ],
    }


def per_frame_median(run_series: list[list[float]]) -> list[float]:
    lengths = {len(series) for series in run_series}
    if len(lengths) != 1:
        raise ReportError("正式运行的逐帧样本数不一致")
    return [
        statistics.median(frame_values)
        for frame_values in zip(*run_series)
    ]


def rolling_mean(values: list[float], window: int = 15) -> list[float]:
    if window <= 1:
        return list(values)
    result: list[float] = []
    running_sum = 0.0
    for index, value in enumerate(values):
        running_sum += value
        if index >= window:
            running_sum -= values[index - window]
        divisor = min(index + 1, window)
        result.append(running_sum / divisor)
    return result


def discover_runs(
    experiment_directory: Path,
    scene_id: str,
    expected_count: int,
) -> dict[str, list[dict[str, Any]]]:
    scene_directory = experiment_directory / "formal" / scene_id
    variants: dict[str, list[dict[str, Any]]] = {}
    for variant in ("A", "B"):
        paths = sorted(scene_directory.glob(f"{variant}[0-9]*.json"))
        if len(paths) != expected_count:
            raise ReportError(
                f"{scene_directory} 的 {variant} 运行数应为 "
                f"{expected_count}，实际为 {len(paths)}"
            )
        variants[variant] = [load_run(path) for path in paths]
    return variants


def write_frame_csv(
    path: Path,
    series: dict[str, dict[str, list[float]]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    metric_names = list(METRICS)
    frame_count = len(series["A"]["frame"])
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream)
        header = ["frame"]
        for metric in metric_names:
            header.extend((f"A_{metric}", f"B_{metric}"))
        writer.writerow(header)
        for frame in range(frame_count):
            row: list[float | int] = [frame]
            for metric in metric_names:
                row.extend(
                    (
                        series["A"][metric][frame],
                        series["B"][metric][frame],
                    )
                )
            writer.writerow(row)


def plot_timeline(
    path: Path,
    profile: str,
    scene_id: str,
    tracks: list[str],
    series: dict[str, dict[str, list[float]]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = series["B"]["frame"]
    figure, axes = plt.subplots(3, 2, figsize=(16, 13), sharex=True)
    axes_flat = list(axes.flat)
    track_config = (
        ("point", "pointMotion", "Point"),
        ("caster", "casterMotion", "Caster"),
        ("camera", "cameraMotion", "Camera"),
    )
    for track, field, label in track_config:
        if track in tracks:
            axes_flat[0].plot(
                frames,
                series["B"][field],
                linewidth=1.5,
                label=label,
            )
    axes_flat[0].set_title("确定性轨迹 X 位移（相对场景半径）")
    axes_flat[0].set_ylabel("ΔX / sceneRadius")
    axes_flat[0].legend(loc="upper right")

    chart_metrics = (
        ("wallMilliseconds", "帧时间", "ms"),
        ("shadowGpuMilliseconds", "Shadow Update GPU", "ms"),
        ("updatedLights", "每帧更新灯数", "盏"),
        ("pointSubmissions", "Point Shadow 提交次数", "次"),
        ("lightCacheHits", "Per-Light Cache Hit", "次"),
    )
    colors = {"A": "#d97706", "B": "#2563eb"}
    labels = {"A": "A 无缓存", "B": "B Per-Light"}
    for axis, (field, title, unit) in zip(axes_flat[1:], chart_metrics):
        for variant in ("A", "B"):
            values = series[variant][field]
            axis.plot(
                frames,
                values,
                color=colors[variant],
                alpha=0.18,
                linewidth=0.7,
            )
            axis.plot(
                frames,
                rolling_mean(values),
                color=colors[variant],
                linewidth=1.5,
                label=labels[variant],
            )
        axis.set_title(title)
        axis.set_ylabel(unit)
        axis.legend(loc="upper right")
    for axis in axes[-1]:
        axis.set_xlabel("测量帧")
    for axis in axes_flat:
        axis.grid(alpha=0.22)
    figure.suptitle(
        f"{SCENE_LABELS.get(scene_id, scene_id)} · "
        f"{PROFILE_LABELS.get(profile, profile)}",
        fontsize=16,
        fontweight="bold",
    )
    figure.tight_layout(rect=(0, 0, 1, 0.97))
    figure.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(figure)


def compare_capture_pair(
    project_directory: Path,
    output_directory: Path,
    profile: str,
    scene_id: str,
    runs: dict[str, list[dict[str, Any]]],
) -> tuple[dict[str, Any], Path, Path]:
    before = Path(runs["A"][0]["capturePath"])
    after = Path(runs["B"][0]["capturePath"])
    if not before.is_absolute():
        before = project_directory / before
    if not after.is_absolute():
        after = project_directory / after
    capture_directory = output_directory / "captures"
    prefix = f"{profile}-{scene_id}"
    metrics_path = capture_directory / f"{prefix}-metrics.json"
    difference_path = capture_directory / f"{prefix}-difference.png"
    comparison_path = capture_directory / f"{prefix}-comparison.png"
    with contextlib.redirect_stdout(io.StringIO()):
        compare_images(
            before,
            after,
            metrics_path,
            difference_path,
            comparison_path,
            "No Cache",
            "Per-Light Cache",
        )
    return read_json(metrics_path), comparison_path, difference_path


def format_number(value: float, unit: str) -> str:
    if unit in ("盏/帧", "次/帧"):
        return f"{value:.2f}"
    return f"{value:.3f}"


def format_delta(value: float | None) -> str:
    if value is None:
        return "N/A"
    return f"{value:+.2f}%"


def relative_link(report_path: Path, target: Path) -> str:
    return target.relative_to(report_path.parent).as_posix()


def portable_path(path: Path, base: Path) -> str:
    try:
        return path.relative_to(base).as_posix()
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    arguments = parser.parse_args()

    configure_chinese_font()
    manifest_path = arguments.manifest.resolve()
    output_directory = arguments.output_dir.resolve()
    output_directory.mkdir(parents=True, exist_ok=True)
    manifest = read_json(manifest_path)
    project_directory = Path(__file__).resolve().parent.parent
    expected_runs = int(manifest["formalRunsPerVariant"])
    scenes = [str(scene) for scene in manifest["scenes"]]
    report_rows: list[dict[str, Any]] = []
    report_sections: list[dict[str, Any]] = []
    reference_metadata: dict[str, Any] | None = None
    reference_run: dict[str, Any] | None = None

    for experiment in manifest["experiments"]:
        profile = str(experiment["profile"])
        experiment_directory = (
            project_directory / str(experiment["directory"])
        ).resolve()
        metadata = read_json(experiment_directory / "metadata.json")
        if reference_metadata is None:
            reference_metadata = metadata
        elif (
            metadata["source"] != reference_metadata["source"]
            or metadata["executables"] != reference_metadata["executables"]
            or metadata["machine"] != reference_metadata["machine"]
        ):
            raise ReportError("各 Timeline Profile 的源码、二进制或机器信息不一致")
        if metadata["settings"]["workload"] != f"timeline-{profile}":
            raise ReportError(f"{profile} workload 与 manifest 不一致")
        for scene_id in scenes:
            runs = discover_runs(
                experiment_directory,
                scene_id,
                expected_runs,
            )
            if reference_run is None:
                reference_run = runs["A"][0]
            extracted = {
                variant: [extract_run_series(run) for run in runs[variant]]
                for variant in ("A", "B")
            }
            frame_series: dict[str, dict[str, list[float]]] = {
                variant: {
                    field: per_frame_median(
                        [series[field] for series in extracted[variant]]
                    )
                    for field in extracted[variant][0]
                }
                for variant in ("A", "B")
            }
            tracks = list(runs["B"][0]["motionTimeline"]["tracks"])
            chart_path = (
                output_directory / f"timeline-{profile}-{scene_id}.png"
            )
            plot_timeline(
                chart_path,
                profile,
                scene_id,
                tracks,
                frame_series,
            )
            csv_path = (
                output_directory / "csv" / f"{profile}-{scene_id}.csv"
            )
            write_frame_csv(csv_path, frame_series)
            image_metrics, comparison_path, difference_path = (
                compare_capture_pair(
                    project_directory,
                    output_directory,
                    profile,
                    scene_id,
                    runs,
                )
            )
            metric_summary: dict[str, Any] = {}
            for field, (label, unit, lower_is_better) in METRICS.items():
                process_values = {
                    variant: [
                        process_statistics(series[field])
                        for series in extracted[variant]
                    ]
                    for variant in ("A", "B")
                }
                variant_stats = {
                    variant: averaged_process_statistics(
                        [series[field] for series in extracted[variant]]
                    )
                    for variant in ("A", "B")
                }
                median_delta = delta_percent(
                    variant_stats["A"]["median"],
                    variant_stats["B"]["median"],
                )
                metric_summary[field] = {
                    "label": label,
                    "unit": unit,
                    "lowerIsBetter": lower_is_better,
                    "A": variant_stats["A"],
                    "B": variant_stats["B"],
                    "processValues": process_values,
                    "medianDeltaPercent": median_delta,
                }
                report_rows.append(
                    {
                        "profile": profile,
                        "scene": scene_id,
                        "metric": field,
                        **metric_summary[field],
                    }
                )
            report_sections.append(
                {
                    "profile": profile,
                    "scene": scene_id,
                    "tracks": tracks,
                    "metrics": metric_summary,
                    "imageMetrics": image_metrics,
                    "chart": chart_path,
                    "csv": csv_path,
                    "comparison": comparison_path,
                    "difference": difference_path,
                }
            )

    if reference_metadata is None or reference_run is None:
        raise ReportError("manifest 没有可报告的实验")

    summary_csv = output_directory / "summary.csv"
    with summary_csv.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "profile",
                "scene",
                "metric",
                "unit",
                "A_median",
                "B_median",
                "delta_percent",
                "A_p95",
                "B_p95",
                "A_p99",
                "B_p99",
            ]
        )
        for row in report_rows:
            writer.writerow(
                [
                    row["profile"],
                    row["scene"],
                    row["metric"],
                    row["unit"],
                    row["A"]["median"],
                    row["B"]["median"],
                    row["medianDeltaPercent"],
                    row["A"]["p95"],
                    row["B"]["p95"],
                    row["A"]["p99"],
                    row["B"]["p99"],
                ]
            )

    report_data = {
        "schemaVersion": 1,
        "manifest": portable_path(manifest_path, project_directory),
        "source": reference_metadata["source"],
        "executables": {
            variant: {
                "fileName": Path(values["path"]).name,
                "sha256": values["sha256"],
            }
            for variant, values in reference_metadata["executables"].items()
        },
        "machine": reference_metadata["machine"],
        "configuration": reference_metadata["configuration"],
        "resolution": reference_metadata["resolution"],
        "opengl": {
            "vendor": reference_run["glVendor"],
            "renderer": reference_run["glRenderer"],
            "version": reference_run["glVersion"],
        },
        "statistics": (
            "每轮独立进程先计算 Median/P95/P99，再对各轮统计值取算术平均"
        ),
        "sections": [
            {
                **section,
                "chart": portable_path(section["chart"], output_directory),
                "csv": portable_path(section["csv"], output_directory),
                "comparison": portable_path(
                    section["comparison"], output_directory
                ),
                "difference": portable_path(
                    section["difference"], output_directory
                ),
            }
            for section in report_sections
        ],
    }
    (output_directory / "report-data.json").write_text(
        json.dumps(report_data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    report_path = output_directory / "report.md"
    lines = [
        "# 确定性 Shadow Motion Timeline A/B 实验报告",
        "",
        f"- 批次：`{manifest['batchId']}`",
        f"- 日期（UTC）：`{reference_metadata['createdUtc']}`",
        (
            f"- 源码提交：`{reference_metadata['source']['gitHead']}`"
            f"（dirty={str(reference_metadata['source']['gitDirty']).lower()}，"
            f"source SHA-256="
            f"`{reference_metadata['source']['sha256']}`）"
        ),
        f"- 构建：{reference_metadata['configuration']}",
        f"- 分辨率：{manifest['resolution'][0]}×{manifest['resolution'][1]}",
        (
            f"- CPU："
            f"{', '.join(reference_metadata['machine'].get('cpu', []))}"
        ),
        (
            f"- OpenGL GPU：{reference_run['glRenderer']}；"
            f"OpenGL：{reference_run['glVersion']}"
        ),
        f"- 每个变体独立运行：{expected_runs} 轮",
        (
            f"- 正式顺序："
            f"`{'/'.join(reference_metadata['order'])}`"
        ),
        f"- 每轮测量帧：{manifest['measuredFrames']}",
        (
            f"- Warm-up：外部 {manifest['externalWarmupFrames']} 帧，"
            f"内部 {manifest['internalWarmupFrames']} 帧"
        ),
        (
            f"- 时间轴：{manifest['timeline']['fixedFramesPerSecond']} Hz，"
            f"{manifest['timeline']['cycleFrames']} 帧一周期"
        ),
        "- A：无缓存；B：Per-Light Revision Cache",
        "",
        "统计口径：每轮独立进程先计算 Median/P95/P99，"
        "再对同一变体各轮统计值取算术平均。逐帧曲线取各轮同帧中位数。",
        "",
        "## 汇总结论表",
        "",
        (
            "| 场景 | 轨迹 | Shadow GPU Median A→B | "
            "帧时间 Median A→B | 更新灯数 A→B | Point 提交 A→B |"
        ),
        "|---|---|---:|---:|---:|---:|",
    ]
    for section in report_sections:
        metrics = section["metrics"]
        gpu = metrics["shadowGpuMilliseconds"]
        wall = metrics["wallMilliseconds"]
        lights = metrics["updatedLights"]
        submissions = metrics["pointSubmissions"]
        lines.append(
            "| "
            f"{SCENE_LABELS.get(section['scene'], section['scene'])} | "
            f"{PROFILE_LABELS.get(section['profile'], section['profile'])} | "
            f"{gpu['A']['median']:.3f}→{gpu['B']['median']:.3f} ms "
            f"({format_delta(gpu['medianDeltaPercent'])}) | "
            f"{wall['A']['median']:.3f}→{wall['B']['median']:.3f} ms "
            f"({format_delta(wall['medianDeltaPercent'])}) | "
            f"{lights['A']['median']:.2f}→{lights['B']['median']:.2f} | "
            f"{submissions['A']['median']:.2f}→"
            f"{submissions['B']['median']:.2f} |"
        )
    lines.extend(
        [
            "",
            "## 分场景逐帧证据",
            "",
        ]
    )
    for section in report_sections:
        lines.extend(
            [
                (
                    f"### {SCENE_LABELS.get(section['scene'], section['scene'])}"
                    f" · {PROFILE_LABELS.get(section['profile'], section['profile'])}"
                ),
                "",
                f"启用轨道：`{', '.join(section['tracks'])}`。",
                "",
                (
                    f"![逐帧时间轴]("
                    f"{relative_link(report_path, section['chart'])})"
                ),
                "",
                "| 指标 | A Median / P95 / P99 | B Median / P95 / P99 | Median 变化 |",
                "|---|---:|---:|---:|",
            ]
        )
        for metric in METRICS:
            values = section["metrics"][metric]
            unit = values["unit"]
            lines.append(
                f"| {values['label']} | "
                f"{format_number(values['A']['median'], unit)} / "
                f"{format_number(values['A']['p95'], unit)} / "
                f"{format_number(values['A']['p99'], unit)} {unit} | "
                f"{format_number(values['B']['median'], unit)} / "
                f"{format_number(values['B']['p95'], unit)} / "
                f"{format_number(values['B']['p99'], unit)} {unit} | "
                f"{format_delta(values['medianDeltaPercent'])} |"
            )
        shadow_process_values = section["metrics"][
            "shadowGpuMilliseconds"
        ]["processValues"]
        lines.extend(
            [
                "",
                "| 配对 | A Shadow GPU Median | B Shadow GPU Median | B 相对 A |",
                "|---|---:|---:|---:|",
            ]
        )
        for process_index in range(expected_runs):
            before_value = shadow_process_values["A"][process_index]["median"]
            after_value = shadow_process_values["B"][process_index]["median"]
            lines.append(
                f"| A{process_index + 1}/B{process_index + 1} | "
                f"{before_value:.3f} ms | {after_value:.3f} ms | "
                f"{format_delta(delta_percent(before_value, after_value))} |"
            )
        image_metrics = section["imageMetrics"]
        lines.extend(
            [
                "",
                (
                    "画面校验：最大通道差 "
                    f"`{image_metrics['maximumChannelDelta']}`，"
                    "变化像素 "
                    f"`{image_metrics['exactChangedPixelCount']}`。"
                ),
                "",
                (
                    f"![A/B 截图]("
                    f"{relative_link(report_path, section['comparison'])})"
                ),
                "",
                (
                    f"![差异热力图]("
                    f"{relative_link(report_path, section['difference'])})"
                ),
                "",
                (
                    f"逐帧原始表："
                    f"[{section['csv'].name}]("
                    f"{relative_link(report_path, section['csv'])})"
                ),
                "",
            ]
        )
    lines.extend(
        [
            "## 解释边界",
            "",
            "- `timeline-point` 用于衡量“只动 Point”时其余灯的缓存收益；"
            "Point Cubemap 仍应每个失效帧提交六面。",
            "- `timeline-camera` 用于证明仅相机运动不会误使阴影缓存失效。",
            "- `timeline-caster` 是所有受影响阴影灯都必须更新的保守路径。",
            "- `timeline-mixed` 是持续运动压力测试，不应被包装成 Per-Light 的最佳案例。",
            "",
        ]
    )
    report_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"中文时间轴报告：{report_path}")
    print(f"汇总 CSV：{summary_csv}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReportError as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
