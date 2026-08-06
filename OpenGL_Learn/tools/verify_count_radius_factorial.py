#!/usr/bin/env python3
"""Independent verifier for the count x radius factorial experiment.

This intentionally does not import the primary analyzer. It rebuilds cell
assignment/membership, reads persisted CSR, and recomputes the main seed's full
pixel/light truth plus deterministic samples for both repeat seeds.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from datetime import datetime
from pathlib import Path
from typing import Any

import numpy as np


COUNTS = (32, 64, 128, 256, 512)
RADII = (1.5, 3.0, 6.0, 12.0)
SEEDS = (0x21D3F3A5, 0xA511E9B3, 0xC0FFEE11)
PRIMARY_SEED = SEEDS[0]
TILE_SIZE = 16
SLICES = 16
EPSILON = 1.0e-6


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
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        magic = stream.readline().strip()
        if magic not in (b"PF", b"Pf"):
            raise ValueError(f"invalid PFM: {path}")
        width, height = (int(value) for value in stream.readline().split())
        scale = float(stream.readline())
        dtype = "<f4" if scale < 0 else ">f4"
        channels = 3 if magic == b"PF" else 1
        values = np.fromfile(stream, dtype=dtype)
    expected = width * height * channels
    if values.size != expected:
        raise ValueError(f"PFM size mismatch: {path}")
    shape = (height, width, channels) if channels == 3 else (height, width)
    values = values.reshape(shape)
    if not np.all(np.isfinite(values)):
        raise ValueError(f"non-finite PFM: {path}")
    return values


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def build_layout(
    width: int,
    height: int,
    projection: np.ndarray,
    valid_flat: np.ndarray,
    view_depth: np.ndarray,
    near_plane: float,
    far_plane: float,
) -> dict[str, Any]:
    tiles_x = (width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (height + TILE_SIZE - 1) // TILE_SIZE
    tile_count = tiles_x * tiles_y
    normals = np.empty((tile_count, 4, 3), dtype=np.float64)
    for ty in range(tiles_y):
        y0, y1 = ty * TILE_SIZE, min(height, (ty + 1) * TILE_SIZE)
        c = (2.0 * y0 / height - 1.0) / projection[1, 1]
        d = (2.0 * y1 / height - 1.0) / projection[1, 1]
        for tx in range(tiles_x):
            x0, x1 = tx * TILE_SIZE, min(width, (tx + 1) * TILE_SIZE)
            a = (2.0 * x0 / width - 1.0) / projection[0, 0]
            b = (2.0 * x1 / width - 1.0) / projection[0, 0]
            raw = np.asarray(([1, 0, a], [-1, 0, -b], [0, 1, c], [0, -1, -d]), dtype=np.float64)
            normals[ty * tiles_x + tx] = raw / np.linalg.norm(raw, axis=1, keepdims=True)
    x = valid_flat % width
    y = valid_flat // width
    tile_ids = (y // TILE_SIZE) * tiles_x + (x // TILE_SIZE)
    edges = np.geomspace(near_plane, far_plane, SLICES + 1)
    slice_ids = np.searchsorted(edges, view_depth, side="right") - 1
    slice_ids = np.clip(slice_ids, 0, SLICES - 1)
    cluster_ids = slice_ids * tile_count + tile_ids
    return {
        "tilesX": tiles_x,
        "tilesY": tiles_y,
        "tileCount": tile_count,
        "normals": normals,
        "tileIds": tile_ids.astype(np.int32),
        "clusterIds": cluster_ids.astype(np.int32),
        "edges": edges,
    }


def build_membership(
    light_view: np.ndarray,
    radius: float,
    near_plane: float,
    far_plane: float,
    layout: dict[str, Any],
) -> tuple[np.ndarray, np.ndarray]:
    guarded = radius + EPSILON
    tile = np.zeros((layout["tileCount"], light_view.shape[0]), dtype=bool)
    for light_index, center in enumerate(light_view):
        depth = -float(center[2])
        if depth + guarded < near_plane or depth - guarded > far_plane:
            continue
        signed = np.einsum("tpc,c->tp", layout["normals"], center)
        tile[:, light_index] = np.all(signed >= -guarded, axis=1)
    depth = -light_view[:, 2]
    edges = layout["edges"]
    z_member = (
        (depth[None, :] + guarded >= edges[:-1, None])
        & (depth[None, :] - guarded <= edges[1:, None])
    )
    cluster = (z_member[:, None, :] & tile[None, :, :]).reshape(SLICES * layout["tileCount"], light_view.shape[0])
    return tile, cluster


def csr_to_membership(offsets: np.ndarray, counts: np.ndarray, indices: np.ndarray, light_count: int) -> np.ndarray:
    membership = np.zeros((counts.size, light_count), dtype=bool)
    for row in np.flatnonzero(counts):
        begin = int(offsets[row])
        end = begin + int(counts[row])
        membership[row, indices[begin:end]] = True
    return membership


def membership_indices(membership: np.ndarray) -> np.ndarray:
    total = int(np.sum(membership, dtype=np.uint64))
    result = np.empty(total, dtype=np.uint32)
    cursor = 0
    for row_start in range(0, membership.shape[0], 4096):
        columns = np.nonzero(membership[row_start : row_start + 4096])[1].astype(np.uint32, copy=False)
        result[cursor : cursor + columns.size] = columns
        cursor += int(columns.size)
    if cursor != total:
        raise RuntimeError("membership index fill mismatch")
    return result


def verify_artifact_manifest(run_dir: Path) -> dict[str, Any]:
    manifest_path = run_dir / "artifact-manifest.json"
    manifest = load_json(manifest_path)
    mismatches = []
    total = 0
    # PowerShell opens/truncates this verifier's own redirected log before the
    # verifier can inspect the previous manifest. Treat only that self-mutating
    # diagnostic log as unverifiable during startup; the final manifest is
    # rebuilt after the verifier's last flushed line and is checked externally.
    self_mutating_logs = {"logs/independent-verification.log"}
    for record in manifest["files"]:
        if record["path"] in self_mutating_logs:
            total += int(record["bytes"])
            continue
        path = run_dir / record["path"]
        if not path.is_file():
            mismatches.append({"path": record["path"], "reason": "missing"})
            continue
        size = path.stat().st_size
        digest = sha256_file(path)
        total += size
        if size != int(record["bytes"]) or digest != record["sha256"]:
            mismatches.append({"path": record["path"], "size": size, "sha256": digest})
    if mismatches or len(manifest["files"]) != int(manifest["fileCount"]) or total != int(manifest["totalBytes"]):
        raise ValueError(f"artifact manifest mismatch: {mismatches[:3]}")
    return {
        "passed": True,
        "fileCount": len(manifest["files"]),
        "totalBytes": total,
        "startupSelfMutatingLogExclusions": sorted(self_mutating_logs),
    }


def rebuild_artifact_manifest(run_dir: Path) -> None:
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


def main_verify(run_dir: Path) -> dict[str, Any]:
    protocol = run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"
    capture_manifest_path = run_dir / "capture-manifest.json"
    aggregate_path = run_dir / "aggregate.json"
    capture_manifest = load_json(capture_manifest_path)
    aggregate = load_json(aggregate_path)
    protocol_hash = sha256_file(protocol)
    if capture_manifest["protocolSha256"] != protocol_hash or aggregate["protocolSha256"] != protocol_hash:
        raise ValueError("protocol SHA mismatch")
    protocol_time = protocol.stat().st_mtime
    starts = [datetime.fromisoformat(case["startUtc"].replace("Z", "+00:00")).timestamp() for case in capture_manifest["cases"] if case.get("startUtc")]
    if starts and protocol_time >= min(starts):
        raise ValueError("protocol does not predate formal capture")
    executable = Path(capture_manifest["executable"])
    if sha256_file(executable) != capture_manifest["executableSha256"]:
        raise ValueError("current executable differs from formal executable")

    position = np.asarray(read_pfm(Path(capture_manifest["sharedCaptures"]["position"])), dtype=np.float64)
    validity = np.asarray(read_pfm(Path(capture_manifest["sharedCaptures"]["validity"])) > 0.0, dtype=bool)
    valid_flat = np.flatnonzero(validity.reshape(-1))
    valid_positions = position.reshape(-1, 3)[valid_flat]
    primary_capture = next(case for case in capture_manifest["cases"] if int(case["seed"]) == PRIMARY_SEED and int(case["lightCount"]) == 512 and float(case["requestedRadius"]) == 1.5)
    primary_scene = load_json(Path(primary_capture["result"]))
    matrices = primary_scene["gBuffer"]["cameraMatrices"]
    view = np.asarray(matrices["view"], dtype=np.float64)
    projection = np.asarray(matrices["projection"], dtype=np.float64)
    valid_h = np.concatenate((valid_positions, np.ones((valid_positions.shape[0], 1))), axis=1)
    view_depth = -(view @ valid_h.T).T[:, 2]
    layout = build_layout(1920, 1080, projection, valid_flat, view_depth, float(matrices["nearPlane"]), float(matrices["farPlane"]))
    aggregate_cases = {(int(case["seed"]), int(case["lightCount"]), float(case["requestedRadius"])): case for case in aggregate["cases"]}
    capture_cases = {(int(case["seed"]), int(case["lightCount"]), float(case["requestedRadius"])): case for case in capture_manifest["cases"]}

    effective_by_radius: dict[float, float] = {}
    for requested in RADII:
        values = []
        for seed in SEEDS:
            for count in COUNTS:
                point = load_json(Path(capture_cases[(seed, count, requested)]["result"]))["pointLightStress"]
                values.append((
                    float(point["volumeRadius"]), float(point["constant"]),
                    float(point["linear"]), float(point["quadratic"]),
                    float(point["attenuationThreshold"]),
                ))
        if any(value != values[0] for value in values):
            raise ValueError(f"effective radius/attenuation invariant failed for R={requested}")
        if abs(values[0][0] - requested) > 1.0e-4:
            raise ValueError(f"effective radius tolerance failed for R={requested}")
        effective_by_radius[requested] = values[0][0]

    main_master = load_json(Path(capture_cases[(PRIMARY_SEED, 512, 1.5)]["result"]))
    light_positions = np.asarray([light["position"] for light in main_master["pointLightStress"]["lights"]], dtype=np.float64)
    light_h = np.concatenate((light_positions, np.ones((512, 1))), axis=1)
    light_view = (view @ light_h.T).T[:, :3]
    light_norm = np.einsum("ij,ij->i", light_positions, light_positions)
    main_records = []
    membership_rebuild_records = []
    full_truth_counts = {(radius, count): np.empty(valid_positions.shape[0], dtype=np.uint16) for radius in RADII for count in COUNTS}
    misses = {(radius, scheme): np.zeros(512, dtype=np.uint64) for radius in RADII for scheme in ("tile", "cluster")}
    memberships: dict[float, tuple[np.ndarray, np.ndarray]] = {}
    for radius in RADII:
        tile_path = run_dir / "csr" / f"s0-n0512-r{int(radius*10):03d}-tile.npz"
        cluster_path = run_dir / "csr" / f"s0-n0512-r{int(radius*10):03d}-cluster-16.npz"
        with np.load(tile_path) as tile_npz, np.load(cluster_path) as cluster_npz:
            tile_offsets, tile_counts, tile_indices = tile_npz["offsets"], tile_npz["counts"], tile_npz["indices"]
            cluster_offsets, cluster_counts, cluster_indices = cluster_npz["offsets"], cluster_npz["counts"], cluster_npz["indices"]
            if sha256_csr(tile_offsets, tile_counts, tile_indices) != aggregate_cases[(PRIMARY_SEED, 512, radius)]["tile"]["csrSha256"]:
                raise ValueError(f"Tile CSR hash mismatch R={radius}")
            if sha256_csr(cluster_offsets, cluster_counts, cluster_indices) != aggregate_cases[(PRIMARY_SEED, 512, radius)]["cluster16"]["csrSha256"]:
                raise ValueError(f"Cluster CSR hash mismatch R={radius}")
            persisted_tile = csr_to_membership(
                tile_offsets, tile_counts, tile_indices, 512
            )
            persisted_cluster = csr_to_membership(
                cluster_offsets, cluster_counts, cluster_indices, 512
            )
            rebuilt_tile, rebuilt_cluster = build_membership(
                light_view,
                effective_by_radius[radius],
                float(matrices["nearPlane"]),
                float(matrices["farPlane"]),
                layout,
            )
            if not np.array_equal(persisted_tile, rebuilt_tile):
                raise ValueError(f"independent Tile membership rebuild mismatch R={radius}")
            if not np.array_equal(persisted_cluster, rebuilt_cluster):
                raise ValueError(f"independent Cluster membership rebuild mismatch R={radius}")
            memberships[radius] = (persisted_tile, persisted_cluster)
            membership_rebuild_records.append({
                "radius": radius,
                "tileCellCount": int(persisted_tile.shape[0]),
                "clusterCellCount": int(persisted_cluster.shape[0]),
                "lightCount": 512,
                "matched": True,
            })
    block_size = 8192
    for begin in range(0, valid_positions.shape[0], block_size):
        end = min(valid_positions.shape[0], begin + block_size)
        points = valid_positions[begin:end]
        point_norm = np.einsum("ij,ij->i", points, points)
        distances = point_norm[:, None] + light_norm[None, :] - 2.0 * (points @ light_positions.T)
        np.maximum(distances, 0.0, out=distances)
        for radius in RADII:
            effective = effective_by_radius[radius]
            inside = distances <= effective * effective
            cumulative = np.cumsum(inside, axis=1, dtype=np.uint16)
            for count in COUNTS:
                full_truth_counts[(radius, count)][begin:end] = cumulative[:, count - 1]
            tile_member, cluster_member = memberships[radius]
            misses[(radius, "tile")] += np.count_nonzero(
                inside & ~tile_member[layout["tileIds"][begin:end]], axis=0
            ).astype(np.uint64, copy=False)
            misses[(radius, "cluster")] += np.count_nonzero(
                inside & ~cluster_member[layout["clusterIds"][begin:end]], axis=0
            ).astype(np.uint64, copy=False)

    for radius in RADII:
        full_tile, full_cluster = memberships[radius]
        tile_miss_prefix = np.cumsum(misses[(radius, "tile")], dtype=np.uint64)
        cluster_miss_prefix = np.cumsum(misses[(radius, "cluster")], dtype=np.uint64)
        for count in COUNTS:
            key = (PRIMARY_SEED, count, radius)
            case = aggregate_cases[key]
            truth = full_truth_counts[(radius, count)]
            truth_total = int(np.sum(truth, dtype=np.uint64))
            if truth_total != int(case["groundTruthInteractions"]):
                raise ValueError(f"Ground Truth mismatch: {key}")
            if int(tile_miss_prefix[count - 1]) != 0 or int(cluster_miss_prefix[count - 1]) != 0:
                raise ValueError(f"independent full miss detected: {key}")
            for scheme, full_membership, cell_ids, case_name in (
                ("tile", full_tile, layout["tileIds"], "tile"),
                ("cluster-16", full_cluster, layout["clusterIds"], "cluster16"),
            ):
                path = run_dir / "csr" / f"s0-n{count:04d}-r{int(radius*10):03d}-{scheme}.npz"
                with np.load(path) as data:
                    offsets, counts_array, indices = data["offsets"], data["counts"], data["indices"]
                    digest = sha256_csr(offsets, counts_array, indices)
                    if digest != case[case_name]["csrSha256"]:
                        raise ValueError(f"CSR digest mismatch: {path}")
                    expected_counts = np.sum(full_membership[:, :count], axis=1, dtype=np.uint32)
                    expected_indices = membership_indices(full_membership[:, :count])
                    if not np.array_equal(counts_array, expected_counts) or not np.array_equal(indices, expected_indices):
                        raise ValueError(f"prefix CSR content mismatch: {path}")
                    candidate = int(np.sum(counts_array[cell_ids], dtype=np.uint64))
                    if candidate != int(case[case_name]["candidateInteractions"]):
                        raise ValueError(f"candidate interaction mismatch: {path}")
            pixel_path = run_dir / "pixel-counts" / f"s0-n{count:04d}-r{int(radius*10):03d}.npz"
            with np.load(pixel_path) as data:
                if not np.array_equal(data["ground_truth"], truth):
                    raise ValueError(f"persisted truth-count mismatch: {pixel_path}")
            main_records.append({"lightCount": count, "radius": radius, "groundTruth": truth_total, "tileMiss": 0, "clusterMiss": 0})

    repeat_records = []
    sample_ordinals = np.linspace(0, valid_positions.shape[0] - 1, 65536, dtype=np.int64)
    sample_positions = valid_positions[sample_ordinals]
    sample_tile_ids = layout["tileIds"][sample_ordinals]
    sample_cluster_ids = layout["clusterIds"][sample_ordinals]
    for seed in SEEDS[1:]:
        scene = load_json(Path(capture_cases[(seed, 512, 1.5)]["result"]))
        positions = np.asarray([light["position"] for light in scene["pointLightStress"]["lights"]], dtype=np.float64)
        light_h = np.concatenate((positions, np.ones((512, 1))), axis=1)
        view_positions = (view @ light_h.T).T[:, :3]
        sample_norm = np.einsum("ij,ij->i", sample_positions, sample_positions)
        light_norm = np.einsum("ij,ij->i", positions, positions)
        distances = sample_norm[:, None] + light_norm[None, :] - 2.0 * (sample_positions @ positions.T)
        np.maximum(distances, 0.0, out=distances)
        for radius in RADII:
            effective = effective_by_radius[radius]
            tile_member, cluster_member = build_membership(
                view_positions, effective, float(matrices["nearPlane"]), float(matrices["farPlane"]), layout
            )
            inside = distances <= effective * effective
            for count in COUNTS:
                tile_miss = int(np.count_nonzero(inside[:, :count] & ~tile_member[sample_tile_ids, :count]))
                cluster_miss = int(np.count_nonzero(inside[:, :count] & ~cluster_member[sample_cluster_ids, :count]))
                if tile_miss or cluster_miss:
                    raise ValueError(f"repeat-seed sampled miss: seed={seed:#x}, N={count}, R={radius}")
                repeat_records.append({"seed": seed, "lightCount": count, "radius": radius, "samplePixels": 65536, "tileMiss": 0, "clusterMiss": 0})

    timing_verification = {"present": False}
    timing_manifest_path = run_dir / "timing-manifest.json"
    if timing_manifest_path.is_file():
        timing_manifest = load_json(timing_manifest_path)
        if len(timing_manifest["runs"]) != 60 or timing_manifest["executableSha256"] != capture_manifest["executableSha256"]:
            raise ValueError("timing manifest mismatch")
        failures = []
        for run in timing_manifest["runs"]:
            result = load_json(Path(run["result"]))
            if (
                not result.get("success")
                or int(result["profiler"]["summary"]["cpuFrame"]["count"]) != 600
                or int(result["profiler"]["summary"]["gpuFrame"]["count"]) != 600
                or int(result["profiler"]["summary"]["gpuZones"]["Deferred Point Lights"]["count"]) != 600
            ):
                failures.append(run["stem"])
        if failures:
            raise ValueError(f"timing failures: {failures}")
        timing_verification = {"present": True, "runCount": 60, "failures": 0, "samplesPerRun": 600}

    error_pattern = re.compile(r"GL_INVALID|GL_OUT_OF_MEMORY|failed with error 0x|OpenGL error", re.IGNORECASE)
    bad_logs = []
    for log in sorted((run_dir / "logs").rglob("*.log")):
        text = log.read_text(encoding="utf-8", errors="replace")
        if error_pattern.search(text):
            bad_logs.append(str(log.relative_to(run_dir)))
    if bad_logs:
        raise ValueError(f"GL error text found in logs: {bad_logs[:5]}")

    artifact = verify_artifact_manifest(run_dir)
    return {
        "schemaVersion": 1,
        "passed": True,
        "verifierProvenance": {
            "captureRecordedVerifierSha256": capture_manifest["sourceHashes"]["independentVerifier"],
            "executedVerifierSha256": sha256_file(Path(__file__).resolve()),
            "postCaptureVerifierFix": (
                capture_manifest["sourceHashes"]["independentVerifier"]
                != sha256_file(Path(__file__).resolve())
            ),
            "captureInputsUnchanged": True,
        },
        "protocol": {"sha256": protocol_hash, "predatesCapture": True},
        "executable": {"sha256": capture_manifest["executableSha256"], "currentHashMatches": True},
        "independentMembershipRebuild": {
            "caseCount": len(membership_rebuild_records),
            "records": membership_rebuild_records,
        },
        "mainSeedFullVerification": {"caseCount": len(main_records), "records": main_records},
        "repeatSeedSampleVerification": {"caseCount": len(repeat_records), "samplePixelsPerCase": 65536, "records": repeat_records},
        "timing": timing_verification,
        "glLogs": {"checked": True, "errorLogCount": 0},
        "artifactManifestBeforeVerificationWrite": artifact,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    result = main_verify(run_dir)
    output = run_dir / "verification" / "independent-verification.json"
    json_dump(output, result)
    print(
        f"[verified] main={result['mainSeedFullVerification']['caseCount']} "
        f"repeat={result['repeatSeedSampleVerification']['caseCount']} timing={result['timing']['present']}",
        flush=True,
    )
    rebuild_artifact_manifest(run_dir)
    verify_artifact_manifest(run_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
