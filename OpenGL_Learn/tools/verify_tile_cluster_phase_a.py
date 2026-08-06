#!/usr/bin/env python3
"""Independent PFM + persisted CSR verifier for Tile/Cluster Phase A."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image


TILE_SIZE = 16
SCHEMES = (("tile", 0), ("cluster-08", 8), ("cluster-16", 16), ("cluster-24", 24), ("cluster-32", 32))


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


def json_dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def load_dense_membership(csr_path: Path, expected_cells: int, light_count: int) -> tuple[np.ndarray, dict[str, Any]]:
    with np.load(csr_path, allow_pickle=False) as data:
        offsets = np.asarray(data["offsets"], dtype=np.uint32)
        counts = np.asarray(data["counts"], dtype=np.uint32)
        indices = np.asarray(data["indices"], dtype=np.uint32)
        stored_slices = int(data["slices"][0])
    if counts.size != expected_cells or offsets.size != expected_cells + 1:
        raise ValueError(f"CSR cell shape mismatch: {csr_path}")
    if int(offsets[0]) != 0 or int(offsets[-1]) != indices.size:
        raise ValueError(f"CSR offsets mismatch: {csr_path}")
    expected_offsets = np.empty(expected_cells + 1, dtype=np.uint64)
    expected_offsets[0] = 0
    np.cumsum(counts, dtype=np.uint64, out=expected_offsets[1:])
    if not np.array_equal(offsets.astype(np.uint64), expected_offsets):
        raise ValueError(f"CSR prefix mismatch: {csr_path}")
    if indices.size and int(np.max(indices)) >= light_count:
        raise ValueError(f"CSR light index out of range: {csr_path}")
    membership = np.zeros((expected_cells, light_count), dtype=bool)
    sorted_unique = True
    for cell in range(expected_cells):
        begin, end = int(offsets[cell]), int(offsets[cell + 1])
        cell_indices = indices[begin:end]
        if cell_indices.size > 1 and np.any(cell_indices[1:] <= cell_indices[:-1]):
            sorted_unique = False
            break
        membership[cell, cell_indices] = True
    if not sorted_unique:
        raise ValueError(f"CSR indices are not strictly ascending: {csr_path}")
    return membership, {
        "csrSha256": sha256_csr(offsets, counts, indices),
        "storedSlices": stored_slices,
        "cellCount": expected_cells,
        "totalIndexReferences": int(indices.size),
        "metadataBytes": int(expected_cells * 8),
        "indexBytes": int(indices.size * 4),
        "counts": counts,
    }


def verify_case(run_dir: Path, case: dict[str, Any], aggregate_case: dict[str, Any]) -> dict[str, Any]:
    result_path = Path(case["result"])
    position_path = Path(case["captures"]["position"])
    validity_path = Path(case["captures"]["validity"])
    app_path = Path(case["ldr"])
    result = json.loads(result_path.read_text(encoding="utf-8-sig"))
    position = np.asarray(read_pfm(position_path), dtype=np.float64)
    validity = read_pfm(validity_path) > 0.0
    height, width, _ = position.shape
    if (width, height) != (1920, 1080) or validity.shape != (height, width):
        raise ValueError(f"capture dimensions invalid: {case['stem']}")
    if Image.open(app_path).size != (width, height):
        raise ValueError(f"renderer screenshot invalid: {app_path}")
    point = result["pointLightStress"]
    matrices = result["gBuffer"]["cameraMatrices"]
    view = np.asarray(matrices["view"], dtype=np.float64)
    near_plane = float(matrices["nearPlane"])
    far_plane = float(matrices["farPlane"])
    lights = np.asarray([item["position"] for item in point["lights"]], dtype=np.float64)
    radius = float(point["volumeRadius"])
    radius_squared = radius * radius
    light_count = lights.shape[0]
    tiles_x = (width + TILE_SIZE - 1) // TILE_SIZE
    tiles_y = (height + TILE_SIZE - 1) // TILE_SIZE
    tile_count = tiles_x * tiles_y

    flat_valid = np.flatnonzero(validity.reshape(-1))
    positions = position.reshape(-1, 3)[flat_valid]
    yy = flat_valid // width
    xx = flat_valid % width
    tile_ids = (yy // TILE_SIZE) * tiles_x + (xx // TILE_SIZE)
    homogeneous = np.concatenate((positions, np.ones((positions.shape[0], 1))), axis=1)
    depths = -((view @ homogeneous.T).T[:, 2])
    if np.any(~np.isfinite(depths)) or np.any(depths < near_plane - 1.0e-3) or np.any(
        depths > far_plane + 1.0e-2
    ):
        raise ValueError(f"valid G-buffer depth outside near/far: {case['stem']}")

    memberships: dict[str, np.ndarray] = {}
    scheme_info: dict[str, dict[str, Any]] = {}
    cell_ids: dict[str, np.ndarray] = {}
    for name, slices in SCHEMES:
        cells = tile_count if slices == 0 else tile_count * slices
        csr_path = run_dir / "csr" / f"{case['stem']}-{name}.npz"
        membership, info = load_dense_membership(csr_path, cells, light_count)
        if info["storedSlices"] != slices:
            raise ValueError(f"stored slice mismatch: {csr_path}")
        if slices == 0:
            ids = tile_ids.astype(np.int32)
        else:
            edges = np.geomspace(near_plane, far_plane, slices + 1)
            slice_ids = np.searchsorted(edges, depths, side="right") - 1
            slice_ids = np.clip(slice_ids, 0, slices - 1)
            ids = (slice_ids * tile_count + tile_ids).astype(np.int32)
        memberships[name] = membership
        scheme_info[name] = info
        cell_ids[name] = ids

    truth = 0
    misses = {name: 0 for name, _ in SCHEMES}
    for light_index, light_position in enumerate(lights):
        delta = positions - light_position[None, :]
        inside = np.einsum("ij,ij->i", delta, delta) <= radius_squared
        actual = int(np.count_nonzero(inside))
        truth += actual
        for name, _ in SCHEMES:
            if actual:
                misses[name] += int(
                    np.count_nonzero(~memberships[name][cell_ids[name][inside], light_index])
                )

    verified_schemes: dict[str, Any] = {}
    for name, _ in SCHEMES:
        info = scheme_info[name]
        ids = cell_ids[name]
        counts = info.pop("counts")
        candidate = int(np.sum(counts[ids], dtype=np.uint64))
        expected = aggregate_case["schemes"][name]
        checks = {
            "csrHash": info["csrSha256"] == expected["csrSha256"],
            "indexReferences": info["totalIndexReferences"] == expected["totalIndexReferences"],
            "candidateInteractions": candidate == expected["candidateInteractions"],
            "groundTruthInteractions": truth == expected["groundTruthInteractions"],
            "missInteractions": misses[name] == expected["missInteractions"],
            "memory": info["metadataBytes"] + info["indexBytes"] == expected["totalLogicalBytes"],
        }
        if not all(checks.values()):
            raise RuntimeError(f"independent cross-check failed: {case['stem']}/{name}: {checks}")
        verified_schemes[name] = {
            **info,
            "candidateInteractions": candidate,
            "groundTruthInteractions": truth,
            "missInteractions": misses[name],
            "checks": checks,
        }
    return {
        "stem": case["stem"],
        "resolution": [width, height],
        "validPixelCount": int(flat_valid.size),
        "invalidPixelCount": int(width * height - flat_valid.size),
        "sceneSignature": point["sceneSignature"],
        "submissionSignature": point["submissionSignature"],
        "schemes": verified_schemes,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    manifest_path = run_dir / "capture-manifest.json"
    aggregate_path = run_dir / "aggregate.json"
    protocol_path = run_dir / "PHASE0_FROZEN_PROTOCOL_CN.md"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    aggregate = json.loads(aggregate_path.read_text(encoding="utf-8-sig"))
    protocol_hash = sha256_file(protocol_path)
    if protocol_hash != manifest["protocolSha256"] or protocol_hash != aggregate["protocolSha256"]:
        raise ValueError("protocol hash chain mismatch")
    aggregate_cases = {case["stem"]: case for case in aggregate["cases"]}
    results = [
        verify_case(run_dir, case, aggregate_cases[case["stem"]]) for case in manifest["cases"]
    ]
    with (run_dir / "summary.csv").open("r", encoding="utf-8-sig", newline="") as stream:
        summary_rows = list(csv.DictReader(stream))
    if len(summary_rows) != 20:
        raise ValueError(f"summary row count is {len(summary_rows)}, expected 20")
    output = {
        "schemaVersion": 1,
        "passed": True,
        "method": "independent PFM sphere ground truth + persisted CSR decode",
        "protocolSha256": protocol_hash,
        "aggregateSha256": sha256_file(aggregate_path),
        "captureManifestSha256": sha256_file(manifest_path),
        "caseCount": len(results),
        "schemeCount": sum(len(result["schemes"]) for result in results),
        "summaryRowsParsed": len(summary_rows),
        "totalGroundTruthInteractions": int(sum(result["schemes"]["tile"]["groundTruthInteractions"] for result in results)),
        "totalMissInteractions": int(sum(scheme["missInteractions"] for result in results for scheme in result["schemes"].values())),
        "cases": results,
    }
    output_path = run_dir / "verification" / "independent-verification.json"
    json_dump(output_path, output)
    print(
        f"[independent-verification] PASS cases={output['caseCount']} schemes={output['schemeCount']} misses={output['totalMissInteractions']}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
