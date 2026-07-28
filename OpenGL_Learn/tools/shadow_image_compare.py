#!/usr/bin/env python3
"""Create reproducible visual metrics and comparison images for shadow tests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import (
    Image,
    ImageChops,
    ImageDraw,
    ImageFilter,
    ImageOps,
    ImageStat,
)


def load_rgb(path: Path) -> Image.Image:
    with Image.open(path) as image:
        return image.convert("RGB")


def pixels(image: Image.Image):
    if hasattr(image, "get_flattened_data"):
        return image.get_flattened_data()
    return image.getdata()


def compare_images(
    before_path: Path,
    after_path: Path,
    metrics_path: Path,
    difference_path: Path,
    side_by_side_path: Path,
    before_label: str,
    after_label: str,
    crop: tuple[int, int, int, int] | None = None,
    crop_comparison_path: Path | None = None,
    crop_scale: int = 3,
) -> None:
    before = load_rgb(before_path)
    after = load_rgb(after_path)
    if before.size != after.size:
        raise ValueError(
            f"Image dimensions differ: {before.size} vs {after.size}"
        )

    difference = ImageChops.difference(before, after)
    difference_stat = ImageStat.Stat(difference)
    pixel_count = before.width * before.height
    exact_changed_pixels = sum(
        1 for pixel in pixels(difference) if max(pixel) > 0
    )
    changed_pixels = sum(
        1 for pixel in pixels(difference) if max(pixel) > 3
    )
    maximum_channel_delta = max(
        maximum
        for channel_extrema in difference.getextrema()
        for maximum in (channel_extrema[1],)
    )

    before_luminance = before.convert("L")
    after_luminance = after.convert("L")
    luminance_difference = ImageChops.difference(
        before_luminance, after_luminance
    )
    darkening = ImageChops.subtract(before_luminance, after_luminance)
    brightening = ImageChops.subtract(after_luminance, before_luminance)
    darkened_pixels = sum(value > 3 for value in pixels(darkening))
    brightened_pixels = sum(value > 3 for value in pixels(brightening))

    metrics = {
        "schemaVersion": 1,
        "before": str(before_path),
        "after": str(after_path),
        "resolution": [before.width, before.height],
        "rgbMeanAbsoluteDifference": (
            sum(difference_stat.mean) / (3.0 * 255.0)
        ),
        "rgbRootMeanSquareDifference": (
            sum(difference_stat.rms) / (3.0 * 255.0)
        ),
        "maximumChannelDelta": maximum_channel_delta,
        "exactChangedPixelCount": exact_changed_pixels,
        "exactChangedPixelRatio": exact_changed_pixels / pixel_count,
        "changedPixelRatio": changed_pixels / pixel_count,
        "luminanceMeanAbsoluteDifference": (
            ImageStat.Stat(luminance_difference).mean[0] / 255.0
        ),
        "meanDarkening": ImageStat.Stat(darkening).mean[0] / 255.0,
        "meanBrightening": ImageStat.Stat(brightening).mean[0] / 255.0,
        "darkenedPixelRatio": darkened_pixels / pixel_count,
        "brightenedPixelRatio": brightened_pixels / pixel_count,
    }

    if crop is not None:
        x, y, width, height = crop
        if width <= 0 or height <= 0:
            raise ValueError("Crop width and height must be positive")
        crop_box = (x, y, x + width, y + height)
        if (
            x < 0
            or y < 0
            or crop_box[2] > before.width
            or crop_box[3] > before.height
        ):
            raise ValueError(
                f"Crop {crop} lies outside image size {before.size}"
            )
        before_crop = before.crop(crop_box)
        after_crop = after.crop(crop_box)

        def high_frequency_residual(image: Image.Image) -> float:
            luminance = image.convert("L")
            low_frequency = luminance.filter(
                ImageFilter.GaussianBlur(radius=1.25)
            )
            residual = ImageChops.difference(luminance, low_frequency)
            return ImageStat.Stat(residual).mean[0] / 255.0

        before_residual = high_frequency_residual(before_crop)
        after_residual = high_frequency_residual(after_crop)
        relative_delta = 0.0
        if before_residual > 1e-12:
            relative_delta = (
                after_residual - before_residual
            ) / before_residual
        metrics["inspectionCrop"] = [x, y, width, height]
        metrics["cropHighFrequencyResidual"] = {
            "method": "luminance residual from Gaussian blur radius 1.25",
            "before": before_residual,
            "after": after_residual,
            "relativeDelta": relative_delta,
        }

        if crop_comparison_path is not None:
            resampling = getattr(Image, "Resampling", Image).NEAREST
            scaled_size = (
                before_crop.width * crop_scale,
                before_crop.height * crop_scale,
            )
            before_scaled = before_crop.resize(scaled_size, resampling)
            after_scaled = after_crop.resize(scaled_size, resampling)
            crop_title_height = 34
            crop_comparison = Image.new(
                "RGB",
                (
                    before_scaled.width * 2,
                    before_scaled.height + crop_title_height,
                ),
                "#111827",
            )
            crop_comparison.paste(before_scaled, (0, crop_title_height))
            crop_comparison.paste(
                after_scaled,
                (before_scaled.width, crop_title_height),
            )
            crop_draw = ImageDraw.Draw(crop_comparison)
            crop_draw.text(
                (12, 10),
                f"A  {before_label}",
                fill="#f8fafc",
            )
            crop_draw.text(
                (before_scaled.width + 12, 10),
                f"B  {after_label}",
                fill="#f8fafc",
            )
            crop_comparison_path.parent.mkdir(
                parents=True,
                exist_ok=True,
            )
            crop_comparison.save(
                crop_comparison_path,
                format="PNG",
                optimize=True,
            )

    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    metrics_path.write_text(
        json.dumps(metrics, indent=2) + "\n", encoding="utf-8"
    )

    enhanced_difference = ImageOps.autocontrast(
        luminance_difference, cutoff=1
    )
    heatmap = ImageOps.colorize(
        enhanced_difference,
        black="#07111f",
        mid="#1d65a6",
        white="#ffb000",
    )
    difference_path.parent.mkdir(parents=True, exist_ok=True)
    heatmap.save(difference_path, format="PNG", optimize=True)

    title_height = 34
    composite = Image.new(
        "RGB",
        (before.width * 2, before.height + title_height),
        "#111827",
    )
    composite.paste(before, (0, title_height))
    composite.paste(after, (before.width, title_height))
    draw = ImageDraw.Draw(composite)
    draw.text((12, 10), f"A  {before_label}", fill="#f8fafc")
    draw.text(
        (before.width + 12, 10),
        f"B  {after_label}",
        fill="#f8fafc",
    )
    side_by_side_path.parent.mkdir(parents=True, exist_ok=True)
    composite.save(side_by_side_path, format="PNG", optimize=True)

    print(json.dumps(metrics, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--metrics", type=Path, required=True)
    parser.add_argument("--difference", type=Path, required=True)
    parser.add_argument("--side-by-side", type=Path, required=True)
    parser.add_argument("--before-label", default="Before")
    parser.add_argument("--after-label", default="After")
    parser.add_argument(
        "--crop",
        nargs=4,
        type=int,
        metavar=("X", "Y", "WIDTH", "HEIGHT"),
    )
    parser.add_argument("--crop-comparison", type=Path)
    parser.add_argument("--crop-scale", type=int, default=3)
    arguments = parser.parse_args()
    if arguments.crop_scale < 1:
        parser.error("--crop-scale must be at least 1")
    if arguments.crop_comparison is not None and arguments.crop is None:
        parser.error("--crop-comparison requires --crop")
    compare_images(
        arguments.before.resolve(),
        arguments.after.resolve(),
        arguments.metrics.resolve(),
        arguments.difference.resolve(),
        arguments.side_by_side.resolve(),
        arguments.before_label,
        arguments.after_label,
        tuple(arguments.crop) if arguments.crop is not None else None,
        (
            arguments.crop_comparison.resolve()
            if arguments.crop_comparison is not None
            else None
        ),
        arguments.crop_scale,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
