#!/usr/bin/env python3
"""Exact Tile vs Cluster point-light list Phase-A analyzer.

This is deliberately an offline candidate-work experiment.  It consumes a real
Release renderer G-buffer and light list, builds conservative CSR lists, checks
every ground-truth pixel/light interaction for misses, and produces evidence.
It does not claim engine CPU or GPU timings.
"""

from __future__ import annotations

import argparse
import csv
import gc
import hashlib
import json
import math
import os
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont


TILE_SIZE = 16
PRIMARY_SLICES = 16
SENSITIVITY_SLICES = (8, 16, 24, 32)
LIGHT_SEED = 0x21D3F3A5
TIMING_WARMUPS = 1
TIMING_REPEATS = 7
EPSILON = 1.0e-6
UINT32_MAX = np.iinfo(np.uint32).max
MIB = 1024.0 * 1024.0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def sha256_csr(offsets: np.ndarray, counts: np.ndarray, indices: np.ndarray) -> str:
    digest = hashlib.sha256()
    for name, value in (("offsets", offsets), ("counts", counts), ("indices", indices)):
        digest.update(name.encode("ascii"))
        digest.update(str(value.dtype).encode("ascii"))
        digest.update(np.asarray(value.shape, dtype=np.uint64).tobytes())
        digest.update(np.ascontiguousarray(value).tobytes())
    return digest.hexdigest().upper()


def json_dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def percentile(values: np.ndarray, q: float) -> float:
    if values.size == 0:
        return 0.0
    return float(np.percentile(values, q))


def distribution(values: np.ndarray) -> dict[str, float | int]:
    values = np.asarray(values)
    if values.size == 0:
        return {
            "count": 0,
            "total": 0,
            "mean": 0.0,
            "median": 0.0,
            "p95": 0.0,
            "p99": 0.0,
            "max": 0.0,
        }
    return {
        "count": int(values.size),
        "total": int(np.sum(values, dtype=np.uint64)),
        "mean": float(np.mean(values, dtype=np.float64)),
        "median": float(np.median(values)),
        "p95": percentile(values, 95.0),
        "p99": percentile(values, 99.0),
        "max": int(np.max(values)),
    }


def timing_distribution(values: list[float]) -> dict[str, float | int]:
    data = np.asarray(values, dtype=np.float64)
    return {
        "samples": int(data.size),
        "medianMs": float(np.median(data)),
        "p95Ms": percentile(data, 95.0),
        "p99Ms": percentile(data, 99.0),
        "minMs": float(np.min(data)),
        "maxMs": float(np.max(data)),
    }


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        magic = stream.readline().strip()
        if magic not in (b"PF", b"Pf"):
            raise ValueError(f"invalid PFM magic in {path}: {magic!r}")
        dimensions = stream.readline().decode("ascii").strip().split()
        while dimensions and dimensions[0].startswith("#"):
            dimensions = stream.readline().decode("ascii").strip().split()
        if len(dimensions) != 2:
            raise ValueError(f"invalid PFM dimensions in {path}")
        width, height = (int(value) for value in dimensions)
        scale = float(stream.readline().decode("ascii").strip())
        dtype = "<f4" if scale < 0.0 else ">f4"
        channels = 3 if magic == b"PF" else 1
        data = np.fromfile(stream, dtype=dtype)
    expected = width * height * channels
    if data.size != expected:
        raise ValueError(f"PFM size mismatch in {path}: {data.size} != {expected}")
    shape = (height, width, channels) if channels == 3 else (height, width)
    result = data.reshape(shape)
    if not np.all(np.isfinite(result)):
        raise ValueError(f"non-finite PFM values in {path}")
    return result


@dataclass
class SceneData:
    stem: str
    coverage: str
    light_count: int
    width: int
    height: int
    position: np.ndarray
    validity: np.ndarray
    valid_positions: np.ndarray
    valid_flat_indices: np.ndarray
    view_depth_valid: np.ndarray
    camera_position: np.ndarray
    view: np.ndarray
    projection: np.ndarray
    near_plane: float
    far_plane: float
    light_positions: np.ndarray
    light_view_positions: np.ndarray
    radius: float
    scene_signature: str
    submission_signature: str
    source_json: Path
    app_ldr: Path


def resolve_manifest_path(value: str, manifest_path: Path) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = manifest_path.parent / path
    return path.resolve()


def load_scene(case: dict[str, Any], manifest_path: Path) -> SceneData:
    result_path = resolve_manifest_path(case["result"], manifest_path)
    position_path = resolve_manifest_path(case["captures"]["position"], manifest_path)
    validity_path = resolve_manifest_path(case["captures"]["validity"], manifest_path)
    app_ldr = resolve_manifest_path(case["ldr"], manifest_path)
    result = json.loads(result_path.read_text(encoding="utf-8-sig"))
    point = result["pointLightStress"]
    if not result.get("success"):
        raise ValueError(f"renderer result is not successful: {result_path}")
    if point["renderMode"] != "analytic-screen" or not point["renderModeExplicit"]:
        raise ValueError(f"capture is not explicit analytic-screen: {result_path}")
    if int(point["seed"]) != LIGHT_SEED:
        raise ValueError(f"unexpected light seed in {result_path}")
    if int(point["generatedLightCount"]) != int(case["lightCount"]):
        raise ValueError(f"light-count mismatch in {result_path}")

    position = np.asarray(read_pfm(position_path), dtype=np.float64)
    validity_raw = read_pfm(validity_path)
    validity = np.asarray(validity_raw > 0.0, dtype=bool)
    height, width, channels = position.shape
    if channels != 3 or validity.shape != (height, width):
        raise ValueError(f"G-buffer shape mismatch in {result_path}")
    if [width, height] != [int(value) for value in result["resolution"]]:
        raise ValueError(f"resolution mismatch in {result_path}")
    if [width, height] != [1920, 1080]:
        raise ValueError(f"formal capture must be 1920x1080: {result_path}")

    matrices = result["gBuffer"]["cameraMatrices"]
    view = np.asarray(matrices["view"], dtype=np.float64)
    projection = np.asarray(matrices["projection"], dtype=np.float64)
    near_plane = float(matrices["nearPlane"])
    far_plane = float(matrices["farPlane"])
    if not (0.0 < near_plane < far_plane):
        raise ValueError(f"invalid near/far planes in {result_path}")
    rotation = view[:3, :3]
    if not np.allclose(rotation @ rotation.T, np.eye(3), atol=2.0e-5):
        raise ValueError(f"View matrix is not rigid; sphere radius would not be preserved: {result_path}")
    if abs(float(projection[0, 2])) > 1.0e-8 or abs(float(projection[1, 2])) > 1.0e-8:
        raise ValueError(
            f"off-axis projection is outside this frozen symmetric-frustum analyzer: {result_path}"
        )
    valid_flat_indices = np.flatnonzero(validity.reshape(-1))
    valid_positions = position.reshape(-1, 3)[valid_flat_indices]
    valid_h = np.concatenate(
        (valid_positions, np.ones((valid_positions.shape[0], 1), dtype=np.float64)), axis=1
    )
    view_positions = (view @ valid_h.T).T
    view_depth_valid = -view_positions[:, 2]
    if np.any(~np.isfinite(view_depth_valid)) or np.any(view_depth_valid <= 0.0):
        raise ValueError(f"invalid view depths in {result_path}")
    if np.any(view_depth_valid < near_plane - 1.0e-3) or np.any(
        view_depth_valid > far_plane + 1.0e-2
    ):
        raise ValueError(f"valid G-buffer pixels outside near/far in {result_path}")

    light_positions = np.asarray(
        [light["position"] for light in point["lights"]], dtype=np.float64
    )
    light_h = np.concatenate(
        (light_positions, np.ones((light_positions.shape[0], 1), dtype=np.float64)), axis=1
    )
    light_view_positions = (view @ light_h.T).T[:, :3]
    radius = float(point["volumeRadius"])
    if light_positions.shape != (int(case["lightCount"]), 3) or radius <= 0.0:
        raise ValueError(f"invalid point-light data in {result_path}")
    image = Image.open(app_ldr)
    if image.size != (width, height):
        raise ValueError(f"renderer screenshot dimensions mismatch: {app_ldr}")

    return SceneData(
        stem=str(case["stem"]),
        coverage=str(case["coverage"]),
        light_count=int(case["lightCount"]),
        width=width,
        height=height,
        position=position,
        validity=validity,
        valid_positions=valid_positions,
        valid_flat_indices=valid_flat_indices,
        view_depth_valid=view_depth_valid,
        camera_position=np.asarray(result["camera"]["position"], dtype=np.float64),
        view=view,
        projection=projection,
        near_plane=near_plane,
        far_plane=far_plane,
        light_positions=light_positions,
        light_view_positions=light_view_positions,
        radius=radius,
        scene_signature=str(point["sceneSignature"]),
        submission_signature=str(point["submissionSignature"]),
        source_json=result_path,
        app_ldr=app_ldr,
    )


