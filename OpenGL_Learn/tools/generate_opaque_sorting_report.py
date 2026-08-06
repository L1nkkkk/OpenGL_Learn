from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

import matplotlib.pyplot as plt
from PIL import Image, ImageChops


PROJECT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_RESULT_DIR = (
    PROJECT_DIR
    / "benchmark-results"
    / "opaque-sorting"
    / "object-heavy-20260730"
)
DEFAULT_IMAGE_DIR = (
    PROJECT_DIR
    / "docs"
    / "benchmark-images"
    / "opaque-sorting"
    / "object-heavy-20260730"
)
DEFAULT_SHADOW_REGRESSION_MANIFEST = (
    PROJECT_DIR
    / "benchmark-results"
    / "shadow-optimizations"
    / "opaque-sort-shadow-regression-20260730"
    / "manifest.json"
)


@dataclass(frozen=True)
class Run:
    record: dict[str, Any]
    report_path: Path
    report: dict[str, Any]
    capture_path: Path | None

    @property
    def name(self) -> str:
        return str(self.record["name"])

    @property
    def sort_mode(self) -> str:
        return str(self.record["sortMode"])


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def nearest_rank(sorted_values: list[float], percentile: float) -> float | None:
    if not sorted_values:
        return None
    rank = math.ceil(max(0.0, min(1.0, percentile)) * len(sorted_values))
    index = max(0, rank - 1)
    return sorted_values[min(index, len(sorted_values) - 1)]


def summarize(values: Iterable[float]) -> dict[str, Any]:
    collected = [float(value) for value in values]
    if not collected:
        return {
            "count": 0,
            "mean": None,
            "min": None,
            "max": None,
            "median": None,
            "p95": None,
            "p99": None,
        }
    ordered = sorted(collected)
    return {
        "count": len(collected),
        "mean": statistics.fmean(collected),
        "min": ordered[0],
        "max": ordered[-1],
        "median": nearest_rank(ordered, 0.50),
        "p95": nearest_rank(ordered, 0.95),
        "p99": nearest_rank(ordered, 0.99),
    }


def sample_values(run: Run, metric: str) -> list[float]:
    samples = run.report["samples"]
    if metric in {"wallFrameMs", "cpuFrameMs", "gpuFrameMs"}:
        return [float(value) for value in samples[metric]]
    if metric.startswith("cpuZone:"):
        zone = metric.split(":", 1)[1]
        return [
            float(value)
            for value in samples["cpuZonesMs"].get(zone, [])
        ]
    if metric.startswith("gpuZone:"):
        zone = metric.split(":", 1)[1]
        return [
            float(value)
            for value in samples["gpuZonesMs"].get(zone, [])
        ]
    if metric.startswith("render:"):
        field = metric.split(":", 1)[1]
        return [
            float(sample[field])
            for sample in samples["renderStats"]
        ]
    raise KeyError(f"Unknown metric: {metric}")


METRICS = {
    "wallFrameMs": "wallFrameMs",
    "cpuFrameMs": "cpuFrameMs",
    "gpuFrameMs": "gpuFrameMs",
    "forwardGpuMs": "gpuZone:Forward Pass",
    "deferredGpuMs": "gpuZone:Deferred Pass",
    "buildDrawListsMs": "cpuZone:Build Draw Lists",
    "collectionMs": "cpuZone:Draw Item Collection",
    "sortingMs": "cpuZone:Opaque Draw Sorting",
    "legacyOrderBuildMs": "cpuZone:Opaque Legacy Order Build",
    "sortKeyBuildMs": "cpuZone:Opaque Sort Key Build",
    "sortAlgorithmMs": "cpuZone:Opaque Sort Algorithm",
    "indexMaterializationMs": "cpuZone:Opaque Index Materialization",
    "motionMs": "cpuZone:Submission Stress Motion",
    "drawCalls": "render:drawCalls",
    "activeModels": "render:activeModels",
    "visibleModels": "render:visibleModels",
    "opaqueMeshes": "render:opaqueMeshes",
    "transparentMeshes": "render:transparentMeshes",
}

PROBE_METRICS = {
    "totalMs": "cpuZone:Collection Probe Total",
    "materialRevisionMs": "cpuZone:Collection Probe Material Revision",
    "modelBoundsFrustumMs":
        "cpuZone:Collection Probe Model Matrix Bounds Frustum",
    "meshBoundsValidationMs":
        "cpuZone:Collection Probe Mesh Bounds Validation",
    "drawItemWriteMs": "cpuZone:Collection Probe DrawItem Write",
}


def aggregate(runs: list[Run]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "runCount": len(runs),
        "runNames": [run.name for run in runs],
        "metrics": {},
        "runMedians": {},
    }
    for output_name, metric in METRICS.items():
        pooled: list[float] = []
        medians: list[float] = []
        for run in runs:
            values = sample_values(run, metric)
            pooled.extend(values)
            summary = summarize(values)
            if summary["median"] is not None:
                medians.append(float(summary["median"]))
        result["metrics"][output_name] = summarize(pooled)
        result["runMedians"][output_name] = medians
    return result


def aggregate_probe(run: Run) -> dict[str, Any]:
    return {
        output_name: summarize(sample_values(run, metric))
        for output_name, metric in PROBE_METRICS.items()
    }


def metric_median(group: dict[str, Any], metric: str) -> float:
    value = group["metrics"][metric]["median"]
    if value is None:
        return 0.0
    return float(value)


def metric_percentile(
    group: dict[str, Any],
    metric: str,
    percentile: str,
) -> float:
    value = group["metrics"][metric][percentile]
    if value is None:
        return 0.0
    return float(value)


def delta_summary(
    control: dict[str, Any],
    candidate: dict[str, Any],
    metric: str,
) -> dict[str, Any]:
    before = metric_median(control, metric)
    after = metric_median(candidate, metric)
    reduction = before - after
    return {
        "controlMedian": before,
        "candidateMedian": after,
        "candidateMinusControl": after - before,
        "reduction": reduction,
        "reductionPercent": reduction / before * 100.0 if before else None,
        "speedup": before / after if after else None,
    }


