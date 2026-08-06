#!/usr/bin/env python3
"""Capture Full-64 guide keyframes and select candidate-independent dynamic ROIs."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont


ROI_SELECTION_FRAMES = (0, 60, 119)
PATH_VALIDATION_FRAMES = (0, 300, 600, 900, 1199)
CAPTURE_FRAMES = tuple(sorted(set(ROI_SELECTION_FRAMES + PATH_VALIDATION_FRAMES)))
WINDOW_WIDTH = 256
WINDOW_HEIGHT = 192
HALO = 4
REVIEWED_ROI_OVERRIDES = {
    "sponza": {
        "contact": {
            "x": 1000,
            "y": 700,
            "reason": (
                "Full-64-only visual review replaces the automatic dark-wall maximum "
                "with persistent flower-pot/floor contact shadows and fine geometry"
            ),
        }
    }
}


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def load_module(path: Path, name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    expect(spec is not None and spec.loader is not None, f"module import failed: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def integral(image: np.ndarray) -> np.ndarray:
    return np.pad(np.cumsum(np.cumsum(image.astype(np.float64), axis=0), axis=1), ((1,0),(1,0)))


def window_sum(table: np.ndarray, x: int, y: int) -> float:
    x1, y1 = x + WINDOW_WIDTH, y + WINDOW_HEIGHT
    return float(table[y1,x1] - table[y,x1] - table[y1,x] + table[y,x])


def overlaps(first: tuple[int,int], second: tuple[int,int]) -> bool:
    ax, ay = first
    bx, by = second
    intersection_width = max(0, min(ax+WINDOW_WIDTH,bx+WINDOW_WIDTH)-max(ax,bx))
    intersection_height = max(0, min(ay+WINDOW_HEIGHT,by+WINDOW_HEIGHT)-max(ay,by))
    return intersection_width * intersection_height > WINDOW_WIDTH * WINDOW_HEIGHT * 0.10


def best_window(
    score_tables: list[np.ndarray],
    foreground_tables: list[np.ndarray],
    excluded: tuple[int,int] | None,
) -> tuple[int,int,float,float]:
    width = score_tables[0].shape[1] - 1
    height = score_tables[0].shape[0] - 1
    area = WINDOW_WIDTH * WINDOW_HEIGHT

    def score(x: int, y: int) -> tuple[float,float]:
        foreground = min(window_sum(table,x,y) for table in foreground_tables) / area
        if foreground < 0.20 or (excluded is not None and overlaps((x,y),excluded)):
            return -1.0, foreground
        robust = min(window_sum(table,x,y) for table in score_tables) / area
        return robust, foreground

    best = (-1.0, HALO, HALO, 0.0)
    for y in range(HALO, height-WINDOW_HEIGHT-HALO+1, 16):
        for x in range(HALO, width-WINDOW_WIDTH-HALO+1, 16):
            value, foreground = score(x,y)
            if value > best[0]:
                best = (value,x,y,foreground)
    _, coarse_x, coarse_y, _ = best
    for y in range(max(HALO,coarse_y-16), min(height-WINDOW_HEIGHT-HALO,coarse_y+16)+1):
        for x in range(max(HALO,coarse_x-16), min(width-WINDOW_WIDTH-HALO,coarse_x+16)+1):
            value, foreground = score(x,y)
            if value > best[0]:
                best = (value,x,y,foreground)
    expect(best[0] >= 0.0, "no valid persistent ROI was found")
    return best[1],best[2],best[0],best[3]


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    paths = (
        ("C:/Windows/Fonts/segoeuib.ttf","C:/Windows/Fonts/arialbd.ttf")
        if bold else ("C:/Windows/Fonts/segoeui.ttf","C:/Windows/Fonts/arial.ttf")
    )
    for path in paths:
        try:
            return ImageFont.truetype(path,size=size)
        except OSError:
            pass
    return ImageFont.load_default()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    script = Path(__file__).resolve()
    project = script.parent.parent
    executable = args.executable.resolve()
    output = args.output_directory.resolve()
    expect(executable.is_file(), f"missing executable: {executable}")
    expect(output.is_relative_to(project), "output must be inside the project")
    output.mkdir(parents=True, exist_ok=True)
    run = load_module(project/"tools"/"run_ssao_temporal_deterministic.py", "ssao_temporal_runner")
    analyze = load_module(project/"tools"/"analyze_ssao_temporal_deterministic.py", "ssao_temporal_analysis")
    scene_manifest = run.read_json(project/"classic-scenes.manifest.json")
    scene_lookup = {str(item["id"]):item for item in scene_manifest["scenes"]}
    executable_hash = sha256_file(executable)
    records = []
    for scene_id in run.SCENE_IDS:
        scene = scene_lookup[scene_id]
        for frame in CAPTURE_FRAMES:
            directory = output/"captures"/scene_id/f"frame-{frame:06d}"
            result_path = directory/"result.json"
            log_path = output/"logs"/f"{scene_id}-frame-{frame:06d}.log"
            paths = {
                "ao": directory/"ao.pfm",
                "depth": directory/"depth.pfm",
                "normal": directory/"normal.pfm",
                "ldr": directory/"ldr.ppm",
            }
            if not (args.resume and result_path.is_file() and all(path.is_file() for path in paths.values())):
                directory.mkdir(parents=True,exist_ok=True)
                arguments = run.base_arguments(
                    scene,
                    run.CONFIG_BY_NAME["legacy-full64"],
                    run.project_relative(result_path,project),
                    300,
                    frame+1,
                    1200,
                )
                arguments.remove("--classic-scene-no-capture")
                arguments.extend(
                    [
                        "--classic-scene-capture", run.project_relative(paths["ldr"],project),
                        "--classic-scene-ssao-float-capture", run.project_relative(paths["ao"],project),
                        "--classic-scene-ssao-depth-capture", run.project_relative(paths["depth"],project),
                        "--classic-scene-ssao-normal-capture", run.project_relative(paths["normal"],project),
                    ]
                )
                log_path.parent.mkdir(parents=True,exist_ok=True)
                started = time.perf_counter()
                with log_path.open("w",encoding="utf-8",newline="") as log:
                    completed = subprocess.run(
                        [str(executable),*arguments], cwd=project,
                        stdout=log, stderr=subprocess.STDOUT, check=False,
                    )
                expect(completed.returncode == 0, f"pilot capture failed: {log_path}")
                elapsed = time.perf_counter()-started
            else:
                elapsed = 0.0
            result = run.read_json(result_path)
            signature = run.validate_result(
                result,run.CONFIG_BY_NAME["legacy-full64"],scene_id,frame+1,1200,
                run.load_validator(project),
            )
            expect(result["motionTimeline"]["samples"][-1]["measurementFrame"] == frame, "pilot pose mismatch")
            for path in paths.values():
                expect(path.is_file() and path.stat().st_size>32, f"pilot output missing: {path}")
            records.append(
                {
                    "scene":scene_id,"measurementFrame":frame,
                    "cameraSignaturePrefixSha256":signature,
                    "result":run.project_relative(result_path,project),
                    "resultSha256":sha256_file(result_path),
                    "log":run.project_relative(log_path,project),
                    "elapsedSeconds":elapsed,
                    "files":{
                        name:{"path":run.project_relative(path,project),"bytes":path.stat().st_size,"sha256":sha256_file(path)}
                        for name,path in paths.items()
                    },
                }
            )
            print(f"pilot {scene_id} frame={frame} elapsed={elapsed:.1f}s",flush=True)
    selections: dict[str,list[dict[str,Any]]] = {}
    figures = {}
    for scene_id in run.SCENE_IDS:
        edge_tables=[]
        foreground_tables=[]
        contact_tables=[]
        ldr_images=[]
        for frame in ROI_SELECTION_FRAMES:
            directory=output/"captures"/scene_id/f"frame-{frame:06d}"
            ao=analyze.read_pfm(directory/"ao.pfm")
            depth=analyze.read_pfm(directory/"depth.pfm")
            normal=analyze.read_pfm(directory/"normal.pfm")
            edge,foreground=analyze.build_edge_mask(depth,normal)
            edge_tables.append(integral(edge))
            foreground_tables.append(integral(foreground))
            contact_tables.append(integral(np.clip(1.0-ao,0.0,1.0)*foreground))
            ldr_images.append(Image.open(directory/"ldr.ppm").convert("RGB"))
        edge_x,edge_y,edge_score,edge_foreground=best_window(edge_tables,foreground_tables,None)
        contact_x,contact_y,contact_score,contact_foreground=best_window(contact_tables,foreground_tables,(edge_x,edge_y))
        automatic_contact = {
            "x": contact_x,
            "y": contact_y,
            "minimumScore": contact_score,
            "minimumForegroundCoverage": contact_foreground,
        }
        contact_override = REVIEWED_ROI_OVERRIDES.get(scene_id, {}).get("contact")
        if contact_override is not None:
            contact_x = int(contact_override["x"])
            contact_y = int(contact_override["y"])
            area = WINDOW_WIDTH * WINDOW_HEIGHT
            contact_score = min(
                window_sum(table, contact_x, contact_y) for table in contact_tables
            ) / area
            contact_foreground = min(
                window_sum(table, contact_x, contact_y) for table in foreground_tables
            ) / area
        selections[scene_id]=[
            {
                "name":"edge","x":edge_x,"y":edge_y,
                "width":WINDOW_WIDTH,"height":WINDOW_HEIGHT,
                "selection":"maximize minimum Full64 depth/normal edge coverage across frames 0/60/119",
                "minimumScore":edge_score,"minimumForegroundCoverage":edge_foreground,
            },
            {
                "name":"contact","x":contact_x,"y":contact_y,
                "width":WINDOW_WIDTH,"height":WINDOW_HEIGHT,
                "selection":(
                    contact_override["reason"]
                    if contact_override is not None
                    else "maximize minimum Full64 foreground AO occlusion across frames 0/60/119, <=10% overlap with edge ROI"
                ),
                "minimumScore":contact_score,"minimumForegroundCoverage":contact_foreground,
                "automaticFull64OnlyCandidate": automatic_contact,
            },
        ]
        canvas=Image.new("RGB",(1920,1080*len(ROI_SELECTION_FRAMES)),"white")
        draw=ImageDraw.Draw(canvas)
        colors={"edge":"#ff3355","contact":"#22bb66"}
        for index,(frame,image) in enumerate(zip(ROI_SELECTION_FRAMES,ldr_images)):
            y_offset=index*1080
            canvas.paste(image,(0,y_offset))
            draw.text((24,y_offset+20),f"{scene_id} Full-64 frame {frame}",fill="white",stroke_width=2,stroke_fill="black",font=font(32,True))
            for roi in selections[scene_id]:
                x,y=int(roi["x"]),int(roi["y"])+y_offset
                draw.rectangle((x,y,x+WINDOW_WIDTH,y+WINDOW_HEIGHT),outline=colors[roi["name"]],width=5)
                draw.text((x+6,y+6),roi["name"],fill=colors[roi["name"]],stroke_width=2,stroke_fill="black",font=font(24,True))
        figure_path=output/f"{scene_id}-full64-dynamic-roi-selection.png"
        canvas.save(figure_path)
        figures[scene_id]=run.project_relative(figure_path,project)

        path_images = [
            Image.open(
                output / "captures" / scene_id / f"frame-{frame:06d}" / "ldr.ppm"
            ).convert("RGB").resize((640, 360), Image.Resampling.LANCZOS)
            for frame in PATH_VALIDATION_FRAMES
        ]
        path_canvas = Image.new("RGB", (640 * 3, 360 * 2), "#202020")
        path_draw = ImageDraw.Draw(path_canvas)
        for index, (frame, image) in enumerate(zip(PATH_VALIDATION_FRAMES, path_images)):
            x = (index % 3) * 640
            y = (index // 3) * 360
            path_canvas.paste(image, (x, y))
            path_draw.text(
                (x + 12, y + 10),
                f"Full-64 frame {frame}",
                fill="white",
                stroke_width=2,
                stroke_fill="black",
                font=font(22, True),
            )
        path_figure = output / f"{scene_id}-full64-path-validation.png"
        path_canvas.save(path_figure)
        figures[f"{scene_id}PathValidation"] = run.project_relative(
            path_figure, project
        )
    manifest={
        "schemaVersion":1,"generatedAtUtc":utc_now(),
        "releaseExecutable":str(executable),"releaseExecutableSha256":executable_hash,
        "protocol":{
            "reference":"legacy-full64",
            "roiSelectionFrames":list(ROI_SELECTION_FRAMES),
            "pathValidationFrames":list(PATH_VALIDATION_FRAMES),
            "captureFrames":list(CAPTURE_FRAMES),
            "resolution":[1920,1080],"window":[WINDOW_WIDTH,WINDOW_HEIGHT],
            "captureHaloPixels":HALO,"candidateInputsUsed":False,
            "reviewedOverrides":REVIEWED_ROI_OVERRIDES,
        },
        "runs":records,"rois":selections,"figures":figures,
        "validation":{
            "status":"pass","allInputsFinite":True,"cameraFramesExact":True,
            "selectionUsesOnlyFull64":True,"executableHashStable":sha256_file(executable)==executable_hash,
        },
    }
    write_json(output/"dynamic-rois.json",manifest)
    print(json.dumps({"status":"pass","rois":selections},indent=2),flush=True)
    return 0


if __name__=="__main__":
    raise SystemExit(main())
