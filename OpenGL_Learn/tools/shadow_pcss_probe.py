#!/usr/bin/env python3
"""Measure contact and far-edge penumbra widths in the Spot PCSS probe."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

from PIL import Image, ImageDraw


Scan = tuple[int, int, int, int, int]


def load_luminance(path: Path) -> Image.Image:
    with Image.open(path) as image:
        return image.convert("L")


def scan_rows(scan: Scan) -> list[int]:
    _, _, first_y, last_y, step = scan
    if step <= 0 or last_y < first_y:
        raise ValueError(f"Invalid scan range: {scan}")
    return list(range(first_y, last_y + 1, step))


def validate_scan(scan: Scan, size: tuple[int, int]) -> None:
    first_x, last_x, first_y, last_y, _ = scan
    if (
        first_x < 0
        or first_x >= last_x
        or last_x >= size[0]
        or first_y < 0
        or last_y >= size[1]
    ):
        raise ValueError(f"Scan {scan} lies outside image size {size}")
    scan_rows(scan)


def darkening_at(
    reference: Image.Image,
    sample: Image.Image,
    x: int,
    y: int,
) -> int:
    return max(reference.getpixel((x, y)) - sample.getpixel((x, y)), 0)


def edge_width(
    reference: Image.Image,
    sample: Image.Image,
    scan: Scan,
    y: int,
    plateau_x: tuple[int, int] | None,
    high_fraction: float,
    low_fraction: float,
) -> dict[str, float | int | None]:
    first_x, last_x, _, _, _ = scan
    values = [
        darkening_at(reference, sample, x, y)
        for x in range(first_x, last_x + 1)
    ]
    if plateau_x is None:
        plateau = float(max(values))
    else:
        plateau = float(
            statistics.median(
                darkening_at(reference, sample, x, y)
                for x in range(plateau_x[0], plateau_x[1] + 1)
            )
        )
    if plateau <= 0.0:
        return {
            "y": y,
            "plateauDarkening": plateau,
            "highX": None,
            "lowX": None,
            "widthPixels": None,
        }

    high_x = max(
        (
            first_x + index
            for index, value in enumerate(values)
            if value >= high_fraction * plateau
        ),
        default=None,
    )
    low_x = max(
        (
            first_x + index
            for index, value in enumerate(values)
            if value >= low_fraction * plateau
        ),
        default=None,
    )
    width = (
        low_x - high_x
        if high_x is not None and low_x is not None and low_x >= high_x
        else None
    )
    return {
        "y": y,
        "plateauDarkening": plateau,
        "highX": high_x,
        "lowX": low_x,
        "widthPixels": width,
    }


def summarize_widths(rows: list[dict[str, float | int | None]]) -> dict:
    widths = [
        int(row["widthPixels"])
        for row in rows
        if row["widthPixels"] is not None
    ]
    if not widths:
        return {
            "sampleCount": 0,
            "minimumPixels": None,
            "medianPixels": None,
            "meanPixels": None,
            "maximumPixels": None,
            "rows": rows,
        }
    return {
        "sampleCount": len(widths),
        "minimumPixels": min(widths),
        "medianPixels": statistics.median(widths),
        "meanPixels": statistics.fmean(widths),
        "maximumPixels": max(widths),
        "rows": rows,
    }


def analyze_variant(
    reference: Image.Image,
    sample: Image.Image,
    near_scan: Scan,
    far_scan: Scan,
    far_plateau_x: tuple[int, int],
    high_fraction: float,
    low_fraction: float,
) -> dict:
    near_rows = [
        edge_width(
            reference,
            sample,
            near_scan,
            y,
            None,
            high_fraction,
            low_fraction,
        )
        for y in scan_rows(near_scan)
    ]
    far_rows = [
        edge_width(
            reference,
            sample,
            far_scan,
            y,
            far_plateau_x,
            high_fraction,
            low_fraction,
        )
        for y in scan_rows(far_scan)
    ]
    near = summarize_widths(near_rows)
    far = summarize_widths(far_rows)
    near_median = near["medianPixels"]
    far_median = far["medianPixels"]
    hardening_ratio = (
        float(far_median) / float(near_median)
        if near_median not in (None, 0) and far_median is not None
        else None
    )
    return {
        "nearContactEdge": near,
        "farShadowEdge": far,
        "farToNearMedianRatio": hardening_ratio,
    }


def create_annotated_crop(
    paths: list[Path],
    labels: list[str],
    output_path: Path,
    crop: tuple[int, int, int, int],
    near_scan: Scan,
    far_scan: Scan,
    scale: int,
) -> None:
    images: list[Image.Image] = []
    for path in paths:
        with Image.open(path) as image:
            images.append(image.convert("RGB"))
    if len({image.size for image in images}) != 1:
        raise ValueError("Probe images have different dimensions")

    crop_x, crop_y, crop_width, crop_height = crop
    crop_box = (
        crop_x,
        crop_y,
        crop_x + crop_width,
        crop_y + crop_height,
    )
    resampling = getattr(Image, "Resampling", Image).NEAREST
    crops = [
        image.crop(crop_box).resize(
            (crop_width * scale, crop_height * scale),
            resampling,
        )
        for image in images
    ]
    title_height = 36
    output = Image.new(
        "RGB",
        (len(crops) * crop_width * scale, crop_height * scale + title_height),
        "#111827",
    )
    draw = ImageDraw.Draw(output)
    for index, (image, label) in enumerate(zip(crops, labels)):
        offset_x = index * crop_width * scale
        output.paste(image, (offset_x, title_height))
        draw.text((offset_x + 12, 11), label, fill="#f8fafc")
        for scan, color in (
            (near_scan, "#f59e0b"),
            (far_scan, "#38bdf8"),
        ):
            first_x, last_x, first_y, last_y, step = scan
            for y in range(first_y, last_y + 1, step):
                draw.line(
                    (
                        offset_x + (first_x - crop_x) * scale,
                        title_height + (y - crop_y) * scale,
                        offset_x + (last_x - crop_x) * scale,
                        title_height + (y - crop_y) * scale,
                    ),
                    fill=color,
                    width=1,
                )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output.save(output_path, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("off", type=Path)
    parser.add_argument("projected", type=Path)
    parser.add_argument("linear", type=Path)
    parser.add_argument("--metrics", type=Path, required=True)
    parser.add_argument("--annotated", type=Path, required=True)
    parser.add_argument(
        "--near-scan",
        type=int,
        nargs=5,
        metavar=("X0", "X1", "Y0", "Y1", "STEP"),
        required=True,
    )
    parser.add_argument(
        "--far-scan",
        type=int,
        nargs=5,
        metavar=("X0", "X1", "Y0", "Y1", "STEP"),
        required=True,
    )
    parser.add_argument(
        "--far-plateau-x",
        type=int,
        nargs=2,
        metavar=("X0", "X1"),
        required=True,
    )
    parser.add_argument(
        "--crop",
        type=int,
        nargs=4,
        metavar=("X", "Y", "WIDTH", "HEIGHT"),
        required=True,
    )
    parser.add_argument("--crop-scale", type=int, default=3)
    parser.add_argument("--high-fraction", type=float, default=0.9)
    parser.add_argument("--low-fraction", type=float, default=0.1)
    parser.add_argument("--projected-label", default="Projected-depth PCSS")
    parser.add_argument("--linear-label", default="Linear-depth PCSS")
    arguments = parser.parse_args()

    if not 0.0 < arguments.low_fraction < arguments.high_fraction <= 1.0:
        parser.error("Require 0 < low fraction < high fraction <= 1")
    if arguments.crop_scale < 1:
        parser.error("--crop-scale must be positive")

    paths = [
        arguments.off.resolve(),
        arguments.projected.resolve(),
        arguments.linear.resolve(),
    ]
    images = [load_luminance(path) for path in paths]
    if len({image.size for image in images}) != 1:
        raise ValueError("Probe images have different dimensions")
    size = images[0].size
    near_scan = tuple(arguments.near_scan)
    far_scan = tuple(arguments.far_scan)
    validate_scan(near_scan, size)
    validate_scan(far_scan, size)
    far_plateau_x = tuple(arguments.far_plateau_x)
    if (
        far_plateau_x[0] < far_scan[0]
        or far_plateau_x[1] > far_scan[1]
        or far_plateau_x[0] > far_plateau_x[1]
    ):
        raise ValueError("Far plateau must lie inside the far scan X range")

    projected = analyze_variant(
        images[0],
        images[1],
        near_scan,
        far_scan,
        far_plateau_x,
        arguments.high_fraction,
        arguments.low_fraction,
    )
    linear = analyze_variant(
        images[0],
        images[2],
        near_scan,
        far_scan,
        far_plateau_x,
        arguments.high_fraction,
        arguments.low_fraction,
    )
    metrics = {
        "schemaVersion": 1,
        "reference": str(paths[0]),
        "projected": str(paths[1]),
        "linear": str(paths[2]),
        "resolution": list(size),
        "transitionThresholds": {
            "highFraction": arguments.high_fraction,
            "lowFraction": arguments.low_fraction,
        },
        "nearScan": list(near_scan),
        "farScan": list(far_scan),
        "farPlateauX": list(far_plateau_x),
        "projectedMetrics": projected,
        "linearMetrics": linear,
        "farMedianWidthDeltaPixels": (
            linear["farShadowEdge"]["medianPixels"]
            - projected["farShadowEdge"]["medianPixels"]
        ),
    }
    arguments.metrics.parent.mkdir(parents=True, exist_ok=True)
    arguments.metrics.write_text(
        json.dumps(metrics, indent=2) + "\n",
        encoding="utf-8",
    )

    create_annotated_crop(
        paths,
        [
            "Shadow off",
            arguments.projected_label,
            arguments.linear_label,
        ],
        arguments.annotated,
        tuple(arguments.crop),
        near_scan,
        far_scan,
        arguments.crop_scale,
    )
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
