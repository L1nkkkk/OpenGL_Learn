#!/usr/bin/env python3
"""Measure shadow acne and contact separation against a shadow-off reference."""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


Roi = tuple[int, int, int, int]


def load_luminance(path: Path) -> Image.Image:
    with Image.open(path) as image:
        return image.convert("L")


def crop_box(roi: Roi) -> tuple[int, int, int, int]:
    x, y, width, height = roi
    if width <= 0 or height <= 0:
        raise ValueError(f"ROI dimensions must be positive: {roi}")
    return x, y, x + width, y + height


def validate_roi(roi: Roi, size: tuple[int, int]) -> None:
    left, top, right, bottom = crop_box(roi)
    if left < 0 or top < 0 or right > size[0] or bottom > size[1]:
        raise ValueError(f"ROI {roi} lies outside image size {size}")


def darkening_mask(
    reference: Image.Image,
    sample: Image.Image,
    threshold: int,
) -> Image.Image:
    darkening = ImageChops.subtract(reference, sample)
    return darkening.point(
        lambda value: 255 if value >= threshold else 0,
        mode="1",
    )


def roi_metrics(mask: Image.Image, roi: Roi) -> dict[str, float | int]:
    region = mask.crop(crop_box(roi))
    width, height = region.size
    values = list(
        region.get_flattened_data()
        if hasattr(region, "get_flattened_data")
        else region.getdata()
    )
    dark_pixels = sum(bool(value) for value in values)
    maximum_horizontal_run = 0
    for row in range(height):
        run = 0
        row_offset = row * width
        for column in range(width):
            if values[row_offset + column]:
                run += 1
                maximum_horizontal_run = max(maximum_horizontal_run, run)
            else:
                run = 0
    return {
        "darkPixelRatio": dark_pixels / max(width * height, 1),
        "darkPixelCount": dark_pixels,
        "maximumHorizontalRunPixels": maximum_horizontal_run,
    }


def contact_gaps(
    mask: Image.Image,
    roi: Roi,
    axis: str,
    origin: int,
) -> dict[str, float | int | None]:
    left, top, right, bottom = crop_box(roi)
    gaps: list[int] = []
    if axis in {"down", "up"}:
        if not top <= origin < bottom:
            raise ValueError("Vertical contact origin must lie inside contact ROI")
        for x in range(left, right):
            positions = (
                range(origin, bottom)
                if axis == "down"
                else range(origin, top - 1, -1)
            )
            for y in positions:
                if mask.getpixel((x, y)):
                    gaps.append(abs(y - origin))
                    break
    else:
        if not left <= origin < right:
            raise ValueError("Horizontal contact origin must lie inside contact ROI")
        for y in range(top, bottom):
            positions = (
                range(origin, right)
                if axis == "right"
                else range(origin, left - 1, -1)
            )
            for x in positions:
                if mask.getpixel((x, y)):
                    gaps.append(abs(x - origin))
                    break

    if not gaps:
        return {
            "sampleCount": 0,
            "minimumPixels": None,
            "medianPixels": None,
            "maximumPixels": None,
        }
    return {
        "sampleCount": len(gaps),
        "minimumPixels": min(gaps),
        "medianPixels": statistics.median(gaps),
        "maximumPixels": max(gaps),
    }


def analyze_variant(
    reference: Image.Image,
    sample: Image.Image,
    threshold: int,
    lit_roi: Roi,
    contact_roi: Roi,
    shadow_roi: Roi,
    contact_axis: str,
    contact_origin: int,
) -> tuple[dict[str, object], Image.Image]:
    mask = darkening_mask(reference, sample, threshold)
    return (
        {
            "litRegion": roi_metrics(mask, lit_roi),
            "contactGap": contact_gaps(
                mask,
                contact_roi,
                contact_axis,
                contact_origin,
            ),
            "expectedShadowRegion": roi_metrics(mask, shadow_roi),
        },
        mask,
    )