def build_layout(scene: SceneData) -> dict[str, Any]:
    tiles_x = (scene.width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (scene.height + TILE_SIZE - 1) // TILE_SIZE
    tile_count = tiles_x * tiles_y
    tile_normals = np.empty((tile_count, 4, 3), dtype=np.float64)
    tile_bounds = np.empty((tile_count, 4), dtype=np.int32)
    tile_ids_full = np.empty(scene.width * scene.height, dtype=np.int32)
    for tile_y in range(tiles_y):
        y0 = tile_y * TILE_SIZE
        y1 = min(scene.height, y0 + TILE_SIZE)
        ndc_y0 = 2.0 * y0 / scene.height - 1.0
        ndc_y1 = 2.0 * y1 / scene.height - 1.0
        c = ndc_y0 / scene.projection[1, 1]
        d = ndc_y1 / scene.projection[1, 1]
        for tile_x in range(tiles_x):
            x0 = tile_x * TILE_SIZE
            x1 = min(scene.width, x0 + TILE_SIZE)
            ndc_x0 = 2.0 * x0 / scene.width - 1.0
            ndc_x1 = 2.0 * x1 / scene.width - 1.0
            a = ndc_x0 / scene.projection[0, 0]
            b = ndc_x1 / scene.projection[0, 0]
            index = tile_y * tiles_x + tile_x
            raw = np.asarray(
                ([1.0, 0.0, a], [-1.0, 0.0, -b], [0.0, 1.0, c], [0.0, -1.0, -d]),
                dtype=np.float64,
            )
            tile_normals[index] = raw / np.linalg.norm(raw, axis=1, keepdims=True)
            tile_bounds[index] = (x0, y0, x1, y1)
            yy, xx = np.mgrid[y0:y1, x0:x1]
            tile_ids_full[(yy * scene.width + xx).reshape(-1)] = index
    valid_tile_ids = tile_ids_full[scene.valid_flat_indices]
    if np.any(valid_tile_ids < 0) or np.any(valid_tile_ids >= tile_count):
        raise RuntimeError(f"invalid Tile assignment in {scene.stem}")
    partial_tiles = int(
        np.count_nonzero(
            (tile_bounds[:, 2] - tile_bounds[:, 0] != TILE_SIZE)
            | (tile_bounds[:, 3] - tile_bounds[:, 1] != TILE_SIZE)
        )
    )
    return {
        "tilesX": tiles_x,
        "tilesY": tiles_y,
        "tileCount": tile_count,
        "tileNormals": tile_normals,
        "tileBounds": tile_bounds,
        "tileIdsFull": tile_ids_full,
        "validTileIds": valid_tile_ids,
        "partialTileCount": partial_tiles,
    }


def pfm_projection_orientation(scene: SceneData) -> dict[str, Any]:
    count = scene.valid_positions.shape[0]
    stride = max(1, count // 20000)
    selected = np.arange(0, count, stride, dtype=np.int64)
    world = scene.valid_positions[selected]
    flat = scene.valid_flat_indices[selected]
    y = flat // scene.width
    x = flat % scene.width
    homogeneous = np.concatenate(
        (world, np.ones((world.shape[0], 1), dtype=np.float64)), axis=1
    )
    clip = (scene.projection @ scene.view @ homogeneous.T).T
    ndc = clip[:, :3] / clip[:, 3, None]
    projected_x = (ndc[:, 0] * 0.5 + 0.5) * scene.width
    projected_y = (ndc[:, 1] * 0.5 + 0.5) * scene.height
    bottom_error = np.sqrt(
        np.square(projected_x - (x + 0.5)) + np.square(projected_y - (y + 0.5))
    )
    top_y = scene.height - 1 - y
    top_error = np.sqrt(
        np.square(projected_x - (x + 0.5)) + np.square(projected_y - (top_y + 0.5))
    )
    result = {
        "sampleCount": int(selected.size),
        "bottomLeftMedianPixelError": float(np.median(bottom_error)),
        "bottomLeftP99PixelError": percentile(bottom_error, 99.0),
        "topLeftMedianPixelError": float(np.median(top_error)),
        "detectedRowOrigin": "bottom-left"
        if float(np.median(bottom_error)) < float(np.median(top_error))
        else "top-left",
    }
    if (
        result["detectedRowOrigin"] != "bottom-left"
        or result["bottomLeftMedianPixelError"] > 1.0
        or result["topLeftMedianPixelError"] < result["bottomLeftMedianPixelError"] * 10.0
    ):
        raise RuntimeError(f"PFM row/projection convention check failed in {scene.stem}: {result}")
    return result


def slice_edges(scene: SceneData, slices: int) -> np.ndarray:
    return np.geomspace(scene.near_plane, scene.far_plane, slices + 1, dtype=np.float64)


def valid_cell_ids(scene: SceneData, layout: dict[str, Any], slices: int) -> tuple[np.ndarray, np.ndarray]:
    if slices == 0:
        return layout["validTileIds"], np.zeros(scene.valid_positions.shape[0], dtype=np.int16)
    edges = slice_edges(scene, slices)
    slice_ids = np.searchsorted(edges, scene.view_depth_valid, side="right") - 1
    slice_ids = np.clip(slice_ids, 0, slices - 1).astype(np.int16)
    cell_ids = slice_ids.astype(np.int64) * layout["tileCount"] + layout["validTileIds"]
    return cell_ids.astype(np.int32), slice_ids


def build_once(scene: SceneData, layout: dict[str, Any], slices: int) -> dict[str, Any]:
    timings: dict[str, float] = {}
    total_start = time.perf_counter_ns()

    start = time.perf_counter_ns()
    tile_membership = np.zeros((layout["tileCount"], scene.light_count), dtype=bool)
    radius = scene.radius + EPSILON
    for light_index, center in enumerate(scene.light_view_positions):
        depth = -float(center[2])
        if depth + radius < scene.near_plane or depth - radius > scene.far_plane:
            continue
        signed = np.einsum("tpc,c->tp", layout["tileNormals"], center)
        tile_membership[:, light_index] = np.all(signed >= -radius, axis=1)

    if slices == 0:
        membership = tile_membership
    else:
        edges = slice_edges(scene, slices)
        depths = -scene.light_view_positions[:, 2]
        depth_membership = (
            (depths[None, :] + radius >= edges[:-1, None])
            & (depths[None, :] - radius <= edges[1:, None])
        )
        membership = (
            depth_membership[:, None, :] & tile_membership[None, :, :]
        ).reshape(slices * layout["tileCount"], scene.light_count)
    timings["transformBoundsMembershipMs"] = (time.perf_counter_ns() - start) / 1.0e6

    start = time.perf_counter_ns()
    counts = np.sum(membership, axis=1, dtype=np.uint32)
    timings["countMs"] = (time.perf_counter_ns() - start) / 1.0e6

    start = time.perf_counter_ns()
    offsets64 = np.empty(counts.size + 1, dtype=np.uint64)
    offsets64[0] = 0
    np.cumsum(counts, dtype=np.uint64, out=offsets64[1:])
    total_indices = int(offsets64[-1])
    if total_indices > UINT32_MAX:
        raise OverflowError(f"CSR index pool exceeds uint32 in {scene.stem}, slices={slices}")
    offsets = offsets64.astype(np.uint32)
    timings["prefixSumMs"] = (time.perf_counter_ns() - start) / 1.0e6

    start = time.perf_counter_ns()
    indices = np.empty(total_indices, dtype=np.uint32)
    cursor = 0
    block_rows = 4096
    for row_start in range(0, membership.shape[0], block_rows):
        block = membership[row_start : row_start + block_rows]
        columns = np.nonzero(block)[1].astype(np.uint32, copy=False)
        indices[cursor : cursor + columns.size] = columns
        cursor += int(columns.size)
    if cursor != total_indices:
        raise RuntimeError("CSR fill count mismatch")
    timings["fillMs"] = (time.perf_counter_ns() - start) / 1.0e6
    timings["totalMs"] = (time.perf_counter_ns() - total_start) / 1.0e6

    csr_hash = sha256_csr(offsets, counts, indices)
    return {
        "slices": slices,
        "membership": membership,
        "counts": counts,
        "offsets": offsets,
        "indices": indices,
        "csrSha256": csr_hash,
        "timing": timings,
    }


def build_with_timing(scene: SceneData, layout: dict[str, Any], slices: int) -> tuple[dict[str, Any], dict[str, Any]]:
    for _ in range(TIMING_WARMUPS):
        warmup = build_once(scene, layout, slices)
        del warmup
        gc.collect()
    samples: list[dict[str, float]] = []
    hashes: list[str] = []
    final: dict[str, Any] | None = None
    for _ in range(TIMING_REPEATS):
        current = build_once(scene, layout, slices)
        samples.append(current["timing"])
        hashes.append(current["csrSha256"])
        if final is not None:
            del final
            gc.collect()
        final = current
    assert final is not None
    if len(set(hashes)) != 1:
        raise RuntimeError(f"non-deterministic CSR in {scene.stem}, slices={slices}")
    components = {}
    for key in samples[0]:
        components[key] = timing_distribution([sample[key] for sample in samples])
    return final, {
        "label": "offline NumPy/Python wall time; not engine CPU or upload time",
        "warmups": TIMING_WARMUPS,
        "repeats": TIMING_REPEATS,
        "components": components,
        "uniqueCsrHashCount": len(set(hashes)),
        "csrSha256": hashes[0],
    }


def save_csr(path: Path, build: dict[str, Any], scene: SceneData) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        path,
        offsets=build["offsets"],
        counts=build["counts"],
        indices=build["indices"],
        width=np.asarray([scene.width], dtype=np.uint32),
        height=np.asarray([scene.height], dtype=np.uint32),
        tile_size=np.asarray([TILE_SIZE], dtype=np.uint32),
        slices=np.asarray([build["slices"]], dtype=np.uint32),
        light_count=np.asarray([scene.light_count], dtype=np.uint32),
    )


def scheme_name(slices: int) -> str:
    return "tile" if slices == 0 else f"cluster-{slices:02d}"


def initial_scheme_metrics(
    scene: SceneData, layout: dict[str, Any], slices: int, build: dict[str, Any]
) -> dict[str, Any]:
    cell_ids, pixel_slice_ids = valid_cell_ids(scene, layout, slices)
    cell_count = layout["tileCount"] if slices == 0 else layout["tileCount"] * slices
    pixel_counts = np.bincount(cell_ids, minlength=cell_count).astype(np.uint32)
    active = pixel_counts > 0
    counts = build["counts"]
    pixel_weighted = counts[cell_ids]
    candidate_interactions = int(np.sum(pixel_weighted, dtype=np.uint64))
    metadata_bytes = int(cell_count * 8)
    total_index_references = int(np.sum(counts, dtype=np.uint64))
    index_bytes = int(total_index_references * 4)
    return {
        "scheme": scheme_name(slices),
        "slices": slices,
        "logicalCellCount": int(cell_count),
        "activeCellCount": int(np.count_nonzero(active)),
        "inactiveCellCount": int(cell_count - np.count_nonzero(active)),
        "validPixelCount": int(cell_ids.size),
        "validPixelRatio": float(cell_ids.size / (scene.width * scene.height)),
        "listLengthAllCells": distribution(counts),
        "listLengthActiveCells": distribution(counts[active]),
        "listLengthPixelWeighted": distribution(pixel_weighted),
        "emptyListRatioAllCells": float(np.count_nonzero(counts == 0) / cell_count),
        "emptyListRatioActiveCells": float(np.count_nonzero(counts[active] == 0) / max(1, np.count_nonzero(active))),
        "validPixelsPerCellAll": distribution(pixel_counts),
        "validPixelsPerCellActive": distribution(pixel_counts[active]),
        "totalIndexReferences": total_index_references,
        "metadataBytes": metadata_bytes,
        "indexBytes": index_bytes,
        "totalLogicalBytes": metadata_bytes + index_bytes,
        "candidateInteractions": candidate_interactions,
        "groundTruthInteractions": 0,
        "truePositiveInteractions": 0,
        "falsePositiveInteractions": 0,
        "falsePositiveRate": 0.0,
        "candidateToGroundTruthRatio": 0.0,
        "missInteractions": 0,
        "overflow": False,
        "csrSha256": build["csrSha256"],
        "cellIds": cell_ids,
        "pixelSliceIds": pixel_slice_ids,
        "pixelCounts": pixel_counts,
    }


def analyze_ground_truth(
    scene: SceneData,
    builds: dict[int, dict[str, Any]],
    metrics: dict[int, dict[str, Any]],
) -> tuple[int, list[dict[str, Any]]]:
    truth_total = 0
    per_light_rows: list[dict[str, Any]] = []
    radius_squared = scene.radius * scene.radius
    for light_index, light_position in enumerate(scene.light_positions):
        delta = scene.valid_positions - light_position[None, :]
        inside = np.einsum("ij,ij->i", delta, delta) <= radius_squared
        actual = int(np.count_nonzero(inside))
        truth_total += actual
        row: dict[str, Any] = {
            "scene": scene.stem,
            "lightIndex": light_index,
            "groundTruthInteractions": actual,
        }
        for slices, build in builds.items():
            name = scheme_name(slices)
            metric = metrics[slices]
            membership = build["membership"]
            cell_ids = metric["cellIds"]
            if actual:
                present = membership[cell_ids[inside], light_index]
                missed = int(np.count_nonzero(~present))
            else:
                missed = 0
            candidate = int(np.sum(metric["pixelCounts"][membership[:, light_index]], dtype=np.uint64))
            metric.setdefault("missByLight", []).append(missed)
            metric.setdefault("candidateByLight", []).append(candidate)
            row[f"{name}CandidateInteractions"] = candidate
            row[f"{name}MissInteractions"] = missed
        per_light_rows.append(row)

    for slices, metric in metrics.items():
        misses = int(sum(metric.pop("missByLight")))
        true_positive = truth_total - misses
        false_positive = metric["candidateInteractions"] - true_positive
        if false_positive < 0:
            raise RuntimeError(f"negative false-positive count in {scene.stem}/{scheme_name(slices)}")
        metric["groundTruthInteractions"] = int(truth_total)
        metric["truePositiveInteractions"] = int(true_positive)
        metric["falsePositiveInteractions"] = int(false_positive)
        metric["falsePositiveRate"] = float(
            false_positive / metric["candidateInteractions"]
            if metric["candidateInteractions"]
            else 0.0
        )
        metric["candidateToGroundTruthRatio"] = float(
            metric["candidateInteractions"] / truth_total if truth_total else 0.0
        )
        metric["missInteractions"] = misses
        metric["candidateByLight"] = metric.pop("candidateByLight")
    return truth_total, per_light_rows


def public_metric(metric: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value
        for key, value in metric.items()
        if key not in {"cellIds", "pixelSliceIds", "pixelCounts", "candidateByLight"}
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        raise ValueError(f"refusing to write empty CSV: {path}")
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_cell_csv(
    path: Path,
    scene: SceneData,
    layout: dict[str, Any],
    slices: int,
    metric: dict[str, Any],
    counts: np.ndarray,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    edges = slice_edges(scene, slices) if slices else None
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "scene",
                "scheme",
                "cellId",
                "tileX",
                "tileY",
                "slice",
                "depthNear",
                "depthFar",
                "validPixelCount",
                "listLength",
                "active",
            ]
        )
        for cell_id in range(counts.size):
            if slices:
                slice_id = cell_id // layout["tileCount"]
                tile_id = cell_id % layout["tileCount"]
                depth_near = float(edges[slice_id])
                depth_far = float(edges[slice_id + 1])
            else:
                slice_id = -1
                tile_id = cell_id
                depth_near = scene.near_plane
                depth_far = scene.far_plane
            writer.writerow(
                [
                    scene.stem,
                    scheme_name(slices),
                    cell_id,
                    tile_id % layout["tilesX"],
                    tile_id // layout["tilesX"],
                    slice_id,
                    f"{depth_near:.9f}",
                    f"{depth_far:.9f}",
                    int(metric["pixelCounts"][cell_id]),
                    int(counts[cell_id]),
                    int(metric["pixelCounts"][cell_id] > 0),
                ]
            )


def find_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
        Path("C:/Windows/Fonts/msyhbd.ttc" if bold else "C:/Windows/Fonts/msyh.ttc"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def annotate_renderer_image(scene: SceneData, output_path: Path) -> Image.Image:
    image = Image.open(scene.app_ldr).convert("RGB")
    draw = ImageDraw.Draw(image, "RGBA")
    draw.rounded_rectangle((24, 22, 840, 130), radius=12, fill=(0, 0, 0, 190))
    title = find_font(34, bold=True)
    body = find_font(24)
    draw.text((44, 35), f"Exact analytic-screen oracle | {scene.stem}", fill="white", font=title)
    draw.text(
        (44, 82),
        f"{scene.width}x{scene.height}  lights={scene.light_count}  seed=0x{LIGHT_SEED:08X}",
        fill=(210, 225, 240),
        font=body,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path)
    return image


def heat_color(normalized: np.ndarray) -> np.ndarray:
    x = np.clip(normalized, 0.0, 1.0)
    stops = np.asarray(
        [
            [20, 24, 82],
            [35, 102, 174],
            [56, 181, 164],
            [234, 222, 70],
            [237, 89, 45],
            [125, 15, 35],
        ],
        dtype=np.float64,
    )
    scaled = x * (len(stops) - 1)
    lower = np.floor(scaled).astype(np.int32)
    upper = np.minimum(lower + 1, len(stops) - 1)
    fraction = (scaled - lower)[..., None]
    return np.asarray(stops[lower] * (1.0 - fraction) + stops[upper] * fraction, dtype=np.uint8)


def make_heatmap_panel(
    scene: SceneData,
    layout: dict[str, Any],
    tile_metric: dict[str, Any],
    cluster_metric: dict[str, Any],
    tile_counts: np.ndarray,
    cluster_counts: np.ndarray,
    renderer_image: Image.Image,
    output_path: Path,
) -> None:
    tile_ids_full = layout["tileIdsFull"]
    tile_values = tile_counts[tile_ids_full].reshape(scene.height, scene.width).astype(np.float64)
    cluster_ids_valid = cluster_metric["cellIds"]
    cluster_map = np.zeros(scene.width * scene.height, dtype=np.int32)
    cluster_map[scene.valid_flat_indices] = cluster_ids_valid
    cluster_values = cluster_counts[cluster_map].reshape(scene.height, scene.width).astype(np.float64)
    valid = scene.validity
    shared_max = int(max(np.max(tile_values[valid]), np.max(cluster_values[valid]), 1))
    tile_rgb = heat_color(tile_values / shared_max)
    cluster_rgb = heat_color(cluster_values / shared_max)
    tile_rgb[~valid] = (0, 0, 0)
    cluster_rgb[~valid] = (0, 0, 0)
    # PFM/glReadPixels rows are bottom-up; displayed images are top-down.
    tile_image = Image.fromarray(tile_rgb[::-1], mode="RGB")
    cluster_image = Image.fromarray(cluster_rgb[::-1], mode="RGB")

    panel_w, panel_h = 620, 349
    margin, header, footer = 24, 86, 62
    canvas = Image.new("RGB", (margin * 4 + panel_w * 3, header + panel_h + footer), (15, 19, 28))
    renderer = renderer_image.resize((panel_w, panel_h), Image.Resampling.LANCZOS)
    tile_image = tile_image.resize((panel_w, panel_h), Image.Resampling.NEAREST)
    cluster_image = cluster_image.resize((panel_w, panel_h), Image.Resampling.NEAREST)
    images = [renderer, tile_image, cluster_image]
    labels = ["Exact renderer oracle", "Tile 16x16 list count", "Cluster 16x16x16 list count"]
    draw = ImageDraw.Draw(canvas)
    draw.text((margin, 18), f"{scene.stem} | shared heat scale 0..{shared_max} lights", fill="white", font=find_font(32, True))
    for index, (image, label) in enumerate(zip(images, labels)):
        x = margin + index * (panel_w + margin)
        canvas.paste(image, (x, header))
        draw.rectangle((x, header, x + panel_w, header + panel_h), outline=(80, 95, 120), width=2)
        draw.text((x, header + panel_h + 14), label, fill=(220, 228, 238), font=find_font(22, True))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output_path)


def chart_canvas(title: str, subtitle: str = "") -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("RGB", (1800, 1000), (249, 250, 252))
    draw = ImageDraw.Draw(image)
    draw.text((80, 42), title, fill=(24, 35, 52), font=find_font(42, True))
    if subtitle:
        draw.text((82, 99), subtitle, fill=(82, 94, 112), font=find_font(24))
    return image, draw


def draw_grouped_bars(
    output_path: Path,
    title: str,
    subtitle: str,
    scenes: list[str],
    series: list[tuple[str, list[float], tuple[int, int, int]]],
    unit: str,
    value_format: str = ".1f",
) -> None:
    image, draw = chart_canvas(title, subtitle)
    left, top, right, bottom = 125, 180, 1730, 850
    values = [value for _, series_values, _ in series for value in series_values]
    maximum = max(values + [1.0]) * 1.16
    for line in range(6):
        value = maximum * line / 5.0
        y = bottom - int((bottom - top) * line / 5.0)
        draw.line((left, y, right, y), fill=(218, 223, 231), width=2)
        draw.text((20, y - 14), f"{value:{value_format}}", fill=(80, 91, 108), font=find_font(20))
    group_width = (right - left) / len(scenes)
    bar_width = min(115, int(group_width / (len(series) + 1)))
    for scene_index, scene in enumerate(scenes):
        center = left + group_width * (scene_index + 0.5)
        total_width = bar_width * len(series)
        for series_index, (label, series_values, color) in enumerate(series):
            value = series_values[scene_index]
            x0 = int(center - total_width / 2 + series_index * bar_width + 5)
            x1 = x0 + bar_width - 10
            y = bottom - int((bottom - top) * value / maximum)
            draw.rounded_rectangle((x0, y, x1, bottom), radius=6, fill=color)
            draw.text((x0, max(top, y - 30)), f"{value:{value_format}}", fill=(35, 45, 60), font=find_font(18, True))
        label_box = draw.textbbox((0, 0), scene, font=find_font(21, True))
        draw.text((center - (label_box[2] - label_box[0]) / 2, bottom + 22), scene, fill=(42, 52, 68), font=find_font(21, True))
    legend_x = left
    for label, _, color in series:
        draw.rounded_rectangle((legend_x, 910, legend_x + 34, 940), radius=4, fill=color)
        draw.text((legend_x + 45, 908), label, fill=(42, 52, 68), font=find_font(22, True))
        legend_x += 310
    draw.text((right - 120, 908), unit, fill=(82, 94, 112), font=find_font(22))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path)


