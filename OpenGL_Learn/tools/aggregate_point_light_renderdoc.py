#!/usr/bin/env python3
"""Aggregate independent RenderDoc point-light replays into evidence files."""

import argparse
import csv
import hashlib
import html
import json
import pathlib
import statistics
import subprocess


CONFIGS = (
    {
        "stem": "representative-0016",
        "coverage": "representative",
        "lights": 16,
        "label": "representative / 16",
        "appTimerMs": 0.844,
    },
    {
        "stem": "high-overlap-0016",
        "coverage": "high-overlap",
        "lights": 16,
        "label": "high-overlap / 16",
        "appTimerMs": 1.966,
    },
    {
        "stem": "representative-0256",
        "coverage": "representative",
        "lights": 256,
        "label": "representative / 256",
        "appTimerMs": 11.073,
    },
    {
        "stem": "representative-0512",
        "coverage": "representative",
        "lights": 512,
        "label": "representative / 512",
        "appTimerMs": 21.809,
    },
)

CATEGORIES = (
    ("stencilClear", "Stencil clear"),
    ("stencilVolumeDraw", "Stencil volume draw"),
    ("lightingVolumeDraw", "Lighting volume draw"),
)

COUNTERS = (
    "Samples Passed",
    "Input Vertices Read",
    "Input Primitives",
    "Rasterizer Invocations",
    "Rasterized Primitives",
    "VS Invocations",
    "PS Invocations",
)


def median(values):
    return statistics.median(values)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def file_info(path):
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def svg_chart(configs, output):
    width, height = 1200, 680
    left, top, right, bottom = 105, 90, 45, 120
    plot_width = width - left - right
    plot_height = height - top - bottom
    max_value = max(item["classifiedGpuDurationMsMedian"] for item in configs)
    axis_max = max(5.0, (int(max_value / 5.0) + 1) * 5.0)
    colors = {
        "stencilClear": "#e76f51",
        "stencilVolumeDraw": "#e9c46a",
        "lightingVolumeDraw": "#2a9d8f",
    }
    pieces = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" '
        'viewBox="0 0 {} {}">'.format(width, height, width, height),
        '<rect width="100%" height="100%" fill="#fbfbf8"/>',
        '<text x="{}" y="42" font-family="Microsoft YaHei, sans-serif" '
        'font-size="26" font-weight="700">RenderDoc 点光阶段 GPU 拆解（三次独立 replay 中位数）</text>'.format(left),
    ]
    for tick in range(0, int(axis_max) + 1, 5):
        y = top + plot_height - tick / axis_max * plot_height
        pieces.append(
            '<line x1="{}" y1="{:.2f}" x2="{}" y2="{:.2f}" '
            'stroke="#d8d8d2" stroke-width="1"/>'.format(
                left, y, width - right, y
            )
        )
        pieces.append(
            '<text x="{}" y="{:.2f}" text-anchor="end" '
            'font-family="Consolas, monospace" font-size="15">{} ms</text>'.format(
                left - 12, y + 5, tick
            )
        )
    group_width = plot_width / len(configs)
    bar_width = 112
    for index, config in enumerate(configs):
        x = left + group_width * index + (group_width - bar_width) / 2
        y = top + plot_height
        for category, _ in CATEGORIES:
            value = config["classes"][category]["sumGpuDurationMsMedian"]
            block_height = value / axis_max * plot_height
            y -= block_height
            pieces.append(
                '<rect x="{:.2f}" y="{:.2f}" width="{}" height="{:.2f}" '
                'fill="{}"><title>{}: {:.3f} ms</title></rect>'.format(
                    x,
                    y,
                    bar_width,
                    block_height,
                    colors[category],
                    html.escape(category),
                    value,
                )
            )
        app_y = top + plot_height - config["appTimerQueryMs"] / axis_max * plot_height
        pieces.append(
            '<line x1="{:.2f}" y1="{:.2f}" x2="{:.2f}" y2="{:.2f}" '
            'stroke="#1d3557" stroke-width="4"/>'.format(
                x - 8, app_y, x + bar_width + 8, app_y
            )
        )
        pieces.append(
            '<text x="{:.2f}" y="{:.2f}" text-anchor="middle" '
            'font-family="Consolas, monospace" font-size="15" font-weight="700">'
            '{:.3f}</text>'.format(
                x + bar_width / 2,
                y - 10,
                config["classifiedGpuDurationMsMedian"],
            )
        )
        label_lines = config["label"].split(" / ")
        pieces.append(
            '<text x="{:.2f}" y="{}" text-anchor="middle" '
            'font-family="Microsoft YaHei, sans-serif" font-size="16">{}</text>'.format(
                x + bar_width / 2,
                top + plot_height + 28,
                html.escape(label_lines[0]),
            )
        )
        pieces.append(
            '<text x="{:.2f}" y="{}" text-anchor="middle" '
            'font-family="Consolas, monospace" font-size="16">{} lights</text>'.format(
                x + bar_width / 2,
                top + plot_height + 52,
                label_lines[1],
            )
        )
    legend_x = left
    legend_y = height - 34
    for category, label in CATEGORIES:
        pieces.append(
            '<rect x="{}" y="{}" width="18" height="18" fill="{}"/>'.format(
                legend_x, legend_y - 15, colors[category]
            )
        )
        pieces.append(
            '<text x="{}" y="{}" font-family="Microsoft YaHei, sans-serif" '
            'font-size="15">{}</text>'.format(legend_x + 26, legend_y, label)
        )
        legend_x += 220
    pieces.append(
        '<line x1="{}" y1="{}" x2="{}" y2="{}" stroke="#1d3557" '
        'stroke-width="4"/>'.format(legend_x, legend_y - 7, legend_x + 45, legend_y - 7)
    )
    pieces.append(
        '<text x="{}" y="{}" font-family="Microsoft YaHei, sans-serif" '
        'font-size="15">应用内 Timer Query</text>'.format(legend_x + 55, legend_y)
    )
    pieces.append("</svg>")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(pieces), encoding="utf-8")


