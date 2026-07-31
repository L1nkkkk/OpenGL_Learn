#!/usr/bin/env python3
"""Summarize a Test-ShadowOptimizations.ps1 experiment without modifying it."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, Callable, Iterable


RUN_NAMES = ("A1", "A2", "A3", "B1", "B2", "B3")
VARIANTS = ("A", "B")
RESOURCE_FIELDS = {
    "textureBytes": "texture",
    "meshCpuBytes": "meshCpu",
    "meshGpuBytes": "meshGpu",
    "renderTargetBytes": "renderTarget",
}
SHADOW_INVARIANT_FIELDS = (
    "workload",
    "mode",
    "lights",
    "legacyCacheSignatureEnabled",
    "perLightCacheEnabled",
    "optimizationFlags",
    "exactEarlyOutEnabled",
    "preparedPointInputsEnabled",
    "adaptivePointSamplesEnabled",
    "adaptivePcssFilterEnabled",
    "stagedPcssBlockerEnabled",
    "hardwareDepthCompareEnabled",
    "hardwareLinearPcfEnabled",
    "hardwareReducedPcfEnabled",
    "texelScaledBiasEnabled",
    "spotRadialBiasDirectionEnabled",
    "spotPcssLinearDepthEnabled",
    "spotPcssReducedFilterEnabled",
    "spotCasterDepthFitEnabled",
    "casterCullingEnabled",
    "pointShadowRenderPolicy",
    "pointShadowFaceCullingEnabled",
    "measuredUpdateCount",
    "measuredCacheCheckCount",
    "measuredCacheHitCount",
    "measuredCacheMissCount",
    "measuredLightCacheHitCount",
    "measuredUpdatedLightCount",
    "measuredDirectionalLightUpdateCount",
    "measuredPointLightUpdateCount",
    "measuredSpotLightUpdateCount",
    "pointLightUpdateCount",
    "pointShadowLayeredUpdateCount",
    "pointShadowSixFaceUpdateCount",
    "pointShadowSubmissionPassCount",
    "pointShadowFaceCullingPassCount",
    "measuredPointShadowLayeredUpdateCount",
    "measuredPointShadowSixFaceUpdateCount",
    "measuredPointShadowSubmissionPassCount",
    "measuredPointShadowFaceCullingPassCount",
)
SWITCH_FIELDS = tuple(
    field
    for field in SHADOW_INVARIANT_FIELDS
    if field.endswith("Enabled") or field == "optimizationFlags"
)


class SummaryError(RuntimeError):
    """Raised when an experiment is incomplete or has an unexpected schema."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Read a Test-ShadowOptimizations.ps1 experiment and emit a compact "
            "A/B performance, resource, invariant, and capture summary."
        )
    )
    parser.add_argument("experiment_dir", type=Path, help="experiment directory")
    parser.add_argument(
        "--output",
        "-o",
        type=Path,
        help="write JSON outside the experiment directory instead of stdout",
    )
    return parser.parse_args(argv)