def draw_slice_sensitivity(output_path: Path, cases: list[dict[str, Any]]) -> None:
    image, draw = chart_canvas(
        "Cluster depth-slice sensitivity",
        "Candidate interactions relative to the same 16x16 Tile baseline (lower is better)",
    )
    left, top, right, bottom = 140, 180, 1710, 830
    x_values = [8, 16, 24, 32]
    colors = [(40, 102, 180), (28, 148, 125), (230, 142, 38), (190, 62, 92)]
    all_ratios = []
    for case in cases:
        tile = case["schemes"]["tile"]["candidateInteractions"]
        all_ratios.extend(case["schemes"][f"cluster-{value:02d}"]["candidateInteractions"] / tile for value in x_values)
    maximum = max(1.05, max(all_ratios) * 1.10)
    for line in range(6):
        value = maximum * line / 5
        y = bottom - int((bottom - top) * value / maximum)
        draw.line((left, y, right, y), fill=(218, 223, 231), width=2)
        draw.text((50, y - 13), f"{value:.2f}x", fill=(80, 91, 108), font=find_font(20))
    x_positions = [left + int(index * (right - left) / 3) for index in range(4)]
    for x, value in zip(x_positions, x_values):
        draw.line((x, top, x, bottom), fill=(232, 235, 240), width=1)
        draw.text((x - 18, bottom + 22), str(value), fill=(42, 52, 68), font=find_font(22, True))
    draw.text((right - 100, bottom + 22), "slices", fill=(82, 94, 112), font=find_font(22))
    legend_y = 890
    for case_index, (case, color) in enumerate(zip(cases, colors)):
        tile = case["schemes"]["tile"]["candidateInteractions"]
        ratios = [case["schemes"][f"cluster-{value:02d}"]["candidateInteractions"] / tile for value in x_values]
        points = []
        for x, ratio in zip(x_positions, ratios):
            y = bottom - int((bottom - top) * ratio / maximum)
            points.append((x, y))
        draw.line(points, fill=color, width=5, joint="curve")
        for (x, y), ratio in zip(points, ratios):
            draw.ellipse((x - 8, y - 8, x + 8, y + 8), fill=color, outline="white", width=2)
            draw.text((x + 10, y - 25), f"{ratio:.2f}", fill=color, font=find_font(18, True))
        legend_x = 140 + case_index * 400
        draw.line((legend_x, legend_y + 14, legend_x + 50, legend_y + 14), fill=color, width=6)
        draw.text((legend_x + 62, legend_y), case["stem"], fill=(42, 52, 68), font=find_font(21, True))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    image.save(output_path)


