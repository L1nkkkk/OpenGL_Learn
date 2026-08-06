"""Verify real invalid G-buffer pixels on a wide-view grid S1/S8 smoke."""

import argparse
import hashlib
import json
import pathlib
import re

import numpy as np
from PIL import Image


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def read_pfm(path):
    with path.open("rb") as stream:
        kind = stream.readline().strip()
        if kind not in (b"Pf", b"PF"):
            raise ValueError("invalid PFM header: {}".format(path))
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        channels = 1 if kind == b"Pf" else 3
        dtype = "<f4" if scale < 0 else ">f4"
        data = np.fromfile(stream, dtype=dtype)
    expected = width * height * channels
    if data.size != expected:
        raise ValueError("PFM size mismatch: {}".format(path))
    return np.flipud(data.reshape(height, width, channels))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    records = []
    for slices in (1, 8):
        stem = "wide-s{:02d}".format(slices)
        result_path = root / (stem + ".json")
        capture_path = root / (stem + ".ppm")
        position_path = root / (stem + "-position.pfm")
        validity_path = root / (stem + "-validity.pfm")
        log_path = root / (stem + ".log")
        result = load(result_path)
        point = result["pointLightStress"]
        grid = point["gridRuntime"]
        validity = read_pfm(validity_path)[:, :, 0]
        position = read_pfm(position_path)
        invalid = int(np.count_nonzero(validity <= 0.0))
        valid = int(np.count_nonzero(validity > 0.0))
        width, height = map(int, result["resolution"])
        image = Image.open(capture_path)
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
        if not bool(result["success"]):
            raise RuntimeError("{} result failed".format(stem))
        if int(grid["sliceCount"]) != slices or not bool(grid["valid"]) or bool(grid["overflow"]) or str(grid["error"]):
            raise RuntimeError("{} grid validation failed".format(stem))
        if not bool(point["gridSliceCountExplicit"]):
            raise RuntimeError("{} slice selection was not explicit".format(stem))
        if image.size != (width, height) or validity.shape != (height, width) or position.shape != (height, width, 3):
            raise RuntimeError("{} capture dimensions differ from the actual framebuffer".format(stem))
        if invalid <= 0 or valid <= 0:
            raise RuntimeError("{} must contain both valid and invalid G-buffer pixels".format(stem))
        if re.search(r"GL_INVALID|GL error|shader compilation failed|shader linking failed|failed to load shader", log_text):
            raise RuntimeError("{} logged a GL/shader failure".format(stem))
        records.append({
            "sliceCount": slices,
            "actualResolution": [width, height],
            "validPixelCount": valid,
            "invalidPixelCount": invalid,
            "invalidPixelRatio": invalid / float(width * height),
            "captureSha256": sha256(capture_path),
            "positionSha256": sha256(position_path),
            "validitySha256": sha256(validity_path),
            "csrSignature": str(grid["csrSignature"]),
        })
    if records[0]["actualResolution"] != records[1]["actualResolution"]:
        raise RuntimeError("S1 and S8 actual resolutions differ")
    if records[0]["captureSha256"] != records[1]["captureSha256"]:
        raise RuntimeError("S1 and S8 output images differ with invalid pixels present")
    if records[0]["positionSha256"] != records[1]["positionSha256"] or records[0]["validitySha256"] != records[1]["validitySha256"]:
        raise RuntimeError("S1 and S8 G-buffer captures differ")
    manifest = {
        "schemaVersion": 1,
        "success": True,
        "requestedResolution": [16384, 64],
        "actualResolution": records[0]["actualResolution"],
        "invalidPixelCount": records[0]["invalidPixelCount"],
        "validPixelCount": records[0]["validPixelCount"],
        "invalidPixelRatio": records[0]["invalidPixelRatio"],
        "s1S8OutputExact": True,
        "s1S8GBufferExact": True,
        "records": records,
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print("[invalid-viewport] PASS actual={}x{} invalid={} ({:.2%}) S1/S8 exact".format(
        manifest["actualResolution"][0], manifest["actualResolution"][1],
        manifest["invalidPixelCount"], manifest["invalidPixelRatio"],
    ))


if __name__ == "__main__":
    main()
