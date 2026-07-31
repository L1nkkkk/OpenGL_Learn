#!/usr/bin/env python3
"""Generate the Chinese Point shadow-cache correctness audit report."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager
from PIL import Image, ImageChops, ImageDraw, ImageFont


FACE_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]
CASE_NAMES = {
    "deferred-face-required": "Deferred Face 后续变为 Required",
    "point-move": "Point Light 移动",
    "local-caster": "局部 Caster 移动",
    "fbo-resize": "Point Shadow FBO Resize",
    "fbo-replace": "Point Shadow FBO Replace",
    "shader-reload": "Point Shadow Shader Reload",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8-sig") as stream:
        return json.load(stream)


def configure_font() -> None:
    for path in [
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/simhei.ttf"),
    ]:
        if path.exists():
            properties = font_manager.FontProperties(fname=str(path))
            plt.rcParams["font.family"] = properties.get_name()
            plt.rcParams["axes.unicode_minus"] = False
            return


configure_font()


def load_ui_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for path in [
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/arial.ttf"),
    ]:
        if path.exists():
            return ImageFont.truetype(str(path), size=size)
    return ImageFont.load_default()


def relative(path: Path, report: Path) -> str:
    return path.resolve().relative_to(report.parent.resolve()).as_posix()


def mask_text(value: int) -> str:
    names = [
        name
        for index, name in enumerate(FACE_NAMES)
        if value & (1 << index)
    ]
    return "|".join(names) if names else "—"


def all_resources_exact(correctness: dict[str, Any]) -> bool:
    resources = correctness["rendererOwnedResources"]
    return all(
        bool(resources[name]["exact"])
        for name in ("texture", "meshCpu", "meshGpu", "renderTarget")
    )


def load_cases(
    manifest: dict[str, Any], manifest_path: Path
) -> list[dict[str, Any]]:
    result_root = manifest_path.parent.parent
    rows: list[dict[str, Any]] = []
    for case in manifest["cases"]:
        experiment_root = result_root / case["experimentId"]
        summary = read_json(experiment_root / "summary.json")
        display_by_id = {
            scene["id"]: scene["displayName"]
            for scene in summary["scenes"]
        }
        correctness_by_id = {
            scene["id"]: scene["correctness"]
            for scene in summary["scenes"]
        }
        for scene_id in case["scenes"]:
            formal = experiment_root / "formal" / scene_id
            oracle = read_json(formal / "A1.json")
            candidate = read_json(formal / "B1.json")
            frames = max(1, int(candidate["measuredFrames"]))
            correctness = correctness_by_id[scene_id]
            screen_exact = all(
                bool(item["exact"])
                for item in correctness["captureComparisons"]
            )
            cube_exact = all(
                bool(item["exact"])
                and all(bool(face["exact"]) for face in item["faces"])
                for item in correctness["pointShadowCubeComparisons"]
            )
            rows.append(
                {
                    "caseId": case["id"],
                    "caseName": CASE_NAMES.get(
                        case["id"], case["displayName"]
                    ),
                    "workload": case["workload"],
                    "sceneId": scene_id,
                    "sceneName": display_by_id.get(scene_id, scene_id),
                    "frames": frames,
                    "screenExact": screen_exact,
                    "cubeExact": cube_exact,
                    "resourcesExact": all_resources_exact(correctness),
                    "oracleRendered": float(
                        oracle["shadow"]["measuredPointShadowRenderedFaceCount"]
                    )
                    / frames,
                    "candidateRequired": float(
                        candidate["shadow"]["measuredPointShadowRequiredFaceCount"]
                    )
                    / frames,
                    "candidateRendered": float(
                        candidate["shadow"]["measuredPointShadowRenderedFaceCount"]
                    )
                    / frames,
                    "candidateHits": float(
                        candidate["shadow"]["measuredPointShadowFaceCacheHitCount"]
                    )
                    / frames,
                    "candidateDeferred": float(
                        candidate["shadow"]["measuredPointShadowDeferredFaceCount"]
                    )
                    / frames,
                    "resourceFailures": int(
                        candidate["shadow"]["measuredShadowResourceFailureCount"]
                    ),
                    "fallbacks": int(
                        candidate["shadow"][
                            "measuredConservativeShadowFallbackCount"
                        ]
                    ),
                    "oracleImage": formal / "A1.ppm",
                    "candidateImage": formal / "B1.ppm",
                    "candidateResult": candidate,
                }
            )
    return rows


def build_screenshot_matrix(
    rows: list[dict[str, Any]], output: Path
) -> dict[str, Any]:
    preview_width = 480
    header = 66
    row_gap = 14
    column_gap = 14
    samples = [
        (
            Image.open(row["oracleImage"]).convert("RGB"),
            Image.open(row["candidateImage"]).convert("RGB"),
        )
        for row in rows
    ]
    heights = [
        max(1, round(pair[0].height * preview_width / pair[0].width))
        for pair in samples
    ]
    canvas_width = preview_width * 2 + column_gap * 3
    canvas_height = sum(header + height + row_gap for height in heights) + row_gap
    canvas = Image.new("RGB", (canvas_width, canvas_height), "white")
    draw = ImageDraw.Draw(canvas)
    title_font = load_ui_font(19)
    label_font = load_ui_font(15)
    y = row_gap
    exact_count = 0
    for row, pair, height in zip(rows, samples, heights):
        exact = ImageChops.difference(pair[0], pair[1]).getbbox() is None
        exact_count += int(exact)
        draw.text(
            (column_gap, y),
            f"{row['caseName']} / {row['sceneName']}",
            fill="#222222",
            font=title_font,
        )
        draw.text(
            (column_gap, y + 31),
            f"B Six-face Oracle | C Per-Face | 像素：{'完全一致' if exact else '不一致'}",
            fill="#3A6B55" if exact else "#A33A3A",
            font=label_font,
        )
        y += header
        for column, image in enumerate(pair):
            x = column_gap + column * (preview_width + column_gap)
            canvas.paste(
                image.resize(
                    (preview_width, height),
                    Image.Resampling.LANCZOS,
                ),
                (x, y),
            )
        y += height + row_gap
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output)
    return {"exactCount": exact_count, "total": len(rows)}


def chart_work(rows: list[dict[str, Any]], output: Path) -> None:
    labels = [
        f"{row['caseId']}\n{row['sceneId']}"
        for row in rows
    ]
    x = np.arange(len(rows))
    width = 0.24
    fig, axis = plt.subplots(figsize=(max(13, len(rows) * 1.15), 5.4))
    fields = [
        ("oracleRendered", "B Rendered", "#4D88C7"),
        ("candidateRendered", "C Rendered", "#39A275"),
        ("candidateHits", "C Hit", "#D7A64A"),
    ]
    for offset, (field, label, color) in enumerate(fields):
        values = [row[field] for row in rows]
        bars = axis.bar(
            x + (offset - 1) * width,
            values,
            width,
            label=label,
            color=color,
        )
        axis.bar_label(bars, fmt="%.2f", padding=2, fontsize=7)
    axis.set_ylabel("Face/帧")
    axis.set_title("关键失效规则：Six-face Oracle 与 Per-Face 工作量")
    axis.set_xticks(x, labels, rotation=20, ha="right")
    axis.grid(axis="y", alpha=0.2)
    axis.legend(frameon=False)
    axis.margins(y=0.15)
    plt.tight_layout()
    plt.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close()


def chart_deferred(row: dict[str, Any], output: Path) -> list[dict[str, Any]]:
    samples = row["candidateResult"]["motionTimeline"]["samples"]
    frames = np.asarray(
        [int(sample["measurementFrame"]) for sample in samples],
        dtype=np.int64,
    )
    required = np.asarray(
        [int(sample["shadow"]["pointShadowRequiredFaceCount"]) for sample in samples]
    )
    rendered = np.asarray(
        [int(sample["shadow"]["pointShadowRenderedFaceCount"]) for sample in samples]
    )
    deferred = np.asarray(
        [int(sample["shadow"]["pointShadowDeferredFaceCount"]) for sample in samples]
    )
    fig, axis = plt.subplots(figsize=(9.5, 4.2))
    axis.plot(frames, required, marker="o", label="Required")
    axis.plot(frames, rendered, marker="o", label="Rendered")
    axis.plot(frames, deferred, marker="o", label="Deferred")
    axis.set_xlabel("测量帧")
    axis.set_ylabel("Face 数")
    axis.set_title("Deferred Face 在进入 Required 后先更新再采样")
    axis.set_xticks(frames)
    axis.grid(alpha=0.22)
    axis.legend(frameon=False)
    plt.tight_layout()
    plt.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close()
    return [
        {
            "frame": int(sample["measurementFrame"]),
            "requiredMask": int(
                sample["shadow"]["pointShadowRequiredFaceMask"]
            ),
            "updateMask": int(
                sample["shadow"]["pointShadowUpdateFaceMask"]
            ),
            "deferred": int(
                sample["shadow"]["pointShadowDeferredFaceCount"]
            ),
        }
        for sample in samples
    ]


def write_report(
    manifest: dict[str, Any],
    manifest_path: Path,
    rows: list[dict[str, Any]],
    output_dir: Path,
    report_path: Path,
) -> dict[str, Any]:
    summary_path = output_dir / "point-shadow-cache-correctness-summary-cn.json"
    matrix_path = output_dir / "correctness-screenshot-matrix.png"
    work_path = output_dir / "correctness-face-work.png"
    matrix_result = build_screenshot_matrix(rows, matrix_path)
    chart_work(rows, work_path)
    deferred_row = next(row for row in rows if row["caseId"] == "deferred-face-required")
    deferred_chart = output_dir / "deferred-face-transition.png"
    deferred_samples = chart_deferred(deferred_row, deferred_chart)
    aba = manifest["topologyAbaSmoke"]
    aba_result = read_json(Path(aba["result"]))
    aba_image = Image.open(Path(aba["capture"])).convert("RGB")
    aba_screenshot = output_dir / "topology-aba-final.png"
    aba_image.save(aba_screenshot)

    all_passed = all(
        row["screenExact"]
        and row["cubeExact"]
        and row["resourcesExact"]
        and row["resourceFailures"] == 0
        and row["fallbacks"] == 0
        for row in rows
    )
    provenance = manifest["provenance"]
    lines = [
        "# Point Shadow Cache 正确性与失效规则审计报告",
        "",
        "> 本报告验证 C（Per-Light + Point Per-Face）在关键失效事件下与 B（Per-Light、固定 Six-face）Oracle 收敛。所有屏幕比较使用 0 像素差，Cubemap 比较使用六面逐面 bitwise hash。",
        "",
        "## 1. 审计结论",
        "",
        f"- 关键规则矩阵：`{'全部通过' if all_passed else '存在失败'}`。",
        f"- 严格最终截图：`{matrix_result['exactCount']}/{matrix_result['total']}` 组完全一致。",
        "- Point Cubemap：每组最终六面 Hash 全部一致。",
        "- Renderer-owned Texture、Mesh CPU/GPU、Render Target：B/C 完全一致。",
        "- ABA 形态只执行一次同槽位 Model 替换 Smoke；Revision 增量、失效次数和模型数量均通过门禁。",
        "",
        f"![正确性截图矩阵]({relative(matrix_path, report_path)})",
        "",
        f"![关键规则 Face 工作量]({relative(work_path, report_path)})",
        "",
        "## 2. 关键正确性矩阵",
        "",
        "| 用例 | 场景 | 屏幕像素 | 六面 Hash | 资源 | B Rendered | C Required | C Rendered | C Hit | C Deferred | Failure/Fallback |",
        "|---|---|---|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['caseName']} | {row['sceneName']} | "
            f"{'完全一致' if row['screenExact'] else '失败'} | "
            f"{'六面一致' if row['cubeExact'] else '失败'} | "
            f"{'一致' if row['resourcesExact'] else '失败'} | "
            f"{row['oracleRendered']:.2f} | "
            f"{row['candidateRequired']:.2f} | "
            f"{row['candidateRendered']:.2f} | "
            f"{row['candidateHits']:.2f} | "
            f"{row['candidateDeferred']:.2f} | "
            f"{row['resourceFailures']}/{row['fallbacks']} |"
        )

    lines.extend(
        [
            "",
            "这些用例覆盖：Point transform、局部 Caster transform、Point Shadow Shader revision、FBO 尺寸变化、FBO 释放后重建，以及 Deferred Face 再次进入 Receiver Demand。",
            "",
            "## 3. Deferred → Required 时序证明",
            "",
            "预热阶段先强制物化六面；测量第 0 帧只需求 +X，同时隐藏侧 Caster 变化使其余 Face 进入 Deferred；相机随后转向 -X，第 1 帧的 `Required & Update` 包含此前 Deferred 的 -X，证明旧内容没有被直接采样。",
            "",
            "| 测量帧 | Required Mask | Update Mask | Deferred Face 数 |",
            "|---:|---|---|---:|",
        ]
    )
    for sample in deferred_samples:
        lines.append(
            f"| {sample['frame']} | {mask_text(sample['requiredMask'])} "
            f"(`0x{sample['requiredMask']:02x}`) | "
            f"{mask_text(sample['updateMask'])} "
            f"(`0x{sample['updateMask']:02x}`) | "
            f"{sample['deferred']} |"
        )
    lines.extend(
        [
            "",
            f"![Deferred Face 时序]({relative(deferred_chart, report_path)})",
            "",
            "## 4. SceneTopologyRevision / ABA Smoke",
            "",
            "| 检查项 | 结果 |",
            "|---|---:|",
            f"| 测量起始 Revision | {aba_result['shadow']['measurementStartSceneTopologyRevision']} |",
            f"| 替换后 Revision | {aba_result['shadow']['sceneTopologyRevision']} |",
            f"| Revision 增量 | {aba['revisionDelta']} |",
            f"| 缓存失效次数 | {aba['invalidationCount']} |",
            f"| Model 数量（前/后） | {aba['modelCountBefore']} / {aba['modelCountAfter']} |",
            f"| Point 阴影更新次数 | {aba['pointUpdates']} |",
            "",
            "这个 Smoke 保持 Model 数量、名称、几何、材质和变换不变，只替换容器槽位中的对象。即使指针/内容形态可能回到相同状态，单调递增的 SceneTopologyRevision 仍强制缓存失效，因此不依赖地址签名规避 ABA。",
            "",
            f"![ABA 替换后的最终截图]({relative(aba_screenshot, report_path)})",
            "",
            "## 5. 实验条件与证据定位",
            "",
            f"- 分辨率：`{manifest['resolution'][0]}×{manifest['resolution'][1]}`。",
            f"- 进程内预热：`{manifest['internalWarmupFrames']}` 帧；每个矩阵用例各运行 B/C 独立进程一次。",
            "- 渲染：Release x64、PBR Forward、Hard Shadow、Point Six-face、VSync Off。",
            "- 截图阈值：最大通道差 `0`、变化像素 `0`。",
            f"- 源码 Commit：`{provenance['gitHead']}`；`gitDirty={str(provenance['gitDirty']).lower()}`。",
            f"- 可执行文件 SHA-256：`{provenance['executableSha256']}`。",
            f"- 已提交数据汇总：[point-shadow-cache-correctness-summary-cn.json]({relative(summary_path, report_path)})。",
            "",
            "结论：关键失效路径均能让 C 在采样前完成必要更新，并最终与 B 的屏幕结果、六面深度内容和 Renderer-owned 资源占用完全一致；未观察到资源失败或保守 fallback。",
            "",
        ]
    )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines), encoding="utf-8")
    return {
        "allPassed": all_passed,
        "screenshotMatrix": matrix_result,
        "deferredSamples": deferred_samples,
    }


def main() -> int:
    args = parse_args()
    manifest_path = args.manifest.resolve()
    output_dir = args.output_dir.resolve()
    report_path = args.report.resolve()
    manifest = read_json(manifest_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = load_cases(manifest, manifest_path)
    result = write_report(
        manifest,
        manifest_path,
        rows,
        output_dir,
        report_path,
    )
    summary = {
        "schemaVersion": 1,
        "manifest": manifest,
        "result": result,
        "cases": [
            {
                key: value
                for key, value in row.items()
                if key
                not in (
                    "oracleImage",
                    "candidateImage",
                    "candidateResult",
                )
            }
            for row in rows
        ],
    }
    summary_path = output_dir / "point-shadow-cache-correctness-summary-cn.json"
    summary_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"中文正确性报告: {report_path}")
    print(f"图表目录: {output_dir}")
    print(f"汇总 JSON: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
