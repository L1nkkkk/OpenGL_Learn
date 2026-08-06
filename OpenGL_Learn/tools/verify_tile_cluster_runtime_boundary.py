#!/usr/bin/env python3
"""Independent CSR, image, edge, and shader-contract verifier."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import subprocess
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


TILE_SIZE = 16
SLICES = 16
NEAR = 0.1
FAR = 100.0
COUNTS = (32, 64, 128, 256, 512)
RADII = (1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def dump_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        magic = stream.readline().strip()
        if magic not in (b"PF", b"Pf"):
            raise ValueError(f"bad PFM magic: {path}")
        width, height = (int(value) for value in stream.readline().split())
        scale = float(stream.readline())
        channels = 3 if magic == b"PF" else 1
        values = np.fromfile(stream, dtype="<f4" if scale < 0 else ">f4")
    shape = (height, width, channels) if channels == 3 else (height, width)
    if values.size != int(np.prod(shape)):
        raise ValueError(f"PFM payload mismatch: {path}")
    result = values.reshape(shape)
    if not np.all(np.isfinite(result)):
        raise ValueError(f"non-finite PFM: {path}")
    return result


def build_fnv_helper(run_dir: Path) -> Path:
    source = Path(__file__).with_name("fnv1a64_stdin.cpp").resolve()
    output_dir = run_dir / "verification"
    output_dir.mkdir(parents=True, exist_ok=True)
    executable = output_dir / "fnv1a64_stdin.exe"
    stamp = output_dir / "fnv1a64_stdin.source.sha256"
    source_hash = sha256_file(source)
    if executable.exists() and stamp.exists() and stamp.read_text(encoding="ascii").strip() == source_hash:
        return executable
    candidates = sorted(
        Path(r"C:\Program Files\Microsoft Visual Studio\2022").glob(
            "*/VC/Auxiliary/Build/vcvars64.bat"
        )
    )
    if not candidates:
        raise RuntimeError("Visual Studio vcvars64.bat was not found for independent FNV helper")
    vcvars = candidates[0]
    object_path = output_dir / "fnv1a64_stdin.obj"
    command = (
        f'call "{vcvars}" >nul && cl /nologo /O2 /EHsc '
        f'"{source}" /Fo:"{object_path}" /Fe:"{executable}"'
    )
    result = subprocess.run(
        command,
        cwd=output_dir,
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode or not executable.exists():
        raise RuntimeError(f"FNV helper compilation failed:\n{result.stdout}")
    stamp.write_text(source_hash + "\n", encoding="ascii")
    return executable


def fnv_arrays(helper: Path, *arrays: np.ndarray) -> str:
    process = subprocess.Popen(
        [str(helper)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    assert process.stdin is not None
    try:
        for array in arrays:
            contiguous = np.ascontiguousarray(array)
            payload = memoryview(contiguous).cast("B")
            for begin in range(0, len(payload), 4 * 1024 * 1024):
                process.stdin.write(payload[begin : begin + 4 * 1024 * 1024])
    finally:
        process.stdin.close()
    assert process.stdout is not None and process.stderr is not None
    output = process.stdout.read().decode("ascii").strip()
    error = process.stderr.read().decode("utf-8", errors="replace")
    return_code = process.wait()
    if return_code:
        raise RuntimeError(f"FNV helper failed ({return_code}): {error}")
    if not re.fullmatch(r"0x[0-9a-f]{16}", output):
        raise ValueError(f"invalid FNV helper output: {output!r}")
    return output


def normalize_rows(vectors: np.ndarray) -> np.ndarray:
    lengths = np.sqrt(np.sum(vectors * vectors, axis=1, dtype=np.float32)).astype(np.float32)
    return vectors / lengths[:, None]


def tile_planes(width: int, height: int, projection: np.ndarray) -> tuple[np.ndarray, ...]:
    tiles_x = (width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (height + TILE_SIZE - 1) // TILE_SIZE
    left = np.empty((tiles_x * tiles_y, 3), dtype=np.float32)
    right = np.empty_like(left)
    bottom = np.empty_like(left)
    top = np.empty_like(left)
    cursor = 0
    fw = np.float32(width)
    fh = np.float32(height)
    for tile_y in range(tiles_y):
        y0 = tile_y * TILE_SIZE
        y1 = min(height, y0 + TILE_SIZE)
        ndc_y0 = np.float32(np.float32(2.0) * np.float32(y0) / fh - np.float32(1.0))
        ndc_y1 = np.float32(np.float32(2.0) * np.float32(y1) / fh - np.float32(1.0))
        c = np.float32(ndc_y0 / projection[1, 1])
        d = np.float32(ndc_y1 / projection[1, 1])
        for tile_x in range(tiles_x):
            x0 = tile_x * TILE_SIZE
            x1 = min(width, x0 + TILE_SIZE)
            ndc_x0 = np.float32(np.float32(2.0) * np.float32(x0) / fw - np.float32(1.0))
            ndc_x1 = np.float32(np.float32(2.0) * np.float32(x1) / fw - np.float32(1.0))
            a = np.float32(ndc_x0 / projection[0, 0])
            b = np.float32(ndc_x1 / projection[0, 0])
            left[cursor] = (1.0, 0.0, a)
            right[cursor] = (-1.0, 0.0, -b)
            bottom[cursor] = (0.0, 1.0, c)
            top[cursor] = (0.0, -1.0, -d)
            cursor += 1
    return tuple(normalize_rows(value) for value in (left, right, bottom, top))


def depth_slice(depth: float) -> int:
    clamped = max(NEAR, min(FAR, depth))
    normalized = math.log(clamped / NEAR) / math.log(FAR / NEAR)
    return max(0, min(SLICES - 1, int(math.floor(normalized * SLICES))))


def encode_csr(membership: np.ndarray, helper: Path) -> dict[str, Any]:
    counts = np.sum(membership, axis=1, dtype=np.uint32)
    prefix = np.empty(counts.size, dtype=np.uint32)
    if counts.size:
        cumulative = np.cumsum(counts, dtype=np.uint64)
        if int(cumulative[-1]) > np.iinfo(np.uint32).max:
            raise OverflowError("independent CSR exceeds uint32")
        prefix[0] = 0
        if counts.size > 1:
            prefix[1:] = cumulative[:-1].astype(np.uint32)
    metadata = np.column_stack((prefix, counts)).astype("<u4", copy=False)
    indices = np.nonzero(membership)[1].astype("<u4", copy=False)
    signature = fnv_arrays(helper, metadata, indices)
    return {
        "logicalCells": int(counts.size),
        "nonEmptyCells": int(np.count_nonzero(counts)),
        "totalIndices": int(indices.size),
        "maximumLightsPerCell": int(np.max(counts)) if counts.size else 0,
        "averageLightsPerCell": float(np.mean(counts)) if counts.size else 0.0,
        "metadataBytes": int(metadata.nbytes),
        "indexBytes": int(indices.nbytes),
        "csrSignature": signature,
        "membership": membership,
    }


def rebuild_pair(result: dict[str, Any], helper: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    point = result["pointLightStress"]
    width, height = (int(value) for value in result["resolution"])
    view = np.asarray(result["gBuffer"]["cameraMatrices"]["view"], dtype=np.float32)
    projection = np.asarray(result["gBuffer"]["cameraMatrices"]["projection"], dtype=np.float32)
    positions = np.asarray([item["position"] for item in point["lights"]], dtype=np.float32)
    light_count = positions.shape[0]
    tiles_x = (width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (height + TILE_SIZE - 1) // TILE_SIZE
    tile_count = tiles_x * tiles_y
    planes = tile_planes(width, height, projection)
    homogeneous = np.concatenate((positions, np.ones((light_count, 1), dtype=np.float32)), axis=1)
    view_centers = (view @ homogeneous.T).T[:, :3].astype(np.float32)
    radius = np.float32(point["volumeRadius"])
    guarded = np.float32(radius + np.float32(1.0e-6))
    tile_membership = np.zeros((tile_count, light_count), dtype=bool)
    slice_ranges: list[tuple[int, int]] = []
    for light_index, center in enumerate(view_centers):
        center_depth = float(-center[2])
        if not math.isfinite(center_depth) or center_depth + float(guarded) < NEAR or center_depth - float(guarded) > FAR:
            slice_ranges.append((1, 0))
            continue
        member = np.ones(tile_count, dtype=bool)
        for normal in planes:
            member &= (normal @ center) >= -guarded
        tile_membership[:, light_index] = member
        minimum = center_depth - float(guarded)
        maximum = center_depth + float(guarded)
        if maximum < NEAR or minimum > FAR:
            slice_ranges.append((1, 0))
        else:
            slice_ranges.append((depth_slice(max(NEAR, minimum)), depth_slice(min(FAR, maximum))))
    cluster_membership = np.zeros((SLICES, tile_count, light_count), dtype=bool)
    for light_index, (minimum_slice, maximum_slice) in enumerate(slice_ranges):
        if minimum_slice <= maximum_slice:
            cluster_membership[minimum_slice : maximum_slice + 1, :, light_index] = tile_membership[:, light_index]
    tile = encode_csr(tile_membership, helper)
    cluster = encode_csr(cluster_membership.reshape(SLICES * tile_count, light_count), helper)
    light_bytes = light_count * 4 * 16
    for encoded in (tile, cluster):
        encoded["lightBytes"] = light_bytes
        encoded["residentBytes"] = encoded["metadataBytes"] + encoded["indexBytes"] + light_bytes
    return tile, cluster


def compare_runtime(encoded: dict[str, Any], runtime: dict[str, Any], stem: str) -> dict[str, bool]:
    integer_fields = (
        "logicalCells", "nonEmptyCells", "totalIndices", "maximumLightsPerCell",
        "metadataBytes", "indexBytes", "lightBytes", "residentBytes",
    )
    checks = {field: int(encoded[field]) == int(runtime[field]) for field in integer_fields}
    checks["averageLightsPerCell"] = abs(float(encoded["averageLightsPerCell"]) - float(runtime["averageLightsPerCell"])) <= 1.0e-9
    checks["csrSignature"] = encoded["csrSignature"].lower() == str(runtime["csrSignature"]).lower()
    failed = [name for name, passed in checks.items() if not passed]
    if failed:
        details = {name: (encoded.get(name), runtime.get(name)) for name in failed}
        raise RuntimeError(f"independent CSR mismatch {stem}: {details}")
    return checks


def image_quality(oracle_path: Path, candidate_path: Path) -> dict[str, Any]:
    oracle = np.asarray(Image.open(oracle_path).convert("RGB"), dtype=np.int16)
    candidate = np.asarray(Image.open(candidate_path).convert("RGB"), dtype=np.int16)
    difference = np.abs(candidate - oracle)
    mse = float(np.mean((candidate.astype(np.float64) - oracle.astype(np.float64)) ** 2))
    psnr = float("inf") if mse == 0.0 else 10.0 * math.log10((255.0 * 255.0) / mse)
    return {
        "maxChannelLsb": int(np.max(difference)),
        "meanChannelLsb": float(np.mean(difference)),
        "p99ChannelLsb": float(np.percentile(difference, 99)),
        "differentPixelCount": int(np.count_nonzero(np.any(difference != 0, axis=2))),
        "differentPixelRatio": float(np.mean(np.any(difference != 0, axis=2))),
        "psnrDb": psnr,
        "passedFrozenGate": bool(np.max(difference) <= 2 and np.mean(difference) <= 0.1 and np.percentile(difference, 99) <= 1.0),
        "difference": difference,
    }


def verify_full_image_misses(
    position_path: Path,
    validity_path: Path,
    result: dict[str, Any],
    tile: dict[str, Any],
    cluster: dict[str, Any],
) -> dict[str, Any]:
    position = np.asarray(read_pfm(position_path), dtype=np.float64)
    validity = read_pfm(validity_path) > 0.0
    height, width, _ = position.shape
    flat_valid = np.flatnonzero(validity.reshape(-1))
    world = position.reshape(-1, 3)[flat_valid]
    yy = flat_valid // width
    xx = flat_valid % width
    tiles_x = (width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (height + TILE_SIZE - 1) // TILE_SIZE
    tile_count = tiles_x * tiles_y
    tile_ids = (yy // TILE_SIZE) * tiles_x + (xx // TILE_SIZE)
    view = np.asarray(result["gBuffer"]["cameraMatrices"]["view"], dtype=np.float64)
    homogeneous = np.concatenate((world, np.ones((world.shape[0], 1))), axis=1)
    depths = -((view @ homogeneous.T).T[:, 2])
    normalized = np.log(np.clip(depths, NEAR, FAR) / NEAR) / math.log(FAR / NEAR)
    slice_ids = np.clip(np.floor(normalized * SLICES).astype(np.int32), 0, SLICES - 1)
    cluster_ids = slice_ids * tile_count + tile_ids
    lights = np.asarray([item["position"] for item in result["pointLightStress"]["lights"]], dtype=np.float64)
    radius_squared = float(result["pointLightStress"]["volumeRadius"]) ** 2
    truth = 0
    tile_misses = 0
    cluster_misses = 0
    tile_membership = tile["membership"]
    cluster_membership = cluster["membership"]
    for light_index, light in enumerate(lights):
        delta = world - light[None, :]
        inside = np.einsum("ij,ij->i", delta, delta) <= radius_squared
        truth += int(np.count_nonzero(inside))
        if np.any(inside):
            tile_misses += int(np.count_nonzero(~tile_membership[tile_ids[inside], light_index]))
            cluster_misses += int(np.count_nonzero(~cluster_membership[cluster_ids[inside], light_index]))
    if tile_misses or cluster_misses:
        raise RuntimeError(f"full-image membership miss: tile={tile_misses}, cluster={cluster_misses}")
    return {
        "validPixelCount": int(flat_valid.size),
        "invalidPixelCount": int(width * height - flat_valid.size),
        "groundTruthLightPixelInteractions": truth,
        "tileMisses": tile_misses,
        "clusterMisses": cluster_misses,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    project_dir = Path(__file__).resolve().parent.parent
    pre = load_json(run_dir / "pre-capture-manifest.json")
    manifest = load_json(run_dir / "capture-manifest.json")
    correctness = load_json(run_dir / "correctness-manifest.json")
    aggregate = load_json(run_dir / "aggregate.json")
    protocol_path = run_dir / pre["protocol"]
    protocol_hash = sha256_file(protocol_path)
    if protocol_hash != pre["protocolSha256"] or protocol_hash != manifest["protocolSha256"] or protocol_hash != correctness["protocolSha256"] or protocol_hash != aggregate["protocolSha256"]:
        raise ValueError("protocol hash chain mismatch")
    helper = build_fnv_helper(run_dir)

    expected_by_stem = {item["stem"]: item for item in pre["expectedRuns"]}
    representative_results: dict[tuple[int, float, str], dict[str, Any]] = {}
    for stem, expected in expected_by_stem.items():
        if expected["regime"] == "cached" and int(expected["round"]) == 1:
            result = load_json(run_dir / expected["result"])
            representative_results[(int(expected["lightCount"]), float(expected["radius"]), expected["renderMode"])] = result

    csr_results = []
    retained_extreme: tuple[dict[str, Any], dict[str, Any], dict[str, Any]] | None = None
    for count in COUNTS:
        for radius in RADII:
            tile_result = representative_results[(count, radius, "tile16")]
            cluster_result = representative_results[(count, radius, "cluster16")]
            tile, cluster = rebuild_pair(tile_result, helper)
            tile_checks = compare_runtime(tile, tile_result["pointLightStress"]["gridRuntime"], f"N{count}/R{radius}/tile16")
            cluster_checks = compare_runtime(cluster, cluster_result["pointLightStress"]["gridRuntime"], f"N{count}/R{radius}/cluster16")
            if tile_result["pointLightStress"]["submissionSignature"] != cluster_result["pointLightStress"]["submissionSignature"]:
                raise ValueError(f"submission signature mismatch N{count}/R{radius}")
            csr_results.append({
                "lightCount": count,
                "radius": radius,
                "submissionSignature": tile_result["pointLightStress"]["submissionSignature"],
                "tile": {key: value for key, value in tile.items() if key != "membership"},
                "cluster": {key: value for key, value in cluster.items() if key != "membership"},
                "tileChecks": tile_checks,
                "clusterChecks": cluster_checks,
            })
            if count == 512 and radius == 12.0:
                retained_extreme = (tile_result, tile, cluster)
            else:
                del tile, cluster

    if retained_extreme is None:
        raise AssertionError("missing retained extreme CSR")

    correctness_expected = {item["stem"]: item for item in correctness["expectedRuns"]}
    correctness_done = {item["stem"]: item for item in correctness["completedRuns"]}
    quality_results = []
    chart_dir = run_dir / "charts" / "correctness"
    chart_dir.mkdir(parents=True, exist_ok=True)
    for prefix in ("quality-n0512-r015", "quality-n0128-r100", "quality-n0512-r120"):
        paths = {}
        results = {}
        for mode in ("analytic-screen", "tile16", "cluster16"):
            stem = f"{prefix}-{mode}"
            expected = correctness_expected[stem]
            done = correctness_done[stem]
            result_path = run_dir / expected["result"]
            capture_path = run_dir / expected["capture"]
            if sha256_file(result_path) != done["resultSha256"] or sha256_file(capture_path) != done["captureSha256"]:
                raise ValueError(f"correctness artifact hash mismatch: {stem}")
            paths[mode] = capture_path
            results[mode] = load_json(result_path)
        if paths["tile16"].read_bytes() != paths["cluster16"].read_bytes():
            raise RuntimeError(f"quality Tile/Cluster image mismatch: {prefix}")
        quality = image_quality(paths["analytic-screen"], paths["tile16"])
        if not quality["passedFrozenGate"]:
            raise RuntimeError(f"Oracle quality gate failed: {prefix}: {quality}")
        difference = quality.pop("difference")
        amplified = np.clip(difference * 96, 0, 255).astype(np.uint8)
        Image.fromarray(amplified, mode="RGB").save(chart_dir / f"{prefix}-oracle-difference-x96.png")
        quality_results.append({"case": prefix, "tileClusterExact": True, **quality})

    edge_results = []
    for prefix in ("edge-n0000", "edge-n0001", "edge-n0016"):
        tile_stem = f"{prefix}-tile16"
        cluster_stem = f"{prefix}-cluster16"
        tile_expected = correctness_expected[tile_stem]
        cluster_expected = correctness_expected[cluster_stem]
        tile_path = run_dir / tile_expected["capture"]
        cluster_path = run_dir / cluster_expected["capture"]
        if tile_path.read_bytes() != cluster_path.read_bytes():
            raise RuntimeError(f"edge Tile/Cluster image mismatch: {prefix}")
        tile_result = load_json(run_dir / tile_expected["result"])
        cluster_result = load_json(run_dir / cluster_expected["result"])
        fixtures = tile_result["pointLightStress"]["fixtures"]
        if prefix == "edge-n0016":
            required = {
                "nearPlaneIntersectionVerified": True,
                "cameraInsideVerified": True,
                "fullyOffscreenVerified": True,
                "depthSliceBoundaryPlaced": True,
            }
            if any(bool(fixtures[name]) != value for name, value in required.items()):
                raise RuntimeError(f"edge fixture failed: {fixtures}")
        edge_results.append({
            "case": prefix,
            "tileClusterExact": True,
            "tileCsr": tile_result["pointLightStress"]["gridRuntime"]["csrSignature"],
            "clusterCsr": cluster_result["pointLightStress"]["gridRuntime"]["csrSignature"],
            "fixtures": fixtures,
        })

    extreme_record = correctness_done["quality-n0512-r120-tile16"]
    full_image = verify_full_image_misses(
        run_dir / extreme_record["gbufferPosition"],
        run_dir / extreme_record["gbufferValidity"],
        retained_extreme[0], retained_extreme[1], retained_extreme[2],
    )

    shader_path = project_dir / "shaders" / "pointLightGridFragment.glsl"
    shader = shader_path.read_text(encoding="utf-8")
    position_guard = shader.index("if (!LoadWorldPosition(texCoords, fragPos)) discard;")
    metadata_fetch = shader.index("texelFetch(gridMetadata")
    normal_fetch = shader.index("texture(gNormal")
    exact_predicate = shader.index("if (distanceSquared > positionRadius.w) continue;")
    shader_checks = {
        "invalidPositionDiscardBeforeGridFetch": position_guard < metadata_fetch,
        "invalidPositionDiscardBeforeGBufferReads": position_guard < normal_fetch,
        "exactSpherePredicatePresent": exact_predicate > metadata_fetch,
        "sameShaderForTileAndCluster": "if (gridMode == 1)" in shader,
    }
    global_header = (project_dir / "Global.h").read_text(encoding="utf-8")
    default_unchanged = bool(re.search(r"POINT_LIGHT_RENDER_MODE\s*=\s*PointLightRenderProperty::AnalyticScreen", global_header))
    if not all(shader_checks.values()) or not default_unchanged:
        raise RuntimeError(f"shader/default contract failed: {shader_checks}, default={default_unchanged}")

    verification = {
        "schemaVersion": 1,
        "passed": True,
        "method": "independent NumPy float32 side-plane/log-Z CSR rebuild + independently compiled streaming FNV-1a helper + full G-buffer pixel truth + exact/tolerant image gates",
        "protocolSha256": protocol_hash,
        "aggregateSha256": sha256_file(run_dir / "aggregate.json"),
        "captureManifestSha256": sha256_file(run_dir / "capture-manifest.json"),
        "correctnessManifestSha256": sha256_file(run_dir / "correctness-manifest.json"),
        "fnvHelperSourceSha256": sha256_file(Path(__file__).with_name("fnv1a64_stdin.cpp")),
        "fnvHelperExecutableSha256": sha256_file(helper),
        "csrCellsVerified": len(csr_results) * 2,
        "csr": csr_results,
        "fullImageMembership": full_image,
        "quality": {
            "frozenGate": {"maxChannelLsb": 2, "meanChannelLsb": 0.1, "p99ChannelLsb": 1.0},
            "cases": quality_results,
        },
        "edgeCases": edge_results,
        "shaderContract": shader_checks,
        "defaultRenderModeRemainsAnalyticScreen": default_unchanged,
    }
    output_path = run_dir / "verification" / "independent-verification.json"
    dump_json(output_path, verification)
    print(
        f"[independent-verification] PASS csr={verification['csrCellsVerified']} "
        f"truth={full_image['groundTruthLightPixelInteractions']} misses=0 "
        f"quality={len(quality_results)} edge={len(edge_results)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