def read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SummaryError(f"missing required file: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SummaryError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise SummaryError(f"expected a JSON object: {path}")
    return value


def require_dict(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise SummaryError(f"expected object at {label}")
    return value


def require_number(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise SummaryError(f"expected number at {label}")
    result = float(value)
    if not math.isfinite(result):
        raise SummaryError(f"expected finite number at {label}")
    return result


def require_int(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise SummaryError(f"expected integer at {label}")
    return value


def nested(value: dict[str, Any], keys: Iterable[str], label: str) -> Any:
    current: Any = value
    traversed: list[str] = []
    for key in keys:
        traversed.append(key)
        current = require_dict(current, f"{label}.{'.'.join(traversed[:-1])}")
        if key not in current:
            raise SummaryError(f"missing field {label}.{'.'.join(traversed)}")
        current = current[key]
    return current


def rounded(value: float) -> float:
    # Six decimals retain the profiler's useful precision without noisy tails.
    return round(value, 6)


def delta_percent(before: float, after: float) -> float | None:
    if before == 0.0:
        return None
    return rounded((after - before) * 100.0 / before)


def mean(values: list[float]) -> float:
    if not values:
        raise SummaryError("cannot average an empty value list")
    return sum(values) / len(values)


def metric_summary(
    runs: dict[str, dict[str, Any]],
    getter: Callable[[dict[str, Any], str], float],
) -> dict[str, Any]:
    values = {
        variant: [getter(runs[f"{variant}{index}"], f"{variant}{index}") for index in range(1, 4)]
        for variant in VARIANTS
    }
    averages = {variant: mean(values[variant]) for variant in VARIANTS}
    return {
        "meanMs": {variant: rounded(averages[variant]) for variant in VARIANTS},
        "deltaPercent": delta_percent(averages["A"], averages["B"]),
        "rangeMs": {
            variant: [rounded(min(values[variant])), rounded(max(values[variant]))]
            for variant in VARIANTS
        },
        "pairedDeltaPercent": [
            delta_percent(values["A"][index], values["B"][index]) for index in range(3)
        ],
    }


def profiler_mean_getter(section: str) -> Callable[[dict[str, Any], str], float]:
    def get(run: dict[str, Any], run_name: str) -> float:
        value = nested(
            run,
            ("profiler", "summary", section, "mean"),
            f"run {run_name}",
        )
        return require_number(value, f"run {run_name}.profiler.summary.{section}.mean")

    return get


def gpu_zone_getter(zone_name: str) -> Callable[[dict[str, Any], str], float]:
    def get(run: dict[str, Any], run_name: str) -> float:
        value = nested(
            run,
            ("profiler", "summary", "gpuZones", zone_name, "mean"),
            f"run {run_name}",
        )
        return require_number(
            value,
            f"run {run_name}.profiler.summary.gpuZones.{zone_name}.mean",
        )

    return get


def stable_value(values: list[Any]) -> tuple[Any, bool]:
    stable = all(value == values[0] for value in values[1:])
    return (values[0] if stable else values), stable


def invariant_summary(
    runs: dict[str, dict[str, Any]], getter: Callable[[dict[str, Any]], Any]
) -> dict[str, Any]:
    values: dict[str, list[Any]] = {
        variant: [getter(runs[f"{variant}{index}"]) for index in range(1, 4)]
        for variant in VARIANTS
    }
    a_value, a_stable = stable_value(values["A"])
    b_value, b_stable = stable_value(values["B"])
    return {
        "A": a_value,
        "B": b_value,
        "stable": a_stable and b_stable,
        "sameAB": a_stable and b_stable and a_value == b_value,
    }


def all_runs(
    runs: dict[str, dict[str, Any]], predicate: Callable[[dict[str, Any]], bool]
) -> bool:
    return all(predicate(runs[name]) for name in RUN_NAMES)


def detect_render_path_and_zone(runs: dict[str, dict[str, Any]]) -> tuple[str, str]:
    render_paths = [runs[name].get("renderPath") for name in RUN_NAMES]
    render_path, stable = stable_value(render_paths)
    if not stable or not isinstance(render_path, str):
        raise SummaryError(f"renderPath differs across formal runs: {render_paths}")

    normalized = render_path.lower()
    if "forward" in normalized:
        candidates = ("Forward Pass",)
    elif "deferred" in normalized:
        # Current captures call the lighting zone "Deferred Pass"; retain
        # compatibility with captures that use the more explicit report label.
        candidates = ("Deferred Lighting", "Deferred Pass")
    else:
        raise SummaryError(f"unsupported renderPath: {render_path}")

    common_zones: set[str] | None = None
    for name in RUN_NAMES:
        zones = nested(
            runs[name], ("profiler", "summary", "gpuZones"), f"run {name}"
        )
        zones = require_dict(zones, f"run {name}.profiler.summary.gpuZones")
        common_zones = set(zones) if common_zones is None else common_zones & set(zones)
    for candidate in candidates:
        if candidate in (common_zones or set()):
            return render_path, candidate
    raise SummaryError(
        f"no render-path GPU zone {candidates} is present in all formal runs"
    )


def summarize_resources(runs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    resources: dict[str, Any] = {}
    all_paired_exact = True
    all_runs_exact = True
    for output_name, input_name in RESOURCE_FIELDS.items():
        values: dict[str, list[int]] = {}
        for variant in VARIANTS:
            values[variant] = []
            for index in range(1, 4):
                run_name = f"{variant}{index}"
                value = nested(
                    runs[run_name], ("memoryBytes", input_name), f"run {run_name}"
                )
                values[variant].append(
                    require_int(value, f"run {run_name}.memoryBytes.{input_name}")
                )
        paired_exact = [
            values["A"][index] == values["B"][index] for index in range(3)
        ]
        resource_all_runs_exact = len(set(values["A"] + values["B"])) == 1
        all_paired_exact = all_paired_exact and all(paired_exact)
        all_runs_exact = all_runs_exact and resource_all_runs_exact
        resources[output_name] = {
            "A": values["A"],
            "B": values["B"],
            "pairedExact": paired_exact,
            "allRunsExact": resource_all_runs_exact,
        }
    resources["allResourcesPairedExact"] = all_paired_exact
    resources["allResourcesAllRunsExact"] = all_runs_exact
    return resources


def shadow_value(run: dict[str, Any], field: str) -> Any:
    shadow = require_dict(run.get("shadow"), "run.shadow")
    if field not in shadow:
        raise SummaryError(f"missing shadow field: {field}")
    return shadow[field]


def point_update_accounting(run: dict[str, Any], measured: bool) -> bool:
    prefix = "measured" if measured else ""
    point_key = f"{prefix}PointLightUpdateCount" if prefix else "pointLightUpdateCount"
    layered_key = (
        f"{prefix}PointShadowLayeredUpdateCount"
        if prefix
        else "pointShadowLayeredUpdateCount"
    )
    six_face_key = (
        f"{prefix}PointShadowSixFaceUpdateCount"
        if prefix
        else "pointShadowSixFaceUpdateCount"
    )
    return (
        shadow_value(run, layered_key) + shadow_value(run, six_face_key)
        == shadow_value(run, point_key)
    )


def point_submission_accounting(run: dict[str, Any], measured: bool) -> bool:
    prefix = "measured" if measured else ""
    layered_key = (
        f"{prefix}PointShadowLayeredUpdateCount"
        if prefix
        else "pointShadowLayeredUpdateCount"
    )
    six_face_key = (
        f"{prefix}PointShadowSixFaceUpdateCount"
        if prefix
        else "pointShadowSixFaceUpdateCount"
    )
    submission_key = (
        f"{prefix}PointShadowSubmissionPassCount"
        if prefix
        else "pointShadowSubmissionPassCount"
    )
    return shadow_value(run, submission_key) == (
        shadow_value(run, layered_key) + 6 * shadow_value(run, six_face_key)
    )


def point_face_culling_accounting(run: dict[str, Any], measured: bool) -> bool:
    prefix = "measured" if measured else ""
    six_face_key = (
        f"{prefix}PointShadowSixFaceUpdateCount"
        if prefix
        else "pointShadowSixFaceUpdateCount"
    )
    face_culling_key = (
        f"{prefix}PointShadowFaceCullingPassCount"
        if prefix
        else "pointShadowFaceCullingPassCount"
    )
    expected = (
        6 * shadow_value(run, six_face_key)
        if shadow_value(run, "pointShadowFaceCullingEnabled")
        else 0
    )
    return shadow_value(run, face_culling_key) == expected


def static_hit_cache_accounting(run: dict[str, Any]) -> bool:
    if shadow_value(run, "workload") != "static-hit":
        return False
    measured_frames = require_int(run.get("measuredFrames"), "run.measuredFrames")
    return (
        shadow_value(run, "measuredCacheCheckCount") == measured_frames
        and shadow_value(run, "measuredCacheHitCount") == measured_frames
        and shadow_value(run, "measuredCacheMissCount") == 0
    )


def static_hit_light_cache_accounting(run: dict[str, Any]) -> bool:
    if shadow_value(run, "workload") != "static-hit":
        return False
    if not shadow_value(run, "perLightCacheEnabled"):
        return shadow_value(run, "measuredLightCacheHitCount") == 0
    lights = shadow_value(run, "lights")
    light_count = 3 if lights == "all" else 1
    measured_frames = require_int(run.get("measuredFrames"), "run.measuredFrames")
    return shadow_value(run, "measuredLightCacheHitCount") == (
        measured_frames * light_count
    )


def summarize_invariants(runs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    fields: dict[str, Any] = {
        "schemaVersion": invariant_summary(
            runs, lambda run: run.get("schemaVersion")
        ),
        "renderPath": invariant_summary(runs, lambda run: run.get("renderPath")),
    }
    for field in SHADOW_INVARIANT_FIELDS:
        fields[field] = invariant_summary(
            runs, lambda run, field=field: shadow_value(run, field)
        )

    static_hit = all_runs(
        runs, lambda run: shadow_value(run, "workload") == "static-hit"
    )
    no_measured_updates = all_runs(
        runs,
        lambda run: all(
            shadow_value(run, field) == 0
            for field in (
                "measuredUpdateCount",
                "measuredUpdatedLightCount",
                "measuredDirectionalLightUpdateCount",
                "measuredPointLightUpdateCount",
                "measuredSpotLightUpdateCount",
            )
        ),
    )
    switches_stable = all(fields[field]["stable"] for field in SWITCH_FIELDS)
    return {
        "checks": {
            "schema16": all_runs(runs, lambda run: run.get("schemaVersion") == 16),
            "staticHit": static_hit,
            "staticHitHasNoMeasuredUpdates": static_hit and no_measured_updates,
            "staticHitCacheAccounting": all_runs(
                runs, static_hit_cache_accounting
            ),
            "staticHitLightCacheAccounting": all_runs(
                runs, static_hit_light_cache_accounting
            ),
            "pointRenderPolicyStableWithinVariant": fields[
                "pointShadowRenderPolicy"
            ]["stable"],
            "pointUpdateAccounting": all_runs(
                runs, lambda run: point_update_accounting(run, measured=False)
            ),
            "measuredPointUpdateAccounting": all_runs(
                runs, lambda run: point_update_accounting(run, measured=True)
            ),
            "pointSubmissionAccounting": all_runs(
                runs, lambda run: point_submission_accounting(run, measured=False)
            ),
            "measuredPointSubmissionAccounting": all_runs(
                runs, lambda run: point_submission_accounting(run, measured=True)
            ),
            "pointFaceCullingAccounting": all_runs(
                runs, lambda run: point_face_culling_accounting(run, measured=False)
            ),
            "measuredPointFaceCullingAccounting": all_runs(
                runs, lambda run: point_face_culling_accounting(run, measured=True)
            ),
            "schema16SwitchesStableWithinVariant": switches_stable,
        },
        "fields": fields,
    }


def summarize_scene(
    scene_id: str,
    display_name: str,
    runs: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    render_path, gpu_zone = detect_render_path_and_zone(runs)
    captures = {
        variant: [runs[f"{variant}{index}"].get("capturePath") for index in range(1, 4)]
        for variant in VARIANTS
    }
    if any(not isinstance(path, str) or not path for paths in captures.values() for path in paths):
        raise SummaryError(f"scene {scene_id} has a missing capturePath")

    return {
        "displayName": display_name,
        "renderPath": render_path,
        "renderPathGpuZone": gpu_zone,
        "metrics": {
            "wall": metric_summary(runs, profiler_mean_getter("wallFrame")),
            "cpu": metric_summary(runs, profiler_mean_getter("cpuFrame")),
            "gpu": metric_summary(runs, profiler_mean_getter("gpuFrame")),
            "renderPathGpu": metric_summary(runs, gpu_zone_getter(gpu_zone)),
        },
        "resources": summarize_resources(runs),
        "shadowInvariants": summarize_invariants(runs),
        "capturePaths": captures,
    }


def scene_order(metadata: dict[str, Any], formal_dir: Path) -> list[str]:
    discovered = sorted(path.name for path in formal_dir.iterdir() if path.is_dir())
    settings = metadata.get("settings")
    configured = settings.get("sceneIds") if isinstance(settings, dict) else None
    if isinstance(configured, list) and all(isinstance(item, str) for item in configured):
        missing = [scene_id for scene_id in configured if scene_id not in discovered]
        if missing:
            raise SummaryError(f"metadata sceneIds missing from formal/: {missing}")
        return list(configured) + [
            scene_id for scene_id in discovered if scene_id not in configured
        ]
    return discovered


def load_formal_runs(scene_dir: Path) -> dict[str, dict[str, Any]]:
    return {
        run_name: read_json(scene_dir / f"{run_name}.json")
        for run_name in RUN_NAMES
    }


def build_summary(experiment_dir: Path) -> dict[str, Any]:
    root = experiment_dir.resolve()
    if not root.is_dir():
        raise SummaryError(f"experiment directory does not exist: {root}")

    metadata = read_json(root / "metadata.json")
    source_summary = read_json(root / "summary.json")
    experiment_id = metadata.get("experimentId")
    if not isinstance(experiment_id, str) or not experiment_id:
        raise SummaryError("metadata.json has no experimentId")
    if source_summary.get("experimentId") != experiment_id:
        raise SummaryError("metadata.json and summary.json experimentId differ")

    formal_dir = root / "formal"
    if not formal_dir.is_dir():
        raise SummaryError(f"missing formal directory: {formal_dir}")

    display_names: dict[str, str] = {}
    source_scenes = source_summary.get("scenes")
    if not isinstance(source_scenes, list):
        raise SummaryError("summary.json scenes is not an array")
    for scene in source_scenes:
        if isinstance(scene, dict) and isinstance(scene.get("id"), str):
            display_name = scene.get("displayName")
            display_names[scene["id"]] = (
                display_name if isinstance(display_name, str) else scene["id"]
            )

    scenes: dict[str, Any] = {}
    for scene_id in scene_order(metadata, formal_dir):
        if scene_id not in display_names:
            raise SummaryError(f"scene {scene_id} is absent from summary.json")
        scenes[scene_id] = summarize_scene(
            scene_id,
            display_names[scene_id],
            load_formal_runs(formal_dir / scene_id),
        )

    settings = metadata.get("settings")
    labels = {}
    if isinstance(settings, dict):
        labels = {
            "A": settings.get("variantALabel"),
            "B": settings.get("variantBLabel"),
        }
    return {
        "schemaVersion": 1,
        "experiment": {
            "id": experiment_id,
            "directory": str(root),
            "configuration": metadata.get("configuration"),
            "resolution": metadata.get("resolution"),
            "order": metadata.get("order"),
            "measuredFrames": metadata.get("measuredFrames"),
            "variantLabels": labels,
            "metadataSchemaVersion": metadata.get("schemaVersion"),
            "sourceSummarySchemaVersion": source_summary.get("schemaVersion"),
        },
        "scenes": scenes,
    }


def is_within(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
    except ValueError:
        return False
    return True


def write_output(result: dict[str, Any], output: Path | None, experiment_dir: Path) -> None:
    serialized = json.dumps(
        result, ensure_ascii=False, separators=(",", ":")
    ) + "\n"
    if output is None:
        sys.stdout.write(serialized)
        return

    target = output.resolve()
    root = experiment_dir.resolve()
    if is_within(target, root):
        raise SummaryError(
            "--output must be outside the source experiment directory "
            "(the analyzer is read-only)"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(serialized, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        result = build_summary(args.experiment_dir)
        write_output(result, args.output, args.experiment_dir)
    except SummaryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
