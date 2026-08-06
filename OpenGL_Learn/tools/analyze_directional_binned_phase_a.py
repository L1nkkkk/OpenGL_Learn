#!/usr/bin/env python3
"""Directional-binned tile/cluster lighting Phase A falsification study.

The thresholds and candidate definitions are frozen in
PHASE0_FROZEN_PROTOCOL_CN.md. This tool deliberately implements only an
offline diagnostic: it never changes the renderer's default path.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable

import numpy as np
from PIL import Image


TILE_SIZE = 16
DEPTH_SLICES = 16
NEAR_PLANE = 0.1
FAR_PLANE = 100.0
LIGHT_SEED = 0x21D3F3A5
LUMA = np.asarray([0.2126, 0.7152, 0.0722], dtype=np.float64)

# These are copied verbatim from the frozen protocol. Do not tune them from
# observed results.
ABS_MEAN_LIMIT = 0.002
ABS_P95_LIMIT = 0.010
ABS_P99_LIMIT = 0.030
ABS_MAX_LIMIT = 0.100
REL_P95_LIMIT = 0.05
REL_P99_LIMIT = 0.10
AFFECTED_LIMIT = 0.01
MISS_LEAK_LIMIT = 0.001
SUBSET_P99_LIMIT = 0.030
SUBSET_AFFECTED_LIMIT = 0.02
TEMPORAL_JUMP_P99_LIMIT = 0.020
TEMPORAL_JUMP_MAX_LIMIT = 0.100
WORK_RATIO_LIMIT = 0.60
MEMORY_LIMIT_BYTES = 32 * 1024 * 1024
SPECULAR_CRITICAL_THRESHOLD = 0.02
FAR_RADIUS_RATIO = 4.0
FAR_ATTENUATION_RATIO = 1.10

U8_2D_DIRECTIONS = np.asarray(
    [
        [1.0, 0.0],
        [math.sqrt(0.5), math.sqrt(0.5)],
        [0.0, 1.0],
        [-math.sqrt(0.5), math.sqrt(0.5)],
        [-1.0, 0.0],
        [-math.sqrt(0.5), -math.sqrt(0.5)],
        [0.0, -1.0],
        [math.sqrt(0.5), -math.sqrt(0.5)],
    ],
    dtype=np.float64,
)
U8_VIEW_DIRECTIONS = np.concatenate(
    [U8_2D_DIRECTIONS, -np.ones((8, 1), dtype=np.float64)], axis=1
)
U8_VIEW_DIRECTIONS /= np.linalg.norm(U8_VIEW_DIRECTIONS, axis=1, keepdims=True)
H8_VIEW_DIRECTIONS = np.asarray(
    [
        [x, y, z]
        for z in (-1.0, 1.0)
        for y in (-1.0, 1.0)
        for x in (-1.0, 1.0)
    ],
    dtype=np.float64,
)
H8_VIEW_DIRECTIONS /= np.linalg.norm(H8_VIEW_DIRECTIONS, axis=1, keepdims=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def json_dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        magic = stream.readline().strip()
        if magic not in (b"PF", b"Pf"):
            raise ValueError(f"{path}: unsupported PFM magic {magic!r}")
        dimensions = stream.readline().decode("ascii").strip().split()
        if len(dimensions) != 2:
            raise ValueError(f"{path}: malformed PFM dimensions")
        width, height = (int(value) for value in dimensions)
        scale = float(stream.readline().decode("ascii").strip())
        channels = 3 if magic == b"PF" else 1
        dtype = "<f4" if scale < 0.0 else ">f4"
        data = np.frombuffer(stream.read(), dtype=dtype)
    expected = width * height * channels
    if data.size != expected:
        raise ValueError(f"{path}: expected {expected} floats, found {data.size}")
    shape = (height, width, channels) if channels == 3 else (height, width)
    result = np.asarray(data.reshape(shape), dtype=np.float64)
    if not np.all(np.isfinite(result)):
        raise ValueError(f"{path}: non-finite PFM sample")
    return result


def write_pfm(path: Path, pixels: np.ndarray) -> None:
    pixels = np.asarray(pixels, dtype=np.float32)
    if pixels.ndim == 2:
        magic = "Pf"
    elif pixels.ndim == 3 and pixels.shape[2] == 3:
        magic = "PF"
    else:
        raise ValueError(f"unsupported PFM shape {pixels.shape}")
    if not np.all(np.isfinite(pixels)):
        raise ValueError(f"refusing to write non-finite PFM: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(
            f"{magic}\n{pixels.shape[1]} {pixels.shape[0]}\n-1.0\n".encode("ascii")
        )
        stream.write(np.asarray(pixels, dtype="<f4").tobytes(order="C"))


def normalized(vectors: np.ndarray, epsilon: float = 1.0e-12) -> np.ndarray:
    vectors = np.asarray(vectors, dtype=np.float64)
    lengths = np.linalg.norm(vectors, axis=-1, keepdims=True)
    return vectors / np.maximum(lengths, epsilon)


def percentile(values: np.ndarray, q: float) -> float:
    if values.size == 0:
        return 0.0
    return float(np.percentile(values, q, method="linear"))


def distribution(values: np.ndarray) -> dict[str, float | int]:
    values = np.asarray(values, dtype=np.float64).reshape(-1)
    values = values[np.isfinite(values)]
    if values.size == 0:
        return {"count": 0, "mean": 0.0, "p50": 0.0, "p95": 0.0, "p99": 0.0, "max": 0.0}
    return {
        "count": int(values.size),
        "mean": float(np.mean(values)),
        "p50": percentile(values, 50.0),
        "p95": percentile(values, 95.0),
        "p99": percentile(values, 99.0),
        "max": float(np.max(values)),
    }


def tone_map(hdr: np.ndarray) -> np.ndarray:
    mapped = np.maximum(np.asarray(hdr, dtype=np.float64), 0.0)
    mapped = mapped / (1.0 + mapped)
    mapped = np.power(mapped, 1.0 / 2.2)
    return np.asarray(np.clip(np.rint(mapped * 255.0), 0.0, 255.0), dtype=np.uint8)


def save_rgb_png(path: Path, rgb_bottom_origin: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.flipud(rgb_bottom_origin), mode="RGB").save(path)


def heatmap_rgb(values: np.ndarray) -> np.ndarray:
    # Fixed logarithmic scale: blue <=1e-5, red >=1e-1 HDR units.
    values = np.asarray(values, dtype=np.float64)
    normalized_value = np.clip((np.log10(np.maximum(values, 1.0e-5)) + 5.0) / 4.0, 0.0, 1.0)
    stops = np.asarray(
        [
            [0.0, 8.0, 48.0],
            [0.0, 96.0, 220.0],
            [0.0, 220.0, 210.0],
            [245.0, 235.0, 40.0],
            [245.0, 32.0, 20.0],
        ],
        dtype=np.float64,
    )
    position = normalized_value * (len(stops) - 1)
    low = np.floor(position).astype(np.int32)
    high = np.minimum(low + 1, len(stops) - 1)
    fraction = (position - low)[..., None]
    rgb = stops[low] * (1.0 - fraction) + stops[high] * fraction
    return np.asarray(np.clip(np.rint(rgb), 0.0, 255.0), dtype=np.uint8)


def signed_diff_rgb(values: np.ndarray, scale: float = 0.05) -> np.ndarray:
    values = np.asarray(values, dtype=np.float64)
    magnitude = np.clip(np.abs(values) / scale, 0.0, 1.0)
    rgb = np.full((*values.shape, 3), 18.0, dtype=np.float64)
    positive = values >= 0.0
    rgb[..., 0] += np.where(positive, 237.0 * magnitude, 20.0 * magnitude)
    rgb[..., 1] += 35.0 * magnitude
    rgb[..., 2] += np.where(positive, 20.0 * magnitude, 237.0 * magnitude)
    return np.asarray(np.clip(np.rint(rgb), 0.0, 255.0), dtype=np.uint8)


@dataclass
class SceneData:
    stem: str
    coverage: str
    light_count: int
    width: int
    height: int
    position: np.ndarray
    validity: np.ndarray
    normal: np.ndarray
    albedo: np.ndarray
    material_rgb: np.ndarray
    material_alpha: np.ndarray
    emissive: np.ndarray
    view_depth: np.ndarray
    camera_position: np.ndarray
    view: np.ndarray
    projection: np.ndarray
    inverse_view: np.ndarray
    light_positions: np.ndarray
    light_diffuse: np.ndarray
    radius: float
    constant: float
    linear: float
    quadratic: float
    source_json: Path
    app_ldr: Path

    @property
    def valid_flat_indices(self) -> np.ndarray:
        return np.flatnonzero(self.validity.reshape(-1))


def resolve_manifest_path(value: str, manifest_path: Path) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = manifest_path.parent / candidate
    return candidate.resolve()


def load_scene(case: dict[str, Any], manifest_path: Path) -> SceneData:
    source_json = resolve_manifest_path(case["result"], manifest_path)
    app_ldr = resolve_manifest_path(case["ldr"], manifest_path)
    captures = {
        key: resolve_manifest_path(value, manifest_path)
        for key, value in case["captures"].items()
    }
    result = json.loads(source_json.read_text(encoding="utf-8-sig"))
    if not result.get("success"):
        raise ValueError(f"capture result is success=false: {source_json}")
    point = result["pointLightStress"]
    if point["renderMode"] != "analytic-screen" or not point["renderModeExplicit"]:
        raise ValueError(f"capture is not explicit analytic-screen: {source_json}")
    if int(point["seed"]) != LIGHT_SEED:
        raise ValueError(f"unexpected light seed: {source_json}")

    position = read_pfm(captures["position"])
    validity_raw = read_pfm(captures["validity"])
    normal = read_pfm(captures["normal"])
    albedo = read_pfm(captures["albedo"])
    material_rgb = read_pfm(captures["material"])
    material_alpha = read_pfm(captures["materialAlpha"])
    emissive = read_pfm(captures["emissive"])
    height, width, channels = position.shape
    if channels != 3:
        raise ValueError(f"position capture has {channels} channels")
    expected_shapes = {
        "validity": (height, width),
        "normal": (height, width, 3),
        "albedo": (height, width, 3),
        "material": (height, width, 3),
        "materialAlpha": (height, width),
        "emissive": (height, width, 3),
    }
    actual = {
        "validity": validity_raw.shape,
        "normal": normal.shape,
        "albedo": albedo.shape,
        "material": material_rgb.shape,
        "materialAlpha": material_alpha.shape,
        "emissive": emissive.shape,
    }
    for name, shape in expected_shapes.items():
        if actual[name] != shape:
            raise ValueError(f"{source_json}: {name} shape {actual[name]} != {shape}")
    if [width, height] != [int(value) for value in result["resolution"]]:
        raise ValueError(f"capture/result resolution mismatch: {source_json}")

    validity = validity_raw > 0.0
    position = np.asarray(position, dtype=np.float64)
    normal = normalized(normal)
    albedo = np.asarray(albedo, dtype=np.float64)
    material_rgb = np.asarray(material_rgb, dtype=np.float64)
    material_alpha = np.asarray(material_alpha, dtype=np.float64)
    emissive = np.asarray(emissive, dtype=np.float64)
    view = np.asarray(result["gBuffer"]["cameraMatrices"]["view"], dtype=np.float64)
    projection = np.asarray(result["gBuffer"]["cameraMatrices"]["projection"], dtype=np.float64)
    inverse_view = np.asarray(result["gBuffer"]["cameraMatrices"]["inverseView"], dtype=np.float64)
    camera_position = np.asarray(result["camera"]["position"], dtype=np.float64)
    view_depth = np.full((height, width), np.inf, dtype=np.float64)
    valid_positions = position[validity]
    valid_h = np.concatenate(
        [valid_positions, np.ones((valid_positions.shape[0], 1), dtype=np.float64)], axis=1
    )
    view_positions = (view @ valid_h.T).T
    view_depth[validity] = -view_positions[:, 2]
    if np.any(view_depth[validity] <= 0.0):
        raise ValueError(f"valid geometry behind camera in {source_json}")

    lights = point["lights"]
    light_positions = np.asarray([item["position"] for item in lights], dtype=np.float64)
    light_diffuse = np.asarray([item["diffuse"] for item in lights], dtype=np.float64)
    if light_positions.shape != (int(case["lightCount"]), 3):
        raise ValueError(f"light count mismatch in {source_json}")
    radius = float(point["volumeRadius"])
    if not math.isfinite(radius) or radius <= 0.0:
        raise ValueError(f"invalid radius in {source_json}")
    return SceneData(
        stem=str(case["stem"]),
        coverage=str(case["coverage"]),
        light_count=int(case["lightCount"]),
        width=width,
        height=height,
        position=position,
        validity=validity,
        normal=normal,
        albedo=albedo,
        material_rgb=material_rgb,
        material_alpha=material_alpha,
        emissive=emissive,
        view_depth=view_depth,
        camera_position=camera_position,
        view=view,
        projection=projection,
        inverse_view=inverse_view,
        light_positions=light_positions,
        light_diffuse=light_diffuse,
        radius=radius,
        constant=float(point["constant"]),
        linear=float(point["linear"]),
        quadratic=float(point["quadratic"]),
        source_json=source_json,
        app_ldr=app_ldr,
    )


def attenuation(scene: SceneData, distances: np.ndarray) -> np.ndarray:
    return 1.0 / (
        scene.constant + scene.linear * distances + scene.quadratic * distances * distances
    )


def exact_components(
    scene: SceneData, light_positions: np.ndarray, light_diffuse: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    total_pixels = scene.width * scene.height
    valid_indices = scene.valid_flat_indices
    positions = scene.position.reshape(-1, 3)[valid_indices]
    normals = scene.normal.reshape(-1, 3)[valid_indices]
    albedo = scene.albedo.reshape(-1, 3)[valid_indices]
    material = scene.material_rgb.reshape(-1, 3)[valid_indices]
    shininess = scene.material_alpha.reshape(-1)[valid_indices]
    view_directions = normalized(scene.camera_position[None, :] - positions)
    diffuse_valid = np.zeros((valid_indices.size, 3), dtype=np.float64)
    specular_valid = np.zeros_like(diffuse_valid)
    hit_valid = np.zeros(valid_indices.size, dtype=np.uint16)
    edge_valid = np.zeros(valid_indices.size, dtype=bool)
    radius_squared = scene.radius * scene.radius
    edge_width = max(0.05, 0.02 * scene.radius)

    for light_position, light_color in zip(light_positions, light_diffuse):
        delta = light_position[None, :] - positions
        distance_squared = np.einsum("ij,ij->i", delta, delta)
        distances = np.sqrt(np.maximum(distance_squared, 1.0e-24))
        edge_valid |= np.abs(distances - scene.radius) <= edge_width
        inside = distance_squared <= radius_squared
        if not np.any(inside):
            continue
        hit_valid[inside] += 1
        light_direction = delta[inside] / distances[inside, None]
        diffuse_term = np.maximum(
            np.einsum("ij,ij->i", normals[inside], light_direction), 0.0
        )
        half_direction = normalized(light_direction + view_directions[inside])
        specular_base = np.maximum(
            np.einsum("ij,ij->i", normals[inside], half_direction), 0.0
        )
        specular_term = np.power(specular_base, shininess[inside])
        attenuation_value = attenuation(scene, distances[inside])
        diffuse_valid[inside] += (
            albedo[inside]
            * light_color[None, :]
            * (material[inside, 1] * diffuse_term * attenuation_value)[:, None]
        )
        specular_valid[inside] += (
            albedo[inside]
            * (0.5 * light_color[None, :])
            * (material[inside, 2] * specular_term * attenuation_value)[:, None]
        )

    diffuse_output = np.zeros((total_pixels, 3), dtype=np.float64)
    specular_output = np.zeros_like(diffuse_output)
    hit_count = np.zeros(total_pixels, dtype=np.uint16)
    edge_mask = np.zeros(total_pixels, dtype=bool)
    diffuse_output[valid_indices] = diffuse_valid
    specular_output[valid_indices] = specular_valid
    hit_count[valid_indices] = hit_valid
    edge_mask[valid_indices] = edge_valid
    shape = (scene.height, scene.width)
    return (
        diffuse_output.reshape(scene.height, scene.width, 3),
        specular_output.reshape(scene.height, scene.width, 3),
        hit_count.reshape(shape),
        edge_mask.reshape(shape),
    )


def exact_contribution_for_pixels(
    scene: SceneData,
    pixel_indices: np.ndarray,
    light_indices: np.ndarray,
    light_positions: np.ndarray,
    light_diffuse: np.ndarray,
    material_b: np.ndarray,
) -> np.ndarray:
    pixel_indices = np.asarray(pixel_indices, dtype=np.int64)
    light_indices = np.asarray(light_indices, dtype=np.int64)
    output = np.zeros((pixel_indices.size, 3), dtype=np.float64)
    if pixel_indices.size == 0 or light_indices.size == 0:
        return output
    positions = scene.position.reshape(-1, 3)[pixel_indices]
    normals = scene.normal.reshape(-1, 3)[pixel_indices]
    albedo = scene.albedo.reshape(-1, 3)[pixel_indices]
    material_g = scene.material_rgb.reshape(-1, 3)[pixel_indices, 1]
    shininess = scene.material_alpha.reshape(-1)[pixel_indices]
    specular_weight = material_b.reshape(-1)[pixel_indices]
    view_directions = normalized(scene.camera_position[None, :] - positions)
    radius_squared = scene.radius * scene.radius
    for light_index in light_indices:
        delta = light_positions[light_index][None, :] - positions
        distance_squared = np.einsum("ij,ij->i", delta, delta)
        inside = distance_squared <= radius_squared
        if not np.any(inside):
            continue
        distances = np.sqrt(np.maximum(distance_squared[inside], 1.0e-24))
        light_direction = delta[inside] / distances[:, None]
        diffuse_term = np.maximum(
            np.einsum("ij,ij->i", normals[inside], light_direction), 0.0
        )
        half_direction = normalized(light_direction + view_directions[inside])
        specular_term = np.power(
            np.maximum(
                np.einsum("ij,ij->i", normals[inside], half_direction), 0.0
            ),
            shininess[inside],
        )
        attenuation_value = attenuation(scene, distances)
        color = light_diffuse[light_index]
        output[inside] += albedo[inside] * (
            color[None, :]
            * (material_g[inside] * diffuse_term * attenuation_value)[:, None]
            + (0.5 * color[None, :])
            * (specular_weight[inside] * specular_term * attenuation_value)[:, None]
        )
    return output


def cluster_corners_world(
    scene: SceneData, x0: int, y0: int, x1: int, y1: int, depth0: float, depth1: float
) -> np.ndarray:
    ndc_x = [2.0 * x0 / scene.width - 1.0, 2.0 * x1 / scene.width - 1.0]
    ndc_y = [2.0 * y0 / scene.height - 1.0, 2.0 * y1 / scene.height - 1.0]
    points = []
    for depth in (depth0, depth1):
        for y in ndc_y:
            for x in ndc_x:
                view_point = np.asarray(
                    [
                        x * depth / scene.projection[0, 0],
                        y * depth / scene.projection[1, 1],
                        -depth,
                        1.0,
                    ],
                    dtype=np.float64,
                )
                world = scene.inverse_view @ view_point
                points.append(world[:3] / world[3])
    return np.asarray(points, dtype=np.float64)


def build_layout(scene: SceneData) -> dict[str, Any]:
    width_tiles = (scene.width + TILE_SIZE - 1) // TILE_SIZE
    height_tiles = (scene.height + TILE_SIZE - 1) // TILE_SIZE
    total_pixels = scene.width * scene.height
    tile_map = np.full(total_pixels, -1, dtype=np.int32)
    cluster_map = np.full(total_pixels, -1, dtype=np.int32)
    depth_edges = np.geomspace(NEAR_PLANE, FAR_PLANE, DEPTH_SLICES + 1)
    depth_flat = scene.view_depth.reshape(-1)
    slice_map = np.full(total_pixels, -1, dtype=np.int16)
    valid_indices = scene.valid_flat_indices
    valid_slices = np.searchsorted(depth_edges, depth_flat[valid_indices], side="right") - 1
    valid_slices = np.clip(valid_slices, 0, DEPTH_SLICES - 1)
    slice_map[valid_indices] = valid_slices.astype(np.int16)
    tiles: list[dict[str, Any]] = []
    clusters: list[dict[str, Any]] = []
    depth_discontinuity_mask = np.zeros(total_pixels, dtype=bool)

    for tile_y in range(height_tiles):
        y0 = tile_y * TILE_SIZE
        y1 = min(y0 + TILE_SIZE, scene.height)
        for tile_x in range(width_tiles):
            x0 = tile_x * TILE_SIZE
            x1 = min(x0 + TILE_SIZE, scene.width)
            yy, xx = np.mgrid[y0:y1, x0:x1]
            rectangular_indices = (yy * scene.width + xx).reshape(-1)
            pixels = rectangular_indices[scene.validity.reshape(-1)[rectangular_indices]]
            tile_index = len(tiles)
            tile_map[rectangular_indices] = tile_index
            tile_depths = depth_flat[pixels]
            tiles.append(
                {
                    "index": tile_index,
                    "tileX": tile_x,
                    "tileY": tile_y,
                    "x0": x0,
                    "y0": y0,
                    "x1": x1,
                    "y1": y1,
                    "pixels": pixels,
                    "validPixelCount": int(pixels.size),
                    "depthMin": float(np.min(tile_depths)) if pixels.size else 0.0,
                    "depthMax": float(np.max(tile_depths)) if pixels.size else 0.0,
                }
            )
            if pixels.size == 0:
                continue
            for slice_index in np.unique(slice_map[pixels]):
                cluster_pixels = pixels[slice_map[pixels] == slice_index]
                depth0 = float(depth_edges[int(slice_index)])
                depth1 = float(depth_edges[int(slice_index) + 1])
                corners = cluster_corners_world(scene, x0, y0, x1, y1, depth0, depth1)
                center = np.mean(corners, axis=0)
                bounding_radius = float(np.max(np.linalg.norm(corners - center, axis=1)))
                actual_depths = depth_flat[cluster_pixels]
                depth_min = float(np.min(actual_depths))
                depth_max = float(np.max(actual_depths))
                discontinuity = (
                    depth_max - depth_min > 1.0
                    and depth_max / max(depth_min, 1.0e-12) > 1.25
                )
                cluster_index = len(clusters)
                cluster_map[cluster_pixels] = cluster_index
                if discontinuity:
                    depth_discontinuity_mask[cluster_pixels] = True
                clusters.append(
                    {
                        "index": cluster_index,
                        "tileIndex": tile_index,
                        "tileX": tile_x,
                        "tileY": tile_y,
                        "slice": int(slice_index),
                        "pixels": cluster_pixels,
                        "corners": corners,
                        "center": center,
                        "boundingRadius": bounding_radius,
                        "validPixelCount": int(cluster_pixels.size),
                        "depthMin": depth_min,
                        "depthMax": depth_max,
                        "depthDiscontinuity": discontinuity,
                    }
                )

    if np.any(cluster_map[valid_indices] < 0):
        raise RuntimeError(f"cluster assignment incomplete for {scene.stem}")
    return {
        "widthTiles": width_tiles,
        "heightTiles": height_tiles,
        "tiles": tiles,
        "clusters": clusters,
        "tileMap": tile_map.reshape(scene.height, scene.width),
        "clusterMap": cluster_map.reshape(scene.height, scene.width),
        "sliceMap": slice_map.reshape(scene.height, scene.width),
        "depthEdges": depth_edges,
        "depthDiscontinuityMask": depth_discontinuity_mask.reshape(
            scene.height, scene.width
        ),
    }


def world_directions(scene: SceneData, view_directions: np.ndarray) -> np.ndarray:
    return normalized((scene.inverse_view[:3, :3] @ view_directions.T).T)


def projected_light_pixels(scene: SceneData, light_positions: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    homogeneous = np.concatenate(
        [light_positions, np.ones((light_positions.shape[0], 1), dtype=np.float64)], axis=1
    )
    clip = (scene.projection @ scene.view @ homogeneous.T).T
    valid_w = np.abs(clip[:, 3]) > 1.0e-12
    ndc = np.zeros((light_positions.shape[0], 2), dtype=np.float64)
    ndc[valid_w] = clip[valid_w, :2] / clip[valid_w, 3, None]
    pixels = np.empty_like(ndc)
    pixels[:, 0] = (ndc[:, 0] * 0.5 + 0.5) * scene.width
    pixels[:, 1] = (ndc[:, 1] * 0.5 + 0.5) * scene.height
    return pixels, valid_w


def u8_bin_from_delta(delta: np.ndarray) -> int:
    if float(np.dot(delta, delta)) <= 1.0e-20:
        return 0
    angle = math.atan2(float(delta[1]), float(delta[0]))
    return int(math.floor((angle + math.pi / 8.0) / (math.pi / 4.0))) % 8


def build_u8(
    scene: SceneData, layout: dict[str, Any], light_positions: np.ndarray, light_diffuse: np.ndarray
) -> dict[str, Any]:
    bins = np.zeros((len(layout["tiles"]), 8, 3), dtype=np.float64)
    assignments = np.full(
        (len(layout["tiles"]), light_positions.shape[0]), -2, dtype=np.int16
    )
    projected, projected_valid = projected_light_pixels(scene, light_positions)
    fixed_world = world_directions(scene, U8_VIEW_DIRECTIONS)
    tile_rows: list[dict[str, Any]] = []
    direction_errors: list[float] = []
    position_flat = scene.position.reshape(-1, 3)
    radius_squared = scene.radius * scene.radius

    for tile in layout["tiles"]:
        pixels = tile["pixels"]
        row = {
            "tileIndex": tile["index"],
            "tileX": tile["tileX"],
            "tileY": tile["tileY"],
            "validPixelCount": int(pixels.size),
            "exactLightCount": 0,
            "occupiedBinCount": 0,
            "directionErrorMeanDegrees": 0.0,
            "directionErrorP95Degrees": 0.0,
            "directionErrorMaxDegrees": 0.0,
        }
        if pixels.size == 0:
            tile_rows.append(row)
            continue
        positions = position_flat[pixels]
        delta_pixels = light_positions[:, None, :] - positions[None, :, :]
        member = np.any(
            np.einsum("lij,lij->li", delta_pixels, delta_pixels) <= radius_squared,
            axis=1,
        )
        member_indices = np.flatnonzero(member)
        row["exactLightCount"] = int(member_indices.size)
        representative = np.mean(positions, axis=0)
        tile_center = np.asarray(
            [(tile["x0"] + tile["x1"]) * 0.5, (tile["y0"] + tile["y1"]) * 0.5],
            dtype=np.float64,
        )
        tile_errors = []
        for light_index in member_indices:
            delta_2d = projected[light_index] - tile_center
            if not projected_valid[light_index] or float(np.dot(delta_2d, delta_2d)) <= 1.0e-20:
                representative_view = scene.view @ np.append(representative, 1.0)
                light_view = scene.view @ np.append(light_positions[light_index], 1.0)
                delta_2d = light_view[:2] - representative_view[:2]
            bin_index = u8_bin_from_delta(delta_2d)
            assignments[tile["index"], light_index] = bin_index
            distance = float(np.linalg.norm(light_positions[light_index] - representative))
            bins[tile["index"], bin_index] += (
                light_diffuse[light_index] * float(attenuation(scene, np.asarray(distance)))
            )
            actual_direction = normalized(
                (light_positions[light_index] - representative)[None, :]
            )[0]
            cosine = float(np.clip(np.dot(actual_direction, fixed_world[bin_index]), -1.0, 1.0))
            error = math.degrees(math.acos(cosine))
            tile_errors.append(error)
            direction_errors.append(error)
        occupied = np.linalg.norm(bins[tile["index"]], axis=1) > 0.0
        row["occupiedBinCount"] = int(np.count_nonzero(occupied))
        if tile_errors:
            values = np.asarray(tile_errors, dtype=np.float64)
            row["directionErrorMeanDegrees"] = float(np.mean(values))
            row["directionErrorP95Degrees"] = percentile(values, 95.0)
            row["directionErrorMaxDegrees"] = float(np.max(values))
        tile_rows.append(row)

    if not np.all(np.isfinite(bins)):
        raise FloatingPointError(f"U8 non-finite bin accumulation in {scene.stem}")
    return {
        "bins": bins,
        "assignments": assignments,
        "tileRows": tile_rows,
        "directionErrorDegrees": distribution(np.asarray(direction_errors, dtype=np.float64)),
    }


def resolve_u8(
    scene: SceneData, layout: dict[str, Any], u8: dict[str, Any], material_b: np.ndarray
) -> np.ndarray:
    output = np.zeros((scene.height * scene.width, 3), dtype=np.float64)
    normal_flat = scene.normal.reshape(-1, 3)
    position_flat = scene.position.reshape(-1, 3)
    albedo_flat = scene.albedo.reshape(-1, 3)
    material_g = scene.material_rgb.reshape(-1, 3)[:, 1]
    material_b_flat = material_b.reshape(-1)
    shininess = scene.material_alpha.reshape(-1)
    fixed_world = world_directions(scene, U8_VIEW_DIRECTIONS)
    for tile in layout["tiles"]:
        pixels = tile["pixels"]
        if pixels.size == 0:
            continue
        normals = normal_flat[pixels]
        view_directions = normalized(scene.camera_position[None, :] - position_flat[pixels])
        for bin_index in range(8):
            color = u8["bins"][tile["index"], bin_index]
            if not np.any(color):
                continue
            direction = fixed_world[bin_index]
            diffuse_term = np.maximum(normals @ direction, 0.0)
            half_direction = normalized(direction[None, :] + view_directions)
            specular_term = np.power(
                np.maximum(np.einsum("ij,ij->i", normals, half_direction), 0.0),
                shininess[pixels],
            )
            scalar = (
                material_g[pixels] * diffuse_term
                + 0.5 * material_b_flat[pixels] * specular_term
            )
            output[pixels] += albedo_flat[pixels] * color[None, :] * scalar[:, None]
    if not np.all(np.isfinite(output)):
        raise FloatingPointError(f"U8 produced non-finite HDR in {scene.stem}")
    return output.reshape(scene.height, scene.width, 3)


def evaluate_h8(
    scene: SceneData,
    layout: dict[str, Any],
    light_positions: np.ndarray,
    light_diffuse: np.ndarray,
    material_b: np.ndarray,
    exact_hdr: np.ndarray,
) -> dict[str, Any]:
    candidate = np.array(exact_hdr.reshape(-1, 3), copy=True)
    assignments = np.full(
        (len(layout["clusters"]), light_positions.shape[0]), -2, dtype=np.int16
    )
    position_flat = scene.position.reshape(-1, 3)
    normal_flat = scene.normal.reshape(-1, 3)
    albedo_flat = scene.albedo.reshape(-1, 3)
    material_g = scene.material_rgb.reshape(-1, 3)[:, 1]
    material_b_flat = material_b.reshape(-1)
    radius_squared = scene.radius * scene.radius
    h8_world = world_directions(scene, H8_VIEW_DIRECTIONS)
    rows: list[dict[str, Any]] = []
    nearest_errors: list[float] = []
    reconstructed_errors: list[float] = []
    totals = {
        "member": 0,
        "partial": 0,
        "full": 0,
        "far": 0,
        "near": 0,
        "eligible": 0,
        "exact": 0,
        "specularFallback": 0,
        "occupiedBins": 0,
        "exactPoolEntries": 0,
    }
    work_equivalent = 8 * int(np.count_nonzero(scene.validity))

    for cluster in layout["clusters"]:
        pixels = cluster["pixels"]
        positions = position_flat[pixels]
        pixel_delta = light_positions[:, None, :] - positions[None, :, :]
        member = np.any(
            np.einsum("lij,lij->li", pixel_delta, pixel_delta) <= radius_squared,
            axis=1,
        )
        member_indices = np.flatnonzero(member)
        corners = cluster["corners"]
        corner_delta = light_positions[:, None, :] - corners[None, :, :]
        corner_distance_squared = np.einsum("lij,lij->li", corner_delta, corner_delta)
        full = member & np.all(corner_distance_squared <= radius_squared, axis=1)
        center_delta = light_positions - cluster["center"][None, :]
        center_distance = np.linalg.norm(center_delta, axis=1)
        corner_distances = np.sqrt(np.maximum(corner_distance_squared, 1.0e-24))
        corner_attenuation = attenuation(scene, corner_distances)
        attenuation_ratio = np.max(corner_attenuation, axis=1) / np.maximum(
            np.min(corner_attenuation, axis=1), 1.0e-24
        )
        far = (
            full
            & (center_distance >= FAR_RADIUS_RATIO * cluster["boundingRadius"])
            & (attenuation_ratio <= FAR_ATTENUATION_RATIO)
        )
        specular_critical = bool(
            np.max(material_b_flat[pixels], initial=0.0) > SPECULAR_CRITICAL_THRESHOLD
        )
        eligible = far & (not specular_critical)
        exact = member & ~eligible
        eligible_indices = np.flatnonzero(eligible)
        exact_count = int(np.count_nonzero(exact))
        bins = np.zeros((8, 3), dtype=np.float64)
        cluster_nearest_errors: list[float] = []
        cluster_reconstructed_errors: list[float] = []

        if eligible_indices.size:
            candidate[pixels] -= exact_contribution_for_pixels(
                scene,
                pixels,
                eligible_indices,
                light_positions,
                light_diffuse,
                material_b,
            )
            actual_world = normalized(center_delta[eligible_indices])
            actual_view = normalized((scene.view[:3, :3] @ actual_world.T).T)
            dots = np.clip(actual_view @ H8_VIEW_DIRECTIONS.T, -1.0, 1.0)
            order = np.argsort(-dots, axis=1)[:, :2]
            angles = np.arccos(
                np.take_along_axis(dots, order, axis=1)
            )
            denominator = np.maximum(angles[:, 0] + angles[:, 1], 1.0e-12)
            weights = np.column_stack(
                [angles[:, 1] / denominator, angles[:, 0] / denominator]
            )
            zero_primary = angles[:, 0] <= 1.0e-12
            weights[zero_primary] = np.asarray([1.0, 0.0])
            center_attenuation = attenuation(scene, center_distance[eligible_indices])
            for local_index, light_index in enumerate(eligible_indices):
                primary = int(order[local_index, 0])
                secondary = int(order[local_index, 1])
                assignments[cluster["index"], light_index] = primary
                radiance = light_diffuse[light_index] * center_attenuation[local_index]
                bins[primary] += radiance * weights[local_index, 0]
                bins[secondary] += radiance * weights[local_index, 1]
                nearest_error = math.degrees(float(angles[local_index, 0]))
                reconstructed = normalized(
                    (
                        H8_VIEW_DIRECTIONS[primary] * weights[local_index, 0]
                        + H8_VIEW_DIRECTIONS[secondary] * weights[local_index, 1]
                    )[None, :]
                )[0]
                reconstructed_error = math.degrees(
                    math.acos(
                        float(
                            np.clip(
                                np.dot(reconstructed, actual_view[local_index]), -1.0, 1.0
                            )
                        )
                    )
                )
                nearest_errors.append(nearest_error)
                reconstructed_errors.append(reconstructed_error)
                cluster_nearest_errors.append(nearest_error)
                cluster_reconstructed_errors.append(reconstructed_error)

            normals = normal_flat[pixels]
            for bin_index in range(8):
                color = bins[bin_index]
                if not np.any(color):
                    continue
                diffuse_term = np.maximum(normals @ h8_world[bin_index], 0.0)
                candidate[pixels] += (
                    albedo_flat[pixels]
                    * color[None, :]
                    * (material_g[pixels] * diffuse_term)[:, None]
                )

        occupied_bins = int(np.count_nonzero(np.linalg.norm(bins, axis=1) > 0.0))
        member_count = int(member_indices.size)
        full_count = int(np.count_nonzero(full))
        far_count = int(np.count_nonzero(far))
        partial_count = member_count - full_count
        near_count = full_count - far_count
        eligible_count = int(eligible_indices.size)
        specular_fallback_count = member_count if specular_critical else 0
        work_equivalent += int(pixels.size) * exact_count
        totals["member"] += member_count
        totals["partial"] += partial_count
        totals["full"] += full_count
        totals["far"] += far_count
        totals["near"] += near_count
        totals["eligible"] += eligible_count
        totals["exact"] += exact_count
        totals["specularFallback"] += specular_fallback_count
        totals["occupiedBins"] += occupied_bins
        totals["exactPoolEntries"] += exact_count
        rows.append(
            {
                "clusterIndex": cluster["index"],
                "tileIndex": cluster["tileIndex"],
                "tileX": cluster["tileX"],
                "tileY": cluster["tileY"],
                "slice": cluster["slice"],
                "validPixelCount": int(pixels.size),
                "depthMin": cluster["depthMin"],
                "depthMax": cluster["depthMax"],
                "depthDiscontinuity": bool(cluster["depthDiscontinuity"]),
                "boundingRadius": cluster["boundingRadius"],
                "memberLightCount": member_count,
                "partialLightCount": partial_count,
                "fullLightCount": full_count,
                "farLightCount": far_count,
                "nearLightCount": near_count,
                "eligibleLightCount": eligible_count,
                "exactLightCount": exact_count,
                "specularCritical": specular_critical,
                "specularFallbackLightCount": specular_fallback_count,
                "occupiedBinCount": occupied_bins,
                "nearestDirectionErrorMeanDegrees": (
                    float(np.mean(cluster_nearest_errors)) if cluster_nearest_errors else 0.0
                ),
                "reconstructedDirectionErrorMeanDegrees": (
                    float(np.mean(cluster_reconstructed_errors))
                    if cluster_reconstructed_errors
                    else 0.0
                ),
            }
        )

    if not np.all(np.isfinite(candidate)):
        raise FloatingPointError(f"H8 produced non-finite HDR in {scene.stem}")
    physical_bytes = len(layout["clusters"]) * 144 + totals["exactPoolEntries"] * 4
    return {
        "hdr": candidate.reshape(scene.height, scene.width, 3),
        "assignments": assignments,
        "clusterRows": rows,
        "classification": totals,
        "directionErrorNearestDegrees": distribution(
            np.asarray(nearest_errors, dtype=np.float64)
        ),
        "directionErrorReconstructedDegrees": distribution(
            np.asarray(reconstructed_errors, dtype=np.float64)
        ),
        "workEquivalent": int(work_equivalent),
        "physicalBytesAtDiagnosticResolution": int(physical_bytes),
    }


def subset_error_metrics(
    exact_hdr: np.ndarray,
    candidate_hdr: np.ndarray,
    mask: np.ndarray,
) -> dict[str, Any]:
    mask = np.asarray(mask, dtype=bool)
    count = int(np.count_nonzero(mask))
    if count == 0:
        return {
            "pixelCount": 0,
            "absoluteMaxChannel": distribution(np.asarray([], dtype=np.float64)),
            "relativeLuminance": distribution(np.asarray([], dtype=np.float64)),
            "affectedPixelCount": 0,
            "affectedPixelRatio": 0.0,
            "missPixelCount": 0,
            "missPixelRatio": 0.0,
            "leakPixelCount": 0,
            "leakPixelRatio": 0.0,
        }
    absolute = np.max(np.abs(candidate_hdr - exact_hdr), axis=2)
    exact_luminance = np.einsum("ijk,k->ij", exact_hdr, LUMA)
    candidate_luminance = np.einsum("ijk,k->ij", candidate_hdr, LUMA)
    luminance_difference = np.abs(candidate_luminance - exact_luminance)
    relative_mask = mask & (np.abs(exact_luminance) > 1.0e-3)
    relative = luminance_difference[relative_mask] / np.maximum(
        np.abs(exact_luminance[relative_mask]), 1.0e-3
    )
    relative_full = np.zeros_like(exact_luminance)
    relative_full[relative_mask] = luminance_difference[relative_mask] / np.maximum(
        np.abs(exact_luminance[relative_mask]), 1.0e-3
    )
    affected = mask & (
        (absolute > 0.01) | (relative_mask & (relative_full > 0.05))
    )
    miss = mask & (exact_luminance > 0.01) & (candidate_luminance <= 1.0e-3)
    leak = mask & (candidate_luminance > 0.01) & (exact_luminance <= 1.0e-3)
    return {
        "pixelCount": count,
        "absoluteMaxChannel": distribution(absolute[mask]),
        "relativeLuminance": distribution(relative),
        "affectedPixelCount": int(np.count_nonzero(affected)),
        "affectedPixelRatio": float(np.count_nonzero(affected) / count),
        "missPixelCount": int(np.count_nonzero(miss)),
        "missPixelRatio": float(np.count_nonzero(miss) / count),
        "leakPixelCount": int(np.count_nonzero(leak)),
        "leakPixelRatio": float(np.count_nonzero(leak) / count),
    }


def error_metrics(
    exact_hdr: np.ndarray,
    candidate_hdr: np.ndarray,
    valid_mask: np.ndarray,
    depth_discontinuity_mask: np.ndarray,
    sphere_edge_mask: np.ndarray,
) -> dict[str, Any]:
    return {
        "allValid": subset_error_metrics(exact_hdr, candidate_hdr, valid_mask),
        "depthDiscontinuity": subset_error_metrics(
            exact_hdr, candidate_hdr, valid_mask & depth_discontinuity_mask
        ),
        "sphereEdge": subset_error_metrics(
            exact_hdr, candidate_hdr, valid_mask & sphere_edge_mask
        ),
    }


def quality_failures(metrics: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    overall = metrics["allValid"]
    absolute = overall["absoluteMaxChannel"]
    relative = overall["relativeLuminance"]
    checks = [
        (absolute["mean"] <= ABS_MEAN_LIMIT, "absolute mean"),
        (absolute["p95"] <= ABS_P95_LIMIT, "absolute P95"),
        (absolute["p99"] <= ABS_P99_LIMIT, "absolute P99"),
        (absolute["max"] <= ABS_MAX_LIMIT, "absolute max"),
        (relative["p95"] <= REL_P95_LIMIT, "relative luminance P95"),
        (relative["p99"] <= REL_P99_LIMIT, "relative luminance P99"),
        (overall["affectedPixelRatio"] <= AFFECTED_LIMIT, "affected ratio"),
        (overall["missPixelRatio"] <= MISS_LEAK_LIMIT, "miss ratio"),
        (overall["leakPixelRatio"] <= MISS_LEAK_LIMIT, "leak ratio"),
    ]
    for passed, label in checks:
        if not passed:
            failures.append(label)
    for subset_name in ("depthDiscontinuity", "sphereEdge"):
        subset = metrics[subset_name]
        if subset["pixelCount"] == 0:
            failures.append(f"{subset_name} empty")
            continue
        if subset["absoluteMaxChannel"]["p99"] > SUBSET_P99_LIMIT:
            failures.append(f"{subset_name} absolute P99")
        if subset["affectedPixelRatio"] > SUBSET_AFFECTED_LIMIT:
            failures.append(f"{subset_name} affected ratio")
    return failures


def save_error_artifacts(
    run_dir: Path,
    prefix: str,
    exact_hdr: np.ndarray,
    candidate_hdr: np.ndarray,
) -> dict[str, str]:
    image_dir = run_dir / "images"
    hdr_dir = run_dir / "hdr"
    exact_path = hdr_dir / f"{prefix}-exact.pfm"
    candidate_path = hdr_dir / f"{prefix}-candidate.pfm"
    diff_path = hdr_dir / f"{prefix}-abs-max-channel-diff.pfm"
    exact_ldr_path = image_dir / f"{prefix}-exact-ldr.png"
    candidate_ldr_path = image_dir / f"{prefix}-candidate-ldr.png"
    heatmap_path = image_dir / f"{prefix}-hdr-diff-heatmap.png"
    signed_path = image_dir / f"{prefix}-signed-luminance-diff.png"
    absolute = np.max(np.abs(candidate_hdr - exact_hdr), axis=2)
    signed_luminance = np.einsum("ijk,k->ij", candidate_hdr - exact_hdr, LUMA)
    write_pfm(exact_path, exact_hdr)
    write_pfm(candidate_path, candidate_hdr)
    write_pfm(diff_path, absolute)
    save_rgb_png(exact_ldr_path, tone_map(exact_hdr))
    save_rgb_png(candidate_ldr_path, tone_map(candidate_hdr))
    save_rgb_png(heatmap_path, heatmap_rgb(absolute))
    save_rgb_png(signed_path, signed_diff_rgb(signed_luminance))
    return {
        "exactHdr": str(exact_path),
        "candidateHdr": str(candidate_path),
        "absoluteDiffHdr": str(diff_path),
        "exactLdr": str(exact_ldr_path),
        "candidateLdr": str(candidate_ldr_path),
        "heatmap": str(heatmap_path),
        "signedLuminanceDiff": str(signed_path),
    }


def write_rows(path: Path, rows: Iterable[dict[str, Any]]) -> None:
    rows = list(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def validate_oracle_against_app_ldr(
    scene: SceneData, exact_hdr: np.ndarray
) -> dict[str, Any]:
    app_top_origin = np.asarray(Image.open(scene.app_ldr).convert("RGB"), dtype=np.uint8)
    if app_top_origin.shape != (scene.height, scene.width, 3):
        raise ValueError(f"unexpected app LDR shape for {scene.stem}: {app_top_origin.shape}")
    app_bottom_origin = np.flipud(app_top_origin)
    # Current fixed stress configuration has HDR tone mapping disabled and
    # gamma correction enabled at 2.2. The framebuffer conversion clamps to
    # [0,1]. This is an independent bulk-agreement sanity check, not a
    # candidate quality gate. The captured JSON serializes light parameters to
    # six decimal places, so an isolated analytic-sphere boundary pixel can
    # legitimately move by several LSB while the rest of the replay remains
    # within one LSB.
    predicted = np.asarray(
        np.clip(
            np.rint(np.power(np.clip(exact_hdr, 0.0, 1.0), 1.0 / 2.2) * 255.0),
            0.0,
            255.0,
        ),
        dtype=np.uint8,
    )
    channel_error = np.abs(
        app_bottom_origin.astype(np.int16) - predicted.astype(np.int16)
    ).astype(np.float64)
    pixel_error = np.max(channel_error, axis=2)
    stats = distribution(channel_error)
    above_one = channel_error > 1.0
    above_one_count = int(np.count_nonzero(above_one))
    above_one_ratio = float(above_one_count / channel_error.size)
    passed_bulk = bool(
        stats["p99"] <= 1.0
        and above_one_ratio <= 1.0e-4
        and stats["max"] <= 8.0
    )
    return {
        "postprocess": "USE_HDR=false, gamma=2.2",
        "channelAbsoluteError": stats,
        "pixelMaxChannelError": distribution(pixel_error),
        "differentPixelCount": int(np.count_nonzero(pixel_error)),
        "differentPixelRatio": float(np.count_nonzero(pixel_error) / pixel_error.size),
        "channelsAboveOneLsbCount": above_one_count,
        "channelsAboveOneLsbRatio": above_one_ratio,
        "bulkAgreementCriteria": {
            "channelP99LsbMaximum": 1.0,
            "channelsAboveOneLsbRatioMaximum": 1.0e-4,
            "channelMaximumLsbMaximum": 8.0,
        },
        "passedBulkAgreementGate": passed_bulk,
    }


def run_invalid_pixel_semantic_smoke(
    scene: SceneData, run_dir: Path
) -> dict[str, Any]:
    yy, xx = np.mgrid[0 : scene.height, 0 : scene.width]
    forced_invalid = (
        ((xx + 3 * yy) % 17 == 0)
        | (xx < 4)
        | (yy < 4)
        | (xx >= scene.width - 4)
        | (yy >= scene.height - 4)
    )
    validity = scene.validity & ~forced_invalid
    newly_invalid = scene.validity & forced_invalid
    if not np.any(newly_invalid) or not np.any(validity):
        raise RuntimeError("invalid-pixel smoke mask is degenerate")

    def poison(array: np.ndarray) -> np.ndarray:
        result = np.array(array, copy=True)
        result[newly_invalid] = np.nan
        return result

    poisoned_scene = replace(
        scene,
        stem=f"{scene.stem}-invalid-sentinel",
        validity=validity,
        position=poison(scene.position),
        normal=poison(scene.normal),
        albedo=poison(scene.albedo),
        material_rgb=poison(scene.material_rgb),
        material_alpha=poison(scene.material_alpha),
        emissive=poison(scene.emissive),
    )
    layout = build_layout(poisoned_scene)
    light_positions = poisoned_scene.light_positions[:16]
    light_diffuse = poisoned_scene.light_diffuse[:16]
    exact_diffuse, exact_specular, _, _ = exact_components(
        poisoned_scene, light_positions, light_diffuse
    )
    exact_hdr = exact_diffuse + exact_specular
    u8 = build_u8(poisoned_scene, layout, light_positions, light_diffuse)
    material_b = poisoned_scene.material_rgb[..., 2]
    u8_hdr = resolve_u8(poisoned_scene, layout, u8, material_b)
    h8 = evaluate_h8(
        poisoned_scene,
        layout,
        light_positions,
        light_diffuse,
        material_b,
        exact_hdr,
    )
    values = {
        "exact": exact_hdr[newly_invalid],
        "u8": u8_hdr[newly_invalid],
        "h8": h8["hdr"][newly_invalid],
    }
    max_abs = {
        name: float(np.max(np.abs(value), initial=0.0)) for name, value in values.items()
    }
    finite = {
        "exact": bool(np.all(np.isfinite(exact_hdr))),
        "u8": bool(np.all(np.isfinite(u8_hdr))),
        "h8": bool(np.all(np.isfinite(h8["hdr"]))),
    }
    passed = all(value == 0.0 for value in max_abs.values()) and all(finite.values())
    result = {
        "sourceScene": scene.stem,
        "lightCount": 16,
        "forcedInvalidPixelCount": int(np.count_nonzero(newly_invalid)),
        "remainingValidPixelCount": int(np.count_nonzero(validity)),
        "poison": "NaN in position/normal/albedo/material/emissive for invalid pixels",
        "maximumAbsoluteOutputOnInvalidPixels": max_abs,
        "fullOutputFinite": finite,
        "passed": passed,
        "gatedCandidateDecision": False,
    }
    json_dump(run_dir / "invalid-pixel-semantic-smoke.json", result)
    if not passed:
        raise RuntimeError("invalid/sky pixel semantic smoke failed")
    return result


def analyze_static_case(
    run_dir: Path, scene: SceneData, layout: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    print(f"[static] {scene.stem}: exact oracle", flush=True)
    exact_diffuse, exact_specular, hit_count, sphere_edge = exact_components(
        scene, scene.light_positions, scene.light_diffuse
    )
    exact_interactions = int(np.sum(hit_count, dtype=np.uint64))
    valid_pixels = int(np.count_nonzero(scene.validity))
    if exact_interactions == 0:
        raise RuntimeError(f"{scene.stem}: exact oracle has zero light interactions")
    u8 = build_u8(scene, layout, scene.light_positions, scene.light_diffuse)
    u8_work = 8 * valid_pixels
    u8_bytes_1080 = ((1920 + 15) // 16) * ((1080 + 15) // 16) * 132
    tile_csv = run_dir / "csv" / f"{scene.stem}-u8-tile-stats.csv"
    write_rows(tile_csv, u8["tileRows"])
    case_result: dict[str, Any] = {
        "stem": scene.stem,
        "coverage": scene.coverage,
        "lightCount": scene.light_count,
        "resolution": [scene.width, scene.height],
        "validPixelCount": valid_pixels,
        "invalidPixelCount": int(scene.width * scene.height - valid_pixels),
        "exactInteractionCount": exact_interactions,
        "sphereEdgePixelCount": int(np.count_nonzero(scene.validity & sphere_edge)),
        "depthDiscontinuityPixelCount": int(
            np.count_nonzero(scene.validity & layout["depthDiscontinuityMask"])
        ),
        "tileCount": len(layout["tiles"]),
        "nonEmptyTileCount": int(
            sum(tile["validPixelCount"] > 0 for tile in layout["tiles"])
        ),
        "nonEmptyClusterCount": len(layout["clusters"]),
        "appAnalyticScreenLdr": str(scene.app_ldr),
        "sourceJson": str(scene.source_json),
        "u8TileCsv": str(tile_csv),
        "materials": {},
    }
    summary_rows: list[dict[str, Any]] = []
    material_variants = {
        "diffuse-only": np.zeros_like(scene.material_rgb[..., 2]),
        "captured-specular": np.asarray(scene.material_rgb[..., 2], dtype=np.float64),
    }
    exact_by_material = {
        "diffuse-only": exact_diffuse,
        "captured-specular": exact_diffuse + exact_specular,
    }
    case_result["oracleAppLdrValidation"] = validate_oracle_against_app_ldr(
        scene, exact_by_material["captured-specular"]
    )
    if not case_result["oracleAppLdrValidation"]["passedBulkAgreementGate"]:
        raise RuntimeError(f"{scene.stem}: CPU oracle does not match app Analytic Screen LDR")

    for material_name, material_b in material_variants.items():
        exact_hdr = exact_by_material[material_name]
        print(f"[static] {scene.stem}/{material_name}: U8", flush=True)
        u8_hdr = resolve_u8(scene, layout, u8, material_b)
        u8_metrics = error_metrics(
            exact_hdr,
            u8_hdr,
            scene.validity,
            layout["depthDiscontinuityMask"],
            sphere_edge,
        )
        u8_failures = quality_failures(u8_metrics)
        u8_prefix = f"static-{scene.stem}-{material_name}-u8"
        u8_artifacts = save_error_artifacts(
            run_dir, u8_prefix, exact_hdr, u8_hdr
        )
        print(f"[static] {scene.stem}/{material_name}: H8", flush=True)
        h8 = evaluate_h8(
            scene,
            layout,
            scene.light_positions,
            scene.light_diffuse,
            material_b,
            exact_hdr,
        )
        h8_metrics = error_metrics(
            exact_hdr,
            h8["hdr"],
            scene.validity,
            layout["depthDiscontinuityMask"],
            sphere_edge,
        )
        h8_failures = quality_failures(h8_metrics)
        h8_prefix = f"static-{scene.stem}-{material_name}-h8"
        h8_artifacts = save_error_artifacts(
            run_dir, h8_prefix, exact_hdr, h8["hdr"]
        )
        h8_csv = run_dir / "csv" / f"{scene.stem}-{material_name}-h8-cluster-stats.csv"
        write_rows(h8_csv, h8["clusterRows"])
        tile_scale_to_1080 = (
            ((1920 + 15) // 16) * ((1080 + 15) // 16)
            / max(1, layout["widthTiles"] * layout["heightTiles"])
        )
        h8_bytes_1080 = int(
            math.ceil(h8["physicalBytesAtDiagnosticResolution"] * tile_scale_to_1080)
        )
        material_result = {
            "oracle": {
                "hdrLuminance": distribution(
                    np.einsum("ijk,k->ij", exact_hdr, LUMA)[scene.validity]
                ),
                "maxChannel": distribution(
                    np.max(exact_hdr, axis=2)[scene.validity]
                ),
            },
            "u8": {
                "metrics": u8_metrics,
                "qualityPassed": not u8_failures,
                "qualityFailures": u8_failures,
                "workEquivalent": u8_work,
                "workRatioVsExactInteractions": float(u8_work / exact_interactions),
                "physicalBytesAt1080p": int(u8_bytes_1080),
                "occupiedBinCount": distribution(
                    np.asarray(
                        [row["occupiedBinCount"] for row in u8["tileRows"]],
                        dtype=np.float64,
                    )
                ),
                "exactLightCountPerTile": distribution(
                    np.asarray(
                        [row["exactLightCount"] for row in u8["tileRows"]],
                        dtype=np.float64,
                    )
                ),
                "directionErrorDegrees": u8["directionErrorDegrees"],
                "artifacts": u8_artifacts,
            },
            "h8": {
                "metrics": h8_metrics,
                "qualityPassed": not h8_failures,
                "qualityFailures": h8_failures,
                "workEquivalent": h8["workEquivalent"],
                "workRatioVsExactInteractions": float(
                    h8["workEquivalent"] / exact_interactions
                ),
                "physicalBytesAtDiagnosticResolution": h8[
                    "physicalBytesAtDiagnosticResolution"
                ],
                "estimatedPhysicalBytesAt1080p": h8_bytes_1080,
                "classification": h8["classification"],
                "memberLightCountPerCluster": distribution(
                    np.asarray(
                        [row["memberLightCount"] for row in h8["clusterRows"]],
                        dtype=np.float64,
                    )
                ),
                "exactLightCountPerCluster": distribution(
                    np.asarray(
                        [row["exactLightCount"] for row in h8["clusterRows"]],
                        dtype=np.float64,
                    )
                ),
                "occupiedBinCountPerCluster": distribution(
                    np.asarray(
                        [row["occupiedBinCount"] for row in h8["clusterRows"]],
                        dtype=np.float64,
                    )
                ),
                "directionErrorNearestDegrees": h8[
                    "directionErrorNearestDegrees"
                ],
                "directionErrorReconstructedDegrees": h8[
                    "directionErrorReconstructedDegrees"
                ],
                "clusterCsv": str(h8_csv),
                "artifacts": h8_artifacts,
            },
        }
        case_result["materials"][material_name] = material_result
        for scheme in ("u8", "h8"):
            scheme_result = material_result[scheme]
            overall = scheme_result["metrics"]["allValid"]
            summary_rows.append(
                {
                    "coverage": scene.coverage,
                    "lightCount": scene.light_count,
                    "material": material_name,
                    "scheme": scheme.upper(),
                    "qualityPassed": scheme_result["qualityPassed"],
                    "absoluteMean": overall["absoluteMaxChannel"]["mean"],
                    "absoluteP95": overall["absoluteMaxChannel"]["p95"],
                    "absoluteP99": overall["absoluteMaxChannel"]["p99"],
                    "absoluteMax": overall["absoluteMaxChannel"]["max"],
                    "relativeLuminanceP95": overall["relativeLuminance"]["p95"],
                    "relativeLuminanceP99": overall["relativeLuminance"]["p99"],
                    "affectedPixelRatio": overall["affectedPixelRatio"],
                    "missPixelRatio": overall["missPixelRatio"],
                    "leakPixelRatio": overall["leakPixelRatio"],
                    "depthP99": scheme_result["metrics"]["depthDiscontinuity"][
                        "absoluteMaxChannel"
                    ]["p99"],
                    "depthAffectedRatio": scheme_result["metrics"][
                        "depthDiscontinuity"
                    ]["affectedPixelRatio"],
                    "edgeP99": scheme_result["metrics"]["sphereEdge"][
                        "absoluteMaxChannel"
                    ]["p99"],
                    "edgeAffectedRatio": scheme_result["metrics"]["sphereEdge"][
                        "affectedPixelRatio"
                    ],
                    "workRatioVsExact": scheme_result[
                        "workRatioVsExactInteractions"
                    ],
                }
            )
    return case_result, summary_rows


def assignment_changes(previous: np.ndarray | None, current: np.ndarray) -> tuple[int, int]:
    if previous is None:
        return 0, 0
    previous_active = previous >= 0
    current_active = current >= 0
    common = previous_active & current_active
    switches = int(np.count_nonzero(common & (previous != current)))
    membership_transitions = int(np.count_nonzero(previous_active != current_active))
    return switches, membership_transitions


def analyze_temporal(
    run_dir: Path, scene: SceneData, layout: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    light_count = min(64, scene.light_positions.shape[0])
    base_positions = np.array(scene.light_positions[:light_count], copy=True)
    light_diffuse = scene.light_diffuse[:light_count]
    phases = 2.0 * math.pi * np.arange(light_count, dtype=np.float64) / light_count
    material_variants = {
        "diffuse-only": np.zeros_like(scene.material_rgb[..., 2]),
        "captured-specular": np.asarray(scene.material_rgb[..., 2], dtype=np.float64),
    }
    temporal: dict[str, Any] = {
        "sourceScene": scene.stem,
        "lightCount": light_count,
        "frameCount": 33,
        "motionEquation": (
            "base + (0.30*sin(2*pi*t/32+phi), "
            "0.18*sin(4*pi*t/32+phi), 0.45*cos(2*pi*t/32+phi))"
        ),
        "materials": {
            name: {
                "u8": {"frames": [], "binSwitchCount": 0, "membershipTransitionCount": 0},
                "h8": {"frames": [], "binSwitchCount": 0, "membershipTransitionCount": 0},
            }
            for name in material_variants
        },
    }
    rows: list[dict[str, Any]] = []
    previous_exact: dict[str, np.ndarray | None] = {name: None for name in material_variants}
    previous_candidate: dict[str, dict[str, np.ndarray | None]] = {
        name: {"u8": None, "h8": None} for name in material_variants
    }
    previous_u8_assignment: np.ndarray | None = None
    previous_h8_assignment: dict[str, np.ndarray | None] = {
        name: None for name in material_variants
    }
    jump_values: dict[str, dict[str, list[np.ndarray]]] = {
        name: {"u8": [], "h8": []} for name in material_variants
    }
    worst: dict[str, dict[str, dict[str, Any]]] = {
        name: {
            "u8": {"score": -1.0},
            "h8": {"score": -1.0},
        }
        for name in material_variants
    }
    valid = scene.validity

    for frame in range(33):
        angle = 2.0 * math.pi * frame / 32.0
        offsets = np.column_stack(
            [
                0.30 * np.sin(angle + phases),
                0.18 * np.sin(2.0 * angle + phases),
                0.45 * np.cos(angle + phases),
            ]
        )
        positions = base_positions + offsets
        print(f"[temporal] frame {frame:02d}/32: exact/U8/H8", flush=True)
        exact_diffuse, exact_specular, hit_count, sphere_edge = exact_components(
            scene, positions, light_diffuse
        )
        exact_interactions = int(np.sum(hit_count, dtype=np.uint64))
        u8 = build_u8(scene, layout, positions, light_diffuse)
        u8_switches, u8_membership = assignment_changes(
            previous_u8_assignment, u8["assignments"]
        )
        previous_u8_assignment = np.array(u8["assignments"], copy=True)

        for material_name, material_b in material_variants.items():
            exact_hdr = (
                exact_diffuse
                if material_name == "diffuse-only"
                else exact_diffuse + exact_specular
            )
            u8_hdr = resolve_u8(scene, layout, u8, material_b)
            h8 = evaluate_h8(
                scene,
                layout,
                positions,
                light_diffuse,
                material_b,
                exact_hdr,
            )
            h8_switches, h8_membership = assignment_changes(
                previous_h8_assignment[material_name], h8["assignments"]
            )
            previous_h8_assignment[material_name] = np.array(
                h8["assignments"], copy=True
            )
            temporal["materials"][material_name]["u8"]["binSwitchCount"] += u8_switches
            temporal["materials"][material_name]["u8"][
                "membershipTransitionCount"
            ] += u8_membership
            temporal["materials"][material_name]["h8"]["binSwitchCount"] += h8_switches
            temporal["materials"][material_name]["h8"][
                "membershipTransitionCount"
            ] += h8_membership

            for scheme, candidate, work in (
                ("u8", u8_hdr, 8 * int(np.count_nonzero(valid))),
                ("h8", h8["hdr"], h8["workEquivalent"]),
            ):
                metrics = error_metrics(
                    exact_hdr,
                    candidate,
                    valid,
                    layout["depthDiscontinuityMask"],
                    sphere_edge,
                )
                failures = quality_failures(metrics)
                overall = metrics["allValid"]
                frame_result = {
                    "frame": frame,
                    "qualityPassed": not failures,
                    "qualityFailures": failures,
                    "metrics": metrics,
                    "exactInteractionCount": exact_interactions,
                    "workEquivalent": int(work),
                    "workRatioVsExactInteractions": (
                        float(work / exact_interactions) if exact_interactions else math.inf
                    ),
                }
                if scheme == "h8":
                    frame_result["classification"] = h8["classification"]
                temporal["materials"][material_name][scheme]["frames"].append(frame_result)
                rows.append(
                    {
                        "frame": frame,
                        "material": material_name,
                        "scheme": scheme.upper(),
                        "qualityPassed": not failures,
                        "absoluteMean": overall["absoluteMaxChannel"]["mean"],
                        "absoluteP95": overall["absoluteMaxChannel"]["p95"],
                        "absoluteP99": overall["absoluteMaxChannel"]["p99"],
                        "absoluteMax": overall["absoluteMaxChannel"]["max"],
                        "relativeLuminanceP95": overall["relativeLuminance"]["p95"],
                        "relativeLuminanceP99": overall["relativeLuminance"]["p99"],
                        "affectedPixelRatio": overall["affectedPixelRatio"],
                        "missPixelRatio": overall["missPixelRatio"],
                        "leakPixelRatio": overall["leakPixelRatio"],
                        "workRatioVsExact": frame_result[
                            "workRatioVsExactInteractions"
                        ],
                    }
                )
                score = overall["absoluteMaxChannel"]["p99"]
                if score > worst[material_name][scheme]["score"]:
                    worst[material_name][scheme] = {
                        "score": score,
                        "frame": frame,
                        "exact": np.array(exact_hdr, copy=True),
                        "candidate": np.array(candidate, copy=True),
                    }
                if previous_exact[material_name] is not None:
                    exact_delta = np.abs(
                        np.einsum(
                            "ijk,k->ij", exact_hdr - previous_exact[material_name], LUMA
                        )
                    )
                    candidate_delta = np.abs(
                        np.einsum(
                            "ijk,k->ij",
                            candidate - previous_candidate[material_name][scheme],
                            LUMA,
                        )
                    )
                    jump_excess = np.maximum(candidate_delta - exact_delta, 0.0)
                    jump_values[material_name][scheme].append(
                        np.asarray(jump_excess[valid], dtype=np.float32)
                    )
                previous_candidate[material_name][scheme] = np.array(candidate, copy=True)
            previous_exact[material_name] = np.array(exact_hdr, copy=True)

    for material_name in material_variants:
        for scheme in ("u8", "h8"):
            values = (
                np.concatenate(jump_values[material_name][scheme]).astype(np.float64)
                if jump_values[material_name][scheme]
                else np.asarray([], dtype=np.float64)
            )
            jump_stats = distribution(values)
            frame_pass = all(
                frame["qualityPassed"]
                for frame in temporal["materials"][material_name][scheme]["frames"]
            )
            jump_pass = (
                jump_stats["p99"] <= TEMPORAL_JUMP_P99_LIMIT
                and jump_stats["max"] <= TEMPORAL_JUMP_MAX_LIMIT
            )
            temporal["materials"][material_name][scheme]["jumpExcess"] = jump_stats
            temporal["materials"][material_name][scheme]["allFramesQualityPassed"] = frame_pass
            temporal["materials"][material_name][scheme]["jumpGatePassed"] = jump_pass
            temporal["materials"][material_name][scheme]["temporalPassed"] = (
                frame_pass and jump_pass
            )
            worst_record = worst[material_name][scheme]
            prefix = (
                f"temporal-worst-frame-{worst_record['frame']:02d}-"
                f"{material_name}-{scheme}"
            )
            temporal["materials"][material_name][scheme]["worstFrame"] = {
                "frame": worst_record["frame"],
                "absoluteP99": worst_record["score"],
                "artifacts": save_error_artifacts(
                    run_dir,
                    prefix,
                    worst_record["exact"],
                    worst_record["candidate"],
                ),
            }
    return temporal, rows


def decide(
    static_cases: list[dict[str, Any]], temporal: dict[str, Any]
) -> dict[str, Any]:
    decisions: dict[str, Any] = {}
    for scheme in ("u8", "h8"):
        static_quality = all(
            case["materials"][material][scheme]["qualityPassed"]
            for case in static_cases
            for material in ("diffuse-only", "captured-specular")
        )
        temporal_quality = all(
            temporal["materials"][material][scheme]["temporalPassed"]
            for material in ("diffuse-only", "captured-specular")
        )
        target_ratios: dict[str, float] = {}
        for coverage in ("representative", "high-overlap"):
            case = next(item for item in static_cases if item["coverage"] == coverage)
            target_ratios[coverage] = max(
                float(case["materials"][material][scheme]["workRatioVsExactInteractions"])
                for material in ("diffuse-only", "captured-specular")
            )
        work_pass = all(value <= WORK_RATIO_LIMIT for value in target_ratios.values())
        if scheme == "u8":
            worst_memory = max(
                int(case["materials"][material][scheme]["physicalBytesAt1080p"])
                for case in static_cases
                for material in ("diffuse-only", "captured-specular")
            )
        else:
            worst_memory = max(
                int(case["materials"][material][scheme]["estimatedPhysicalBytesAt1080p"])
                for case in static_cases
                for material in ("diffuse-only", "captured-specular")
            )
        memory_pass = worst_memory <= MEMORY_LIMIT_BYTES
        phase_b_eligible = static_quality and temporal_quality and work_pass and memory_pass
        reasons = []
        if not static_quality:
            reasons.append("one or more frozen static quality gates failed")
        if not temporal_quality:
            reasons.append("one or more frozen temporal quality gates failed")
        if not work_pass:
            reasons.append("representative/high-overlap work ratio exceeded 0.60")
        if not memory_pass:
            reasons.append("estimated 1080p candidate memory exceeded 32 MiB")
        decisions[scheme] = {
            "label": "Phase-B-eligible" if phase_b_eligible else "No-Go",
            "staticQualityPassed": static_quality,
            "temporalQualityPassed": temporal_quality,
            "workGatePassed": work_pass,
            "memoryGatePassed": memory_pass,
            "phaseBEligible": phase_b_eligible,
            "targetWorkRatios": target_ratios,
            "worstEstimated1080pBytes": worst_memory,
            "reasons": reasons,
        }
    return {
        "u8": decisions["u8"],
        "h8": decisions["h8"],
        "phaseBEntered": bool(
            decisions["u8"]["phaseBEligible"] or decisions["h8"]["phaseBEligible"]
        ),
    }


def fmt(value: float, digits: int = 6) -> str:
    return f"{float(value):.{digits}f}"


def percent(value: float, digits: int = 3) -> str:
    return f"{100.0 * float(value):.{digits}f}%"


def relative_link(path_text: str, run_dir: Path) -> str:
    path = Path(path_text)
    try:
        return path.resolve().relative_to(run_dir.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def generate_report(
    run_dir: Path,
    manifest: dict[str, Any],
    static_cases: list[dict[str, Any]],
    temporal: dict[str, Any],
    decisions: dict[str, Any],
    validity_smoke: dict[str, Any],
) -> None:
    representative = next(case for case in static_cases if case["coverage"] == "representative")
    high_overlap = next(case for case in static_cases if case["coverage"] == "high-overlap")
    small_local = next(case for case in static_cases if case["coverage"] == "small-local")
    rep_u8 = representative["materials"]["captured-specular"]["u8"]
    rep_h8 = representative["materials"]["captured-specular"]["h8"]
    high_u8 = high_overlap["materials"]["captured-specular"]["u8"]
    high_h8 = high_overlap["materials"]["captured-specular"]["h8"]
    small_h8 = small_local["materials"]["captured-specular"]["h8"]
    lines: list[str] = []
    lines.extend(
        [
            "# Directional-Binned Tile/Cluster Lighting 可证伪实验报告",
            "",
            "- 日期：2026-08-04",
            "- 阶段：Phase A 离线诊断",
            "- Control：当前默认 `analytic-screen` / 逐灯三维解析球 Oracle",
            f"- 抓取 EXE SHA-256：`{manifest['executableSha256']}`",
            f"- Phase 0 协议 SHA-256：`{manifest['protocolSha256']}`",
            "- 分辨率：640×360（feasibility，不是正式性能分辨率）",
            "- 默认值：未改变，仍为固定 `analytic-screen`",
            "",
            "## 1. 结论",
            "",
            "| 方案 | Phase A 决策 | 静态质量 | 连续帧质量 | Work 门槛 | 1080p 内存门槛 |",
            "|---|---|---:|---:|---:|---:|",
        ]
    )
    for scheme, label in (("u8", "原始 U8"), ("h8", "Hybrid H8")):
        decision = decisions[scheme]
        lines.append(
            f"| {label} | **{decision['label']}** | "
            f"{'Pass' if decision['staticQualityPassed'] else 'Fail'} | "
            f"{'Pass' if decision['temporalQualityPassed'] else 'Fail'} | "
            f"{'Pass' if decision['workGatePassed'] else 'Fail'} | "
            f"{'Pass' if decision['memoryGatePassed'] else 'Fail'} |"
        )
    lines.extend(
        [
            "",
            (
                "Phase B **进入**：至少一个候选通过全部预设门槛，后续必须实现独立 runtime "
                "模式后才能形成 Go/No-Go。"
                if decisions["phaseBEntered"]
                else "Phase B **未进入**：两个候选均未通过全部预设门槛；按阶段门控停止大规模 Runtime 改造，也没有为了实现而更改门槛。"
            ),
            "",
            "关键可证伪结果（captured-specular）：",
            "",
            f"- U8 representative：Abs P99={fmt(rep_u8['metrics']['allValid']['absoluteMaxChannel']['p99'])}，affected={percent(rep_u8['metrics']['allValid']['affectedPixelRatio'])}；high-overlap：Abs P99={fmt(high_u8['metrics']['allValid']['absoluteMaxChannel']['p99'])}，affected={percent(high_u8['metrics']['allValid']['affectedPixelRatio'])}；",
            f"- H8 representative：Abs P99={fmt(rep_h8['metrics']['allValid']['absoluteMaxChannel']['p99'])}，affected={percent(rep_h8['metrics']['allValid']['affectedPixelRatio'])}，work ratio={rep_h8['workRatioVsExactInteractions']:.3f}；high-overlap：Abs P99={fmt(high_h8['metrics']['allValid']['absoluteMaxChannel']['p99'])}，affected={percent(high_h8['metrics']['allValid']['affectedPixelRatio'])}，work ratio={high_h8['workRatioVsExactInteractions']:.3f}；",
            f"- H8 在 small-local 的零误差不是聚合成功：eligible={small_h8['classification']['eligible']}，全部 {small_h8['classification']['exact']:,} 个 cluster-light membership 走 exact，work 是 Oracle 球内 interaction 的 {small_h8['workRatioVsExactInteractions']:.3f}×；",
            "- U8 虽在 representative/high-overlap 的理论 work ratio 为 0.205/0.044，但质量先失败，因此这些比例不能被当成可实现的性能收益。",
            "",
            "本轮不把 U8/H8 称为 exact、Adaptive 或已经完成的 Tiled/Clustered。没有 Runtime Candidate，因此没有可计时的 build/upload/resolve，也不产生伪造的 GPU 性能数字或 RenderDoc Candidate 帧。",
            "",
            "## 2. Exact Oracle 与语义对齐",
            "",
            "Oracle 读取本轮 Release 程序抓取的实际 explicit `gPosition`、validity alpha、normal、albedo、material RGB/alpha；天空先跳过。随后逐灯执行当前 shader 的三维球 predicate、distance attenuation、Lambert diffuse 和 Blinn-Phong specular，再在线性 HDR 中相加。压力场景点光 ambient、点阴影、SSAO、方向灯和聚光灯均为 0/off。",
            "",
            "已验收的旧证据 `point-light-screen-routing/screen-bounds-scissor-analytic-20260804/REPORT_CN.md` 已证明当前 `analytic-screen` 对 per-light fullscreen Oracle 静态逐像素一致。本轮没有重新定义 Oracle，也没有用最终 8-bit 截图替代 HDR 比较。",
            "",
            "CPU Oracle 另经过当前固定后处理（HDR off、gamma=2.2）与本轮 app Analytic Screen LDR 逐通道比对：",
            "",
            "| Coverage | Channel mean / P95 / P99 / max（8-bit LSB） | 不同像素 | >1 LSB 通道 | Bulk sanity |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for case in static_cases:
        validation = case["oracleAppLdrValidation"]
        error = validation["channelAbsoluteError"]
        lines.append(
            f"| {case['coverage']} | {fmt(error['mean'])} / {fmt(error['p95'])} / "
            f"{fmt(error['p99'])} / {fmt(error['max'])} | "
            f"{validation['differentPixelCount']} / {percent(validation['differentPixelRatio'])} | "
            f"{validation['channelsAboveOneLsbCount']} / {percent(validation['channelsAboveOneLsbRatio'])} | "
            f"{'Pass' if validation['passedBulkAgreementGate'] else 'Fail'} |"
        )
    lines.extend(
        [
            "",
            "该 sanity 只用于排除 CPU Oracle 的整体实现偏差，不是候选质量门槛：要求通道误差 P99≤1 LSB、>1 LSB 通道≤0.01%、max≤8 LSB。捕获 JSON 将灯参数序列化为六位小数，极少数恰在解析球边界的像素可因命中集合移动而超过 1 LSB；原始计数与 max 均保留在 `aggregate.json`。",
            "",
            f"无效/天空语义 smoke：从 representative G-Buffer 额外标记并以 NaN 毒化 {validity_smoke['forcedInvalidPixelCount']} 个像素；Exact/U8/H8 在这些像素的最大绝对输出均为 0，完整输出 finite，结果 **{'Pass' if validity_smoke['passed'] else 'Fail'}**。固定相机本身恰好没有天空像素，因此这项 smoke 只验证‘先跳过再读取’代码契约，不冒充真实天空画质场景。原始结果见 [`invalid-pixel-semantic-smoke.json`](invalid-pixel-semantic-smoke.json)。",
            "",
            "## 3. 静态 HDR 质量",
            "",
            "误差均为 tone mapping 前。Abs 是逐像素 RGB 最大通道绝对误差；Rel 是 Oracle luminance>1e-3 像素的相对亮度误差。",
            "",
            "| Coverage / 灯数 | 材质 | 方案 | Abs mean / P95 / P99 / max | Rel P95 / P99 | Affected | Miss / Leak | 判定 |",
            "|---|---|---|---|---|---:|---:|---:|",
        ]
    )
    for case in static_cases:
        for material in ("diffuse-only", "captured-specular"):
            for scheme in ("u8", "h8"):
                result = case["materials"][material][scheme]
                overall = result["metrics"]["allValid"]
                absolute = overall["absoluteMaxChannel"]
                relative = overall["relativeLuminance"]
                lines.append(
                    f"| {case['coverage']} / {case['lightCount']} | {material} | {scheme.upper()} | "
                    f"{fmt(absolute['mean'])} / {fmt(absolute['p95'])} / "
                    f"{fmt(absolute['p99'])} / {fmt(absolute['max'])} | "
                    f"{percent(relative['p95'])} / {percent(relative['p99'])} | "
                    f"{percent(overall['affectedPixelRatio'])} | "
                    f"{percent(overall['missPixelRatio'])} / {percent(overall['leakPixelRatio'])} | "
                    f"{'Pass' if result['qualityPassed'] else 'Fail'} |"
                )
    lines.extend(
        [
            "",
            "### 3.1 深度不连续与点光球边缘",
            "",
            "| Coverage / 材质 / 方案 | Depth P99 / affected | Sphere-edge P99 / affected |",
            "|---|---:|---:|",
        ]
    )
    for case in static_cases:
        for material in ("diffuse-only", "captured-specular"):
            for scheme in ("u8", "h8"):
                result = case["materials"][material][scheme]
                depth = result["metrics"]["depthDiscontinuity"]
                edge = result["metrics"]["sphereEdge"]
                lines.append(
                    f"| {case['coverage']} / {material} / {scheme.upper()} | "
                    f"{fmt(depth['absoluteMaxChannel']['p99'])} / {percent(depth['affectedPixelRatio'])} | "
                    f"{fmt(edge['absoluteMaxChannel']['p99'])} / {percent(edge['affectedPixelRatio'])} |"
                )
    lines.extend(
        [
            "",
            "每个静态组合都保存了 exact/candidate HDR PFM、absolute HDR diff PFM、tone-mapped PNG、固定量程 heatmap 和 signed luminance 伪彩图。图像目录为 [`images`](images/)，HDR 目录为 [`hdr`](hdr/)。原程序 Analytic Screen LDR 与完整 G-Buffer 抓取保留在 [`captures`](captures/)。",
            "",
            "## 4. 压缩、分类与方向量化",
            "",
            "Work-equivalent 不是 GPU 时间：Exact 计实际球内 shading interaction；U8 为每个有效像素固定 8 次；H8 为固定 8 次加 cluster exact-list 长度。只有质量先通过时，该数值才有资格作为进入 Runtime 的可行性门槛。",
            "",
            "| Coverage / 材质 | Exact interactions | U8 work / ratio | H8 work / ratio | H8 partial / full / far / eligible / exact |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for case in static_cases:
        for material in ("diffuse-only", "captured-specular"):
            u8 = case["materials"][material]["u8"]
            h8 = case["materials"][material]["h8"]
            cls = h8["classification"]
            lines.append(
                f"| {case['coverage']} / {material} | {case['exactInteractionCount']} | "
                f"{u8['workEquivalent']} / {u8['workRatioVsExactInteractions']:.3f} | "
                f"{h8['workEquivalent']} / {h8['workRatioVsExactInteractions']:.3f} | "
                f"{cls['partial']} / {cls['full']} / {cls['far']} / "
                f"{cls['eligible']} / {cls['exact']} |"
            )
    lines.extend(
        [
            "",
            "U8 每 tile 的 exact light count、occupied bin count 和方向误差在各 `*-u8-tile-stats.csv`。H8 每 cluster 的 partial/full/far/eligible/exact、specular fallback 和方向误差在各 `*-h8-cluster-stats.csv`。汇总入口为 [`static-summary.csv`](static-summary.csv)。",
            "",
            "H8 的 `full` 是完整 Tile×depth-slice frustum 八角点都在解析光球内，不是二维 Screen Rect 覆盖。`far` 还要求距离≥4×cluster radius 且八角 attenuation max/min≤1.10。captured specular 中 `material.b>0.02` 的 cluster 走 exact fallback；质量恢复如果伴随 exact pool 膨胀，会在 Work 与内存门槛中如实失败。",
            "",
            "| Coverage | U8 exact lights/tile P50/P95/max | U8 bins/tile P50/P95/max | H8 exact lights/cluster P50/P95/max | H8 bins/cluster P50/P95/max | U8 dir error P50/P95/max | H8 reconstructed dir error P50/P95/max |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for case in static_cases:
        u8 = case["materials"]["captured-specular"]["u8"]
        h8 = case["materials"]["captured-specular"]["h8"]
        u8_lights = u8["exactLightCountPerTile"]
        u8_bins = u8["occupiedBinCount"]
        h8_lights = h8["exactLightCountPerCluster"]
        h8_bins = h8["occupiedBinCountPerCluster"]
        u8_direction = u8["directionErrorDegrees"]
        h8_direction = h8["directionErrorReconstructedDegrees"]
        lines.append(
            f"| {case['coverage']} | {fmt(u8_lights['p50'], 2)}/{fmt(u8_lights['p95'], 2)}/{fmt(u8_lights['max'], 2)} | "
            f"{fmt(u8_bins['p50'], 2)}/{fmt(u8_bins['p95'], 2)}/{fmt(u8_bins['max'], 2)} | "
            f"{fmt(h8_lights['p50'], 2)}/{fmt(h8_lights['p95'], 2)}/{fmt(h8_lights['max'], 2)} | "
            f"{fmt(h8_bins['p50'], 2)}/{fmt(h8_bins['p95'], 2)}/{fmt(h8_bins['max'], 2)} | "
            f"{fmt(u8_direction['p50'], 2)}/{fmt(u8_direction['p95'], 2)}/{fmt(u8_direction['max'], 2)} | "
            f"{fmt(h8_direction['p50'], 2)}/{fmt(h8_direction['p95'], 2)}/{fmt(h8_direction['max'], 2)} |"
        )
    lines.extend(
        [
            "",
            "## 5. 连续移动灯序列",
            "",
            "representative 场景前 64 灯在固定实际 G-Buffer 上按冻结正弦轨迹移动 33 个连续样本。它是离线连续帧诊断，不伪称 GPU runtime 动画。",
            "",
            "| 材质 | 方案 | 全帧质量 | Jump P99 / max | Bin switches | Membership transitions | 最坏帧 |",
            "|---|---|---:|---:|---:|---:|---:|",
        ]
    )
    for material in ("diffuse-only", "captured-specular"):
        for scheme in ("u8", "h8"):
            result = temporal["materials"][material][scheme]
            jump = result["jumpExcess"]
            lines.append(
                f"| {material} | {scheme.upper()} | "
                f"{'Pass' if result['temporalPassed'] else 'Fail'} | "
                f"{fmt(jump['p99'])} / {fmt(jump['max'])} | "
                f"{result['binSwitchCount']} | {result['membershipTransitionCount']} | "
                f"{result['worstFrame']['frame']} |"
            )
    lines.extend(
        [
            "",
            "逐帧原始统计在 [`temporal-frames.csv`](temporal-frames.csv) 和 [`aggregate.json`](aggregate.json)；每个方案/材质的最坏帧 HDR 与伪彩图在 `hdr/`、`images/`。Bin switch 只统计同一 tile/cluster、同一灯连续两帧都处于 aggregate 状态而主 bin 改变；进入/离开 aggregate 另列 membership transition。",
            "",
            "## 6. 原始想法为何成立或不成立",
            "",
        ]
    )
    if decisions["u8"]["phaseBEligible"]:
        lines.append(
            "U8 在本轮固定场景、材质和运动轨迹下同时通过质量、Work 与内存预门槛，说明其信息预算在这些边界内尚未被证伪；但这仍不是 Runtime Go，必须把 build/upload/resolve 全部实现和计时。"
        )
    else:
        lines.append(
            "U8 的动机成立在“把多灯循环压成固定八方向”这一复杂度目标上，但 RGB+二维方位没有携带每像素深度方向、距离衰减和球边界。tile 中只要一个样本被灯命中，整个 tile 都会收到该 bin，天然产生 leak；反过来，固定方向和代表点 attenuation 又会造成 miss/幅值错误。Specular 还依赖 half-vector，二维量化误差会被高光指数放大。只要任一冻结质量门槛失败，U8 即为 No-Go，而不是通过改阈值或偷加 per-light 数据挽救原定义。"
        )
    if decisions["h8"]["phaseBEligible"]:
        lines.append(
            "H8 通过三维 cluster 包含、far attenuation 限制、相邻 bin 加权和 exact fallback 建立了可解释的质量边界，并通过预门槛；下一步只能作为独立实验模式进入 Runtime，不得替换默认 Control。"
        )
    else:
        lines.append(
            "H8 用三维包含、far 限制和 exact fallback 修复 U8 最危险的语义漏洞，但修复存在硬交换：保守 partial/near/specular-critical 会回到逐灯 exact；允许 aggregate 的部分仍只有八个固定 3D 方向和 cluster-center attenuation。若质量不通过，说明八方向本身仍不足；若质量通过而 Work/内存失败，说明 exact fallback 已吃掉压缩收益。两种情况都不应进入大规模 Runtime。"
        )
    lines.extend(
        [
            "",
            "## 7. 内存与 overflow",
            "",
            f"- U8 1080p 物理估算：{decisions['u8']['worstEstimated1080pBytes'] / (1024*1024):.3f} MiB；",
            f"- H8 按 640×360 非空 cluster/exact pool 随 tile 数外推的最坏 1080p 估算：{decisions['h8']['worstEstimated1080pBytes'] / (1024*1024):.3f} MiB；",
            "- Phase A 使用动态容器和 float64 累加；所有 PFM 写出前检查 finite；没有固定 N 截断；",
            "- 若未来实现 Runtime，必须 count/prefix-sum 精确分配 exact pool；不足时重分配、整 cluster 回退 Analytic Screen 或明确失败，禁止静默丢灯。",
            "",
            "## 8. 可复现与证据",
            "",
            "```powershell",
            "powershell -ExecutionPolicy Bypass -File .\\tools\\run_directional_binned_phase_a.ps1 `",
            "  -Mode All `",
            "  -RunDirectory .\\benchmark-results\\directional-binned-lighting\\directional-binned-phase-a-20260804 `",
            "  -PythonExecutable C:\\Users\\Link\\.cache\\codex-runtimes\\codex-primary-runtime\\dependencies\\python\\python.exe",
            "```",
            "",
            "关键证据：",
            "",
            "- [`run_directional_binned_phase_a.ps1`](../../../tools/run_directional_binned_phase_a.ps1)、[`analyze_directional_binned_phase_a.py`](../../../tools/analyze_directional_binned_phase_a.py)：抓取编排与离线 Oracle/U8/H8 分析源码；",
            "- [`PHASE0_FROZEN_PROTOCOL_CN.md`](PHASE0_FROZEN_PROTOCOL_CN.md)：结果前冻结的定义和门槛；",
            "- [`capture-manifest.json`](capture-manifest.json)：EXE/协议/源文件哈希与场景抓取；",
            "- [`aggregate.json`](aggregate.json)：全部静态、连续帧与门控原始聚合；",
            "- [`static-summary.csv`](static-summary.csv)、[`temporal-frames.csv`](temporal-frames.csv)：扁平统计；",
            "- [`captures`](captures/)：app LDR、G-Buffer PFM、逐灯参数与 bounds telemetry；",
            "- [`csv`](csv/)：每 tile/cluster 明细；",
            "- [`hdr`](hdr/)、[`images`](images/)：HDR exact/candidate/diff 与可视化；",
            "- [`artifact-manifest.json`](artifact-manifest.json)：产物大小和 SHA-256。",
            "",
            "## 9. 限制与适用边界",
            "",
            "- Phase A 是 640×360、固定 RTX 主机上由 Release 程序抓取 G-Buffer 后的 CPU/NumPy 重放；Python wall time 不是渲染性能；",
            "- 静态覆盖 small/medium/representative/high-overlap；连续帧是移动灯、固定几何，不包含移动相机导致的新遮挡；",
            "- 压力场景点阴影关闭；设计上 shadowed 灯只能 exact；",
            "- H8 运行时若改 depth slice、tile size、方向集合、specular 阈值或 far 条件，必须视为新候选重新冻结并重测；",
            "- 结论不外推其他 GPU，也不证明透明 Forward 或 PBR backend；",
            "- Phase B 未进入时没有 Candidate RenderDoc；旧 Analytic Screen RenderDoc 证据仍保留在旧 benchmark 目录，未覆盖。",
        ]
    )
    verification_path = run_dir / "verification" / "verification.json"
    if verification_path.is_file():
        verification = json.loads(verification_path.read_text(encoding="utf-8-sig"))
        build = verification["releaseBuild"]
        executable = verification["executable"]
        smoke = verification["smoke"]
        integrity = verification["artifactIntegrity"]
        semantic = verification["semanticArtifactCrossCheck"]
        lines.extend(
            [
                "",
                "## 10. Release 构建与默认路径 Smoke",
                "",
                f"- Release x64 构建：**{'Pass' if build['success'] else 'Fail'}**，exit code={build['exitCode']}；",
                f"- EXE SHA-256：`{executable['sha256']}`；",
                f"- 不显式传 `--point-light-render-mode` 的 {smoke['resolution'][0]}×{smoke['resolution'][1]} Smoke：**{'Pass' if smoke['success'] else 'Fail'}**，result success={str(smoke['resultSuccess']).lower()}，renderMode=`{smoke['renderMode']}`，renderModeExplicit={str(smoke['renderModeExplicit']).lower()}；",
                f"- 默认值改变：**{str(verification['defaultChanged']).lower()}**。本实验没有新增 Runtime Candidate，也没有切换当前 Analytic Screen 默认路径。",
                f"- 产物完整性复算：**{'Pass' if integrity['passed'] else 'Fail'}**；重算 {integrity['manifestRecordsRehashed']} 个清单哈希，解析 {integrity['jsonFilesParsed']} 个 JSON、{integrity['csvFilesParsed']} 个 CSV（{integrity['csvDataRowsParsed']} 行），检查 {integrity['pfmFilesWithFiniteValues']} 个 PFM（{integrity['pfmValuesChecked']} 个 finite 值）并解码 {integrity['pngAndPpmFilesDecoded']} 个 PNG/PPM。",
                f"- 数值产物交叉校验：**{'Pass' if semantic['passed'] else 'Fail'}**；从 exact/candidate PFM 重算 {semantic['staticCombinationCount']} 个静态组合与 4 个连续序列最坏帧的 diff/P99，并验证 H8 分类恒等式、8-bin 上限与灯数上限。",
                "",
                "机器可读结果与运行输出见 [`verification/verification.json`](verification/verification.json)、[`verification/smoke.json`](verification/smoke.json) 和 [`verification/smoke.log`](verification/smoke.log)。",
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
            "totalBytes": sum(item["bytes"] for item in records),
            "files": records,
        },
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="regenerate REPORT_CN.md and artifact-manifest.json from aggregate.json",
    )
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    manifest_path = run_dir / "capture-manifest.json"
    protocol_path = run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"
    if not manifest_path.is_file() or not protocol_path.is_file():
        raise FileNotFoundError("capture manifest or frozen protocol is missing")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    protocol_hash = sha256_file(protocol_path)
    if protocol_hash != manifest["protocolSha256"]:
        raise ValueError(
            "frozen protocol hash changed after capture: "
            f"{protocol_hash} != {manifest['protocolSha256']}"
        )
    if args.report_only:
        aggregate_path = run_dir / "aggregate.json"
        if not aggregate_path.is_file():
            raise FileNotFoundError("aggregate.json is missing")
        aggregate = json.loads(aggregate_path.read_text(encoding="utf-8-sig"))
        if aggregate["protocolSha256"] != protocol_hash:
            raise ValueError("aggregate protocol hash does not match frozen protocol")
        generate_report(
            run_dir,
            manifest,
            aggregate["staticCases"],
            aggregate["temporal"],
            aggregate["decision"],
            aggregate["invalidPixelSemanticSmoke"],
        )
        build_artifact_manifest(run_dir)
        print("[report-only] regenerated report and artifact manifest", flush=True)
        return 0
    cases = manifest["cases"]
    if len(cases) != 4:
        raise ValueError(f"expected four static captures, found {len(cases)}")
    expected = {
        ("small-local", 64),
        ("medium-local", 64),
        ("representative", 256),
        ("high-overlap", 256),
    }
    actual = {(str(item["coverage"]), int(item["lightCount"])) for item in cases}
    if actual != expected:
        raise ValueError(f"capture matrix mismatch: {actual}")

    static_cases: list[dict[str, Any]] = []
    static_rows: list[dict[str, Any]] = []
    representative_scene: SceneData | None = None
    representative_layout: dict[str, Any] | None = None
    for case in cases:
        scene = load_scene(case, manifest_path)
        print(
            f"[load] {scene.stem}: {scene.width}x{scene.height}, "
            f"valid={np.count_nonzero(scene.validity)}, lights={scene.light_count}",
            flush=True,
        )
        layout = build_layout(scene)
        result, rows = analyze_static_case(run_dir, scene, layout)
        static_cases.append(result)
        static_rows.extend(rows)
        if scene.coverage == "representative":
            representative_scene = scene
            representative_layout = layout
    if representative_scene is None or representative_layout is None:
        raise RuntimeError("representative scene was not loaded")

    print("[semantic-smoke] invalid/sky pixels", flush=True)
    validity_smoke = run_invalid_pixel_semantic_smoke(
        representative_scene, run_dir
    )
    temporal, temporal_rows = analyze_temporal(
        run_dir, representative_scene, representative_layout
    )
    decisions = decide(static_cases, temporal)
    aggregate = {
        "schemaVersion": 1,
        "experiment": "directional-binned-tile-cluster-phase-a",
        "protocolSha256": protocol_hash,
        "captureManifestSha256": sha256_file(manifest_path),
        "thresholds": {
            "absolute": {
                "mean": ABS_MEAN_LIMIT,
                "p95": ABS_P95_LIMIT,
                "p99": ABS_P99_LIMIT,
                "max": ABS_MAX_LIMIT,
            },
            "relativeLuminance": {"p95": REL_P95_LIMIT, "p99": REL_P99_LIMIT},
            "affectedRatio": AFFECTED_LIMIT,
            "missLeakRatio": MISS_LEAK_LIMIT,
            "subset": {
                "absoluteP99": SUBSET_P99_LIMIT,
                "affectedRatio": SUBSET_AFFECTED_LIMIT,
            },
            "temporalJump": {
                "p99": TEMPORAL_JUMP_P99_LIMIT,
                "max": TEMPORAL_JUMP_MAX_LIMIT,
            },
            "workRatio": WORK_RATIO_LIMIT,
            "memoryBytes": MEMORY_LIMIT_BYTES,
        },
        "staticCases": static_cases,
        "temporal": temporal,
        "invalidPixelSemanticSmoke": validity_smoke,
        "decision": decisions,
        "runtimeCandidateImplemented": False,
        "defaultChanged": False,
    }
    json_dump(run_dir / "aggregate.json", aggregate)
    write_rows(run_dir / "static-summary.csv", static_rows)
    write_rows(run_dir / "temporal-frames.csv", temporal_rows)
    generate_report(
        run_dir, manifest, static_cases, temporal, decisions, validity_smoke
    )
    build_artifact_manifest(run_dir)
    print(
        "[decision] "
        f"U8={decisions['u8']['label']} H8={decisions['h8']['label']} "
        f"phaseBEntered={decisions['phaseBEntered']}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"[directional-binned-phase-a] fatal: {error}", file=sys.stderr)
        raise
