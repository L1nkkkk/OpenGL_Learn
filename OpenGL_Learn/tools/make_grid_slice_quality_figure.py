"""Create a readable screenshot/diff plate from the frozen correctness captures."""

import argparse
import json
import pathlib

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    run_dir = arguments.run_dir.resolve()
    captures = run_dir / "captures"
    analytic = load_rgb(captures / "quality-boundary-n0256-r060-analytic.ppm")
    s1 = load_rgb(captures / "quality-boundary-n0256-r060-s01.ppm")
    s8 = load_rgb(captures / "quality-boundary-n0256-r060-s08.ppm")
    verification = json.loads(
        (run_dir / "verification" / "independent-verification.json").read_text(encoding="utf-8-sig")
    )
    quality = next(case for case in verification["quality"]["cases"] if case["case"].startswith("boundary-"))

    oracle_diff = np.abs(s8.astype(np.int16) - analytic.astype(np.int16)).astype(np.uint8)
    slice_diff = np.abs(s8.astype(np.int16) - s1.astype(np.int16)).astype(np.uint8)
    oracle_visible = np.clip(oracle_diff.astype(np.uint16) * 64, 0, 255).astype(np.uint8)
    slice_visible = np.clip(slice_diff.astype(np.uint16) * 64, 0, 255).astype(np.uint8)

    figure, axes = plt.subplots(2, 3, figsize=(16, 9), constrained_layout=True)
    panels = (
        (analytic, "Analytic-screen oracle"),
        (s8, "Grid S=8"),
        (oracle_visible, "|S8 - oracle| ×64"),
        (s1, "Grid S=1"),
        (s8, "Grid S=8"),
        (slice_visible, "|S8 - S1| ×64 (exact black)"),
    )
    for axis, (image, title) in zip(axes.flat, panels):
        axis.imshow(image)
        axis.set_title(title)
        axis.axis("off")
    figure.suptitle(
        "Boundary case N=256, R=6 — oracle max={} LSB, mean={:.4f} LSB, P99={}; all grid S byte-exact".format(
            quality["maxChannelLsb"], quality["meanChannelLsb"], quality["p99ChannelLsb"]
        ),
        fontsize=15,
    )
    output = run_dir / "charts" / "quality-boundary-oracle-diff.png"
    figure.savefig(output, dpi=140)
    plt.close(figure)
    print("[quality-figure] {}".format(output))


if __name__ == "__main__":
    main()