def make_charts(run_dir: Path, cases: list[dict[str, Any]]) -> None:
    scenes = [case["stem"] for case in cases]
    tile_mean = [case["schemes"]["tile"]["listLengthPixelWeighted"]["mean"] for case in cases]
    cluster_mean = [case["schemes"]["cluster-16"]["listLengthPixelWeighted"]["mean"] for case in cases]
    tile_p95 = [case["schemes"]["tile"]["listLengthPixelWeighted"]["p95"] for case in cases]
    cluster_p95 = [case["schemes"]["cluster-16"]["listLengthPixelWeighted"]["p95"] for case in cases]
    draw_grouped_bars(
        run_dir / "charts" / "list-length-mean-p95.png",
        "Pixel-weighted point-light list length",
        "Same valid G-buffer pixels; Cluster primary = 16 logarithmic depth slices",
        scenes,
        [
            ("Tile mean", tile_mean, (52, 104, 188)),
            ("Cluster16 mean", cluster_mean, (40, 159, 132)),
            ("Tile P95", tile_p95, (118, 155, 218)),
            ("Cluster16 P95", cluster_p95, (118, 204, 183)),
        ],
        "lights / valid pixel",
    )

    tile_interactions = [case["schemes"]["tile"]["candidateInteractions"] / 1.0e6 for case in cases]
    cluster_interactions = [case["schemes"]["cluster-16"]["candidateInteractions"] / 1.0e6 for case in cases]
    draw_grouped_bars(
        run_dir / "charts" / "candidate-interactions.png",
        "Candidate pixel-light interactions",
        "Offline work proxy only; not measured GPU shader invocations or speedup",
        scenes,
        [
            ("Tile", tile_interactions, (52, 104, 188)),
            ("Cluster16", cluster_interactions, (40, 159, 132)),
        ],
        "million candidate interactions",
        ".1f",
    )

    tile_memory = [case["schemes"]["tile"]["totalLogicalBytes"] / MIB for case in cases]
    cluster_memory = [case["schemes"]["cluster-16"]["totalLogicalBytes"] / MIB for case in cases]
    draw_grouped_bars(
        run_dir / "charts" / "csr-memory.png",
        "Logical CSR metadata + light-index memory",
        "8-byte metadata per cell + 4-byte uint light index; excludes GPU alignment and LightData",
        scenes,
        [
            ("Tile", tile_memory, (52, 104, 188)),
            ("Cluster16", cluster_memory, (40, 159, 132)),
        ],
        "MiB",
        ".2f",
    )
    draw_slice_sensitivity(run_dir / "charts" / "slice-sensitivity.png", cases)


