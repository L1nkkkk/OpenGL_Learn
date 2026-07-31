#!/usr/bin/env python3
"""Image conversion helpers for the classic scene acceptance suite."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys

from PIL import Image


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def convert_dds_payloads(root: Path) -> int:
    converted = 0
    skipped = 0
    failures: list[tuple[Path, str]] = []
    for path in sorted(root.rglob("*.dds")):
        with path.open("rb") as source:
            if source.read(len(PNG_SIGNATURE)) == PNG_SIGNATURE:
                skipped += 1
                continue

        temporary = path.with_name(path.name + ".png-payload.tmp")
        try:
            with Image.open(path) as image:
                image.load()
                image.save(temporary, format="PNG", compress_level=1)
            os.replace(temporary, path)
            converted += 1
        except Exception as error:  # Keep processing so the report is complete.
            temporary.unlink(missing_ok=True)
            failures.append((path, str(error)))

    print(
        f"DDS payload conversion: converted={converted} "
        f"alreadyConverted={skipped} failed={len(failures)}"
    )
    for path, error in failures:
        print(f"ERROR {path}: {error}", file=sys.stderr)
    return 1 if failures else 0


def ppm_to_png(source: Path, destination: Path) -> int:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with Image.open(source) as image:
        image.save(destination, format="PNG", optimize=True)
    print(f"Converted {source} -> {destination}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    dds_parser = subparsers.add_parser(
        "dds-payloads",
        help="Replace DDS file contents with PNG payloads while preserving paths.",
    )
    dds_parser.add_argument("root", type=Path)

    ppm_parser = subparsers.add_parser(
        "ppm-to-png",
        help="Convert a renderer PPM capture to PNG.",
    )
    ppm_parser.add_argument("source", type=Path)
    ppm_parser.add_argument("destination", type=Path)

    arguments = parser.parse_args()
    if arguments.command == "dds-payloads":
        return convert_dds_payloads(arguments.root.resolve())
    if arguments.command == "ppm-to-png":
        return ppm_to_png(
            arguments.source.resolve(),
            arguments.destination.resolve(),
        )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