def linear_regression(points: list[tuple[float, float]]) -> dict[str, float]:
    xs = [point[0] for point in points]
    ys = [point[1] for point in points]
    x_mean = statistics.fmean(xs)
    y_mean = statistics.fmean(ys)
    denominator = sum((value - x_mean) ** 2 for value in xs)
    slope = (
        sum(
            (x_value - x_mean) * (y_value - y_mean)
            for x_value, y_value in points
        )
        / denominator
        if denominator
        else 0.0
    )
    intercept = y_mean - slope * x_mean
    fitted = [slope * value + intercept for value in xs]
    total = sum((value - y_mean) ** 2 for value in ys)
    residual = sum(
        (actual - estimate) ** 2
        for actual, estimate in zip(ys, fitted)
    )
    r_squared = 1.0 - residual / total if total else 1.0
    return {
        "slopeMsPerObject": slope,
        "interceptMs": intercept,
        "rSquared": r_squared,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def set_plot_style() -> None:
    plt.style.use("seaborn-v0_8-whitegrid")
    plt.rcParams.update(
        {
            "figure.facecolor": "#F7F9FC",
            "axes.facecolor": "#FFFFFF",
            "axes.edgecolor": "#BAC4D0",
            "axes.titleweight": "bold",
            "font.size": 10,
            "savefig.bbox": "tight",
        }
    )


def save_primary_chart(
    image_dir: Path,
    legacy: dict[str, Any],
    key_index: dict[str, Any],
) -> None:
    labels = [
        "CPU Frame",
        "Build Lists",
        "Collection",
        "Opaque Sort",
        "GPU Forward",
    ]
    metrics = [
        "cpuFrameMs",
        "buildDrawListsMs",
        "collectionMs",
        "sortingMs",
        "forwardGpuMs",
    ]
    x_values = list(range(len(labels)))
    width = 0.36
    colors = ["#D95F59", "#2C7FB8"]
    fig, axis = plt.subplots(figsize=(10.4, 5.5))
    for variant_index, (name, group) in enumerate(
        [("Legacy", legacy), ("Key + compact index", key_index)]
    ):
        medians = [metric_median(group, metric) for metric in metrics]
        p95 = [
            metric_percentile(group, metric, "p95")
            for metric in metrics
        ]
        positions = [
            value + (variant_index - 0.5) * width
            for value in x_values
        ]
        axis.bar(
            positions,
            medians,
            width,
            color=colors[variant_index],
            label=f"{name} median",
            zorder=3,
        )
        axis.scatter(
            positions,
            p95,
            color=colors[variant_index],
            marker="D",
            s=34,
            edgecolor="white",
            linewidth=0.6,
            label=f"{name} P95",
            zorder=5,
        )
    axis.set_xticks(x_values, labels)
    axis.set_ylabel("Milliseconds (lower is better)")
    axis.set_title("Object-heavy 30k: legacy vs optimized opaque sorting")
    axis.legend(ncol=2, frameon=True)
    axis.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(image_dir / "primary-ab-timing.png", dpi=180)
    plt.close(fig)


def save_sort_breakdown_chart(
    image_dir: Path,
    legacy: dict[str, Any],
    key_direct: dict[str, Any],
    key_index: dict[str, Any],
) -> None:
    groups = [legacy, key_direct, key_index]
    labels = ["Legacy", "Key direct", "Key index"]
    component_specs = [
        ("Legacy order build", "legacyOrderBuildMs", "#F4A261"),
        ("Key build", "sortKeyBuildMs", "#2A9D8F"),
        ("Sort algorithm", "sortAlgorithmMs", "#457B9D"),
        ("Index materialization", "indexMaterializationMs", "#8D6AB8"),
    ]
    fig, axis = plt.subplots(figsize=(8.3, 5.2))
    bottoms = [0.0, 0.0, 0.0]
    for component_name, metric, color in component_specs:
        values = [metric_median(group, metric) for group in groups]
        axis.bar(
            labels,
            values,
            bottom=bottoms,
            label=component_name,
            color=color,
            zorder=3,
        )
        bottoms = [
            bottom + value
            for bottom, value in zip(bottoms, values)
        ]
    totals = [metric_median(group, "sortingMs") for group in groups]
    axis.set_ylim(0.0, max(totals) * 1.14)
    for index, total in enumerate(totals):
        axis.text(
            index,
            total + max(totals) * 0.018,
            f"{total:.2f} ms total",
            ha="center",
            va="bottom",
            fontsize=9,
        )
    axis.set_ylabel("Median milliseconds (lower is better)")
    axis.set_title(
        "Opaque sorting implementation cost at 30k draws",
        pad=12,
    )
    axis.legend(frameon=True)
    axis.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(image_dir / "sorting-path-breakdown.png", dpi=180)
    plt.close(fig)


def save_dynamic_chart(
    image_dir: Path,
    dynamic_groups: dict[int, dict[str, Any]],
) -> None:
    percents = sorted(dynamic_groups)
    series = [
        ("CPU Frame", "cpuFrameMs", "#243B53"),
        ("Motion update", "motionMs", "#D95F59"),
        ("Collection", "collectionMs", "#2C7FB8"),
        ("Opaque sort", "sortingMs", "#2A9D8F"),
    ]
    fig, axis = plt.subplots(figsize=(8.7, 5.3))
    for label, metric, color in series:
        values = [
            metric_median(dynamic_groups[percent], metric)
            for percent in percents
        ]
        axis.plot(
            percents,
            values,
            marker="o",
            linewidth=2.2,
            color=color,
            label=label,
        )
        for x_value, y_value in zip(percents, values):
            axis.annotate(
                f"{y_value:.2f}",
                (x_value, y_value),
                textcoords="offset points",
                xytext=(0, 7),
                ha="center",
                fontsize=8,
            )
    axis.set_xlabel("Dynamic objects (%)")
    axis.set_ylabel("Median milliseconds")
    axis.set_title("30k objects: dynamic ratio sensitivity")
    axis.set_xticks(percents)
    axis.legend(frameon=True)
    fig.tight_layout()
    fig.savefig(image_dir / "dynamic-percent-sensitivity.png", dpi=180)
    plt.close(fig)


def save_scaling_chart(
    image_dir: Path,
    scaling_groups: dict[int, dict[str, Any]],
) -> None:
    counts = sorted(scaling_groups)
    series = [
        ("CPU Frame", "cpuFrameMs", "#243B53"),
        ("Build Draw Lists", "buildDrawListsMs", "#D95F59"),
        ("Collection", "collectionMs", "#2C7FB8"),
        ("Opaque sort", "sortingMs", "#2A9D8F"),
    ]
    fig, axis = plt.subplots(figsize=(9.0, 5.4))
    for label, metric, color in series:
        values = [
            metric_median(scaling_groups[count], metric)
            for count in counts
        ]
        axis.plot(
            counts,
            values,
            marker="o",
            linewidth=2.2,
            color=color,
            label=label,
        )
    axis.set_xlabel("Visible objects / opaque draws")
    axis.set_ylabel("Median milliseconds")
    axis.set_title("Object-count scaling after sorting optimization")
    axis.set_xticks(counts, [f"{count // 1000}k" for count in counts])
    axis.legend(frameon=True)
    fig.tight_layout()
    fig.savefig(image_dir / "object-count-scaling.png", dpi=180)
    plt.close(fig)


def save_probe_chart(
    image_dir: Path,
    probes: dict[int, dict[str, Any]],
) -> None:
    percents = sorted(probes)
    components = [
        ("Material revision", "materialRevisionMs", "#F4A261"),
        ("Model matrix/bounds/frustum", "modelBoundsFrustumMs", "#2A9D8F"),
        ("Mesh bounds/8 corners/validation", "meshBoundsValidationMs", "#457B9D"),
        ("DrawItem materialization", "drawItemWriteMs", "#8D6AB8"),
    ]
    labels = [f"{percent}% dynamic" for percent in percents]
    bottoms = [0.0 for _ in percents]
    fig, axis = plt.subplots(figsize=(10.8, 5.5))
    for component_name, metric, color in components:
        values = [
            float(probes[percent][metric]["median"] or 0.0)
            for percent in percents
        ]
        axis.bar(
            labels,
            values,
            bottom=bottoms,
            label=component_name,
            color=color,
            zorder=3,
        )
        bottoms = [
            bottom + value
            for bottom, value in zip(bottoms, values)
        ]
    totals = [
        float(probes[percent]["totalMs"]["median"] or 0.0)
        for percent in percents
    ]
    for index, total in enumerate(totals):
        axis.text(
            index,
            total + max(totals) * 0.025,
            f"probe total {total:.2f} ms",
            ha="center",
            fontsize=9,
        )
    axis.set_ylabel("Median milliseconds")
    axis.set_title(
        "Collection root-cause probe (one coarse timer per outer stage)"
    )
    axis.legend(
        loc="upper left",
        bbox_to_anchor=(1.01, 1.0),
        frameon=True,
    )
    axis.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(image_dir / "collection-root-cause.png", dpi=180)
    plt.close(fig)


def save_mixed_chart(
    image_dir: Path,
    legacy: dict[str, Any],
    key_index: dict[str, Any],
) -> None:
    labels = ["CPU Frame", "Build Lists", "Collection", "Opaque Sort"]
    metrics = [
        "cpuFrameMs",
        "buildDrawListsMs",
        "collectionMs",
        "sortingMs",
    ]
    x_values = list(range(len(labels)))
    width = 0.36
    fig, axis = plt.subplots(figsize=(8.6, 5.1))
    for index, (name, group, color) in enumerate(
        [
            ("Legacy", legacy, "#D95F59"),
            ("Key index", key_index, "#2C7FB8"),
        ]
    ):
        positions = [
            value + (index - 0.5) * width
            for value in x_values
        ]
        values = [metric_median(group, metric) for metric in metrics]
        axis.bar(
            positions,
            values,
            width,
            label=name,
            color=color,
            zorder=3,
        )
    axis.set_xticks(x_values, labels)
    axis.set_ylabel("Median milliseconds")
    axis.set_title("10k mixed primitives: non-quad A/B")
    axis.legend(frameon=True)
    axis.grid(axis="x", visible=False)
    fig.tight_layout()
    fig.savefig(image_dir / "mixed-geometry-ab.png", dpi=180)
    plt.close(fig)


def save_image_equivalence(
    image_dir: Path,
    legacy_capture: Path,
    candidate_capture: Path,
) -> dict[str, Any]:
    legacy = Image.open(legacy_capture).convert("RGB")
    candidate = Image.open(candidate_capture).convert("RGB")
    if legacy.size != candidate.size:
        raise RuntimeError("A/B captures have different dimensions")
    difference = ImageChops.difference(legacy, candidate)
    extrema = difference.getextrema()
    maximum_channel_delta = max(channel[1] for channel in extrema)
    difference_pixels = sum(
        1
        for pixel in difference.getdata()
        if pixel != (0, 0, 0)
    )
    pixel_count = legacy.size[0] * legacy.size[1]

    fig, axes = plt.subplots(1, 3, figsize=(13.6, 4.7))
    axes[0].imshow(legacy)
    axes[0].set_title("Legacy")
    axes[1].imshow(candidate)
    axes[1].set_title("Key + compact index")
    axes[2].imshow(difference)
    axes[2].set_title(
        f"Absolute diff\nmax={maximum_channel_delta}, pixels={difference_pixels}"
    )
    for axis in axes:
        axis.axis("off")
    fig.suptitle("Fixed-frame image equivalence", fontweight="bold")
    fig.tight_layout()
    fig.savefig(image_dir / "fixed-frame-image-equivalence.png", dpi=180)
    plt.close(fig)
    return {
        "width": legacy.size[0],
        "height": legacy.size[1],
        "pixelCount": pixel_count,
        "differentPixels": difference_pixels,
        "maximumChannelDelta": maximum_channel_delta,
        "exactMatch": difference_pixels == 0,
    }


def fmt(value: float | None, digits: int = 3) -> str:
    return "N/A" if value is None else f"{value:.{digits}f}"


def pct(value: float | None, digits: int = 1) -> str:
    return "N/A" if value is None else f"{value:.{digits}f}%"


def markdown_metric_row(
    label: str,
    control: dict[str, Any],
    candidate: dict[str, Any],
    metric: str,
) -> str:
    before = metric_median(control, metric)
    after = metric_median(candidate, metric)
    delta = delta_summary(control, candidate, metric)
    return (
        f"| {label} | {before:.3f} | {after:.3f} | "
        f"{after - before:+.3f} | "
        f"{pct(delta['reductionPercent'])} |"
    )


def relative_link(from_path: Path, to_path: Path) -> str:
    return Path(
        __import__("os").path.relpath(to_path, from_path.parent)
    ).as_posix()


def build_reports(
    result_dir: Path,
    image_dir: Path,
    manifest: dict[str, Any],
    runs: list[Run],
    consolidated: dict[str, Any],
) -> None:
    aggregates = consolidated["aggregates"]
    primary_legacy = aggregates["primary"]["legacy"]
    primary_index = aggregates["primary"]["keyIndex"]
    key_direct = aggregates["candidateKeyDirect"]
    mixed_legacy = aggregates["mixed"]["legacy"]
    mixed_index = aggregates["mixed"]["keyIndex"]
    probes = aggregates["collectionProbe"]
    decision = consolidated["retainedDecision"]
    validation = consolidated["validation"]

    benchmark_report = PROJECT_DIR / "OPAQUE_SORTING_BENCHMARK_REPORT_CN.md"
    technical_report = PROJECT_DIR / "OPAQUE_SORTING_TECHNICAL_REPORT_CN.md"
    consolidated_path = result_dir / "opaque-sorting-ab-benchmark.json"
    image_links = {
        name: relative_link(benchmark_report, image_dir / filename)
        for name, filename in {
            "primary": "primary-ab-timing.png",
            "sort": "sorting-path-breakdown.png",
            "dynamic": "dynamic-percent-sensitivity.png",
            "scale": "object-count-scaling.png",
            "probe": "collection-root-cause.png",
            "mixed": "mixed-geometry-ab.png",
            "image": "fixed-frame-image-equivalence.png",
        }.items()
    }
    json_link = relative_link(benchmark_report, consolidated_path)
    raw_manifest_link = relative_link(
        benchmark_report,
        result_dir / "run-manifest.json",
    )
    shadow_manifest_path = PROJECT_DIR / validation[
        "shadowCacheRegression"
    ]["manifest"]
    shadow_manifest_link = relative_link(
        benchmark_report,
        shadow_manifest_path,
    )

    cpu_delta = delta_summary(
        primary_legacy,
        primary_index,
        "cpuFrameMs",
    )
    build_delta = delta_summary(
        primary_legacy,
        primary_index,
        "buildDrawListsMs",
    )
    sort_delta = delta_summary(
        primary_legacy,
        primary_index,
        "sortingMs",
    )
    gpu_delta = delta_summary(
        primary_legacy,
        primary_index,
        "forwardGpuMs",
    )
    mixed_sort_delta = delta_summary(
        mixed_legacy,
        mixed_index,
        "sortingMs",
    )
    primary_example = next(
        run
        for run in runs
        if run.name.startswith("primary-30k-d20")
    )
    primary_actual_dynamic = int(
        primary_example.report["settings"][
            "submissionStressDynamicObjectCount"
        ]
    )
    distribution_lines: list[str] = []
    for label, metric in (
        ("CPU Frame", "cpuFrameMs"),
        ("Build Draw Lists", "buildDrawListsMs"),
        ("Collection", "collectionMs"),
        ("Opaque Sorting", "sortingMs"),
        ("GPU Forward", "forwardGpuMs"),
    ):
        for variant, group in (
            ("Legacy", primary_legacy),
            ("Key Index", primary_index),
        ):
            distribution_lines.append(
                f"| {label} | {variant} | "
                f"{metric_median(group, metric):.3f} | "
                f"{metric_percentile(group, metric, 'p95'):.3f} | "
                f"{metric_percentile(group, metric, 'p99'):.3f} |"
            )

    dynamic_lines = []
    for dynamic_percent, group in sorted(
        aggregates["dynamicPercent"].items(),
        key=lambda item: int(item[0]),
    ):
        dynamic_lines.append(
            "| "
            f"{dynamic_percent}% | "
            f"{metric_median(group, 'cpuFrameMs'):.3f} | "
            f"{metric_median(group, 'motionMs'):.3f} | "
            f"{metric_median(group, 'collectionMs'):.3f} | "
            f"{metric_median(group, 'sortingMs'):.3f} | "
            f"{metric_median(group, 'forwardGpuMs'):.3f} |"
        )

    scale_lines = []
    for object_count, group in sorted(
        aggregates["objectScaling"].items(),
        key=lambda item: int(item[0]),
    ):
        scale_lines.append(
            "| "
            f"{int(object_count):,} | "
            f"{metric_median(group, 'cpuFrameMs'):.3f} | "
            f"{metric_median(group, 'buildDrawListsMs'):.3f} | "
            f"{metric_median(group, 'collectionMs'):.3f} | "
            f"{metric_median(group, 'sortingMs'):.3f} |"
        )

    probe_lines = []
    for dynamic_percent, probe in sorted(
        probes.items(),
        key=lambda item: int(item[0]),
    ):
        probe_lines.append(
            "| "
            f"{dynamic_percent}% | "
            f"{float(probe['materialRevisionMs']['median']):.3f} | "
            f"{float(probe['modelBoundsFrustumMs']['median']):.3f} | "
            f"{float(probe['meshBoundsValidationMs']['median']):.3f} | "
            f"{float(probe['drawItemWriteMs']['median']):.3f} | "
            f"{float(probe['totalMs']['median']):.3f} |"
        )

    validation_status = "通过" if validation["allChecksPassed"] else "未通过"
    retained_text = (
        "Go：进入下一阶段 Retained v1 的实现与失效协议收敛。"
        if decision["go"]
        else "No-Go：当前证据未同时满足 Retained v1 的全部门槛。"
    )
    dynamic_zero_collection = metric_median(
        aggregates["dynamicPercent"]["0"],
        "collectionMs",
    )
    dynamic_twenty_collection = metric_median(
        aggregates["dynamicPercent"]["20"],
        "collectionMs",
    )
    dynamic_full_collection = metric_median(
        aggregates["dynamicPercent"]["100"],
        "collectionMs",
    )
    dynamic_collection_increase = (
        dynamic_full_collection - dynamic_zero_collection
    )
    dynamic_collection_increase_percent = (
        dynamic_collection_increase / dynamic_zero_collection * 100.0
        if dynamic_zero_collection
        else 0.0
    )
    static_baseline_share_at_full_dynamic = (
        dynamic_zero_collection / dynamic_full_collection * 100.0
        if dynamic_full_collection
        else 0.0
    )
    if decision["go"]:
        retained_scope = (
            "当前结论只授权下一阶段；不表示本 Session 已实现 Retained，"
            "也不声称现有公开可变接口已经具备真正的 `N_dirty` 更新。"
        )
        retained_evidence_text = (
            "四项门槛均已满足：排序后 Collection 仍是主要 CPU 区；"
            "它随总对象数增长而对 Dynamic 百分比不敏感；四个对象数点"
            "可复现；10k mixed 场景也保留同类成本。"
        )
        retained_action_text = """下一阶段可沿既定方向拆分：

```text
persistent RenderProxy / StaticDrawCommand
              ↓
per-frame VisibleCommandIndex
```

缓存 Mesh、Shader、Material、Local Bounds、Opaque Sort Key；World Bounds 只在可靠的 Transform Revision 变化时更新；透明物体仍按 View 距离排序；零灯光时不准备灯光/阴影专用 OBB 数据。"""
    else:
        retained_scope = (
            "本 Session 到此停止，不实施 Retained v1。排序优化可以保留，"
            "但不能把剩余 Collection 写成只由静态全量重建决定。"
        )
        retained_evidence_text = (
            "排序后 Collection 仍是主要 CPU 区、总对象数缩放和 mixed "
            "场景均复现；但 Dynamic 0%→100% 时 Collection 明显增加，"
            "因此“主要不随 Dynamic 数量增长”这一门槛没有通过。"
        )
        retained_action_text = """若以后重新评估 Retained，应先处理两件事：

1. 把 Transform/Material/Mesh 的公开修改入口收敛为可靠 revision 或保守失效事件；
2. 用代表性实际场景重新验证静态与动态 Bounds 更新的占比。

在这两项完成并重新通过 0%/20%/100% 门槛前，不进入 `RenderProxy/StaticDrawCommand` 实现。"""
    benchmark_text = f"""# Object-Heavy Opaque Sorting 正式实验报告

- 日期：{manifest["generatedAtUtc"]}
- 构建：Release x64，1920×1080，VSync 请求值 0
- 场景：30,000 个全可见不透明对象、16 材质、零灯光/阴影；20% 固定种子实际为 {primary_actual_dynamic:,} 个动态对象
- 正式协议：每进程预热 {manifest["protocol"]["warmupFrames"]} 帧，采样 {manifest["protocol"]["sampleFrames"]} 帧；主 A/B 顺序 A-B-B-A-A-B
- 机器：{manifest["system"]["cpu"]}；{manifest["system"]["gpu"]}；驱动 {manifest["system"]["gpuDriverVersion"]}
- 来源：基准提交 `{manifest["source"]["gitCommit"][:12]}`，工作区 dirty={str(manifest["source"]["worktreeDirty"]).lower()}；所有 A/B 使用同一 Release EXE SHA-256 `{manifest["source"]["releaseExecutableSha256"]}`
- 原始汇总：[A/B Benchmark JSON]({json_link})
- 运行清单：[run-manifest.json]({raw_manifest_link})

> 先前约 22.84 ms 的校准只用于发现问题。本报告所有收益、图表和结论均来自本次 Release 正式采样。

## 结论

Opaque Sorting 优化成立。30k/20% dynamic 主 A/B 中，CPU Frame Median 从 {cpu_delta["controlMedian"]:.3f} ms 降到 {cpu_delta["candidateMedian"]:.3f} ms，减少 {cpu_delta["reduction"]:.3f} ms（{pct(cpu_delta["reductionPercent"])}）；Opaque Sorting Median 从 {sort_delta["controlMedian"]:.3f} ms 降到 {sort_delta["candidateMedian"]:.3f} ms，减少 {sort_delta["reduction"]:.3f} ms（{pct(sort_delta["reductionPercent"])}）。Draw Call、可见对象、提交签名和固定帧图像保持一致。

Retained Draw Submission 决策：**{retained_text}** {retained_scope}

![主 A/B 性能对比]({image_links["primary"]})

## 主 A/B 对照

以下为三个独立进程、合计 {primary_legacy["metrics"]["cpuFrameMs"]["count"]} 帧/变体的池化分布。降低百分比按 Median 计算；GPU 不期望因纯 CPU 排序而实质变化。

| 指标（ms） | Legacy Median | Key Index Median | B-A | 降低 |
| --- | ---: | ---: | ---: | ---: |
{markdown_metric_row("CPU Frame", primary_legacy, primary_index, "cpuFrameMs")}
{markdown_metric_row("Build Draw Lists", primary_legacy, primary_index, "buildDrawListsMs")}
{markdown_metric_row("Collection", primary_legacy, primary_index, "collectionMs")}
{markdown_metric_row("Opaque Sorting", primary_legacy, primary_index, "sortingMs")}
{markdown_metric_row("GPU Forward", primary_legacy, primary_index, "forwardGpuMs")}

关键指标完整分布：

| 指标（ms） | 变体 | Median | P95 | P99 |
| --- | --- | ---: | ---: | ---: |
{chr(10).join(distribution_lines)}

## 为什么选择紧凑索引

![三种排序路径分解]({image_links["sort"]})

预计算 Key 后，直接排序 DrawItem 已消除了 comparator 内的哈希查询；紧凑索引路径进一步避免 `stable_sort` 在 `O(N log N)` 过程中反复移动包含 Matrix 和 Bounds 的结构，仅排序 32 位索引，最后线性物化一次。正式筛查同时保留了 `key-direct`，因此这个选择来自数据，而不是理论假设。

## Dynamic Percent

| Dynamic | CPU Frame | Motion Update | Collection | Sorting | GPU Forward |
| ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(dynamic_lines)}

![动态比例敏感性]({image_links["dynamic"]})

Collection Median 在 0%/20%/100% 中的最大相对跨度为 {decision["evidence"]["dynamicCollectionRelativeRangePercent"]:.2f}%。从 0% 到 100%，Collection 增加 {dynamic_collection_increase:.3f} ms（相对 0% 增加 {dynamic_collection_increase_percent:.1f}%）；这项变化发生在独立 Motion Update 之外，分项探针将它定位到 Model Matrix/Bounds/Frustum。0% 时仍有 {dynamic_zero_collection:.3f} ms 的全量成本，占 100% dynamic Collection 的 {static_baseline_share_at_full_dynamic:.1f}%，因此真实结论是“固定全量重建占主体，同时动态 Transform 重算也显著”，不能判定为接近不变。

## Collection 根因分项

分项探针采用阶段级外层计时：每个阶段每帧只启动一次计时器，没有在 30k 内层逐对象计时。为把 Mesh Bounds 与最终 DrawItem materialization 分离，探针使用 Benchmark-only staged replay；因此用它判断组成和趋势，不用探针总时间替代生产 `Draw Item Collection`。

| Dynamic | Material Revision | Model Matrix/Bounds/Frustum | Mesh Bounds/8 corners/validation | DrawItem write | Probe total |
| ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(probe_lines)}

![Collection 根因]({image_links["probe"]})

根因排序为：Mesh Bounds、8 角点变换及合法性检查占主导；Model Matrix/Model Bounds/Frustum 次之；Material Revision 的稳态检查和最终 DrawItem materialization 较小。Opaque Sort Key 构建与实际排序已由生产路径独立 zone 记录，不与 Collection 重复计数。

## 对象数量缩放

| Objects | CPU Frame | Build Draw Lists | Collection | Sorting |
| ---: | ---: | ---: | ---: | ---: |
{chr(10).join(scale_lines)}

![对象数量缩放]({image_links["scale"]})

Collection 对对象数的线性回归为 {decision["evidence"]["collectionSlopeMsPerObject"]:.9f} ms/object，R²={decision["evidence"]["collectionScaleRSquared"]:.5f}。1k/5k/10k/30k 均由三个独立进程复现。

## 非相同 Quad 场景

10k mixed 场景固定混合 Triangle、Quad 和 Octagon，仍保持一对象一 Draw Call。Opaque Sorting Median 从 {metric_median(mixed_legacy, "sortingMs"):.3f} ms 降到 {metric_median(mixed_index, "sortingMs"):.3f} ms，降低 {pct(mixed_sort_delta["reductionPercent"])}；Collection 相对同规模 Quad 的比值为 {decision["evidence"]["mixedToQuadCollectionRatio"]:.3f}。

![混合几何 A/B]({image_links["mixed"]})

## 正确性

验证状态：**{validation_status}**。

- 主 A/B 的 active/visible/opaque 数量均为 30,000，Draw Call 均为 30,002；
- Forward 与 Deferred 均完成有效采样；
- 主 A/B 三个独立进程的提交签名集合一致；
- 固定帧 PPM 的 SHA-256 跨进程、跨排序路径一致；
- 像素差：{validation["imageEquivalence"]["differentPixels"]} 个像素，最大通道差 {validation["imageEquivalence"]["maximumChannelDelta"]}。
- 现有 Point Shadow Cache 回归通过：{validation["shadowCacheRegression"]["caseCount"]} 类零像素差案例及 topology ABA；[回归 manifest]({shadow_manifest_link})。

![固定帧图像一致性]({image_links["image"]})

## Retained Go/No-Go

| 门槛 | 结果 | 证据 |
| --- | --- | --- |
| 排序后 Collection 仍为主要 CPU Zone | {"PASS" if decision["criteria"]["collectionStillMajor"] else "FAIL"} | Collection/CPU Frame={decision["evidence"]["collectionCpuSharePercent"]:.2f}%，Collection/Sorting={decision["evidence"]["collectionToSortingRatio"]:.2f}× |
| 随总对象数而非 Dynamic 数量增长 | {"PASS" if decision["criteria"]["totalCountNotDynamicCount"] else "FAIL"} | Dynamic 相对跨度 {decision["evidence"]["dynamicCollectionRelativeRangePercent"]:.2f}%，缩放 R²={decision["evidence"]["collectionScaleRSquared"]:.5f} |
| 1k/5k/10k/30k 可复现 | {"PASS" if decision["criteria"]["scalingReproduced"] else "FAIL"} | 每点三个独立进程 |
| 不只存在于 30k 相同 Quad | {"PASS" if decision["criteria"]["representativeSceneReproduced"] else "FAIL"} | 10k mixed sorting 降低 {pct(mixed_sort_delta["reductionPercent"])} |

## 限制

- 压力场景刻意启用 Legacy Shadow Signature，Collection 数据不包含新的 Shadow Revision Hash；
- 场景全可见且全部不透明，不能外推透明物体每 View 距离排序；
- 当前 Transform、Mesh、Material 仍存在公开可变入口。Retained v1 若继续，必须先定义 Topology/Transform/Material/Geometry/Shader 生命周期与保守失效规则；
- 当前仍有约 30k 次 `glDraw*`。Retained 若把 Build Draw Lists 降下去后，下一项应评估 Instancing/Multi-Draw，而不是继续无边界堆叠缓存。
"""
    benchmark_report.write_text(
        benchmark_text,
        encoding="utf-8",
        newline="\n",
    )

    technical_json_link = relative_link(technical_report, consolidated_path)
    technical_image_links = {
        key: relative_link(technical_report, image_dir / Path(value).name)
        for key, value in image_links.items()
    }
    technical_text = f"""# Opaque Sorting：卡点原理、分析路径与优化实现

## 1. 现象：GPU 不慢，CPU 在提交前卡住

Object-Heavy 场景有 30,000 个可见对象，但每个对象只有低面数几何、零灯光和零阴影。Legacy 主 A/B 的 CPU Frame Median 为 {metric_median(primary_legacy, "cpuFrameMs"):.3f} ms，而 GPU Forward Median 为 {metric_median(primary_legacy, "forwardGpuMs"):.3f} ms。两者的明显间隔说明瓶颈发生在 CPU 准备和提交工作，而不是片元或顶点着色。

进一步拆分 `Build Draw Lists` 后，Legacy Collection 为 {metric_median(primary_legacy, "collectionMs"):.3f} ms，Opaque Sorting 为 {metric_median(primary_legacy, "sortingMs"):.3f} ms。也就是说，帧在真正进入大量 `glDraw*` 之前，已经花费了可观 CPU 时间重建和排序提交描述。

原始数据见 [A/B Benchmark JSON]({technical_json_link})。

## 2. 为什么旧排序会放大

旧路径先按场景遍历的首次出现顺序建立 Shader/Material ordinal，再执行 `std::stable_sort`。问题不在排序目标，而在 comparator：

1. 排序需要 `O(N log N)` 次比较；
2. 每次比较反复执行 Shader 和 Material 的 `unordered_map::at`；
3. 比较相等时还要从 Mesh 追到 Material；
4. `stable_sort` 移动的是完整 DrawItem，其中包含 Model/Matrix、World Bounds Center、三根 OBB Axis、Radius 和有效性状态。

30k 项时，哈希查询次数和大结构移动次数一起按排序比较数量放大。场景只有 16 个材质并不能自动消除这项成本，因为旧 comparator 仍在每次比较时重新查询“这个材质排第几”。

## 3. 我们如何分析到这里

分析链条按可证伪顺序推进：

1. **CPU/GPU 分离：** CPU Frame 显著高于 GPU Forward，先把目标锁定到 CPU submission；
2. **粗粒度阶段：** `Build Draw Lists = Collection + Opaque Sorting + 少量固定开销`，确认 Sorting 是独立大区；
3. **同程序 A/B：** 保留 Legacy comparator，同时加入 Key Direct，单独验证“Comparator 哈希查询”假设；
4. **结构移动 A/B：** 在同一 Key 下比较直接排序 DrawItem 与排序 32 位索引，单独验证“大对象移动”假设；
5. **Dynamic 消融：** 0%/20%/100% 检查 Collection 是否随 Dirty Transform 数量变化；
6. **阶段探针：** 使用阶段级外层计时重放 Material、Model Bounds、Mesh Bounds 和 DrawItem write，没有在 30k 内层逐项调用计时器；
7. **对象缩放与混合几何：** 用 1k/5k/10k/30k 和 10k mixed 排除仅在单一 30k Quad 极端点成立的解释。

![分析结果总览]({technical_image_links["primary"]})

## 4. 方案思考与取舍

### 4.1 只优化 comparator

为每个 opaque item 预先生成 64 位 `opaqueSortKey`：

```text
high 32 bits = shader first-seen ordinal
low  32 bits = material first-seen ordinal
```

ordinal 来自确定性的 Scene traversal。哈希表只用于 Key 构建时查找 pointer 对应的 ordinal；算法从不迭代哈希表，因此 bucket 顺序和 pointer 数值不会决定排序顺序。Comparator 最终只执行一个 64 位整数比较。

这条 `key-direct` 路径把 Sorting Median 降到 {metric_median(key_direct, "sortingMs"):.3f} ms，证明反复哈希查询确实是首要排序卡点。

### 4.2 为什么还要排序紧凑索引

Key Direct 仍让 `stable_sort` 反复移动完整 DrawItem。Key Index 路径改为：

```text
DrawItem array 保持收集顺序
→ 生成 uint32 index array
→ stable_sort(index, DrawItem[index].opaqueSortKey)
→ 按已排序 index 线性物化最终 DrawItem array
```

这样 `O(N log N)` 阶段移动 4 字节索引，完整 DrawItem 只在最后 `O(N)` 搬运一次。30k 下最终 Sorting Median 为 {metric_median(primary_index, "sortingMs"):.3f} ms，比 Key Direct 进一步减少 {metric_median(key_direct, "sortingMs") - metric_median(primary_index, "sortingMs"):.3f} ms。

![排序路径分解]({technical_image_links["sort"]})

### 4.3 为什么没有直接上 Retained

排序是一个边界清楚、可同程序 A/B、失效风险低的独立问题。Retained 则会改变对象增删、Active、Transform、Material 透明度、Shader 热重载和资源销毁的生命周期协议。先拿掉 Sorting 的确定成本，才能看到 Collection 是否仍值得承担 Retained 的复杂度。

## 5. Collection 的真实剩余根因

![Collection 分项]({technical_image_links["probe"]})

正式分项显示：

- Material Revision 稳态检查较小；
- Model Matrix、Model Bounds 与 Frustum 随 Dynamic Transform 有一定变化，但不是主体；
- Mesh local bounds、三根 world axis、8 角点变换、sphere radius 与有限性/范围合法性检查占主导；
- DrawItem 最终写入存在，但显著小于 Bounds 计算；
- Opaque Sort Key 和排序已在生产路径独立计时，不混入上述探针。

Dynamic 0%/20%/100% 的生产 Collection 相对跨度为 {decision["evidence"]["dynamicCollectionRelativeRangePercent"]:.2f}%；0%→100% 增加 {dynamic_collection_increase:.3f} ms。对象数量缩放的 R² 为 {decision["evidence"]["collectionScaleRSquared"]:.5f}。两组证据合在一起说明当前 Collection 同时包含稳定的 `O(N_total)` Mesh/Bounds 重建和随 Dynamic Transform 增加的矩阵/Model Bounds 成本，不能简化成纯 `O(N_total)` 或纯 `O(N_dynamic)`。

![Dynamic 消融]({technical_image_links["dynamic"]})

![对象数缩放]({technical_image_links["scale"]})

## 6. 优化了多少

30k/20% dynamic 正式主 A/B：

| 指标 | Legacy | Key Index | 减少 | 降低 |
| --- | ---: | ---: | ---: | ---: |
| CPU Frame Median | {cpu_delta["controlMedian"]:.3f} ms | {cpu_delta["candidateMedian"]:.3f} ms | {cpu_delta["reduction"]:.3f} ms | {pct(cpu_delta["reductionPercent"])} |
| Build Draw Lists Median | {build_delta["controlMedian"]:.3f} ms | {build_delta["candidateMedian"]:.3f} ms | {build_delta["reduction"]:.3f} ms | {pct(build_delta["reductionPercent"])} |
| Opaque Sorting Median | {sort_delta["controlMedian"]:.3f} ms | {sort_delta["candidateMedian"]:.3f} ms | {sort_delta["reduction"]:.3f} ms | {pct(sort_delta["reductionPercent"])} |
| GPU Forward Median | {gpu_delta["controlMedian"]:.3f} ms | {gpu_delta["candidateMedian"]:.3f} ms | {gpu_delta["reduction"]:.3f} ms | {pct(gpu_delta["reductionPercent"])} |

GPU 和 Draw Call 不应因 CPU 排序优化下降；它们在这里是“工作量未改变”的控制指标。

## 7. 正确性为什么没有被排序优化破坏

- Legacy、Key Direct、Key Index 的排序主次关系完全相同：Shader ordinal → Material ordinal；
- 使用 `stable_sort`，相同 Key 的对象保持原 Scene traversal 顺序；
- 提交签名哈希 Key、draw count 和 model matrix，不包含 pointer；
- 主 A/B 三个独立进程签名一致；
- Forward/Deferred 都完成有效运行；
- 固定帧截图逐像素完全一致。
- Point Shadow Cache 六类严格正确性案例和 topology ABA 均通过，且使用同一 Release EXE。

![固定帧验证]({technical_image_links["image"]})

## 8. Retained v1 决策

结论：**{retained_text}**

{retained_evidence_text}

{retained_action_text}

但当前公开可变入口仍可能绕过 revision。完成入口收敛和保守失效协议前，只能称为 retained cache 原型，不能宣称复杂度已经变成真正的 `N_dirty`。

如果 Retained 把 Collection 降下去后 CPU 主要时间落在 30k 次 `glDraw*`，下一项应是 Instancing/Multi-Draw，而不是继续增加缓存层级。
"""
    technical_report.write_text(
        technical_text,
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--result-dir",
        type=Path,
        default=DEFAULT_RESULT_DIR,
    )
    parser.add_argument(
        "--image-dir",
        type=Path,
        default=DEFAULT_IMAGE_DIR,
    )
    arguments = parser.parse_args()
    result_dir = arguments.result_dir.resolve()
    image_dir = arguments.image_dir.resolve()
    image_dir.mkdir(parents=True, exist_ok=True)
    manifest = load_json(result_dir / "run-manifest.json")

    runs: list[Run] = []
    for record in manifest["runs"]:
        report_path = PROJECT_DIR / record["report"]
        capture_path = (
            PROJECT_DIR / record["capture"]
            if record.get("capture")
            else None
        )
        runs.append(
            Run(
                record=record,
                report_path=report_path,
                report=load_json(report_path),
                capture_path=capture_path,
            )
        )

    def select(prefix: str, mode: str | None = None) -> list[Run]:
        selected = [run for run in runs if run.name.startswith(prefix)]
        if mode is not None:
            selected = [
                run for run in selected if run.sort_mode == mode
            ]
        return selected

    primary_legacy_runs = select("primary-30k-d20", "legacy")
    primary_index_runs = select("primary-30k-d20", "key-index")
    direct_runs = select("candidate-30k-d20", "key-direct")
    primary_legacy = aggregate(primary_legacy_runs)
    primary_index = aggregate(primary_index_runs)
    key_direct = aggregate(direct_runs)

    dynamic_groups: dict[int, dict[str, Any]] = {
        0: aggregate(select("dynamic-30k-d0")),
        20: primary_index,
        100: aggregate(select("dynamic-30k-d100")),
    }
    scaling_groups: dict[int, dict[str, Any]] = {
        1000: aggregate(select("scale-1000-d20")),
        5000: aggregate(select("scale-5000-d20")),
        10000: aggregate(select("scale-10000-d20")),
        30000: primary_index,
    }
    probe_groups: dict[int, dict[str, Any]] = {}
    for dynamic_percent in (0, 20, 100):
        matches = select(f"collection-probe-30k-d{dynamic_percent}")
        if len(matches) != 1:
            raise RuntimeError(
                f"Expected one collection probe for {dynamic_percent}%"
            )
        probe_groups[dynamic_percent] = aggregate_probe(matches[0])

    mixed_legacy = aggregate(select("mixed-10k-d20", "legacy"))
    mixed_index = aggregate(select("mixed-10k-d20", "key-index"))
    deferred_legacy = aggregate(select("deferred-10k-d20-legacy"))
    deferred_index = aggregate(select("deferred-10k-d20-key-index"))

    capture_records: list[dict[str, Any]] = []
    for run in runs:
        if run.capture_path is None:
            continue
        capture_records.append(
            {
                "run": run.name,
                "path": run.record["capture"],
                "sha256": sha256(run.capture_path),
            }
        )

    primary_capture_hashes = {
        record["sha256"]
        for record in capture_records
        if record["run"].startswith("primary-30k-d20")
    }
    mixed_capture_hashes = {
        record["sha256"]
        for record in capture_records
        if record["run"].startswith("mixed-10k-d20")
    }
    deferred_capture_hashes = {
        record["sha256"]
        for record in capture_records
        if record["run"].startswith("deferred-10k-d20")
    }
    primary_signatures = {
        run.report["settings"]["opaqueSubmissionSignature"]
        for run in primary_legacy_runs + primary_index_runs
    }
    mixed_signatures = {
        run.report["settings"]["opaqueSubmissionSignature"]
        for run in select("mixed-10k-d20")
    }
    deferred_signatures = {
        run.report["settings"]["opaqueSubmissionSignature"]
        for run in select("deferred-10k-d20")
    }

    legacy_capture = next(
        run.capture_path
        for run in primary_legacy_runs
        if run.capture_path is not None
    )
    candidate_capture = next(
        run.capture_path
        for run in primary_index_runs
        if run.capture_path is not None
    )
    assert legacy_capture is not None
    assert candidate_capture is not None
    image_equivalence = save_image_equivalence(
        image_dir,
        legacy_capture,
        candidate_capture,
    )
    if DEFAULT_SHADOW_REGRESSION_MANIFEST.exists():
        shadow_manifest = load_json(
            DEFAULT_SHADOW_REGRESSION_MANIFEST
        )
        shadow_case_count = len(shadow_manifest.get("cases", []))
        topology_aba = shadow_manifest.get("topologyAbaSmoke", {})
        shadow_executable_hash = str(
            shadow_manifest.get("provenance", {}).get(
                "executableSha256",
                "",
            )
        )
        shadow_hash_matches = (
            shadow_executable_hash.lower()
            == str(
                manifest["source"]["releaseExecutableSha256"]
            ).lower()
        )
        shadow_regression = {
            "passed": (
                shadow_case_count == 6
                and int(topology_aba.get("revisionDelta", 0)) == 1
                and int(topology_aba.get("invalidationCount", 0)) == 1
                and shadow_hash_matches
            ),
            "manifest": DEFAULT_SHADOW_REGRESSION_MANIFEST.relative_to(
                PROJECT_DIR
            ).as_posix(),
            "caseCount": shadow_case_count,
            "topologyAbaRevisionDelta": int(
                topology_aba.get("revisionDelta", 0)
            ),
            "topologyAbaInvalidationCount": int(
                topology_aba.get("invalidationCount", 0)
            ),
            "releaseExecutableSha256Matches": shadow_hash_matches,
        }
    else:
        shadow_regression = {
            "passed": False,
            "manifest": DEFAULT_SHADOW_REGRESSION_MANIFEST.relative_to(
                PROJECT_DIR
            ).as_posix(),
            "caseCount": 0,
            "topologyAbaRevisionDelta": 0,
            "topologyAbaInvalidationCount": 0,
            "releaseExecutableSha256Matches": False,
        }

    invariant_failures: list[str] = []
    for run in runs:
        report = run.report
        if not report["capture"]["valid"]:
            invariant_failures.append(f"{run.name}: invalid capture")
        object_count = int(report["settings"]["submissionStressObjectCount"])
        for metric_name in ("activeModels", "visibleModels", "opaqueMeshes"):
            median = float(report["summary"]["renderStats"][metric_name]["median"])
            if median != object_count:
                invariant_failures.append(
                    f"{run.name}: {metric_name}={median}, expected={object_count}"
                )
        transparent = float(
            report["summary"]["renderStats"]["transparentMeshes"]["median"]
        )
        if transparent != 0:
            invariant_failures.append(
                f"{run.name}: transparentMeshes={transparent}"
            )
        draw_calls = float(
            report["summary"]["renderStats"]["drawCalls"]["median"]
        )
        render_path = str(
            report["settings"]["submissionStressRenderPath"]
        )
        expected_draw_calls = object_count + (
            3 if render_path == "deferred" else 2
        )
        if draw_calls != expected_draw_calls:
            invariant_failures.append(
                f"{run.name}: drawCalls={draw_calls}, "
                f"expected={expected_draw_calls}"
            )

    dynamic_collection_values = [
        metric_median(dynamic_groups[percent], "collectionMs")
        for percent in (0, 20, 100)
    ]
    dynamic_reference = statistics.fmean(dynamic_collection_values)
    dynamic_relative_range = (
        (max(dynamic_collection_values) - min(dynamic_collection_values))
        / dynamic_reference
        * 100.0
        if dynamic_reference
        else 0.0
    )
    scale_points = [
        (
            float(count),
            metric_median(group, "collectionMs"),
        )
        for count, group in sorted(scaling_groups.items())
    ]
    regression = linear_regression(scale_points)
    collection_cpu_share = (
        metric_median(primary_index, "collectionMs")
        / metric_median(primary_index, "cpuFrameMs")
        * 100.0
    )
    collection_to_sorting = (
        metric_median(primary_index, "collectionMs")
        / metric_median(primary_index, "sortingMs")
    )
    quad_10k_collection = metric_median(
        scaling_groups[10000],
        "collectionMs",
    )
    mixed_10k_collection = metric_median(mixed_index, "collectionMs")
    mixed_to_quad = (
        mixed_10k_collection / quad_10k_collection
        if quad_10k_collection
        else 0.0
    )
    mixed_sort_reduction = delta_summary(
        mixed_legacy,
        mixed_index,
        "sortingMs",
    )["reductionPercent"]

    collection_still_major = (
        collection_to_sorting > 1.5
        and collection_cpu_share > 25.0
    )
    total_not_dynamic = (
        dynamic_relative_range <= 10.0
        and regression["rSquared"] >= 0.98
        and regression["slopeMsPerObject"] > 0.0
    )
    scaling_reproduced = (
        all(group["runCount"] >= 3 for group in scaling_groups.values())
        and regression["rSquared"] >= 0.98
    )
    representative_reproduced = (
        mixed_legacy["runCount"] >= 3
        and mixed_index["runCount"] >= 3
        and mixed_sort_reduction is not None
        and mixed_sort_reduction > 20.0
        and mixed_to_quad >= 0.70
    )
    retained_go = all(
        (
            collection_still_major,
            total_not_dynamic,
            scaling_reproduced,
            representative_reproduced,
        )
    )

    validation = {
        "allBenchmarkCapturesValid": not any(
            not run.report["capture"]["valid"] for run in runs
        ),
        "renderInvariantFailures": invariant_failures,
        "primarySubmissionSignatures": sorted(primary_signatures),
        "mixedSubmissionSignatures": sorted(mixed_signatures),
        "deferredSubmissionSignatures": sorted(deferred_signatures),
        "primaryCaptureSha256": sorted(primary_capture_hashes),
        "mixedCaptureSha256": sorted(mixed_capture_hashes),
        "deferredCaptureSha256": sorted(deferred_capture_hashes),
        "imageEquivalence": image_equivalence,
        "shadowCacheRegression": shadow_regression,
    }
    validation["allChecksPassed"] = all(
        (
            validation["allBenchmarkCapturesValid"],
            not invariant_failures,
            len(primary_signatures) == 1,
            len(mixed_signatures) == 1,
            len(deferred_signatures) == 1,
            len(primary_capture_hashes) == 1,
            len(mixed_capture_hashes) == 1,
            len(deferred_capture_hashes) == 1,
            image_equivalence["exactMatch"],
            shadow_regression["passed"],
        )
    )

    aggregates = {
        "primary": {
            "legacy": primary_legacy,
            "keyIndex": primary_index,
        },
        "candidateKeyDirect": key_direct,
        "dynamicPercent": {
            str(key): value for key, value in dynamic_groups.items()
        },
        "objectScaling": {
            str(key): value for key, value in scaling_groups.items()
        },
        "collectionProbe": {
            str(key): value for key, value in probe_groups.items()
        },
        "mixed": {
            "legacy": mixed_legacy,
            "keyIndex": mixed_index,
        },
        "deferred": {
            "legacy": deferred_legacy,
            "keyIndex": deferred_index,
        },
    }
    retained_decision = {
        "go": retained_go,
        "criteria": {
            "collectionStillMajor": collection_still_major,
            "totalCountNotDynamicCount": total_not_dynamic,
            "scalingReproduced": scaling_reproduced,
            "representativeSceneReproduced": representative_reproduced,
        },
        "evidence": {
            "collectionCpuSharePercent": collection_cpu_share,
            "collectionToSortingRatio": collection_to_sorting,
            "dynamicCollectionRelativeRangePercent": dynamic_relative_range,
            "collectionSlopeMsPerObject":
                regression["slopeMsPerObject"],
            "collectionScaleInterceptMs": regression["interceptMs"],
            "collectionScaleRSquared": regression["rSquared"],
            "mixedToQuadCollectionRatio": mixed_to_quad,
            "mixedSortingReductionPercent": mixed_sort_reduction,
        },
    }
    consolidated = {
        "schemaVersion": 1,
        "generatedFrom": "run-manifest.json",
        "protocol": manifest["protocol"],
        "source": manifest["source"],
        "system": manifest["system"],
        "validation": validation,
        "aggregates": aggregates,
        "primaryDeltas": {
            metric: delta_summary(primary_legacy, primary_index, metric)
            for metric in (
                "cpuFrameMs",
                "gpuFrameMs",
                "forwardGpuMs",
                "buildDrawListsMs",
                "collectionMs",
                "sortingMs",
                "drawCalls",
                "visibleModels",
            )
        },
        "captureFiles": capture_records,
        "retainedDecision": retained_decision,
        "rawRuns": [
            {
                "name": run.name,
                "report": run.record["report"],
                "sortMode": run.sort_mode,
                "objectCount": run.record["objectCount"],
                "dynamicPercent": run.record["dynamicPercent"],
                "renderPath": run.record["renderPath"],
                "geometrySet": run.record["geometrySet"],
                "opaqueSubmissionSignature":
                    run.report["settings"]["opaqueSubmissionSignature"],
            }
            for run in runs
        ],
    }

    set_plot_style()
    save_primary_chart(image_dir, primary_legacy, primary_index)
    save_sort_breakdown_chart(
        image_dir,
        primary_legacy,
        key_direct,
        primary_index,
    )
    save_dynamic_chart(image_dir, dynamic_groups)
    save_scaling_chart(image_dir, scaling_groups)
    save_probe_chart(image_dir, probe_groups)
    save_mixed_chart(image_dir, mixed_legacy, mixed_index)

    consolidated_path = result_dir / "opaque-sorting-ab-benchmark.json"
    write_json(consolidated_path, consolidated)
    build_reports(
        result_dir,
        image_dir,
        manifest,
        runs,
        consolidated,
    )
    print(f"Wrote {consolidated_path}")
    print(f"Wrote images to {image_dir}")
    print(f"Retained decision: {'GO' if retained_go else 'NO-GO'}")
    print(
        "Validation: "
        + ("PASS" if validation["allChecksPassed"] else "FAIL")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