def create_annotated_comparison(
    paths: list[Path],
    labels: list[str],
    output_path: Path,
    lit_roi: Roi,
    contact_roi: Roi,
    shadow_roi: Roi,
) -> None:
    images: list[Image.Image] = []
    for path in paths:
        with Image.open(path) as image:
            images.append(image.convert("RGB"))
    if len({image.size for image in images}) != 1:
        raise ValueError("Annotated comparison images have different dimensions")

    title_height = 36
    width, height = images[0].size
    composite = Image.new(
        "RGB",
        (width * len(images), height + title_height),
        "#111827",
    )
    colors = {
        "lit": "#22c55e",
        "contact": "#f59e0b",
        "shadow": "#38bdf8",
    }
    for index, (image, label) in enumerate(zip(images, labels)):
        offset_x = index * width
        composite.paste(image, (offset_x, title_height))
        draw = ImageDraw.Draw(composite)
        draw.text((offset_x + 12, 11), label, fill="#f8fafc")
        for name, roi in (
            ("lit", lit_roi),
            ("contact", contact_roi),
            ("shadow", shadow_roi),
        ):
            x, y, roi_width, roi_height = roi
            draw.rectangle(
                (
                    offset_x + x,
                    title_height + y,
                    offset_x + x + roi_width,
                    title_height + y + roi_height,
                ),
                outline=colors[name],
                width=2,
            )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    composite.save(output_path, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("off", type=Path)
    parser.add_argument("legacy", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--metrics", type=Path, required=True)
    parser.add_argument("--annotated", type=Path, required=True)
    parser.add_argument("--lit-roi", type=int, nargs=4, required=True)
    parser.add_argument("--contact-roi", type=int, nargs=4, required=True)
    parser.add_argument("--shadow-roi", type=int, nargs=4, required=True)
    parser.add_argument(
        "--contact-axis",
        choices=("down", "up", "left", "right"),
        required=True,
    )
    parser.add_argument("--contact-origin", type=int, required=True)
    parser.add_argument("--threshold", type=int, default=4)
    parser.add_argument("--legacy-label", default="Legacy bias")
    parser.add_argument("--candidate-label", default="Texel-scaled bias")
    arguments = parser.parse_args()
    if not 1 <= arguments.threshold <= 255:
        parser.error("--threshold must be between 1 and 255")

    paths = [
        arguments.off.resolve(),
        arguments.legacy.resolve(),
        arguments.candidate.resolve(),
    ]
    off = load_luminance(paths[0])
    legacy = load_luminance(paths[1])
    candidate = load_luminance(paths[2])
    if off.size != legacy.size or off.size != candidate.size:
        raise ValueError("Probe images have different dimensions")

    lit_roi = tuple(arguments.lit_roi)
    contact_roi = tuple(arguments.contact_roi)
    shadow_roi = tuple(arguments.shadow_roi)
    for roi in (lit_roi, contact_roi, shadow_roi):
        validate_roi(roi, off.size)

    legacy_metrics, _ = analyze_variant(
        off,
        legacy,
        arguments.threshold,
        lit_roi,
        contact_roi,
        shadow_roi,
        arguments.contact_axis,
        arguments.contact_origin,
    )
    candidate_metrics, _ = analyze_variant(
        off,
        candidate,
        arguments.threshold,
        lit_roi,
        contact_roi,
        shadow_roi,
        arguments.contact_axis,
        arguments.contact_origin,
    )
    metrics = {
        "schemaVersion": 1,
        "reference": str(paths[0]),
        "legacy": str(paths[1]),
        "candidate": str(paths[2]),
        "resolution": list(off.size),
        "thresholdLevels": arguments.threshold,
        "regions": {
            "lit": list(lit_roi),
            "contact": list(contact_roi),
            "expectedShadow": list(shadow_roi),
        },
        "contactAxis": arguments.contact_axis,
        "contactOrigin": arguments.contact_origin,
        "legacyLabel": arguments.legacy_label,
        "candidateLabel": arguments.candidate_label,
        "legacyMetrics": legacy_metrics,
        "candidateMetrics": candidate_metrics,
    }
    arguments.metrics.parent.mkdir(parents=True, exist_ok=True)
    arguments.metrics.write_text(
        json.dumps(metrics, indent=2) + "\n",
        encoding="utf-8",
    )
    create_annotated_comparison(
        paths,
        [
            "Shadow off",
            arguments.legacy_label,
            arguments.candidate_label,
        ],
        arguments.annotated.resolve(),
        lit_roi,
        contact_roi,
        shadow_roi,
    )
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