def decide(cases: list[dict[str, Any]]) -> dict[str, Any]:
    failures: list[str] = []
    for case in cases:
        for scheme, metric in case["schemes"].items():
            if metric["missInteractions"] != 0:
                failures.append(f"{case['stem']}/{scheme}: missInteractions={metric['missInteractions']}")
            if metric["overflow"]:
                failures.append(f"{case['stem']}/{scheme}: overflow")
        for name in ("tile", "cluster-16"):
            timing = case["offlineTiming"][name]
            if timing["uniqueCsrHashCount"] != 1:
                failures.append(f"{case['stem']}/{name}: non-deterministic CSR")

    by_coverage = {case["coverage"]: case for case in cases}
    representative = by_coverage["representative"]
    high = by_coverage["high-overlap"]

    def ratio(case: dict[str, Any], field_path: tuple[str, ...]) -> float:
        tile: Any = case["schemes"]["tile"]
        cluster: Any = case["schemes"]["cluster-16"]
        for field in field_path:
            tile = tile[field]
            cluster = cluster[field]
        return float(cluster / tile) if tile else 0.0

    rep_candidate = ratio(representative, ("candidateInteractions",))
    rep_p95 = ratio(representative, ("listLengthPixelWeighted", "p95"))
    high_candidate = ratio(high, ("candidateInteractions",))
    high_p95 = ratio(high, ("listLengthPixelWeighted", "p95"))
    combined_cluster = (
        representative["schemes"]["cluster-16"]["candidateInteractions"]
        + high["schemes"]["cluster-16"]["candidateInteractions"]
    )
    combined_tile = (
        representative["schemes"]["tile"]["candidateInteractions"]
        + high["schemes"]["tile"]["candidateInteractions"]
    )
    combined_ratio = float(combined_cluster / combined_tile)
    max_memory = max(case["schemes"]["cluster-16"]["totalLogicalBytes"] for case in cases)
    gates = {
        "correctness": {"passed": len(failures) == 0, "failures": failures},
        "representativeCandidateRatio": {"limit": 0.70, "actual": rep_candidate, "passed": rep_candidate <= 0.70},
        "representativePixelP95Ratio": {"limit": 0.75, "actual": rep_p95, "passed": rep_p95 <= 0.75},
        "highOverlapCandidateRatio": {"limit": 0.85, "actual": high_candidate, "passed": high_candidate <= 0.85},
        "highOverlapPixelP95Ratio": {"limit": 0.90, "actual": high_p95, "passed": high_p95 <= 0.90},
        "combinedCandidateRatio": {"limit": 0.75, "actual": combined_ratio, "passed": combined_ratio <= 0.75},
        "maxCluster16MemoryBytes": {"limit": 64 * 1024 * 1024, "actual": max_memory, "passed": max_memory <= 64 * 1024 * 1024},
    }
    passed = all(gate["passed"] for gate in gates.values())
    if not gates["correctness"]["passed"]:
        label = "No-Go: incorrect"
    elif passed:
        label = "Go to Phase B benchmark-only runtime"
    else:
        label = "No-Go for Phase B under this configuration"
    return {
        "label": label,
        "passed": passed,
        "phaseBEntered": False,
        "runtimeCandidateImplemented": False,
        "defaultChanged": False,
        "gates": gates,
        "meaning": "Phase-A admission only; not a GPU speedup or default-path decision",
    }


