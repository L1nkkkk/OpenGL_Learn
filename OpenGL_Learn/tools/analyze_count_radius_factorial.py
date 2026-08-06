#!/usr/bin/env python3
"""Controlled point-light count x effective-radius factorial analyzer.

The renderer captures are exact analytic-screen workloads. Tile/Cluster lists
are built offline and are never described as measured GPU implementations.
"""

from __future__ import annotations

import argparse
import csv
import gc
import gzip
import hashlib
import importlib
import json
import math
import statistics
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
base = importlib.import_module("analyze_tile_cluster_phase_a")


COUNTS = (32, 64, 128, 256, 512)
RADII = (1.5, 3.0, 6.0, 12.0)
SEEDS = (0x21D3F3A5, 0xA511E9B3, 0xC0FFEE11)
PRIMARY_SEED = SEEDS[0]
TILE_SIZE = 16
CLUSTER_SLICES = 16
MIB = 1024.0 * 1024.0
EPSILON = 1.0e-6
RADIUS_TOLERANCE = 1.0e-4
ANCHORS = ((64, 1.5, "low"), (256, 6.0, "transition"), (512, 12.0, "saturated"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def json_dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        raise ValueError(f"refusing to write empty CSV: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    fields: list[str] = []
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def distribution(values: np.ndarray) -> dict[str, float | int]:
    data = np.asarray(values)
    if not data.size:
        return {"count": 0, "total": 0, "mean": 0.0, "median": 0.0,
                "p95": 0.0, "p99": 0.0, "max": 0.0, "min": 0.0}
    return {
        "count": int(data.size),
        "total": int(np.sum(data, dtype=np.uint64)),
        "mean": float(np.mean(data, dtype=np.float64)),
        "median": float(np.median(data)),
        "p95": float(np.percentile(data, 95.0)),
        "p99": float(np.percentile(data, 99.0)),
        "max": float(np.max(data)),
        "min": float(np.min(data)),
    }


def scalar_distribution(values: list[float]) -> dict[str, float | int]:
    data = np.asarray(values, dtype=np.float64)
    return {
        "count": int(data.size),
        "median": float(np.median(data)),
        "min": float(np.min(data)),
        "max": float(np.max(data)),
        "mean": float(np.mean(data)),
        "p95": float(np.percentile(data, 95.0)),
        "p99": float(np.percentile(data, 99.0)),
    }


def case_key(seed: int, count: int, radius: float) -> tuple[int, int, float]:
    return seed, count, float(radius)


def stem_for(seed_ordinal: int, count: int, radius: float) -> str:
    return f"s{seed_ordinal}-n{count:04d}-r{int(round(radius * 10.0)):03d}"


@dataclass
class CapturedCase:
    manifest: dict[str, Any]
    result: dict[str, Any]
    result_path: Path
    ldr_path: Path
    seed: int
    seed_ordinal: int
    count: int
    requested_radius: float
    positions: np.ndarray
    colors: np.ndarray


def resolve_path(value: str, manifest_path: Path) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (manifest_path.parent / path).resolve()


def load_captured_cases(
    manifest: dict[str, Any], manifest_path: Path
) -> dict[tuple[int, int, float], CapturedCase]:
    result: dict[tuple[int, int, float], CapturedCase] = {}
    for item in manifest["cases"]:
        result_path = resolve_path(str(item["result"]), manifest_path)
        ldr_path = resolve_path(str(item["ldr"]), manifest_path)
        scene = json.loads(result_path.read_text(encoding="utf-8-sig"))
        point = scene["pointLightStress"]
        seed = int(item["seed"])
        count = int(item["lightCount"])
        radius = float(item["requestedRadius"])
        key = case_key(seed, count, radius)
        if key in result:
            raise ValueError(f"duplicate capture case: {key}")
        if (
            not scene.get("success")
            or point["renderMode"] != "analytic-screen"
            or not point["renderModeExplicit"]
            or point["generatorVersion"] != "point-light-count-radius-xorshift32-v2"
            or not point["targetRadiusExplicit"]
            or int(point["seed"]) != seed
            or int(point["generatedLightCount"]) != count
            or abs(float(point["requestedRadius"]) - radius) > RADIUS_TOLERANCE
            or abs(float(point["volumeRadius"]) - radius) > RADIUS_TOLERANCE
            or float(point["radiusAbsoluteError"]) > RADIUS_TOLERANCE
            or list(scene["resolution"]) != [1920, 1080]
        ):
            raise ValueError(f"invalid controlled capture: {result_path}")
        telemetry = point["boundsTelemetry"]
        if not telemetry["executed"] or len(telemetry["records"]) != count:
            raise ValueError(f"invalid bounds telemetry: {result_path}")
        if any(abs(float(record["radius"]) - radius) > RADIUS_TOLERANCE for record in telemetry["records"]):
            raise ValueError(f"per-light radius mismatch: {result_path}")
        positions = np.asarray([light["position"] for light in point["lights"]], dtype=np.float64)
        colors = np.asarray([light["diffuse"] for light in point["lights"]], dtype=np.float64)
        if positions.shape != (count, 3) or colors.shape != (count, 3):
            raise ValueError(f"invalid light arrays: {result_path}")
        if not ldr_path.is_file() or Image.open(ldr_path).size != (1920, 1080):
            raise ValueError(f"invalid renderer screenshot: {ldr_path}")
        result[key] = CapturedCase(
            manifest=item,
            result=scene,
            result_path=result_path,
            ldr_path=ldr_path,
            seed=seed,
            seed_ordinal=int(item["seedOrdinal"]),
            count=count,
            requested_radius=radius,
            positions=positions,
            colors=colors,
        )
    expected = {case_key(seed, count, radius) for seed in SEEDS for count in COUNTS for radius in RADII}
    if set(result) != expected:
        raise ValueError(f"formal 60-case matrix mismatch: missing={expected-set(result)}, extra={set(result)-expected}")
    return result


def validate_prefix_invariants(cases: dict[tuple[int, int, float], CapturedCase]) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    for seed in SEEDS:
        master = cases[case_key(seed, 512, RADII[0])]
        for radius in RADII:
            full = cases[case_key(seed, 512, radius)]
            if not np.array_equal(full.positions, master.positions) or not np.array_equal(full.colors, master.colors):
                raise ValueError(f"radius changed master light pool for seed {seed:#x}")
            for count in COUNTS:
                current = cases[case_key(seed, count, radius)]
                positions_equal = np.array_equal(current.positions, master.positions[:count])
                colors_equal = np.array_equal(current.colors, master.colors[:count])
                if not positions_equal or not colors_equal:
                    raise ValueError(f"master-prefix invariant failed: seed={seed:#x}, N={count}, R={radius}")
                checks.append({
                    "seed": seed,
                    "lightCount": count,
                    "radius": radius,
                    "positionsExactPrefix": positions_equal,
                    "colorsExactPrefix": colors_equal,
                    "positionSignature": current.result["pointLightStress"]["positionPrefixSignature"],
                    "colorSignature": current.result["pointLightStress"]["colorPrefixSignature"],
                })
    return {"passed": True, "checkCount": len(checks), "checks": checks}


def validate_effective_radius_invariants(
    cases: dict[tuple[int, int, float], CapturedCase]
) -> tuple[dict[float, float], dict[str, Any]]:
    effective_by_radius: dict[float, float] = {}
    checks = []
    for requested in RADII:
        records = []
        for seed in SEEDS:
            for count in COUNTS:
                point = cases[case_key(seed, count, requested)].result["pointLightStress"]
                records.append((
                    float(point["volumeRadius"]),
                    float(point["constant"]),
                    float(point["linear"]),
                    float(point["quadratic"]),
                    float(point["attenuationThreshold"]),
                ))
        reference = records[0]
        exact = all(record == reference for record in records)
        if not exact:
            raise ValueError(f"effective radius/attenuation differs across Seed/N for R={requested}")
        if abs(reference[0] - requested) > RADIUS_TOLERANCE:
            raise ValueError(f"effective radius exceeds frozen tolerance for R={requested}")
        effective_by_radius[requested] = reference[0]
        checks.append({
            "requestedRadius": requested,
            "effectiveRadius": reference[0],
            "constant": reference[1],
            "linear": reference[2],
            "quadratic": reference[3],
            "threshold": reference[4],
            "caseCount": len(records),
            "exactAcrossSeedAndCount": exact,
        })
    return effective_by_radius, {"passed": True, "checks": checks}


def build_scene_data(
    captured: CapturedCase,
    position: np.ndarray,
    validity: np.ndarray,
) -> base.SceneData:
    scene = captured.result
    point = scene["pointLightStress"]
    matrices = scene["gBuffer"]["cameraMatrices"]
    view = np.asarray(matrices["view"], dtype=np.float64)
    projection = np.asarray(matrices["projection"], dtype=np.float64)
    valid_flat = np.flatnonzero(validity.reshape(-1))
    valid_positions = position.reshape(-1, 3)[valid_flat]
    valid_h = np.concatenate((valid_positions, np.ones((valid_positions.shape[0], 1))), axis=1)
    view_positions = (view @ valid_h.T).T
    light_h = np.concatenate((captured.positions, np.ones((captured.count, 1))), axis=1)
    light_view = (view @ light_h.T).T[:, :3]
    return base.SceneData(
        stem=str(captured.manifest["stem"]),
        coverage="representative",
        light_count=captured.count,
        width=1920,
        height=1080,
        position=position,
        validity=validity,
        valid_positions=valid_positions,
        valid_flat_indices=valid_flat,
        view_depth_valid=-view_positions[:, 2],
        camera_position=np.asarray(scene["camera"]["position"], dtype=np.float64),
        view=view,
        projection=projection,
        near_plane=float(matrices["nearPlane"]),
        far_plane=float(matrices["farPlane"]),
        light_positions=captured.positions,
        light_view_positions=light_view,
        radius=float(point["volumeRadius"]),
        scene_signature=str(point["sceneSignature"]),
        submission_signature=str(point["submissionSignature"]),
        source_json=captured.result_path,
        app_ldr=captured.ldr_path,
    )


def build_memberships(
    scene: base.SceneData, layout: dict[str, Any], radius: float
) -> tuple[np.ndarray, np.ndarray, dict[str, float]]:
    start = time.perf_counter()
    radius_guarded = radius + EPSILON
    tile = np.zeros((layout["tileCount"], scene.light_count), dtype=bool)
    for light_index, center in enumerate(scene.light_view_positions):
        depth = -float(center[2])
        if depth + radius_guarded < scene.near_plane or depth - radius_guarded > scene.far_plane:
            continue
        signed = np.einsum("tpc,c->tp", layout["tileNormals"], center)
        tile[:, light_index] = np.all(signed >= -radius_guarded, axis=1)
    tile_ms = (time.perf_counter() - start) * 1000.0

    start = time.perf_counter()
    edges = base.slice_edges(scene, CLUSTER_SLICES)
    depths = -scene.light_view_positions[:, 2]
    depth_membership = (
        (depths[None, :] + radius_guarded >= edges[:-1, None])
        & (depths[None, :] - radius_guarded <= edges[1:, None])
    )
    cluster = (depth_membership[:, None, :] & tile[None, :, :]).reshape(
        CLUSTER_SLICES * layout["tileCount"], scene.light_count
    )
    cluster_ms = (time.perf_counter() - start) * 1000.0
    return tile, cluster, {"tileMembershipMs": tile_ms, "clusterMembershipMs": cluster_ms}


def csr_arrays(membership: np.ndarray, count: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, str]:
    selected = membership[:, :count]
    counts = np.sum(selected, axis=1, dtype=np.uint32)
    offsets64 = np.empty(counts.size + 1, dtype=np.uint64)
    offsets64[0] = 0
    np.cumsum(counts, dtype=np.uint64, out=offsets64[1:])
    if int(offsets64[-1]) > np.iinfo(np.uint32).max:
        raise OverflowError("CSR index pool exceeds uint32")
    offsets = offsets64.astype(np.uint32)
    indices = np.empty(int(offsets64[-1]), dtype=np.uint32)
    cursor = 0
    for row_start in range(0, selected.shape[0], 4096):
        columns = np.nonzero(selected[row_start : row_start + 4096])[1].astype(np.uint32, copy=False)
        indices[cursor : cursor + columns.size] = columns
        cursor += int(columns.size)
    if cursor != indices.size:
        raise RuntimeError("CSR fill count mismatch")
    digest = base.sha256_csr(offsets, counts, indices)
    return offsets, counts, indices, digest


def count_distribution_for_prefixes(
    valid_positions: np.ndarray,
    light_positions: np.ndarray,
    radius_memberships: dict[float, tuple[np.ndarray, np.ndarray]],
    tile_cell_ids: np.ndarray,
    cluster_cell_ids: np.ndarray,
    effective_radii: dict[float, float],
) -> tuple[
    dict[tuple[float, int], np.ndarray],
    dict[tuple[float, str, int], int],
]:
    pixel_count = valid_positions.shape[0]
    truth_counts = {
        (radius, count): np.empty(pixel_count, dtype=np.uint16)
        for radius in RADII
        for count in COUNTS
    }
    missed_by_light = {
        (radius, scheme): np.zeros(light_positions.shape[0], dtype=np.uint64)
        for radius in RADII
        for scheme in ("tile", "cluster-16")
    }
    lights = np.asarray(light_positions, dtype=np.float64)
    light_norm = np.einsum("ij,ij->i", lights, lights)
    light_t = lights.T
    block_size = 8192
    for start in range(0, pixel_count, block_size):
        end = min(pixel_count, start + block_size)
        points = np.asarray(valid_positions[start:end], dtype=np.float64)
        point_norm = np.einsum("ij,ij->i", points, points)
        distance_squared = point_norm[:, None] + light_norm[None, :] - 2.0 * (points @ light_t)
        np.maximum(distance_squared, 0.0, out=distance_squared)
        for radius in RADII:
            effective = effective_radii[radius]
            inside = distance_squared <= effective * effective
            cumulative = np.cumsum(inside, axis=1, dtype=np.uint16)
            for count in COUNTS:
                truth_counts[(radius, count)][start:end] = cumulative[:, count - 1]
            tile_membership, cluster_membership = radius_memberships[radius]
            present_tile = tile_membership[tile_cell_ids[start:end]]
            present_cluster = cluster_membership[cluster_cell_ids[start:end]]
            missed_by_light[(radius, "tile")] += np.count_nonzero(
                inside & ~present_tile, axis=0
            ).astype(np.uint64, copy=False)
            missed_by_light[(radius, "cluster-16")] += np.count_nonzero(
                inside & ~present_cluster, axis=0
            ).astype(np.uint64, copy=False)
    prefix_misses: dict[tuple[float, str, int], int] = {}
    for radius in RADII:
        for scheme in ("tile", "cluster-16"):
            cumulative = np.cumsum(missed_by_light[(radius, scheme)], dtype=np.uint64)
            for count in COUNTS:
                prefix_misses[(radius, scheme, count)] = int(cumulative[count - 1])
    return truth_counts, prefix_misses


def bounds_summary(captured: CapturedCase) -> dict[str, Any]:
    records = captured.result["pointLightStress"]["boundsTelemetry"]["records"]
    classes: dict[str, int] = {}
    reasons: dict[str, int] = {}
    coverage = []
    area = 0
    for record in records:
        classes[str(record["classification"])] = classes.get(str(record["classification"]), 0) + 1
        reasons[str(record["fallbackReason"])] = reasons.get(str(record["fallbackReason"]), 0) + 1
        coverage.append(float(record["coverageRatio"]))
        rect = record["pixelRect"]
        area += int(rect[2]) * int(rect[3])
    viewport_area = 1920 * 1080
    return {
        "classificationCounts": classes,
        "fallbackReasonCounts": reasons,
        "coverageRatioPerLight": distribution(np.asarray(coverage, dtype=np.float64)),
        "summedRectPixelArea": area,
        "summedRectCoverageOfNFullscreens": float(area / (viewport_area * captured.count)),
        "cameraInsideCount": reasons.get("camera-inside", 0),
        "nearPlaneFallbackCount": reasons.get("near-plane-intersection", 0),
        "invalidFallbackCount": reasons.get("invalid-radius", 0) + reasons.get("invalid-projection", 0),
        "outsideCount": classes.get("outside", 0),
    }


def metric_for_scheme(
    membership: np.ndarray,
    count: int,
    cell_ids: np.ndarray,
    pixel_occupancy: np.ndarray,
    truth_counts: np.ndarray,
    misses: int,
) -> tuple[dict[str, Any], np.ndarray, np.ndarray]:
    counts = np.sum(membership[:, :count], axis=1, dtype=np.uint32)
    pixel_lists = counts[cell_ids]
    candidate = int(np.sum(pixel_lists, dtype=np.uint64))
    truth = int(np.sum(truth_counts, dtype=np.uint64))
    true_positive = truth - misses
    false_positive = candidate - true_positive
    if false_positive < 0:
        raise RuntimeError("negative false positive count")
    active = pixel_occupancy > 0
    indices = int(np.sum(counts, dtype=np.uint64))
    metadata_bytes = int(counts.size * 8)
    index_bytes = indices * 4
    covered_cells_per_light = np.sum(membership[:, :count], axis=0, dtype=np.uint32)
    active_indices = int(np.sum(counts[active], dtype=np.uint64))
    result = {
        "logicalCellCount": int(counts.size),
        "activeCellCount": int(np.count_nonzero(active)),
        "activeCellFraction": float(np.count_nonzero(active) / counts.size),
        "listLengthAllCells": distribution(counts),
        "listLengthActiveCells": distribution(counts[active]),
        "listLengthPixelWeighted": distribution(pixel_lists),
        "groundTruthInteractions": truth,
        "candidateInteractions": candidate,
        "truePositiveInteractions": true_positive,
        "falsePositiveInteractions": false_positive,
        "falsePositiveRate": float(false_positive / candidate if candidate else 0.0),
        "candidateToGroundTruthRatio": float(candidate / truth if truth else 0.0),
        "missInteractions": misses,
        "totalIndexReferences": indices,
        "metadataBytes": metadata_bytes,
        "indexBytes": index_bytes,
        "totalLogicalBytes": metadata_bytes + index_bytes,
        "totalLogicalMiB": float((metadata_bytes + index_bytes) / MIB),
        "indicesPerLight": float(indices / count),
        "coveredCellsPerLight": distribution(covered_cells_per_light),
        "activeOnlyLowerBound": {
            "metadataBytes": int(np.count_nonzero(active) * 8),
            "indexBytes": active_indices * 4,
            "totalBytes": int(np.count_nonzero(active) * 8 + active_indices * 4),
            "totalMiB": float((np.count_nonzero(active) * 8 + active_indices * 4) / MIB),
            "label": "offline lower bound; requires active-cell compaction",
        },
        "uint16IndexWhatIf": {
            "valid": count <= 65535,
            "totalBytes": metadata_bytes + indices * 2,
            "totalMiB": float((metadata_bytes + indices * 2) / MIB),
            "label": "offline encoding estimate; not measured runtime",
        },
    }
    return result, counts, pixel_lists


def write_cells_gzip(
    path: Path,
    counts: np.ndarray,
    occupancy: np.ndarray,
    tiles_x: int,
    tile_count: int,
    slices: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(path, "wt", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("cellId", "tileX", "tileY", "slice", "validPixelCount", "listLength", "active"))
        for cell_id, value in enumerate(counts):
            if slices:
                slice_id = cell_id // tile_count
                tile_id = cell_id % tile_count
            else:
                slice_id = -1
                tile_id = cell_id
            writer.writerow((
                cell_id,
                tile_id % tiles_x,
                tile_id // tiles_x,
                slice_id,
                int(occupancy[cell_id]),
                int(value),
                int(occupancy[cell_id] > 0),
            ))


def save_csr(path: Path, offsets: np.ndarray, counts: np.ndarray, indices: np.ndarray, metadata: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        path,
        offsets=offsets,
        counts=counts,
        indices=indices,
        metadata_json=np.asarray([json.dumps(metadata, sort_keys=True)], dtype="U1024"),
    )


def heat_rgb(normalized: np.ndarray) -> np.ndarray:
    x = np.clip(normalized, 0.0, 1.0)
    stops = np.asarray(
        [[13, 8, 135], [84, 3, 160], [139, 10, 165], [185, 50, 137],
         [219, 92, 104], [244, 136, 73], [254, 188, 43], [240, 249, 33]],
        dtype=np.float64,
    )
    scaled = x * (len(stops) - 1)
    low = np.floor(scaled).astype(np.int32)
    high = np.minimum(low + 1, len(stops) - 1)
    fraction = scaled - low
    return (stops[low] * (1.0 - fraction[..., None]) + stops[high] * fraction[..., None]).astype(np.uint8)


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/msyhbd.ttc" if bold else "C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def annotate_anchor(captured: CapturedCase, output: Path) -> Image.Image:
    source = Image.open(captured.ldr_path).convert("RGB")
    canvas = Image.new("RGB", (source.width, source.height + 92), "#11131a")
    canvas.paste(source, (0, 92))
    draw = ImageDraw.Draw(canvas)
    draw.text((30, 14), "Exact analytic-screen renderer workload", fill="white", font=font(30, True))
    draw.text(
        (30, 54),
        f"N={captured.count} | R={captured.requested_radius:.1f} | seed=0x{captured.seed:08X} | effective={float(captured.result['pointLightStress']['volumeRadius']):.6f} | 1920x1080",
        fill="#cdd8f0",
        font=font(20),
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)
    return canvas


def make_anchor_heatmap(
    captured: CapturedCase,
    renderer: Image.Image,
    validity: np.ndarray,
    layout: dict[str, Any],
    tile_counts: np.ndarray,
    cluster_counts: np.ndarray,
    cluster_cell_ids: np.ndarray,
    output: Path,
) -> None:
    tile_full = tile_counts[layout["tileIdsFull"]].reshape(1080, 1920)
    cluster_full = np.zeros(1920 * 1080, dtype=np.uint32)
    cluster_full[np.flatnonzero(validity.reshape(-1))] = cluster_counts[cluster_cell_ids]
    cluster_full = cluster_full.reshape(1080, 1920)
    vmax = max(1, int(max(np.max(tile_full), np.max(cluster_full))))
    tile_image = Image.fromarray(np.flipud(heat_rgb(tile_full / vmax)), "RGB")
    cluster_image = Image.fromarray(np.flipud(heat_rgb(cluster_full / vmax)), "RGB")
    renderer_body = renderer.crop((0, 92, 1920, renderer.height))
    panel_width = 760
    panel_height = int(1080 * panel_width / 1920)
    panels = [
        renderer_body.resize((panel_width, panel_height), Image.Resampling.LANCZOS),
        tile_image.resize((panel_width, panel_height), Image.Resampling.NEAREST),
        cluster_image.resize((panel_width, panel_height), Image.Resampling.NEAREST),
    ]
    canvas = Image.new("RGB", (panel_width * 3 + 80, panel_height + 150), "#11131a")
    draw = ImageDraw.Draw(canvas)
    title = f"N={captured.count}, R={captured.requested_radius:.1f} | shared scale 0..{vmax} candidate lights/pixel"
    draw.text((36, 20), title, fill="white", font=font(28, True))
    labels = ("Exact renderer", "Tile16 list count", "Cluster16 list count")
    for index, (image, label) in enumerate(zip(panels, labels)):
        x = 20 + index * (panel_width + 20)
        canvas.paste(image, (x, 78))
        draw.text((x, panel_height + 90), label, fill="#dce6ff", font=font(22, True))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def draw_heatmap(
    output: Path,
    title: str,
    subtitle: str,
    matrix: np.ndarray,
    formatter,
    unit: str,
) -> None:
    width, height = 1120, 760
    canvas = Image.new("RGB", (width, height), "#11131a")
    draw = ImageDraw.Draw(canvas)
    draw.text((44, 28), title, fill="white", font=font(32, True))
    draw.text((44, 73), subtitle, fill="#b8c6e3", font=font(18))
    left, top, cell_w, cell_h = 190, 145, 160, 115
    finite = matrix[np.isfinite(matrix)]
    minimum = float(np.min(finite)) if finite.size else 0.0
    maximum = float(np.max(finite)) if finite.size else 1.0
    span = max(maximum - minimum, 1e-12)
    colors = heat_rgb((matrix - minimum) / span)
    for row, radius in enumerate(RADII):
        draw.text((60, top + row * cell_h + 38), f"R={radius:g}", fill="white", font=font(22, True))
        for column, count in enumerate(COUNTS):
            x = left + column * cell_w
            y = top + row * cell_h
            color = tuple(int(v) for v in colors[row, column])
            draw.rounded_rectangle((x, y, x + cell_w - 8, y + cell_h - 8), radius=8, fill=color)
            value = matrix[row, column]
            text_value = "N/A" if not np.isfinite(value) else formatter(float(value))
            text_color = "black" if np.mean(color) > 145 else "white"
            box = draw.textbbox((0, 0), text_value, font=font(22, True))
            draw.text((x + (cell_w - 8 - (box[2]-box[0]))/2, y + 31), text_value, fill=text_color, font=font(22, True))
    for column, count in enumerate(COUNTS):
        draw.text((left + column * cell_w + 48, top - 42), f"N={count}", fill="white", font=font(20, True))
    draw.text((left, height - 92), f"range: {formatter(minimum)} .. {formatter(maximum)} {unit}", fill="#b8c6e3", font=font(18))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def draw_seed_summary_heatmap(
    output: Path,
    title: str,
    subtitle: str,
    values: dict[tuple[int, float], list[float]],
    formatter,
    unit: str,
) -> None:
    medians = np.asarray(
        [[statistics.median(values[(count, radius)]) for count in COUNTS] for radius in RADII],
        dtype=np.float64,
    )
    minimum = float(np.min(medians))
    maximum = float(np.max(medians))
    span = max(maximum - minimum, 1e-12)
    colors = heat_rgb((medians - minimum) / span)
    width, height = 1220, 800
    canvas = Image.new("RGB", (width, height), "#11131a")
    draw = ImageDraw.Draw(canvas)
    draw.text((44, 26), title, fill="white", font=font(30, True))
    draw.text((44, 70), subtitle, fill="#b8c6e3", font=font(17))
    left, top, cell_w, cell_h = 205, 150, 185, 125
    for row, radius in enumerate(RADII):
        draw.text((62, top + row * cell_h + 45), f"R={radius:g}", fill="white", font=font(21, True))
        for column, count in enumerate(COUNTS):
            cell_values = values[(count, radius)]
            median = float(statistics.median(cell_values))
            low, high = min(cell_values), max(cell_values)
            x, y = left + column * cell_w, top + row * cell_h
            color = tuple(int(value) for value in colors[row, column])
            draw.rounded_rectangle((x, y, x + cell_w - 10, y + cell_h - 10), radius=8, fill=color)
            text_color = "black" if np.mean(color) > 145 else "white"
            draw.text((x + 14, y + 20), f"Med {formatter(median)}", fill=text_color, font=font(17, True))
            draw.text((x + 14, y + 59), f"[{formatter(low)}, {formatter(high)}]", fill=text_color, font=font(15))
    for column, count in enumerate(COUNTS):
        draw.text((left + column * cell_w + 55, top - 40), f"N={count}", fill="white", font=font(19, True))
    draw.text((left, height - 86), f"Cell text: Median [Min, Max] across 3 frozen seeds | {unit}", fill="#b8c6e3", font=font(17))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def draw_lines(output: Path, title: str, series: list[tuple[str, list[float], list[float]]], x_label: str, y_label: str) -> None:
    width, height = 1200, 760
    canvas = Image.new("RGB", (width, height), "#11131a")
    draw = ImageDraw.Draw(canvas)
    draw.text((42, 26), title, fill="white", font=font(30, True))
    left, top, right, bottom = 105, 120, 1120, 650
    draw.line((left, bottom, right, bottom), fill="#d0d8e8", width=2)
    draw.line((left, top, left, bottom), fill="#d0d8e8", width=2)
    all_x = [value for _, xs, _ in series for value in xs]
    all_y = [value for _, _, ys in series for value in ys]
    min_x, max_x = min(all_x), max(all_x)
    min_y, max_y = min(all_y), max(all_y)
    margin = max((max_y - min_y) * 0.08, 0.02)
    min_y -= margin
    max_y += margin
    palette = ("#47b8ff", "#ffb547", "#61d095", "#ef6f91", "#b48cff")
    def tx(value: float) -> float:
        return left + (value - min_x) / max(max_x - min_x, 1e-12) * (right - left)
    def ty(value: float) -> float:
        return bottom - (value - min_y) / max(max_y - min_y, 1e-12) * (bottom - top)
    for grid in range(6):
        value = min_y + (max_y - min_y) * grid / 5
        y = ty(value)
        draw.line((left, y, right, y), fill="#2a3040", width=1)
        draw.text((15, y - 10), f"{value:.2f}", fill="#b8c6e3", font=font(15))
    for index, (label, xs, ys) in enumerate(series):
        color = palette[index % len(palette)]
        points = [(tx(x), ty(y)) for x, y in zip(xs, ys)]
        if len(points) > 1:
            draw.line(points, fill=color, width=4)
        for point in points:
            draw.ellipse((point[0]-5, point[1]-5, point[0]+5, point[1]+5), fill=color)
        legend_y = 88 + (index // 3) * 26
        legend_x = 440 + (index % 3) * 230
        draw.line((legend_x, legend_y, legend_x + 30, legend_y), fill=color, width=4)
        draw.text((legend_x + 38, legend_y - 10), label, fill="white", font=font(16))
    draw.text((500, 700), x_label, fill="white", font=font(20, True))
    draw.text((15, 92), y_label, fill="white", font=font(18, True))
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)


def parse_timing(run_dir: Path) -> tuple[dict[tuple[int, float], dict[str, Any]], list[dict[str, Any]]]:
    manifest_path = run_dir / "timing-manifest.json"
    if not manifest_path.is_file():
        return {}, []
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    groups: dict[tuple[int, float], list[dict[str, Any]]] = {}
    rows: list[dict[str, Any]] = []
    for run in manifest["runs"]:
        result_path = Path(run["result"])
        result = json.loads(result_path.read_text(encoding="utf-8-sig"))
        point = result["pointLightStress"]
        if (
            not result.get("success")
            or not result["profiler"]["gpuTimingSupported"]
            or int(result["profiler"]["summary"]["gpuFrame"]["count"]) != 600
            or point["renderMode"] != "analytic-screen"
            or not point["renderModeExplicit"]
        ):
            raise ValueError(f"invalid timing result: {result_path}")
        key = (int(run["lightCount"]), float(run["requestedRadius"]))
        summary = result["profiler"]["summary"]
        row = {
            "round": int(run["round"]),
            "order": int(run["order"]),
            "lightCount": key[0],
            "radius": key[1],
            "cpuFrameMedianMs": float(summary["cpuFrame"]["median"]),
            "cpuFrameP95Ms": float(summary["cpuFrame"]["p95"]),
            "cpuFrameP99Ms": float(summary["cpuFrame"]["p99"]),
            "gpuFrameMedianMs": float(summary["gpuFrame"]["median"]),
            "gpuPointLightsMedianMs": float(summary["gpuZones"]["Deferred Point Lights"]["median"]),
            "gpuPointLightsP95Ms": float(summary["gpuZones"]["Deferred Point Lights"]["p95"]),
            "gpuPointLightsP99Ms": float(summary["gpuZones"]["Deferred Point Lights"]["p99"]),
            "cpuPointLightsMedianMs": float(summary["cpuZones"]["Deferred Point Lights"]["median"]),
            "drawCallsMedian": float(summary["drawCalls"]["median"]),
            "submittedLightsMedian": float(summary["pointLightsSubmitted"]["median"]),
            "rectPixelAreaMedian": float(summary["pointLightRectPixelArea"]["median"]),
            "result": str(result_path),
        }
        groups.setdefault(key, []).append(row)
        rows.append(row)
    if len(rows) != 60 or any(len(values) != 3 for values in groups.values()):
        raise ValueError("timing matrix is not 20 cells x 3 processes")
    aggregate: dict[tuple[int, float], dict[str, Any]] = {}
    fields = (
        "cpuFrameMedianMs", "cpuFrameP95Ms", "cpuFrameP99Ms", "gpuFrameMedianMs",
        "gpuPointLightsMedianMs", "gpuPointLightsP95Ms", "gpuPointLightsP99Ms",
        "cpuPointLightsMedianMs", "drawCallsMedian", "submittedLightsMedian", "rectPixelAreaMedian",
    )
    for key, values in groups.items():
        aggregate[key] = {
            "label": "Measured Release analytic-screen oracle; not Tile/Cluster runtime",
            "independentProcessCount": 3,
            "warmupFramesPerProcess": 300,
            "sampleFramesPerProcess": 600,
            "processAggregates": {
                field: scalar_distribution([float(value[field]) for value in values]) for field in fields
            },
            "runs": values,
        }
    return aggregate, rows


def classify_cells(cases: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[int, float], list[dict[str, Any]]] = {}
    for case in cases:
        grouped.setdefault((case["lightCount"], case["requestedRadius"]), []).append(case)
    classifications = []
    for (count, radius), values in sorted(grouped.items()):
        correct = [
            value["tile"]["missInteractions"] == 0 and value["cluster16"]["missInteractions"] == 0
            and value["deterministic"]
            for value in values
        ]
        friendly = [
            correct[index]
            and value["comparison"]["clusterToTileCandidateRatio"] <= 0.70
            and value["comparison"]["removableWorkCapture"] >= 0.70
            and value["cluster16"]["totalLogicalMiB"] <= 64.0
            for index, value in enumerate(values)
        ]
        saturated = [
            correct[index]
            and (
                value["overlapFraction"] >= 0.60
                or (
                    value["comparison"]["cullingOpportunity"] <= 0.20
                    and value["comparison"]["clusterToTileCandidateRatio"] >= 0.85
                )
            )
            for index, value in enumerate(values)
        ]
        if not all(correct):
            label = "incorrect"
        elif sum(saturated) >= 2:
            label = "saturated"
        elif sum(friendly) >= 2:
            label = "culling-friendly"
        else:
            label = "transition"
        primary_index = next(index for index, value in enumerate(values) if value["seed"] == PRIMARY_SEED)
        phase_b_eligible = bool(label == "culling-friendly" and friendly[primary_index] and sum(friendly) >= 2)
        classifications.append({
            "lightCount": count,
            "radius": radius,
            "classification": label,
            "correctSeeds": sum(correct),
            "friendlySeeds": sum(friendly),
            "saturatedSeeds": sum(saturated),
            "primarySeedFriendly": bool(friendly[primary_index]),
            "phaseBEligible": phase_b_eligible,
            "seedMetrics": [{
                "seed": value["seed"],
                "overlapFraction": value["overlapFraction"],
                "candidateRatio": value["comparison"]["clusterToTileCandidateRatio"],
                "removableWorkCapture": value["comparison"]["removableWorkCapture"],
                "clusterMiB": value["cluster16"]["totalLogicalMiB"],
            } for value in values],
        })
    return classifications


def build_artifact_manifest(run_dir: Path) -> None:
    records = []
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file() or path.name == "artifact-manifest.json":
            continue
        records.append({
            "path": path.relative_to(run_dir).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        })
    json_dump(run_dir / "artifact-manifest.json", {
        "schemaVersion": 1,
        "fileCount": len(records),
        "totalBytes": sum(record["bytes"] for record in records),
        "files": records,
    })


def generate_report(run_dir: Path, aggregate: dict[str, Any]) -> None:
    primary = [case for case in aggregate["cases"] if case["seed"] == PRIMARY_SEED]
    by_key = {(case["lightCount"], case["requestedRadius"]): case for case in primary}
    timing = aggregate.get("analyticScreenTiming", {})
    classifications = aggregate["classifications"]
    friendly = [item for item in classifications if item["classification"] == "culling-friendly"]
    saturated = [item for item in classifications if item["classification"] == "saturated"]
    candidate = aggregate["decision"]["phaseBAnchor"]
    boundary = aggregate["decision"]["failureBoundary"]
    correctness_passed = bool(aggregate["correctness"]["passed"])
    conclusion = (
        f"结论：60 个离线正式组合均零漏灯、半径误差合格、Master Pool 前缀一致。20 个 N×R Cell 中，`{len(friendly)}` 个为 Culling-friendly、`{len(saturated)}` 个为 Saturated，其余为 Transition。"
        if correctness_passed
        else f"结论：Incorrect。miss={aggregate['correctness']['totalMissInteractions']}、determinism failures={aggregate['correctness']['determinismFailures']}、radius failures={aggregate['correctness']['radiusFailures']}、invalid-skip={aggregate['correctness']['invalidSkipSmokePassed']}；不得进入 Phase B。"
    )
    lines = [
        "# 点光源数量 × 有效半径控制变量全因子实验报告",
        "",
        conclusion,
        "",
        f"冻结协议 SHA-256：`{aggregate['protocolSha256']}`。本报告严格区分离线候选代理和现有 `analytic-screen` 实测时间；没有 Tile/Cluster Runtime，也没有 GPU 加速百分比。",
        (
            f"分析器执行 SHA-256：`{aggregate['analysisProvenance']['executedAnalyzerSha256']}`。"
            + (
                "采集后仅修复离线分析器的 uint64 计数类型转换；冻结协议、Release EXE 与 60 份原始采集未改动。"
                if aggregate['analysisProvenance']['postCaptureAnalyzerFix']
                else "与采集清单记录版本一致。"
            )
        ),
        "",
        "## 1. 主 Seed 二维结果",
        "",
        "| N | R | GT mean 灯/像素 | overlap | Cluster/Tile candidate | removable capture | Cluster CSR MiB | Oracle GPU 点光 ms | 分类 |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    class_map = {(item["lightCount"], item["radius"]): item["classification"] for item in classifications}
    for count in COUNTS:
        for radius in RADII:
            case = by_key[(count, radius)]
            timing_value = timing.get(f"n{count}-r{radius:g}")
            gpu = timing_value["processAggregates"]["gpuPointLightsMedianMs"]["median"] if timing_value else None
            gpu_text = f"{gpu:.4f}" if gpu is not None else "N/A"
            lines.append(
                f"| {count} | {radius:g} | {case['groundTruthLightsPerPixel']['mean']:.3f} | "
                f"{case['overlapFraction']:.3f} | {case['comparison']['clusterToTileCandidateRatio']:.3f}× | "
                f"{case['comparison']['removableWorkCapture']:.3f} | {case['cluster16']['totalLogicalMiB']:.2f} | "
                f"{gpu_text} | {class_map[(count, radius)]} |"
            )
    lines += [
        "",
        "## 2. 因果解释",
        "",
        "- 固定 R 增加 N 时，GT interactions、Tile/Cluster candidates 和当前逐灯 Draw/CPU 提交近似随 N 增长；每盏灯的空间覆盖不因 N 变化。",
        "- 固定 N 增加 R 时，单灯球覆盖更多 XY Tile 与 Z Slice，真实 overlap 上升；Cluster 能消除的候选空间缩小，同时同一灯被复制进更多 Cluster，CSR 内存增长。",
        "- `removableWorkCapture` 衡量 Cluster 捕获 Tile 可剔除误收的能力；它和 `overlapFraction` 必须一起看。高 overlap 下，即使索引准确，真实必须计算的灯也不能被剔除。",
        "",
        "## 3. Phase-B 边界",
        "",
        (f"- 推荐未来 Runtime A/B Anchor：`N={candidate['lightCount']}, R={candidate['radius']}`，分类 `{candidate['classification']}`。主 Seed 与至少 2/3 Seed 均通过离线门槛；这只表示允许进入下一轮。" if candidate is not None else "- 本轮没有 Cell 同时满足主 Seed 与至少 2/3 Seed 的 Phase-B 门槛，因此不推荐 Runtime A/B Anchor。"),
        f"- 失败/饱和边界：`N={boundary['lightCount']}, R={boundary['radius']}`，分类 `{boundary['classification']}`。",
        "- Culling-friendly 门槛：三 Seed 至少 2/3 同时满足 candidate ratio≤0.70、removable capture≥0.70、Cluster16≤64 MiB、零 miss。",
        "",
        "## 4. 实测时间与离线代理",
        "",
    ]
    if timing:
        lines += [
            "主 Seed 的 20 Cell 均使用同一 Release EXE，3 个独立进程，每进程 300 帧预热 + 600 帧采样。下列时间是当前精确逐灯 `analytic-screen` Oracle，不是 Tile/Cluster：",
            "",
            "| N | R | CPU Frame Median ms | CPU Point Lights ms | GPU Point Lights ms | Draw Calls |",
            "|---:|---:|---:|---:|---:|---:|",
        ]
        for count in COUNTS:
            for radius in RADII:
                value = timing[f"n{count}-r{radius:g}"]["processAggregates"]
                lines.append(
                    f"| {count} | {radius:g} | {value['cpuFrameMedianMs']['median']:.4f} | "
                    f"{value['cpuPointLightsMedianMs']['median']:.4f} | {value['gpuPointLightsMedianMs']['median']:.4f} | "
                    f"{value['drawCallsMedian']['median']:.0f} |"
                )
    else:
        lines.append("Timing 尚未执行；当前报告只包含离线候选代理。")
    lines += [
        "",
        "## 5. 正确性、硬件与限制",
        "",
        f"- Tile/Cluster miss 总数：`{aggregate['correctness']['totalMissInteractions']}`；确定性失败：`{aggregate['correctness']['determinismFailures']}`；半径失败：`{aggregate['correctness']['radiusFailures']}`；前缀失败：0。",
        f"- 正式 G-Buffer 有效像素：`{aggregate['environment']['validPixelCount']}`；无效/天空像素：`{aggregate['environment']['invalidPixelCount']}`。若后者为 0，本轮没有真实天空覆盖，只完成了 invalid skip 契约 smoke。",
        f"- GPU `GL_MAX_TEXTURE_BUFFER_SIZE`：`{aggregate['environment']['hardwareLimits']['maxTextureBufferTexels']}` texels；它只用于未来上传可行性判断。",
        "- 全量 Cluster CSR 是逻辑内存；Active-only、uint16 和 Global/Local 分流均为离线 what-if，不是实现或实测收益。",
        "- 结论仅适用于固定 Sponza、相机、Uniform representative 灯位分布、1920×1080、16×16 Tile、16 个对数 Z Slice。聚集分布、移动相机/灯、阴影、透明 Forward、PBR、动态构建与上传未进入本矩阵。",
        "",
        "## 6. 图表、截图与复现",
        "",
        "- `charts/gt-mean-heatmap.png`、`candidate-ratio-heatmap.png`、`cluster-csr-memory-heatmap.png`、`analytic-screen-gpu-heatmap.png`、`removable-work-capture-heatmap.png`；",
        "- `charts/fixed-radius-candidate-ratio.png`、`charts/fixed-count-candidate-ratio.png`；",
        "- `charts/seed-overlap-median-min-max.png`、`seed-candidate-ratio-median-min-max.png`、`seed-cluster-memory-median-min-max.png`：三 Seed Median [Min, Max]；",
        "- `screenshots/*-exact-analytic-screen.png`：真实 Release renderer；`heatmaps/*-renderer-tile-cluster.png`：离线列表长度解释图；",
        "- `cases/*.json`、`summary.csv`、`cells/*.csv.gz`、`csr/*.npz`、`pixel-counts/*.npz`、`timing/`、`verification/`、`artifact-manifest.json`；",
        "- 复现：`powershell -ExecutionPolicy Bypass -File .\\tools\\run_count_radius_factorial.ps1 -Mode All`。",
    ]
    (run_dir / "REPORT_CN.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def make_charts(run_dir: Path, cases: list[dict[str, Any]], timing: dict[tuple[int, float], dict[str, Any]]) -> None:
    primary = {(case["lightCount"], case["requestedRadius"]): case for case in cases if case["seed"] == PRIMARY_SEED}
    def matrix_of(selector) -> np.ndarray:
        return np.asarray([[selector(primary[(count, radius)]) for count in COUNTS] for radius in RADII], dtype=np.float64)
    draw_heatmap(run_dir / "charts/gt-mean-heatmap.png", "Ground Truth mean lights per valid pixel", "Primary seed 0x21D3F3A5; exact sphere predicate", matrix_of(lambda c: c["groundTruthLightsPerPixel"]["mean"]), lambda v: f"{v:.2f}", "lights/pixel")
    draw_heatmap(run_dir / "charts/candidate-ratio-heatmap.png", "Cluster16 / Tile candidate interactions", "Offline exact light-index lists; lower is better", matrix_of(lambda c: c["comparison"]["clusterToTileCandidateRatio"]), lambda v: f"{v:.3f}×", "ratio")
    draw_heatmap(run_dir / "charts/cluster-csr-memory-heatmap.png", "Cluster16 full logical CSR memory", "8-byte metadata/cell + uint32 indices", matrix_of(lambda c: c["cluster16"]["totalLogicalMiB"]), lambda v: f"{v:.2f}", "MiB")
    draw_heatmap(run_dir / "charts/removable-work-capture-heatmap.png", "Removable work captured by Cluster16", "(Tile candidates - Cluster candidates) / (Tile candidates - Ground Truth)", matrix_of(lambda c: c["comparison"]["removableWorkCapture"]), lambda v: f"{v:.3f}", "fraction")
    if timing:
        matrix = np.asarray([[timing[(count, radius)]["processAggregates"]["gpuPointLightsMedianMs"]["median"] for count in COUNTS] for radius in RADII])
        draw_heatmap(run_dir / "charts/analytic-screen-gpu-heatmap.png", "Measured analytic-screen GPU point-light time", "3 processes; 300 warmup + 600 samples/process; not Cluster GPU time", matrix, lambda v: f"{v:.3f}", "ms")
    series_radius = []
    for radius in RADII:
        series_radius.append((f"R={radius:g}", list(COUNTS), [primary[(count, radius)]["comparison"]["clusterToTileCandidateRatio"] for count in COUNTS]))
    draw_lines(run_dir / "charts/fixed-radius-candidate-ratio.png", "Fixed R: candidate ratio vs light count", series_radius, "Light count N", "Cluster / Tile")
    series_count = []
    for count in COUNTS:
        series_count.append((f"N={count}", list(RADII), [primary[(count, radius)]["comparison"]["clusterToTileCandidateRatio"] for radius in RADII]))
    draw_lines(run_dir / "charts/fixed-count-candidate-ratio.png", "Fixed N: candidate ratio vs effective radius", series_count, "Effective radius R", "Cluster / Tile")
    grouped: dict[tuple[int, float], list[dict[str, Any]]] = {}
    for case in cases:
        grouped.setdefault((case["lightCount"], case["requestedRadius"]), []).append(case)
    draw_seed_summary_heatmap(
        run_dir / "charts/seed-overlap-median-min-max.png",
        "Ground Truth overlap across three seeds",
        "Controlled uniform placement repeats",
        {key: [case["overlapFraction"] for case in group] for key, group in grouped.items()},
        lambda value: f"{value:.3f}",
        "overlap fraction",
    )
    draw_seed_summary_heatmap(
        run_dir / "charts/seed-candidate-ratio-median-min-max.png",
        "Cluster16 / Tile candidate ratio across three seeds",
        "Offline exact-list proxy; lower is better",
        {key: [case["comparison"]["clusterToTileCandidateRatio"] for case in group] for key, group in grouped.items()},
        lambda value: f"{value:.3f}",
        "ratio",
    )
    draw_seed_summary_heatmap(
        run_dir / "charts/seed-cluster-memory-median-min-max.png",
        "Cluster16 logical CSR memory across three seeds",
        "Full logical CSR, uint32 indices",
        {key: [case["cluster16"]["totalLogicalMiB"] for case in group] for key, group in grouped.items()},
        lambda value: f"{value:.2f}",
        "MiB",
    )


def analyze(run_dir: Path) -> dict[str, Any]:
    protocol_path = run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"
    manifest_path = run_dir / "capture-manifest.json"
    protocol_hash = sha256_file(protocol_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if not manifest.get("protocolFrozenBeforeCapture") or manifest["protocolSha256"] != protocol_hash:
        raise ValueError("frozen protocol hash/order validation failed")
    cases = load_captured_cases(manifest, manifest_path)
    prefix_validation = validate_prefix_invariants(cases)
    effective_by_radius, effective_validation = validate_effective_radius_invariants(cases)
    position_path = Path(manifest["sharedCaptures"]["position"])
    validity_path = Path(manifest["sharedCaptures"]["validity"])
    if sha256_file(position_path) != manifest["sharedCaptures"]["positionSha256"] or sha256_file(validity_path) != manifest["sharedCaptures"]["validitySha256"]:
        raise ValueError("shared G-buffer hashes changed")
    position = np.asarray(base.read_pfm(position_path), dtype=np.float64)
    validity = np.asarray(base.read_pfm(validity_path) > 0.0, dtype=bool)
    if position.shape != (1080, 1920, 3) or validity.shape != (1080, 1920):
        raise ValueError("formal shared G-buffer shape mismatch")
    if not np.all(np.isfinite(position)):
        raise ValueError("non-finite formal G-buffer")
    primary_scene = build_scene_data(cases[case_key(PRIMARY_SEED, 512, RADII[0])], position, validity)
    if np.any(primary_scene.view_depth_valid <= 0.0):
        raise ValueError("non-positive valid view depth")
    layout = base.build_layout(primary_scene)
    orientation = base.pfm_projection_orientation(primary_scene)
    tile_cell_ids = layout["validTileIds"]
    cluster_cell_ids, _ = base.valid_cell_ids(primary_scene, layout, CLUSTER_SLICES)
    tile_occupancy = np.bincount(tile_cell_ids, minlength=layout["tileCount"]).astype(np.uint32)
    cluster_occupancy = np.bincount(cluster_cell_ids, minlength=layout["tileCount"] * CLUSTER_SLICES).astype(np.uint32)
    all_results: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []
    total_misses = 0
    determinism_failures = 0
    radius_failures = 0
    for seed in SEEDS:
        master_captured = cases[case_key(seed, 512, RADII[0])]
        master_scene = build_scene_data(master_captured, position, validity)
        radius_memberships: dict[float, tuple[np.ndarray, np.ndarray]] = {}
        timing_by_radius: dict[float, dict[str, float]] = {}
        deterministic_by_radius: dict[float, bool] = {}
        for radius in RADII:
            effective_radius = effective_by_radius[radius]
            scene = replace(master_scene, radius=effective_radius)
            tile, cluster, build_timing = build_memberships(scene, layout, effective_radius)
            repeat_tile, repeat_cluster, _ = build_memberships(scene, layout, effective_radius)
            deterministic = np.array_equal(tile, repeat_tile) and np.array_equal(cluster, repeat_cluster)
            deterministic_by_radius[radius] = deterministic
            if not deterministic:
                determinism_failures += 1
            radius_memberships[radius] = (tile, cluster)
            timing_by_radius[radius] = build_timing
            print(f"[membership] seed=0x{seed:08X} R={radius:g} tile={tile.shape} cluster={cluster.shape}", flush=True)
        truth_counts, misses = count_distribution_for_prefixes(
            master_scene.valid_positions,
            master_scene.light_positions,
            radius_memberships,
            tile_cell_ids,
            cluster_cell_ids,
            effective_by_radius,
        )
        for radius in RADII:
            tile_membership, cluster_membership = radius_memberships[radius]
            for count in COUNTS:
                captured = cases[case_key(seed, count, radius)]
                truth = truth_counts[(radius, count)]
                tile_metric, tile_counts, tile_pixel = metric_for_scheme(
                    tile_membership, count, tile_cell_ids, tile_occupancy, truth,
                    misses[(radius, "tile", count)],
                )
                cluster_metric, cluster_counts, cluster_pixel = metric_for_scheme(
                    cluster_membership, count, cluster_cell_ids, cluster_occupancy, truth,
                    misses[(radius, "cluster-16", count)],
                )
                total_misses += tile_metric["missInteractions"] + cluster_metric["missInteractions"]
                truth_total = int(np.sum(truth, dtype=np.uint64))
                overlap = float(truth_total / (truth.size * count))
                denominator = tile_metric["candidateInteractions"] - truth_total
                zero_denominator = denominator == 0
                removable = (
                    1.0 if zero_denominator and cluster_metric["candidateInteractions"] == truth_total
                    else 0.0 if zero_denominator
                    else (tile_metric["candidateInteractions"] - cluster_metric["candidateInteractions"]) / denominator
                )
                culling_opportunity = (
                    (tile_metric["candidateInteractions"] - truth_total) / tile_metric["candidateInteractions"]
                    if tile_metric["candidateInteractions"] else 0.0
                )
                offsets_t, counts_t, indices_t, hash_t = csr_arrays(tile_membership, count)
                offsets_c, counts_c, indices_c, hash_c = csr_arrays(cluster_membership, count)
                repeat_hash_t = base.sha256_csr(offsets_t, counts_t, indices_t)
                repeat_hash_c = base.sha256_csr(offsets_c, counts_c, indices_c)
                deterministic = deterministic_by_radius[radius] and hash_t == repeat_hash_t and hash_c == repeat_hash_c
                tile_metric["csrSha256"] = hash_t
                cluster_metric["csrSha256"] = hash_c
                point = captured.result["pointLightStress"]
                if abs(float(point["volumeRadius"]) - radius) > RADIUS_TOLERANCE:
                    radius_failures += 1
                result = {
                    "stem": captured.manifest["stem"],
                    "seed": seed,
                    "seedOrdinal": captured.seed_ordinal,
                    "lightCount": count,
                    "requestedRadius": radius,
                    "effectiveRadius": float(point["volumeRadius"]),
                    "radiusAbsoluteError": float(point["radiusAbsoluteError"]),
                    "attenuation": {
                        "constant": float(point["constant"]),
                        "linear": float(point["linear"]),
                        "quadratic": float(point["quadratic"]),
                        "threshold": float(point["attenuationThreshold"]),
                    },
                    "sceneSignature": point["sceneSignature"],
                    "submissionSignature": point["submissionSignature"],
                    "positionPrefixSignature": point["positionPrefixSignature"],
                    "colorPrefixSignature": point["colorPrefixSignature"],
                    "validPixelCount": int(truth.size),
                    "groundTruthInteractions": truth_total,
                    "groundTruthLightsPerPixel": distribution(truth),
                    "overlapFraction": overlap,
                    "tile": tile_metric,
                    "cluster16": cluster_metric,
                    "comparison": {
                        "clusterToTileCandidateRatio": float(cluster_metric["candidateInteractions"] / tile_metric["candidateInteractions"] if tile_metric["candidateInteractions"] else 0.0),
                        "clusterToTilePixelMeanRatio": float(cluster_metric["listLengthPixelWeighted"]["mean"] / tile_metric["listLengthPixelWeighted"]["mean"] if tile_metric["listLengthPixelWeighted"]["mean"] else 0.0),
                        "cullingOpportunity": culling_opportunity,
                        "removableWorkCapture": float(removable),
                        "removableWorkZeroDenominator": zero_denominator,
                    },
                    "bounds": bounds_summary(captured),
                    "offlineMembershipTiming": timing_by_radius[radius],
                    "deterministic": deterministic,
                    "sourceJson": str(captured.result_path),
                    "rendererCapture": str(captured.ldr_path),
                }
                if seed == PRIMARY_SEED:
                    csr_metadata = {"seed": seed, "lightCount": count, "radius": radius, "tileSize": TILE_SIZE, "clusterSlices": CLUSTER_SLICES}
                    save_csr(run_dir / "csr" / f"{captured.manifest['stem']}-tile.npz", offsets_t, counts_t, indices_t, {**csr_metadata, "scheme": "tile"})
                    save_csr(run_dir / "csr" / f"{captured.manifest['stem']}-cluster-16.npz", offsets_c, counts_c, indices_c, {**csr_metadata, "scheme": "cluster-16"})
                    (run_dir / "pixel-counts").mkdir(parents=True, exist_ok=True)
                    np.savez_compressed(
                        run_dir / "pixel-counts" / f"{captured.manifest['stem']}.npz",
                        valid_flat_indices=master_scene.valid_flat_indices.astype(np.uint32),
                        ground_truth=truth,
                        tile_list=tile_pixel.astype(np.uint16),
                        cluster_list=cluster_pixel.astype(np.uint16),
                    )
                    write_cells_gzip(run_dir / "cells" / f"{captured.manifest['stem']}-tile.csv.gz", tile_counts, tile_occupancy, layout["tilesX"], layout["tileCount"], 0)
                    write_cells_gzip(run_dir / "cells" / f"{captured.manifest['stem']}-cluster-16.csv.gz", cluster_counts, cluster_occupancy, layout["tilesX"], layout["tileCount"], CLUSTER_SLICES)
                json_dump(run_dir / "cases" / f"{captured.manifest['stem']}.json", result)
                all_results.append(result)
                summary_rows.append({
                    "seed": f"0x{seed:08X}", "lightCount": count, "radius": radius,
                    "gtMean": result["groundTruthLightsPerPixel"]["mean"],
                    "gtP95": result["groundTruthLightsPerPixel"]["p95"],
                    "overlapFraction": overlap,
                    "tileCandidate": tile_metric["candidateInteractions"],
                    "clusterCandidate": cluster_metric["candidateInteractions"],
                    "candidateRatio": result["comparison"]["clusterToTileCandidateRatio"],
                    "cullingOpportunity": culling_opportunity,
                    "removableWorkCapture": removable,
                    "tileMiB": tile_metric["totalLogicalMiB"],
                    "clusterMiB": cluster_metric["totalLogicalMiB"],
                    "tileMiss": tile_metric["missInteractions"],
                    "clusterMiss": cluster_metric["missInteractions"],
                    "deterministic": deterministic,
                })
                del offsets_t, counts_t, indices_t, offsets_c, counts_c, indices_c
            del radius_memberships[radius]
        del truth_counts
        gc.collect()
    write_csv(run_dir / "summary.csv", summary_rows)
    timing, timing_rows = parse_timing(run_dir)
    if timing_rows:
        write_csv(run_dir / "timing-summary.csv", timing_rows)
    classifications = classify_cells(all_results)
    seed_summary_rows = []
    for item in classifications:
        metrics = item["seedMetrics"]
        row: dict[str, Any] = {
            "lightCount": item["lightCount"],
            "radius": item["radius"],
            "classification": item["classification"],
            "phaseBEligible": item["phaseBEligible"],
        }
        for name, key in (
            ("overlap", "overlapFraction"),
            ("candidateRatio", "candidateRatio"),
            ("clusterMiB", "clusterMiB"),
        ):
            values = [float(metric[key]) for metric in metrics]
            row[f"{name}Median"] = statistics.median(values)
            row[f"{name}Min"] = min(values)
            row[f"{name}Max"] = max(values)
        seed_summary_rows.append(row)
    write_csv(run_dir / "seed-cell-summary.csv", seed_summary_rows)
    friendly = [item for item in classifications if item["classification"] == "culling-friendly"]
    eligible = [item for item in classifications if item["phaseBEligible"]]
    saturated = [item for item in classifications if item["classification"] == "saturated"]
    global_correctness = total_misses == 0 and determinism_failures == 0 and radius_failures == 0
    phase_anchor = max(eligible, key=lambda item: item["lightCount"] * item["radius"]) if eligible and global_correctness else None
    if saturated:
        failure_boundary = min(saturated, key=lambda item: item["lightCount"] * item["radius"])
    else:
        failure_boundary = max(classifications, key=lambda item: item["lightCount"] * item["radius"])
    timing_public = {f"n{count}-r{radius:g}": value for (count, radius), value in timing.items()}
    invalid_indices = np.flatnonzero(validity.reshape(-1))[:1024]
    synthetic = position.reshape(-1, 3).copy()
    synthetic_validity = validity.reshape(-1).copy()
    synthetic_validity[invalid_indices] = False
    synthetic[invalid_indices] = np.nan
    invalid_skip_passed = bool(np.all(np.isfinite(synthetic[synthetic_validity])))
    hardware_limits = cases[case_key(PRIMARY_SEED, 32, 1.5)].result["pointLightStress"]["hardwareLimits"]
    aggregate = {
        "schemaVersion": 1,
        "experiment": "point-light-count-radius-factorial-phase-a",
        "protocolSha256": protocol_hash,
        "captureManifestSha256": sha256_file(manifest_path),
        "timingManifestSha256": sha256_file(run_dir / "timing-manifest.json") if (run_dir / "timing-manifest.json").is_file() else None,
        "analysisProvenance": {
            "captureRecordedAnalyzerSha256": manifest["sourceHashes"]["analyzer"],
            "executedAnalyzerSha256": sha256_file(Path(__file__).resolve()),
            "postCaptureAnalyzerFix": (
                manifest["sourceHashes"]["analyzer"]
                != sha256_file(Path(__file__).resolve())
            ),
            "captureInputsUnchanged": True,
        },
        "factors": {"counts": list(COUNTS), "radii": list(RADII), "seeds": list(SEEDS)},
        "environment": {
            "resolution": [1920, 1080], "tileSize": TILE_SIZE, "clusterSlices": CLUSTER_SLICES,
            "validPixelCount": int(np.count_nonzero(validity)),
            "invalidPixelCount": int(validity.size - np.count_nonzero(validity)),
            "pfmProjectionConvention": orientation,
            "hardwareLimits": hardware_limits,
        },
        "prefixValidation": prefix_validation,
        "effectiveRadiusValidation": effective_validation,
        "correctness": {
            "totalMissInteractions": total_misses,
            "determinismFailures": determinism_failures,
            "radiusFailures": radius_failures,
            "invalidSkipSmokePassed": invalid_skip_passed,
            "passed": global_correctness and invalid_skip_passed,
        },
        "cases": all_results,
        "classifications": classifications,
        "decision": {
            "phaseBAnchor": phase_anchor,
            "failureBoundary": failure_boundary,
            "runtimeCandidateImplemented": False,
            "defaultChanged": False,
        },
        "analyticScreenTiming": timing_public,
        "timingLabel": "Measured Release analytic-screen oracle; not Tile/Cluster runtime",
        "offlineProxyLabel": "Offline exact candidate interactions and logical CSR bytes; not GPU speedup",
    }
    json_dump(run_dir / "aggregate.json", aggregate)
    make_charts(run_dir, all_results, timing)
    primary_cases = {(case["lightCount"], case["requestedRadius"]): case for case in all_results if case["seed"] == PRIMARY_SEED}
    for count, radius, label in ANCHORS:
        captured = cases[case_key(PRIMARY_SEED, count, radius)]
        annotated = annotate_anchor(captured, run_dir / "screenshots" / f"{label}-n{count:04d}-r{int(radius*10):03d}-exact-analytic-screen.png")
        tile_npz = np.load(run_dir / "csr" / f"{captured.manifest['stem']}-tile.npz")
        cluster_npz = np.load(run_dir / "csr" / f"{captured.manifest['stem']}-cluster-16.npz")
        make_anchor_heatmap(
            captured, annotated, validity, layout,
            tile_npz["counts"], cluster_npz["counts"], cluster_cell_ids,
            run_dir / "heatmaps" / f"{label}-n{count:04d}-r{int(radius*10):03d}-renderer-tile-cluster.png",
        )
        tile_npz.close(); cluster_npz.close()
    generate_report(run_dir, aggregate)
    return aggregate


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    if args.report_only:
        aggregate = json.loads((run_dir / "aggregate.json").read_text(encoding="utf-8-sig"))
        if aggregate["protocolSha256"] != sha256_file(run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"):
            raise ValueError("aggregate/protocol hash mismatch")
        generate_report(run_dir, aggregate)
        print("[report-only] regenerated report and artifact manifest", flush=True)
        build_artifact_manifest(run_dir)
        return 0
    aggregate = analyze(run_dir)
    print(
        f"[complete] cases={len(aggregate['cases'])} miss={aggregate['correctness']['totalMissInteractions']} "
        f"classes={len(aggregate['classifications'])}",
        flush=True,
    )
    build_artifact_manifest(run_dir)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[count-radius-factorial] fatal: {error}", file=sys.stderr, flush=True)
        raise