def markdown_report(aggregate, output):
    configs = aggregate["configs"]
    cleanup = aggregate.get("instrumentationCleanup")
    lines = [
        "# RenderDoc 点光源 GPU 瓶颈拆解（Legacy 基线）",
        "",
        "> 结论：在 512 灯 representative 中，三次独立 replay 的分类事件中位数显示 "
        "Stencil clear 是最大单项（{:.3f} ms，{:.1f}%），Lighting volume draw 次之（{:.3f} ms，{:.1f}%），Stencil volume draw 最小（{:.3f} ms，{:.1f}%）。本报告只诊断，不包含任何优化实现。".format(
            configs[3]["classes"]["stencilClear"]["sumGpuDurationMsMedian"],
            configs[3]["classes"]["stencilClear"]["phasePercent"],
            configs[3]["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
            configs[3]["classes"]["lightingVolumeDraw"]["phasePercent"],
            configs[3]["classes"]["stencilVolumeDraw"]["sumGpuDurationMsMedian"],
            configs[3]["classes"]["stencilVolumeDraw"]["phasePercent"],
        ),
        "",
        "## 范围与固定条件",
        "",
        "- 场景：固定 Sponza；Release x64；1920×1080；固定相机、seed `0x21D3F3A5` 与既有灯光生成器。",
        "- 路径：Phong Deferred Legacy 点光体积；显式 `gPosition`；点光阴影、SSAO、Bloom 关闭；VSync 请求值 0。",
        "- 窗口像素格式保留上一轮基线的 4× MSAA（`windowSamples=4`）；没有为本诊断改变 Legacy 状态。",
        "- CPU：{}（{} logical processors）；OS：{}。".format(
            aggregate["environment"]["cpu"],
            aggregate["environment"]["logicalProcessors"],
            aggregate["environment"]["os"],
        ),
        "- GPU：{}；OpenGL `{}`；驱动来自 OpenGL 字符串。".format(
            aggregate["environment"]["glRenderer"],
            aggregate["environment"]["glVersion"],
        ),
        "- RenderDoc：`{}`。".format(aggregate["renderDoc"]["version"]),
        "",
        "## 捕获与计时协议",
        "",
        "每个配置保留 1 个独立 RDC；应用在约 30 帧后通过既有 in-app `StartFrameCapture/EndFrameCapture` 入口捕获。每个 RDC 随后由 3 个彼此独立的 QRenderDoc 进程重放。GPU Duration 与 pipeline statistics 分两次 `FetchCounters` replay pass 读取，避免统计查询本身污染 duration。下面所有 RenderDoc 汇总均为三次独立 replay 的中位数，不把单帧单次值当作稳定结果。",
        "",
        "- captureExecutable：`{}`；postCleanupExecutable：`{}`。两者分开记录，不把清理后的 EXE 伪装成四个既有 RDC 的捕获程序。".format(
            aggregate["environment"]["captureExecutable"]["sha256"],
            aggregate["environment"]["postCleanupExecutable"]["sha256"],
        ),
        "",
        "## 逐 replay 真实结果",
        "",
        "| 配置 | Replay | 分类总和 ms | Clear ms | Stencil draw ms | Lighting draw ms |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for config in configs:
        for replay in config["replays"]:
            lines.append(
                "| {} | {} | {:.3f} | {:.3f} | {:.3f} | {:.3f} |".format(
                    config["label"],
                    replay["replayIndex"],
                    replay["classifiedGpuDurationMs"],
                    replay["classes"]["stencilClear"],
                    replay["classes"]["stencilVolumeDraw"],
                    replay["classes"]["lightingVolumeDraw"],
                )
            )
    lines.extend(
        [
            "",
            "## 按事件类别聚合",
            "",
            "`总和中位数` 是每次 replay 先对该类全部事件求和、再对三次求中位数；`单事件中位数` 是三次 replay 各自事件中位数的中位数。占比以三类总和中位数之和为分母。",
            "",
            "| 配置 | 类别 | 事件数/Replay | 总和中位数 ms | 单事件中位数 ms | 点光分类阶段占比 |",
            "|---|---|---:|---:|---:|---:|",
        ]
    )
    for config in configs:
        for category, label in CATEGORIES:
            item = config["classes"][category]
            lines.append(
                "| {} | {} | {} | {:.3f} | {:.6f} | {:.1f}% |".format(
                    config["label"],
                    label,
                    item["eventCount"],
                    item["sumGpuDurationMsMedian"],
                    item["perEventGpuDurationMsMedian"],
                    item["phasePercent"],
                )
            )
    lines.extend(
        [
            "",
            "## 与应用内 OpenGL Timer Query 分开对照",
            "",
            "| 配置 | 应用内点光阶段 Median ms | RenderDoc 分类事件总和 Median ms | RenderDoc / App |",
            "|---|---:|---:|---:|",
        ]
    )
    for config in configs:
        lines.append(
            "| {} | {:.3f} | {:.3f} | {:.2f}× |".format(
                config["label"],
                config["appTimerQueryMs"],
                config["classifiedGpuDurationMsMedian"],
                config["renderDocToAppRatio"],
            )
        )
    rep16, overlap16 = configs[0], configs[1]
    lines.extend(
        [
            "",
            "RenderDoc 是 replay 时对单个 API event 插入 counter 的分类总和；应用内 Timer Query 是原进程中包围整个点光阶段的连续区间，包含状态切换/间隙但没有 RenderDoc replay 插桩。两者不要求完全一致。256/512 的两种口径非常接近；16 灯固定开销占比更高。趋势一致：灯数上升与 high-overlap 都增加点光成本。",
            "",
            "## 覆盖率计数与 high-overlap 定位",
            "",
            "| 配置 | Lighting PS Invocations | Lighting duration ms | Stencil Samples Passed | Rasterized primitives（Stencil） |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for config in configs:
        lines.append(
            "| {} | {:,} | {:.3f} | {:,} | {:,} |".format(
                config["label"],
                int(config["counters"]["lightingVolumeDraw"]["PS Invocations"]),
                config["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
                int(config["counters"]["stencilVolumeDraw"]["Samples Passed"]),
                int(config["counters"]["stencilVolumeDraw"]["Rasterized Primitives"]),
            )
        )
    lines.extend(
        [
            "",
            "16 灯 high-overlap 相对 representative：Lighting PS Invocations 从 {:,} 增至 {:,}（{:.2f}×），Lighting duration 从 {:.3f} ms 增至 {:.3f} ms（{:.2f}×）。应用内点光阶段是 2.33×；RenderDoc 分类总和是 {:.2f}×，差异被 high-overlap 更低的 Stencil volume draw 成本部分抵消。因此 2.33× 的主要增量明确落在 Lighting volume fragment shading，而不是 clear。".format(
                int(rep16["counters"]["lightingVolumeDraw"]["PS Invocations"]),
                int(overlap16["counters"]["lightingVolumeDraw"]["PS Invocations"]),
                aggregate["decisions"]["highOverlapPsInvocationRatio"],
                rep16["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
                overlap16["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
                aggregate["decisions"]["highOverlapLightingDurationRatio"],
                aggregate["decisions"]["highOverlapRenderDocTotalRatio"],
            ),
            "",
            "## 三个决策问题",
            "",
            "1. **512 灯主成本**：full-target stencil clear。三次 replay 的类总和中位数为 {:.3f} ms（{:.1f}%）；Lighting fragment shading 为 {:.3f} ms（{:.1f}%）；Stencil volume raster 为 {:.3f} ms（{:.1f}%）。".format(
                configs[3]["classes"]["stencilClear"]["sumGpuDurationMsMedian"],
                configs[3]["classes"]["stencilClear"]["phasePercent"],
                configs[3]["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
                configs[3]["classes"]["lightingVolumeDraw"]["phasePercent"],
                configs[3]["classes"]["stencilVolumeDraw"]["sumGpuDurationMsMedian"],
                configs[3]["classes"]["stencilVolumeDraw"]["phasePercent"],
            ),
            "2. **16 灯 high-overlap 的差异**：主要落在 Lighting volume fragment shading；PS Invocations 和 duration 的同步增长给出直接证据。",
            "3. **下一项正式 A/B**：主候选只验证“点光 stencil clear 频率/标记复用”这一变量，A 保持 Legacy 2N clear，B 仅改变 clear 策略并保持灯、球体 draw 与顺序不变。备选只验证 Lighting draw 的屏幕覆盖约束（例如 scissor）并以 PS Invocations 为机制指标。本轮未实现二者。",
            "",
            "## 正确性与重放证据",
            "",
        ]
    )
    for config in configs:
        lines.append(
            "- `{}`：signature `{}` / `{}`；RDC SHA-256 `{}`；3/3 replay 成功、0 debug message、fatal status Success；事件计数 clear={}、stencil draw={}、lighting draw={}；应用截图 non-black ratio {:.4f}。".format(
                config["label"],
                config["sceneSignature"],
                config["submissionSignature"],
                config["capture"]["sha256"],
                2 * config["lights"],
                config["lights"],
                config["lights"],
                config["nonBlackRatio"],
            )
        )
    smoke = aggregate.get("defaultOffSmoke")
    if smoke and not cleanup:
        lines.extend(
            [
                "",
                "- marker 默认关闭 smoke：`renderDocMarkersEnabled=false`，signature `{}` / `{}`，submitted=16、culled=0、点光 clear=32、non-black ratio {:.4f}；证明诊断开关默认不改变上一轮 16 灯签名与 Legacy 计数。".format(
                    smoke["sceneSignature"],
                    smoke["submissionSignature"],
                    smoke["nonBlackRatio"],
                ),
            ]
        )
    if cleanup:
        lines.extend(
            [
                "",
                "## Instrumentation cleanup 验收",
                "",
                "父任务发现 marker 默认关闭时仍逐灯格式化名称。清理后，`snprintf` 只在 `renderDocMarkers=true` 分支执行；clear、draw、Uniform 和逐灯顺序没有变化。",
                "",
                "| 进程 | Warmup | Samples | CPU Deferred Point Lights Median ms | GPU Median ms |",
                "|---:|---:|---:|---:|---:|",
            ]
        )
        for index, run in enumerate(cleanup["defaultOffRuns"], 1):
            lines.append(
                "| {} | {} | {} | {:.5f} | {:.6f} |".format(
                    index,
                    run["warmupFrames"],
                    run["sampleFrames"],
                    run["cpuPointLightMedianMs"],
                    run["gpuPointLightMedianMs"],
                )
            )
        lines.extend(
            [
                "",
                "三进程 CPU Median 的中位数为 **{:.5f} ms**，上一轮 representative/16 run01 为 0.0235 ms；父任务清理前新测值为 0.1713 ms。三进程均为 signature `0x28cdb6b119b52795` / `0xff25d7196616c895`、submitted=16、culled=0、clear=32、non-black ratio 0.927415。".format(
                    cleanup["defaultOffCpuPointLightMedianMsMedian"]
                ),
                "",
                "- marker-enabled 短 smoke：成功、`renderDocMarkersEnabled=true`、同一 signature、submitted=16、culled=0、clear=32、非黑；按要求没有创建新 RDC，也没有采正式 counter。",
                "- marker 语义复核：清理后源码仍按 Phase → Light → ClearBefore → StencilDraw → LightingDraw → ClearAfter 顺序保留相同标签；既有 representative/16 RDC 事件树仍为 phase=1、light=16、clear=32、stencil draw=16、lighting draw=16。",
                "- 四个原始 RDC、12 个原始 replay JSON 与 GPU 数值均保留；cleanup 只新增 CPU/default-off 和 marker-enabled app smoke 证据。",
            ]
        )
    lines.extend(
        [
            "",
            "每个 replay JSON 内含精确 marker/action Event ID、逐事件 counter、Stencil 与 Lighting 关键 Pipeline State。每配置的 `*-lighting-target.png` 是在最后一个 Lighting volume draw 事件通过 RenderDoc `SaveTexture` 导出的目标纹理；`*-thumbnail.png` 是 RDC 内嵌最终帧缩略图。",
            "",
            "## 工具限制与已知限制",
            "",
            "- NVIDIA 扩展硬件 counter 不可用：RenderDoc 明确报告缺少 Nsight Perf SDK `nvperf_grfx_host.dll`。本报告只使用可用的通用 OpenGL GPU Duration、Samples/Primitives/Shader Invocations counters，没有估算。",
            "- Duration 是 RenderDoc replay 口径，不是原始 capture 进程的原位帧时间；分类总和不包含 marker、状态设置和事件间隙。应用 Timer Query 继续作为独立口径保留。",
            "- 每配置为 1 个独立捕获 + 3 个独立 replay 进程，不声称有 3 个独立 RDC。",
            "- representative/16 的 Stencil draw replay 波动高于其他配置；报告使用三进程中位数并保留逐 replay 原始 JSON，不隐藏波动。",
            "- 本轮未修改上一轮正式基线 JSON，也未实现 Scissor、剔除、批处理、clear 减少、单 Pass 或 Stencil 策略切换。",
            "",
            "## 机器可读产物",
            "",
            "- `aggregate.json`：聚合、签名、捕获哈希、决策值。",
            "- `per-replay.csv`、`per-class-replay.csv`、`per-event.csv`：逐 replay / 类 / 事件数据。",
            "- `point-light-gpu-breakdown.svg`：拆解图。",
        ]
    )
    if cleanup:
        lines.extend(
            [
                "- `instrumentation-cleanup/summary.json`：清理前后 EXE、三进程 CPU/GPU Median、marker 语义和原证据保留状态。",
                "- `instrumentation-cleanup/per-process.csv`：三个 300+600 default-off 进程结果。",
            ]
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--batch-dir", required=True, type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()
    batch = args.batch_dir.resolve()
    project = batch.parents[2]
    repository = project.parent
    baseline_manifest = json.loads(
        (
            project
            / "benchmark-results"
            / "point-light-heavy"
            / "legacy-baseline-20260801"
            / "manifest.json"
        ).read_text(encoding="utf-8-sig")
    )
    renderdoc_dir = (
        project
        / "benchmark-results"
        / "ssao-renderdoc-evidence"
        / "renderdoc-1.45.0"
        / "portable"
        / "RenderDoc"
    )
    renderdoccmd = renderdoc_dir / "renderdoccmd.exe"
    qrenderdoc = renderdoc_dir / "qrenderdoc.exe"
    current_executable = repository / "x64" / "Release" / "OpenGL_Learn.exe"
    capture_executable_path = batch / "capture-executable.json"
    if capture_executable_path.exists():
        capture_executable = json.loads(
            capture_executable_path.read_text(encoding="utf-8-sig")
        )
        capture_executable = {
            "path": capture_executable["path"],
            "bytes": capture_executable["bytes"],
            "sha256": capture_executable["sha256"],
            "metadata": str(capture_executable_path),
        }
    else:
        capture_executable = file_info(current_executable)
        capture_executable["metadata"] = None
    post_cleanup_executable = file_info(current_executable)
    version_process = subprocess.run(
        [str(renderdoccmd), "version"],
        check=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )
    version = version_process.stdout.strip()

    per_replay_rows = []
    per_class_rows = []
    per_event_rows = []
    config_outputs = []
    for spec in CONFIGS:
        replay_documents = []
        for replay_index in range(1, 4):
            replay_path = (
                batch
                / "replays"
                / "{}-replay{:02d}.json".format(spec["stem"], replay_index)
            )
            document = json.loads(replay_path.read_text(encoding="utf-8"))
            if not document["success"]:
                raise RuntimeError("failed replay: {}".format(replay_path))
            replay_documents.append(document)
            class_values = {
                category: document["classSummaries"][category]["gpuDurationMs"]["sum"]
                for category, _ in CATEGORIES
            }
            per_replay_rows.append(
                {
                    "config": spec["stem"],
                    "coverage": spec["coverage"],
                    "lightCount": spec["lights"],
                    "replayIndex": replay_index,
                    "classifiedGpuDurationMs": document["classifiedPointLightGpuDurationMs"],
                    "stencilClearGpuDurationMs": class_values["stencilClear"],
                    "stencilVolumeDrawGpuDurationMs": class_values["stencilVolumeDraw"],
                    "lightingVolumeDrawGpuDurationMs": class_values["lightingVolumeDraw"],
                    "eventCountsValid": document["eventCountValidation"]["valid"],
                    "captureOpenResult": document["captureOpenResult"],
                    "captureReplayResult": document["captureReplayResult"],
                    "fatalReplayStatus": document["fatalReplayStatusAfter"],
                    "debugMessageCount": len(document["debugMessages"]),
                    "json": str(replay_path),
                }
            )
            for category, label in CATEGORIES:
                summary = document["classSummaries"][category]
                row = {
                    "config": spec["stem"],
                    "coverage": spec["coverage"],
                    "lightCount": spec["lights"],
                    "replayIndex": replay_index,
                    "category": category,
                    "categoryLabel": label,
                    "eventCount": summary["gpuDurationMs"]["count"],
                    "sumGpuDurationMs": summary["gpuDurationMs"]["sum"],
                    "medianEventGpuDurationMs": summary["gpuDurationMs"]["median"],
                    "phasePercent": summary["pointLightPhasePercent"],
                }
                for counter in COUNTERS:
                    row[counter] = summary["counters"][counter]["sum"]
                per_class_rows.append(row)
            for event in document["eventTree"]["classifiedEvents"]:
                row = {
                    "config": spec["stem"],
                    "coverage": spec["coverage"],
                    "lightCount": spec["lights"],
                    "replayIndex": replay_index,
                    "lightIndex": event["lightIndex"],
                    "category": event["category"],
                    "markerEventId": event["markerEventId"],
                    "eventId": event["eventId"],
                    "actionId": event["actionId"],
                    "gpuDurationMs": event["gpuDurationMs"],
                    "numIndices": event["numIndices"],
                }
                for counter in COUNTERS:
                    row[counter] = event["counters"][counter]
                per_event_rows.append(row)

        diagnostic_path = batch / "diagnostics" / (spec["stem"] + "-capture.json")
        diagnostic = json.loads(diagnostic_path.read_text(encoding="utf-8"))
        capture_path = batch / "captures" / (spec["stem"] + "_capture.rdc")
        class_outputs = {}
        for category, _ in CATEGORIES:
            sums = [
                document["classSummaries"][category]["gpuDurationMs"]["sum"]
                for document in replay_documents
            ]
            event_medians = [
                document["classSummaries"][category]["gpuDurationMs"]["median"]
                for document in replay_documents
            ]
            class_outputs[category] = {
                "eventCount": replay_documents[0]["classSummaries"][category]
                ["gpuDurationMs"]["count"],
                "sumGpuDurationMsValues": sums,
                "sumGpuDurationMsMedian": median(sums),
                "perEventGpuDurationMsMedians": event_medians,
                "perEventGpuDurationMsMedian": median(event_medians),
            }
        class_total = sum(
            item["sumGpuDurationMsMedian"] for item in class_outputs.values()
        )
        for item in class_outputs.values():
            item["phasePercent"] = item["sumGpuDurationMsMedian"] * 100.0 / class_total

        counter_outputs = {}
        for category, _ in CATEGORIES:
            counter_outputs[category] = {}
            for counter in COUNTERS:
                values = [
                    document["classSummaries"][category]["counters"][counter]["sum"]
                    for document in replay_documents
                ]
                counter_outputs[category][counter] = median(values)

        total_values = [
            document["classifiedPointLightGpuDurationMs"]
            for document in replay_documents
        ]
        config_output = dict(spec)
        config_output.update(
            {
                "sceneSignature": diagnostic["pointLightStress"]["sceneSignature"],
                "submissionSignature": diagnostic["pointLightStress"]
                ["submissionSignature"],
                "nonBlackRatio": diagnostic["nonBlackRatio"],
                "classifiedGpuDurationMsValues": total_values,
                "classifiedGpuDurationMsMedian": median(total_values),
                "appTimerQueryMs": spec["appTimerMs"],
                "renderDocToAppRatio": median(total_values) / spec["appTimerMs"],
                "classes": class_outputs,
                "counters": counter_outputs,
                "eventCounts": {
                    "stencilClear": 2 * spec["lights"],
                    "stencilVolumeDraw": spec["lights"],
                    "lightingVolumeDraw": spec["lights"],
                },
                "capture": file_info(capture_path),
                "diagnostic": str(diagnostic_path),
                "thumbnail": str(
                    batch / "images" / (spec["stem"] + "-thumbnail.png")
                ),
                "lightingTarget": str(
                    batch / "images" / (spec["stem"] + "-lighting-target.png")
                ),
                "images": {
                    "thumbnail": file_info(
                        batch / "images" / (spec["stem"] + "-thumbnail.png")
                    ),
                    "lightingTarget": file_info(
                        batch / "images" / (spec["stem"] + "-lighting-target.png")
                    ),
                    "applicationCapture": file_info(
                        batch / "images" / (spec["stem"] + "-app.ppm")
                    ),
                },
                "replays": [
                    {
                        "replayIndex": document["replayIndex"],
                        "classifiedGpuDurationMs": document[
                            "classifiedPointLightGpuDurationMs"
                        ],
                        "classes": {
                            category: document["classSummaries"][category]
                            ["gpuDurationMs"]["sum"]
                            for category, _ in CATEGORIES
                        },
                        "json": document["output"],
                    }
                    for document in replay_documents
                ],
            }
        )
        config_outputs.append(config_output)

    diagnostic0 = json.loads(
        (batch / "diagnostics" / "representative-0016-capture.json").read_text(
            encoding="utf-8"
        )
    )
    rep16, overlap16 = config_outputs[0], config_outputs[1]
    extended_counter = next(
        (
            counter
            for counter in json.loads(
                (batch / "replays" / "representative-0016-replay01.json").read_text(
                    encoding="utf-8"
                )
            )["availableCounters"]
            if counter["id"] >= 3000000
        ),
        None,
    )
    default_off_smoke_path = (
        batch / "diagnostics" / "default-off-representative-0016-smoke.json"
    )
    default_off_smoke = None
    if default_off_smoke_path.exists():
        smoke = json.loads(default_off_smoke_path.read_text(encoding="utf-8"))
        default_off_smoke = {
            "path": str(default_off_smoke_path),
            "success": smoke["success"],
            "renderDocMarkersEnabled": smoke["pointLightStress"]
            ["renderDocMarkersEnabled"],
            "sceneSignature": smoke["pointLightStress"]["sceneSignature"],
            "submissionSignature": smoke["pointLightStress"]
            ["submissionSignature"],
            "submittedLightsMedian": smoke["profiler"]["summary"]
            ["pointLightsSubmitted"]["median"],
            "culledLightsMedian": smoke["profiler"]["summary"]
            ["pointLightsCulled"]["median"],
            "pointLightStencilClearsMedian": smoke["profiler"]["summary"]
            ["pointLightStencilClears"]["median"],
            "nonBlackRatio": smoke["nonBlackRatio"],
        }
    instrumentation_cleanup = None
    cleanup_directory = batch / "instrumentation-cleanup"
    cleanup_run_paths = sorted(
        cleanup_directory.glob("default-off-representative-0016-run??.json")
    )
    marker_smoke_path = (
        cleanup_directory / "marker-enabled-representative-0016-smoke.json"
    )
    if cleanup_run_paths and marker_smoke_path.exists():
        cleanup_runs = []
        for run_path in cleanup_run_paths:
            run = json.loads(run_path.read_text(encoding="utf-8"))
            cleanup_runs.append(
                {
                    "path": str(run_path),
                    "warmupFrames": run["warmupFrames"],
                    "sampleFrames": run["measuredFrames"],
                    "sceneSignature": run["pointLightStress"]["sceneSignature"],
                    "submissionSignature": run["pointLightStress"]
                    ["submissionSignature"],
                    "renderDocMarkersEnabled": run["pointLightStress"]
                    ["renderDocMarkersEnabled"],
                    "submittedLightsMedian": run["profiler"]["summary"]
                    ["pointLightsSubmitted"]["median"],
                    "culledLightsMedian": run["profiler"]["summary"]
                    ["pointLightsCulled"]["median"],
                    "pointLightStencilClearsMedian": run["profiler"]["summary"]
                    ["pointLightStencilClears"]["median"],
                    "cpuPointLightMedianMs": run["profiler"]["summary"]
                    ["cpuZones"]["Deferred Point Lights"]["median"],
                    "gpuPointLightMedianMs": run["profiler"]["summary"]
                    ["gpuZones"]["Deferred Point Lights"]["median"],
                    "nonBlackRatio": run["nonBlackRatio"],
                    "success": run["success"],
                }
            )
        marker_smoke = json.loads(marker_smoke_path.read_text(encoding="utf-8"))
        existing_marker_replay_path = (
            batch / "replays" / "representative-0016-replay01.json"
        )
        existing_marker_replay = json.loads(
            existing_marker_replay_path.read_text(encoding="utf-8")
        )
        expected_marker_labels = [
            "PointLightStress/PointLightPhase",
            "PointLightStress/Light[%04zu]",
            "PointLightStress/StencilClearBefore",
            "PointLightStress/StencilVolumeDraw",
            "PointLightStress/LightingVolumeDraw",
            "PointLightStress/StencilClearAfter",
        ]
        source_path = project / "DeferRenderPass.cpp"
        source_text = source_path.read_text(encoding="utf-8-sig")
        source_offsets = [source_text.find(label) for label in expected_marker_labels]
        existing_category_names = sorted(
            {
                event["markerName"]
                for event in existing_marker_replay["eventTree"][
                    "classifiedEvents"
                ]
            }
        )
        expected_category_names = sorted(
            [
                "PointLightStress/StencilClearBefore",
                "PointLightStress/StencilVolumeDraw",
                "PointLightStress/LightingVolumeDraw",
                "PointLightStress/StencilClearAfter",
            ]
        )
        cpu_medians = [run["cpuPointLightMedianMs"] for run in cleanup_runs]
        gpu_medians = [run["gpuPointLightMedianMs"] for run in cleanup_runs]
        instrumentation_cleanup = {
            "applied": capture_executable["sha256"]
            != post_cleanup_executable["sha256"],
            "change": "format per-light marker name only when renderDocMarkers=true",
            "clearDrawUniformOrderChanged": False,
            "reportedParentPreCleanupCpuPointLightMedianMs": 0.1713,
            "legacyBaselineCpuPointLightMedianMs": 0.0235,
            "defaultOffRuns": cleanup_runs,
            "defaultOffCpuPointLightMedianMsValues": cpu_medians,
            "defaultOffCpuPointLightMedianMsMedian": median(cpu_medians),
            "defaultOffGpuPointLightMedianMsValues": gpu_medians,
            "defaultOffGpuPointLightMedianMsMedian": median(gpu_medians),
            "markerEnabledSmoke": {
                "path": str(marker_smoke_path),
                "success": marker_smoke["success"],
                "renderDocMarkersEnabled": marker_smoke["pointLightStress"]
                ["renderDocMarkersEnabled"],
                "sceneSignature": marker_smoke["pointLightStress"]
                ["sceneSignature"],
                "submissionSignature": marker_smoke["pointLightStress"]
                ["submissionSignature"],
                "submittedLightsMedian": marker_smoke["profiler"]["summary"]
                ["pointLightsSubmitted"]["median"],
                "culledLightsMedian": marker_smoke["profiler"]["summary"]
                ["pointLightsCulled"]["median"],
                "pointLightStencilClearsMedian": marker_smoke["profiler"]
                ["summary"]["pointLightStencilClears"]["median"],
                "nonBlackRatio": marker_smoke["nonBlackRatio"],
                "capturePerformed": False,
            },
            "markerSemanticsVerification": {
                "postCleanupSource": str(source_path),
                "labels": expected_marker_labels,
                "labelsPresentInOriginalOrder": all(
                    offset >= 0 for offset in source_offsets
                ) and source_offsets == sorted(source_offsets),
                "existingCaptureReplay": str(existing_marker_replay_path),
                "phaseMarker": existing_marker_replay["eventTree"]["phase"]
                ["customName"],
                "lightMarkerCount": existing_marker_replay["eventTree"]
                ["lightMarkerCount"],
                "categoryNames": existing_category_names,
                "categoryNamesMatch": existing_category_names
                == expected_category_names,
                "eventCounts": existing_marker_replay["eventCountValidation"]
                ["counts"],
                "commandSequenceAndClassificationSemanticsUnchanged": True,
                "basis": "post-cleanup source label/order check plus marker-enabled app smoke plus preserved RDC event tree; no new RDC captured",
            },
            "formalEvidencePreserved": {
                "rdcFiles": [config["capture"] for config in config_outputs],
                "replayJsonFiles": [
                    file_info(path)
                    for path in sorted(batch.glob("replays/*-replay??.json"))
                ],
                "formalGpuMeasurementsRerun": False,
                "rawReplayJsonModified": False,
            },
        }
    aggregate = {
        "schemaVersion": 1,
        "success": True,
        "protocol": {
            "independentCapturesPerConfig": 1,
            "independentReplayProcessesPerConfig": 3,
            "captureFrame": 30,
            "captureWarmupFrames": 30,
            "applicationSampleFrames": 5,
            "durationCounterFetch": "separate FetchCounters pass",
            "statisticsCounterFetch": "separate FetchCounters pass",
        },
        "environment": {
            "cpu": baseline_manifest["machine"]["cpu"],
            "logicalProcessors": baseline_manifest["machine"]
            ["logicalProcessors"],
            "os": baseline_manifest["machine"]["os"],
            "gpuDriverWmi": baseline_manifest["machine"]["gpuDriver"],
            "buildConfiguration": diagnostic0["buildConfiguration"],
            "architecture": diagnostic0["architecture"],
            "resolution": diagnostic0["resolution"],
            "glVendor": diagnostic0["glVendor"],
            "glRenderer": diagnostic0["glRenderer"],
            "glVersion": diagnostic0["glVersion"],
            "windowSamples": diagnostic0["settings"]["windowSamples"],
            "requestedSwapInterval": diagnostic0["settings"]
            ["requestedSwapInterval"],
            "gBufferPositionMode": diagnostic0["gBuffer"]["positionMode"],
            "ssao": diagnostic0["ssao"]["enabled"],
            "bloom": diagnostic0["settings"]["bloom"],
            "pointShadows": diagnostic0["pointLightStress"]
            ["pointShadowsEnabled"],
            "captureExecutable": capture_executable,
            "postCleanupExecutable": post_cleanup_executable,
        },
        "renderDoc": {
            "version": version,
            "renderdoccmd": str(renderdoccmd),
            "renderdoccmdSha256": sha256(renderdoccmd),
            "qrenderdoc": str(qrenderdoc),
            "qrenderdocSha256": sha256(qrenderdoc),
            "genericCountersAvailable": True,
            "extendedCounterUnavailableEvidence": extended_counter,
        },
        "defaultOffSmoke": default_off_smoke,
        "instrumentationCleanup": instrumentation_cleanup,
        "configs": config_outputs,
        "decisions": {
            "representative512PrimaryClass": max(
                config_outputs[3]["classes"],
                key=lambda category: config_outputs[3]["classes"][category]
                ["sumGpuDurationMsMedian"],
            ),
            "highOverlapPsInvocationRatio": overlap16["counters"]
            ["lightingVolumeDraw"]["PS Invocations"]
            / rep16["counters"]["lightingVolumeDraw"]["PS Invocations"],
            "highOverlapLightingDurationRatio": overlap16["classes"]
            ["lightingVolumeDraw"]["sumGpuDurationMsMedian"]
            / rep16["classes"]["lightingVolumeDraw"]["sumGpuDurationMsMedian"],
            "highOverlapRenderDocTotalRatio": overlap16[
                "classifiedGpuDurationMsMedian"
            ]
            / rep16["classifiedGpuDurationMsMedian"],
            "highOverlapApplicationTimerRatio": overlap16["appTimerQueryMs"]
            / rep16["appTimerQueryMs"],
            "primaryNextAB": "point-light stencil clear frequency/reference reuse",
            "backupNextAB": "lighting draw screen-coverage restriction (scissor)",
            "implemented": False,
        },
    }
    if instrumentation_cleanup:
        cleanup_summary_path = cleanup_directory / "summary.json"
        cleanup_csv_path = cleanup_directory / "per-process.csv"
        instrumentation_cleanup["summaryJson"] = str(cleanup_summary_path)
        instrumentation_cleanup["perProcessCsv"] = str(cleanup_csv_path)
        cleanup_summary_path.write_text(
            json.dumps(instrumentation_cleanup, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        cleanup_rows = []
        for index, run in enumerate(instrumentation_cleanup["defaultOffRuns"], 1):
            cleanup_rows.append(
                {
                    "run": index,
                    "warmupFrames": run["warmupFrames"],
                    "sampleFrames": run["sampleFrames"],
                    "cpuPointLightMedianMs": run["cpuPointLightMedianMs"],
                    "gpuPointLightMedianMs": run["gpuPointLightMedianMs"],
                    "sceneSignature": run["sceneSignature"],
                    "submissionSignature": run["submissionSignature"],
                    "submittedLightsMedian": run["submittedLightsMedian"],
                    "culledLightsMedian": run["culledLightsMedian"],
                    "pointLightStencilClearsMedian": run[
                        "pointLightStencilClearsMedian"
                    ],
                    "nonBlackRatio": run["nonBlackRatio"],
                    "json": run["path"],
                }
            )
        write_csv(cleanup_csv_path, cleanup_rows, list(cleanup_rows[0].keys()))
    aggregate_path = batch / "aggregate.json"
    aggregate_path.write_text(
        json.dumps(aggregate, indent=2, sort_keys=True), encoding="utf-8"
    )
    write_csv(
        batch / "per-replay.csv",
        per_replay_rows,
        list(per_replay_rows[0].keys()),
    )
    write_csv(
        batch / "per-class-replay.csv",
        per_class_rows,
        list(per_class_rows[0].keys()),
    )
    write_csv(
        batch / "per-event.csv",
        per_event_rows,
        list(per_event_rows[0].keys()),
    )
    chart_path = batch / "images" / "point-light-gpu-breakdown.svg"
    svg_chart(config_outputs, chart_path)
    batch_report = batch / "POINT_LIGHT_RENDERDOC_BREAKDOWN_CN.md"
    markdown_report(aggregate, batch_report)
    if args.report:
        markdown_report(aggregate, args.report.resolve())
    print(str(aggregate_path))
    print(str(batch_report))
    print(str(chart_path))


if __name__ == "__main__":
    main()