def summary_row(case: dict[str, Any], metric: dict[str, Any]) -> dict[str, Any]:
    return {
        "scene": case["stem"],
        "coverage": case["coverage"],
        "lightCount": case["lightCount"],
        "scheme": metric["scheme"],
        "slices": metric["slices"],
        "logicalCellCount": metric["logicalCellCount"],
        "activeCellCount": metric["activeCellCount"],
        "validPixelCount": metric["validPixelCount"],
        "validPixelRatio": metric["validPixelRatio"],
        "listAllMean": metric["listLengthAllCells"]["mean"],
        "listAllMedian": metric["listLengthAllCells"]["median"],
        "listAllP95": metric["listLengthAllCells"]["p95"],
        "listAllP99": metric["listLengthAllCells"]["p99"],
        "listAllMax": metric["listLengthAllCells"]["max"],
        "listActiveMean": metric["listLengthActiveCells"]["mean"],
        "listActiveP95": metric["listLengthActiveCells"]["p95"],
        "listPixelMean": metric["listLengthPixelWeighted"]["mean"],
        "listPixelMedian": metric["listLengthPixelWeighted"]["median"],
        "listPixelP95": metric["listLengthPixelWeighted"]["p95"],
        "listPixelP99": metric["listLengthPixelWeighted"]["p99"],
        "listPixelMax": metric["listLengthPixelWeighted"]["max"],
        "emptyRatioAll": metric["emptyListRatioAllCells"],
        "emptyRatioActive": metric["emptyListRatioActiveCells"],
        "totalIndexReferences": metric["totalIndexReferences"],
        "metadataBytes": metric["metadataBytes"],
        "indexBytes": metric["indexBytes"],
        "totalLogicalBytes": metric["totalLogicalBytes"],
        "groundTruthInteractions": metric["groundTruthInteractions"],
        "candidateInteractions": metric["candidateInteractions"],
        "falsePositiveInteractions": metric["falsePositiveInteractions"],
        "falsePositiveRate": metric["falsePositiveRate"],
        "candidateToGroundTruthRatio": metric["candidateToGroundTruthRatio"],
        "missInteractions": metric["missInteractions"],
        "csrSha256": metric["csrSha256"],
    }


def generate_report(run_dir: Path, aggregate: dict[str, Any]) -> None:
    decision = aggregate["decision"]
    cases = aggregate["cases"]
    lines = [
        "# Tile vs Cluster 精确点光源索引 Phase A 实验报告",
        "",
        f"结论：**{decision['label']}**。这里的 Go/No-Go 只判断是否值得实现 benchmark-only runtime；本轮没有 Cluster Lighting runtime、GPU Timer Query 或 GPU 加速数据，默认路径未改变。",
        "",
        "## 1. 实验回答的问题",
        "",
        "Tile 与 Cluster 都只保存原始 `light index`，不压缩方向、颜色、深度或距离。未来像素阶段仍必须执行真实球形范围和原始点光公式。因此本轮比较的是空间索引候选工作量与列表代价，不是画质近似。",
        "",
        f"冻结协议 SHA-256：`{aggregate['protocolSha256']}`。正式主配置为 1920×1080、16×16 Tile、Cluster16 对数 Z slices；8/24/32 只作敏感性，门槛在正式结果前冻结。",
        "",
        "## 2. 正式主结果",
        "",
        "| 场景 | 灯数 | Tile pixel-mean / P95 | Cluster16 pixel-mean / P95 | Candidate Tile → Cluster16 | 比例 | CSR 内存 Tile → Cluster16 | Miss |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for case in cases:
        tile = case["schemes"]["tile"]
        cluster = case["schemes"]["cluster-16"]
        ratio_value = cluster["candidateInteractions"] / tile["candidateInteractions"]
        lines.append(
            f"| {case['coverage']} | {case['lightCount']} | "
            f"{tile['listLengthPixelWeighted']['mean']:.2f} / {tile['listLengthPixelWeighted']['p95']:.0f} | "
            f"{cluster['listLengthPixelWeighted']['mean']:.2f} / {cluster['listLengthPixelWeighted']['p95']:.0f} | "
            f"{tile['candidateInteractions']:,} → {cluster['candidateInteractions']:,} | {ratio_value:.3f}× | "
            f"{tile['totalLogicalBytes']/MIB:.2f} → {cluster['totalLogicalBytes']/MIB:.2f} MiB | "
            f"{tile['missInteractions']}/{cluster['missInteractions']} |"
        )

    lines.extend(
        [
            "",
            "`pixel-mean/P95` 是每个有效 G-Buffer 像素实际查到的列表长度分布；Candidate interactions 是该长度对所有有效像素求和。它比全 logical-cell 的简单平均更接近未来 fragment loop 工作量，但仍不是 GPU shader invocation 或耗时。",
            "",
            "## 3. 冻结门槛判定",
            "",
            "| 门槛 | 实际 | 上限 | Pass |",
            "|---|---:|---:|:---:|",
        ]
    )
    gate_labels = {
        "representativeCandidateRatio": "Representative candidate ratio",
        "representativePixelP95Ratio": "Representative pixel P95 ratio",
        "highOverlapCandidateRatio": "High-overlap candidate ratio",
        "highOverlapPixelP95Ratio": "High-overlap pixel P95 ratio",
        "combinedCandidateRatio": "Representative+High candidate ratio",
        "maxCluster16MemoryBytes": "Max Cluster16 memory",
    }
    correctness = decision["gates"]["correctness"]
    lines.append(f"| Zero miss / deterministic / no overflow | {'0 failures' if correctness['passed'] else str(len(correctness['failures']))+' failures'} | 0 failures | {'Yes' if correctness['passed'] else 'No'} |")
    for key, label in gate_labels.items():
        gate = decision["gates"][key]
        if key == "maxCluster16MemoryBytes":
            actual = f"{gate['actual']/MIB:.2f} MiB"
            limit = f"{gate['limit']/MIB:.0f} MiB"
        else:
            actual = f"{gate['actual']:.3f}×"
            limit = f"{gate['limit']:.2f}×"
        lines.append(f"| {label} | {actual} | {limit} | {'Yes' if gate['passed'] else 'No'} |")

    lines.extend(
        [
            "",
            "## 4. 为什么 Cluster 能缩短名单",
            "",
            "Tile 只知道像素位于屏幕哪一格，同一格中近处和远处的灯都进入一张名单。Cluster 额外用线性 View depth 找到对数 Z slice；点光球只有和该截锥体保守相交才写入。它没有把球或光照改成立方体，Cluster 只是原始光源编号的三维通讯录。",
            "",
            "球体只在完全落到任一 Tile/Cluster 平面外时才被排除，因此允许误收、不允许漏收。所有真实球内 pixel-light interaction 都由独立 Ground Truth 复核。",
            "",
            "## 5. Slice 敏感性",
            "",
            "| 场景 | Cluster8 / Tile | Cluster16 / Tile | Cluster24 / Tile | Cluster32 / Tile |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for case in cases:
        tile_value = case["schemes"]["tile"]["candidateInteractions"]
        values = [case["schemes"][f"cluster-{slices:02d}"]["candidateInteractions"] / tile_value for slices in SENSITIVITY_SLICES]
        lines.append(f"| {case['coverage']}/{case['lightCount']} | " + " | ".join(f"{value:.3f}×" for value in values) + " |")

    invalid_counts = [case["invalidPixelCount"] for case in cases]
    lines.extend(
        [
            "",
            "## 6. 正确性、截图和可视化",
            "",
            f"四个场景全部方案的 miss 总数为 `{sum(metric['missInteractions'] for case in cases for metric in case['schemes'].values())}`；CSR 按 light index 升序、无重复、无静默截断，Tile/Cluster16 七次进程内重建各只有一个 hash。",
            "",
            "运行截图来自 Release renderer 的共同 exact `analytic-screen` Oracle，不是离线 Tile/Cluster 着色截图。Heatmap 仅把同一有效像素未来会遍历的列表长度映射回屏幕，用于解释候选工作量。",
            "",
            "- `screenshots/representative-0256-exact-analytic-screen.png`、`screenshots/high-overlap-0512-exact-analytic-screen.png`：真实 1920×1080 renderer 运行截图；",
            "- `heatmaps/*-renderer-tile-cluster.png`：运行截图、Tile list count、Cluster16 list count 并排，相同色标；",
            "- `charts/list-length-mean-p95.png`、`candidate-interactions.png`、`csr-memory.png`、`slice-sensitivity.png`：正式图表。",
            "",
            "## 7. CPU/GPU 时间的解释边界",
            "",
            "`offlineTiming` 是同一 Python 进程中的 NumPy wall time（1 warmup + 7 samples），拆分 membership/count/prefix/fill。它不包含引擎对象访问、C++ allocator、GL buffer upload、driver 或 GPU，未进入 Go 门槛，也不能写成 CPU Frame 或 GPU Lighting 提升。",
            "",
            "只有 Phase B benchmark-only runtime 才能测 Release C++ 构建/上传、GPU Timer Query、RenderDoc 和最终 HDR 一致性；Phase A 即使 Go，也不授权切换默认路径。",
            "",
            "## 8. 有效像素、天空与局限",
            "",
            f"正式捕获的 invalid/sky pixel 数依次为 `{invalid_counts}`。无效像素在 Position 读取和列表统计前排除。若这些值均为 0，本轮固定相机没有真实天空覆盖，只验证了代码的 skip 条件和全有效 Sponza 画面，不能外推为天空场景验证。",
            "",
            "结论仅适用于固定 Sponza、固定相机/seed、1920×1080、16×16 XY 与本轮 8/16/24/32 对数切片。动态相机/灯导致的每帧重建和上传尚未测量；透明 Forward、阴影、PBR 后端也不在本轮范围。",
            "",
            "## 9. 可复现与原始证据",
            "",
            "```powershell",
            "powershell -ExecutionPolicy Bypass -File .\\tools\\run_tile_cluster_phase_a.ps1 -Mode All",
            "```",
            "",
            "- `PHASE0_FROZEN_PROTOCOL_CN.md`：结果前冻结定义与门槛；",
            "- `capture-manifest.json`、`captures/`、`logs/`：EXE/source hash、场景 JSON、G-Buffer PFM、原始 PPM 与运行日志；",
            "- `aggregate.json`、`summary.csv`、`scenes/*.json`、`cells/*.csv`、`per-light/*.csv`：聚合和逐层证据；",
            "- `csr/*.npz`：所有正式方案的原始 CSR；",
            "- `verification/independent-verification.json`：独立验证器从 PFM + CSR 复算 miss/candidate/hash；",
            "- `artifact-manifest.json`：产物大小与 SHA-256。",
        ]
    )
    verification_path = run_dir / "verification" / "verification.json"
    if verification_path.is_file():
        verification = json.loads(verification_path.read_text(encoding="utf-8-sig"))
        lines.extend(
            [
                "",
                "## 10. 最终 Release 与默认 Smoke",
                "",
                f"- Release build：**{'Pass' if verification['releaseBuild']['success'] else 'Fail'}**，exit code={verification['releaseBuild']['exitCode']}；",
                f"- EXE SHA-256：`{verification['executable']['sha256']}`；",
                f"- 默认 smoke：**{'Pass' if verification['smoke']['success'] else 'Fail'}**，renderMode=`{verification['smoke']['renderMode']}`，explicit={str(verification['smoke']['renderModeExplicit']).lower()}；",
                f"- 默认改变：**{str(verification['defaultChanged']).lower()}**；GL error：{verification['smoke']['glErrorCount']}；遗留 renderer 进程：{verification['processCleanup']['remainingRendererProcesses']}。",
            ]
        )
    (run_dir / "REPORT_CN.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_artifact_manifest(run_dir: Path) -> None:
    records = []
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file() or path.name == "artifact-manifest.json":
            continue
        records.append(
            {
                "path": path.relative_to(run_dir).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    json_dump(
        run_dir / "artifact-manifest.json",
        {
            "schemaVersion": 1,
            "fileCount": len(records),
            "totalBytes": sum(record["bytes"] for record in records),
            "files": records,
        },
    )


def analyze_case(run_dir: Path, case: dict[str, Any], manifest_path: Path) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    scene = load_scene(case, manifest_path)
    layout = build_layout(scene)
    orientation = pfm_projection_orientation(scene)
    print(
        f"[scene] {scene.stem}: {scene.width}x{scene.height}, valid={scene.valid_positions.shape[0]}, lights={scene.light_count}",
        flush=True,
    )

    builds: dict[int, dict[str, Any]] = {}
    offline_timing: dict[str, Any] = {}
    for slices in (0,) + SENSITIVITY_SLICES:
        name = scheme_name(slices)
        print(f"[build] {scene.stem}/{name}", flush=True)
        if slices in (0, PRIMARY_SLICES):
            build, timing = build_with_timing(scene, layout, slices)
            offline_timing[name] = timing
        else:
            build = build_once(scene, layout, slices)
        save_csr(run_dir / "csr" / f"{scene.stem}-{name}.npz", build, scene)
        # CSR arrays are now persisted; retain only membership/counts for one truth pass.
        del build["offsets"], build["indices"]
        builds[slices] = build
        gc.collect()

    metrics: dict[int, dict[str, Any]] = {
        slices: initial_scheme_metrics(scene, layout, slices, build)
        for slices, build in builds.items()
    }
    truth, per_light_rows = analyze_ground_truth(scene, builds, metrics)
    if truth <= 0:
        raise RuntimeError(f"empty ground truth in {scene.stem}")

    for slices in (0, PRIMARY_SLICES):
        name = scheme_name(slices)
        write_cell_csv(
            run_dir / "cells" / f"{scene.stem}-{name}.csv",
            scene,
            layout,
            slices,
            metrics[slices],
            builds[slices]["counts"],
        )

    screenshot_path = run_dir / "screenshots" / f"{scene.stem}-exact-analytic-screen.png"
    renderer_image = annotate_renderer_image(scene, screenshot_path)
    if scene.coverage in ("representative", "high-overlap"):
        make_heatmap_panel(
            scene,
            layout,
            metrics[0],
            metrics[PRIMARY_SLICES],
            builds[0]["counts"],
            builds[PRIMARY_SLICES]["counts"],
            renderer_image,
            run_dir / "heatmaps" / f"{scene.stem}-renderer-tile-cluster.png",
        )

    public_schemes = {scheme_name(slices): public_metric(metric) for slices, metric in metrics.items()}
    result = {
        "stem": scene.stem,
        "coverage": scene.coverage,
        "lightCount": scene.light_count,
        "resolution": [scene.width, scene.height],
        "tileSize": TILE_SIZE,
        "tilesX": layout["tilesX"],
        "tilesY": layout["tilesY"],
        "tileCount": layout["tileCount"],
        "partialTileCount": layout["partialTileCount"],
        "nearPlane": scene.near_plane,
        "farPlane": scene.far_plane,
        "radius": scene.radius,
        "sceneSignature": scene.scene_signature,
        "submissionSignature": scene.submission_signature,
        "validPixelCount": int(scene.valid_positions.shape[0]),
        "invalidPixelCount": int(scene.width * scene.height - scene.valid_positions.shape[0]),
        "pfmProjectionConvention": orientation,
        "groundTruthInteractions": int(truth),
        "schemes": public_schemes,
        "offlineTiming": offline_timing,
        "sourceJson": str(scene.source_json),
        "rendererScreenshot": str(screenshot_path),
    }
    json_dump(run_dir / "scenes" / f"{scene.stem}.json", result)
    summary_rows = [summary_row(result, public_schemes[scheme_name(slices)]) for slices in (0,) + SENSITIVITY_SLICES]
    del builds, metrics
    gc.collect()
    return result, summary_rows, per_light_rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--report-only", action="store_true")
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    protocol_path = run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"
    manifest_path = run_dir / "capture-manifest.json"
    if not protocol_path.is_file() or not manifest_path.is_file():
        raise FileNotFoundError("frozen protocol or capture manifest is missing")
    protocol_hash = sha256_file(protocol_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest["protocolSha256"] != protocol_hash:
        raise ValueError("frozen protocol SHA-256 changed after capture")

    aggregate_path = run_dir / "aggregate.json"
    if args.report_only:
        aggregate = json.loads(aggregate_path.read_text(encoding="utf-8-sig"))
        if aggregate["protocolSha256"] != protocol_hash:
            raise ValueError("aggregate/protocol SHA-256 mismatch")
        generate_report(run_dir, aggregate)
        build_artifact_manifest(run_dir)
        print("[report-only] report and artifact manifest regenerated", flush=True)
        return 0

    expected = {
        ("small-local", 64),
        ("medium-local", 64),
        ("representative", 256),
        ("high-overlap", 512),
    }
    cases = manifest["cases"]
    actual = {(str(case["coverage"]), int(case["lightCount"])) for case in cases}
    if actual != expected or len(cases) != 4:
        raise ValueError(f"formal matrix mismatch: {actual}")

    case_results: list[dict[str, Any]] = []
    summary_rows: list[dict[str, Any]] = []
    per_light_rows: list[dict[str, Any]] = []
    for case in cases:
        result, case_summary, case_lights = analyze_case(run_dir, case, manifest_path)
        case_results.append(result)
        summary_rows.extend(case_summary)
        per_light_rows.extend(case_lights)

    decision = decide(case_results)
    aggregate = {
        "schemaVersion": 1,
        "experiment": "exact-tile-vs-cluster-light-list-phase-a",
        "protocolSha256": protocol_hash,
        "captureManifestSha256": sha256_file(manifest_path),
        "resolution": [1920, 1080],
        "tileSize": TILE_SIZE,
        "primaryClusterSlices": PRIMARY_SLICES,
        "sensitivityClusterSlices": list(SENSITIVITY_SLICES),
        "listEncoding": {"metadataBytesPerCell": 8, "indexBytes": 4, "indexOrder": "ascending"},
        "cases": case_results,
        "decision": decision,
        "runtimeCandidateImplemented": False,
        "gpuTimingMeasured": False,
        "defaultChanged": False,
    }
    json_dump(aggregate_path, aggregate)
    write_csv(run_dir / "summary.csv", summary_rows)
    write_csv(run_dir / "per-light" / "per-light-summary.csv", per_light_rows)
    make_charts(run_dir, case_results)
    generate_report(run_dir, aggregate)
    build_artifact_manifest(run_dir)
    print(f"[decision] {decision['label']}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[tile-cluster-phase-a] fatal: {error}", file=sys.stderr, flush=True)
        raise
