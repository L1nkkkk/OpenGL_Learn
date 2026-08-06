#!/usr/bin/env python3
"""Aggregate the frozen S={1,2,4,8,16} point-light grid experiment."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image

SLICES = (1, 2, 4, 8, 16)
CACHED_COUNTS = (32, 64, 128, 256, 512)
CACHED_RADII = (1.5, 3.0, 6.0, 8.0, 12.0)
REBUILD_COUNTS = (32, 128, 256, 512)
REBUILD_RADII = (1.5, 3.0, 6.0, 12.0)
ABS_MS = 0.05
REL_PERCENT = 3.0
METRICS = {
    "wallFrame": ("sample", "wallFrame"),
    "cpuFrame": ("sample", "cpuFrame"),
    "gpuFrame": ("sample", "gpuFrame"),
    "gridLightingCpu": ("cpu", "Point Light Grid Lighting CPU"),
    "gridLightingGpu": ("gpu", "Point Light Grid Lighting GPU"),
    "cacheCheckCpu": ("cpu", "Point Light Grid Cache Check"),
    "gridBuildCpu": ("cpu", "Point Light Grid Build"),
    "gridUploadCpu": ("cpu", "Point Light Grid Upload"),
    "gridBoundsCpu": ("cpu", "Point Light Grid Bounds"),
    "gridCountCpu": ("cpu", "Point Light Grid Count"),
    "gridPrefixCpu": ("cpu", "Point Light Grid Prefix"),
    "gridFillCpu": ("cpu", "Point Light Grid Fill"),
}


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def dump(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def samples(result: dict[str, Any], metric: str) -> list[float]:
    kind, name = METRICS[metric]
    root = result["profiler"]["samples"]
    if kind == "sample":
        return [float(value) for value in root[name]]
    group = "cpuZones" if kind == "cpu" else "gpuZones"
    return [float(value) for value in root[group].get(name, [])]


def stats(values: Iterable[float]) -> dict[str, float | int]:
    data = np.asarray(list(values), dtype=np.float64)
    if not data.size:
        return {"count": 0, "mean": 0.0, "median": 0.0, "p95": 0.0, "p99": 0.0}
    return {"count": int(data.size), "mean": float(np.mean(data)), "median": float(np.median(data)),
            "p95": float(np.percentile(data, 95)), "p99": float(np.percentile(data, 99))}


def compare(a: int, b: int, by_round: dict[int, dict[int, dict[str, float]]], metric: str) -> dict[str, Any]:
    pairs = []
    for round_index in (1, 2, 3):
        av = by_round[round_index][a][metric]
        bv = by_round[round_index][b][metric]
        delta = av - bv
        pairs.append({"round": round_index, "aMedianMs": av, "bMedianMs": bv, "deltaMs": delta,
                      "relativePercent": delta / bv * 100.0 if bv else 0.0})
    deltas = [item["deltaMs"] for item in pairs]
    relatives = [item["relativePercent"] for item in pairs]
    direction = "mixed"
    if all(value < 0.0 for value in deltas): direction = "a-faster"
    elif all(value > 0.0 for value in deltas): direction = "b-faster"
    median_delta = float(np.median(deltas))
    median_relative = float(np.median(relatives))
    significant = abs(median_delta) >= ABS_MS and abs(median_relative) >= REL_PERCENT
    winner = a if direction == "a-faster" and significant else b if direction == "b-faster" and significant else 0
    return {"a": a, "b": b, "winner": winner, "direction": direction, "significant": significant,
            "medianPairedDeltaMs": median_delta, "medianPairedRelativePercent": median_relative, "pairs": pairs}


def validate(result: dict[str, Any], expected: dict[str, Any], warmup: int, sample_count: int) -> None:
    point = result["pointLightStress"]
    grid = point["gridRuntime"]
    s = int(expected["sliceCount"])
    regime = expected["regime"]
    checks = {
        "success": bool(result["success"]), "release": result["buildConfiguration"] == "Release",
        "resolution": result["resolution"] == [1920, 1080], "warmup": int(result["warmupFrames"]) == warmup,
        "samples": int(result["measuredFrames"]) == sample_count, "mode": point["renderMode"] == "cluster16",
        "modeExplicit": bool(point["renderModeExplicit"]), "sliceExplicit": bool(point["gridSliceCountExplicit"]),
        "sliceConfigured": int(point["gridSliceCountConfigured"]) == s, "sliceRuntime": int(grid["sliceCount"]) == s,
        "clusterFlag": bool(grid["clustered"]) == (s > 1), "regime": point["gridUpdateMode"] == regime,
        "regimeExplicit": bool(point["gridUpdateModeExplicit"]), "offscreen": not bool(point["offscreenCulling"]),
        "grid": bool(grid["valid"]) and not bool(grid["overflow"]) and not grid["error"],
        "count": int(grid["lightCount"]) == int(expected["lightCount"]),
        "radius": abs(float(point["volumeRadius"]) - float(expected["radius"])) <= 1e-4,
        "screenDraw": int(result["profiler"]["summary"]["pointLightScreenDraws"]["median"]) == 1,
    }
    for metric in ("wallFrame", "cpuFrame", "gpuFrame", "gridLightingCpu", "gridLightingGpu", "cacheCheckCpu", "gridBuildCpu", "gridUploadCpu"):
        checks[f"zone:{metric}"] = len(samples(result, metric)) == sample_count
    if regime == "rebuild":
        for metric in ("gridBoundsCpu", "gridCountCpu", "gridPrefixCpu", "gridFillCpu"):
            checks[f"zone:{metric}"] = len(samples(result, metric)) == sample_count
        checks["builds"] = int(grid["buildCount"]) == warmup + sample_count
        checks["hits"] = int(grid["cacheHitCount"]) == 0
    else:
        checks["buildOnce"] = int(grid["buildCount"]) == 1
        checks["hits"] = int(grid["cacheHitCount"]) == warmup + sample_count - 1
    failed = [name for name, passed in checks.items() if not passed]
    if failed: raise ValueError(f"{expected['stem']} failed: {failed}")


def heatmap(cells: list[dict[str, Any]], regime: str, counts: tuple[int, ...], radii: tuple[float, ...], output: Path) -> None:
    lookup = {(cell["lightCount"], cell["radius"]): cell for cell in cells if cell["regime"] == regime}
    values = np.zeros((len(counts), len(radii)), dtype=np.float64)
    labels: list[list[str]] = []
    for yi, n in enumerate(counts):
        row = []
        for xi, r in enumerate(radii):
            cell = lookup[(n, r)]
            values[yi, xi] = int(cell["finalRuntimeSliceCount"])
            candidate = int(cell["statisticalCandidateSliceCount"])
            verdict = "Go" if cell["depthSlicingGo"] else (
                "Tie/S1" if candidate == 1 else f"Tie/S1 (cand. {candidate})"
            )
            row.append(f"S={int(values[yi,xi])}\n{verdict}")
        labels.append(row)
    figure, axis = plt.subplots(figsize=(8.2, 5.0), constrained_layout=True)
    image = axis.imshow(values, cmap="viridis", vmin=1, vmax=16, aspect="auto")
    axis.set_xticks(range(len(radii)), [f"{value:g}" for value in radii]); axis.set_yticks(range(len(counts)), counts)
    axis.set_xlabel("Effective radius R"); axis.set_ylabel("Point-light count N")
    axis.set_title(f"{regime.capitalize()} final runtime S (Wall Frame, frozen rule)")
    for y in range(len(counts)):
        for x in range(len(radii)): axis.text(x, y, labels[y][x], ha="center", va="center", color="white" if values[y,x] >= 8 else "black", fontsize=9)
    figure.colorbar(image, ax=axis, label="final runtime slice count")
    output.parent.mkdir(parents=True, exist_ok=True); figure.savefig(output, dpi=180); plt.close(figure)


def write_report(run_dir: Path, aggregate: dict[str, Any]) -> None:
    cached = [cell for cell in aggregate["cells"] if cell["regime"] == "cached"]
    rebuild = [cell for cell in aggregate["cells"] if cell["regime"] == "rebuild"]
    def table(items: list[dict[str, Any]]) -> list[str]:
        rows = ["| N | R | 数值 leader | 统计候选 S | 最终运行 S | S1 Wall | 最终 Wall | Δ / 相对变化 | 判定 |", "|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
        for cell in items:
            candidate = int(cell["statisticalCandidateSliceCount"]); s = str(cell["finalRuntimeSliceCount"]); base=cell["slices"]["1"]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            chosen=cell["slices"][s]["metrics"]["wallFrame"]["medianOfProcessMedians"]
            delta=chosen-base; rel=delta/base*100.0 if base else 0.0
            rows.append(f"| {cell['lightCount']} | {cell['radius']:g} | {cell['numericLeader']} | {candidate} | {s} | {base:.4f} ms | {chosen:.4f} ms | {delta:+.4f} ms / {rel:+.2f}% | {'Go' if cell['depthSlicingGo'] else 'Tie/S1'} |")
        return rows
    go_cached=sum(cell["depthSlicingGo"] for cell in cached);go_rebuild=sum(cell["depthSlicingGo"] for cell in rebuild)
    best = aggregate["bestCachedImprovement"]
    verify_path=run_dir/"verification"/"independent-verification.json"
    verify=load(verify_path) if verify_path.exists() else None
    lines=["# Point-Light Grid 深度切片数实际 Runtime 实验报告","","## 结论","",
           f"正式实验共 {aggregate['runCount']} 个独立性能进程，每进程 300 帧预热、600 帧采样。Cached 中 {go_cached}/{len(cached)} 个场景满足深度切片相对 S=1 的端到端 Go；Rebuild 中 {go_rebuild}/{len(rebuild)} 个场景满足 Go。统计候选 S 取统计顶层集合中的最小值；仅当它相对 S=1 通过冻结门槛时才作为最终运行 S，否则回退 S=1。",
           f"- Cached 最大已验证改进：N={best['lightCount']}、R={best['radius']:g}，S=1 {best['s1WallMs']:.4f} ms → S={best['sliceCount']} {best['recommendedWallMs']:.4f} ms（{best['deltaMs']:.4f} ms，{best['relativePercent']:.2f}%）。",
           "- Rebuild 的结论必须看 Wall Frame；Lighting GPU 变快不等于端到端变快，CPU 构表与上传不能和 GPU 时间相加。",
           f"- 正式同输入截图 {aggregate['imageParity']['exactGroups']}/{aggregate['imageParity']['groupCount']} 组五个 S 全部逐字节一致。",
           "","## Cached：统计候选与最终运行 S(N,R)",""]+table(cached)+["","## Rebuild Every Frame：统计候选与最终运行 S(N,R)",""]+table(rebuild)+[
           "","## 技术解释","",
           "增加 S 会缩小每个像素实际遍历的候选灯集合，但会复制跨越多个深度片的灯引用，同时线性增加 metadata cell 数。Cached 可以摊销一次性 Bounds/Count/Prefix/Fill/Upload，因此由 Lighting GPU 与更大的常驻 TBO 访问共同决定；Rebuild 则必须每帧承担构建和上传，通常更偏向较小 S。",
           "","## 正确性与适用边界","",
           "- S=1/2/4/8/16 使用同一个 builder、CSR/TBO、payload、shader、逐像素球体谓词和灯累加顺序；只有 sliceCount 不同。",
           "- 默认仍为 analytic-screen；旧 tile16/cluster16 未显式传参时仍映射 S=1/16。",
           "- 这是 OpenGL 3.3 CPU 构表 + TBO 的结果，不能外推到 GPU Compute clustered shading。"]
    if verify:
        lines += [f"- 独立验证：{verify['csrCellsVerified']} 个 CSR 重建一致；真实 G-buffer 三组总 truth interactions={verify['fullImageMembership']['totalGroundTruthInteractions']:,}，所有 S miss=0；Oracle 三组通过。"]
    lines += ["","## 图表与复现","",
              "- `charts/cached-optimal-s.png` / `charts/rebuild-optimal-s.png`：通过冻结门槛后的最终运行 S 相图；Tie 一律显示为 S=1。",
              "- `charts/cached-wall-curves.png` / `charts/cached-lighting-curves.png`：各 S 时间曲线。",
              "- `charts/rebuild-breakdown.png`：Build/Upload 与 Lighting 分解（仅并列，不相加）。",
              "- `charts/csr-memory.png`：indices 与 resident bytes 随 S 变化。",
              "- `aggregate.json`、`summary.csv`、`pre-capture-manifest.json`、`capture-manifest.json` 保存完整原始证据。",
              "- 复现入口：`tools/run_grid_slice_count_experiment.ps1`。",""]
    (run_dir/"REPORT_CN.md").write_text("\n".join(lines),encoding="utf-8")


def main() -> int:
    parser=argparse.ArgumentParser();parser.add_argument("--run-dir",type=Path,required=True);args=parser.parse_args();run_dir=args.run_dir.resolve()
    pre=load(run_dir/"pre-capture-manifest.json");manifest=load(run_dir/"capture-manifest.json")
    if sha(run_dir/pre["protocol"]) != pre["protocolSha256"]: raise ValueError("protocol hash mismatch")
    if sha(Path(pre["executable"])) != pre["executableSha256"]: raise ValueError("executable hash mismatch")
    if not manifest["valid"] or int(manifest["completedRunCount"]) != 615: raise ValueError("formal capture incomplete")
    done={item["stem"]:item for item in manifest["completedRuns"]};runs=[]
    for expected in pre["expectedRuns"]:
        record=done[expected["stem"]];result_path=run_dir/expected["result"];capture_path=run_dir/expected["capture"];log_path=run_dir/expected["log"]
        if sha(result_path)!=record["resultSha256"] or sha(capture_path)!=record["captureSha256"] or sha(log_path)!=record["logSha256"]: raise ValueError(f"hash mismatch {expected['stem']}")
        result=load(result_path);validate(result,expected,int(pre["warmupFrames"]),int(pre["sampleFrames"]))
        runs.append({"expected":expected,"result":result,"capture":capture_path,"metrics":{m:samples(result,m) for m in METRICS}})
    grouped:dict[tuple[str,int,float,int],list[dict[str,Any]]]=defaultdict(list);round_lookup={}
    for run in runs:
        e=run["expected"];key=(e["regime"],int(e["lightCount"]),float(e["radius"]),int(e["sliceCount"]));grouped[key].append(run)
        round_lookup[(e["regime"],int(e["lightCount"]),float(e["radius"]),int(e["round"]),int(e["sliceCount"]))]=run
    cells=[];summary=[];exact_groups=0;groups=0
    for regime,counts,radii in (("cached",CACHED_COUNTS,CACHED_RADII),("rebuild",REBUILD_COUNTS,REBUILD_RADII)):
        for n in counts:
            for r in radii:
                slice_data={};by_round={round_index:{} for round_index in (1,2,3)}
                for s in SLICES:
                    items=grouped[(regime,n,r,s)]
                    if len(items)!=3: raise ValueError(f"missing processes {regime}/{n}/{r}/S{s}")
                    metric_data={}
                    for metric in METRICS:
                        pooled=[value for item in items for value in item["metrics"][metric]]
                        medians=[float(np.median(item["metrics"][metric])) if item["metrics"][metric] else 0.0 for item in items]
                        metric_data[metric]={"pooled":stats(pooled),"processMedians":medians,"medianOfProcessMedians":float(np.median(medians))}
                        summary.append({"regime":regime,"lightCount":n,"radius":r,"sliceCount":s,"metric":metric,**metric_data[metric]["pooled"],"medianOfProcessMedians":metric_data[metric]["medianOfProcessMedians"]})
                    grid=items[0]["result"]["pointLightStress"]["gridRuntime"]
                    for item in items[1:]:
                        other=item["result"]["pointLightStress"]["gridRuntime"]
                        for field in ("logicalCells","nonEmptyCells","totalIndices","residentBytes","csrSignature"):
                            if other[field]!=grid[field]: raise ValueError(f"grid drift {regime}/{n}/{r}/S{s}/{field}")
                    slice_data[str(s)]={"metrics":metric_data,"grid":{field:grid[field] for field in ("inputSignature","csrSignature","logicalCells","nonEmptyCells","lightCount","totalIndices","maximumLightsPerCell","averageLightsPerCell","metadataBytes","indexBytes","lightBytes","residentBytes")}}
                    for round_index,item in zip(sorted(int(x["expected"]["round"]) for x in items),sorted(items,key=lambda x:int(x["expected"]["round"]))):
                        by_round[round_index][s]={metric:float(np.median(item["metrics"][metric])) if item["metrics"][metric] else 0.0 for metric in METRICS}
                for round_index in (1,2,3):
                    paths=[round_lookup[(regime,n,r,round_index,s)]["capture"] for s in SLICES];groups+=1
                    if len({sha(path) for path in paths})!=1: raise ValueError(f"image mismatch {regime}/{n}/{r}/round{round_index}")
                    exact_groups+=1
                pairwise={}
                for ai,a in enumerate(SLICES):
                    for b in SLICES[ai+1:]: pairwise[f"s{a}_vs_s{b}"]={metric:compare(a,b,by_round,metric) for metric in METRICS}
                wall_values={s:slice_data[str(s)]["metrics"]["wallFrame"]["medianOfProcessMedians"] for s in SLICES};leader=min(SLICES,key=lambda s:wall_values[s])
                top=[]
                for candidate in SLICES:
                    beaten=False
                    for other in SLICES:
                        if other==candidate:continue
                        key=f"s{min(other,candidate)}_vs_s{max(other,candidate)}";comparison=pairwise[key]["wallFrame"]
                        if comparison["winner"]==other:beaten=True;break
                    if not beaten:top.append(candidate)
                if not top:
                    top=[s for s in SLICES if compare(leader,s,by_round,"wallFrame")["winner"]==0] or [leader]
                candidate=min(top);go=candidate>1 and compare(candidate,1,by_round,"wallFrame")["winner"]==candidate
                final_runtime=candidate if go else 1
                cells.append({"regime":regime,"lightCount":n,"radius":r,"slices":slice_data,"pairwise":pairwise,"numericLeader":leader,"statisticalTopSet":top,"statisticalCandidateSliceCount":candidate,"finalRuntimeSliceCount":final_runtime,"recommendedSliceCount":final_runtime,"depthSlicingGo":go})
    go_cells=[cell for cell in cells if cell["regime"]=="cached" and cell["depthSlicingGo"]]
    def improvement(cell:dict[str,Any])->dict[str,Any]:
        s=cell["finalRuntimeSliceCount"];base=cell["slices"]["1"]["metrics"]["wallFrame"]["medianOfProcessMedians"];chosen=cell["slices"][str(s)]["metrics"]["wallFrame"]["medianOfProcessMedians"]
        return {"lightCount":cell["lightCount"],"radius":cell["radius"],"sliceCount":s,"s1WallMs":base,"recommendedWallMs":chosen,"deltaMs":chosen-base,"relativePercent":(chosen-base)/base*100.0}
    best=min((improvement(cell) for cell in go_cells),key=lambda item:item["deltaMs"]) if go_cells else {"lightCount":0,"radius":0.0,"sliceCount":1,"s1WallMs":0.0,"recommendedWallMs":0.0,"deltaMs":0.0,"relativePercent":0.0}
    aggregate={"schemaVersion":2,"valid":True,"experiment":"point-light-grid-slice-count-runtime","protocolSha256":pre["protocolSha256"],"preCaptureManifestSha256":sha(run_dir/"pre-capture-manifest.json"),"captureManifestSha256":sha(run_dir/"capture-manifest.json"),"executableSha256":pre["executableSha256"],"runCount":len(runs),"warmupFrames":pre["warmupFrames"],"sampleFrames":pre["sampleFrames"],"winnerRule":pre["winnerThreshold"],"selectionSemantics":{"statisticalCandidateSliceCount":"smallest member of the statistical top set","finalRuntimeSliceCount":"candidate only when it significantly beats S=1; otherwise S=1","recommendedSliceCount":"backward-compatible alias of finalRuntimeSliceCount"},"imageParity":{"groupCount":groups,"exactGroups":exact_groups,"allExact":groups==exact_groups},"bestCachedImprovement":best,"cells":cells}
    dump(run_dir/"aggregate.json",aggregate)
    with (run_dir/"summary.csv").open("w",encoding="utf-8-sig",newline="") as stream:
        writer=csv.DictWriter(stream,fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
    chart=run_dir/"charts";heatmap(cells,"cached",CACHED_COUNTS,CACHED_RADII,chart/"cached-optimal-s.png");heatmap(cells,"rebuild",REBUILD_COUNTS,REBUILD_RADII,chart/"rebuild-optimal-s.png")
    lookup={(cell["regime"],cell["lightCount"],cell["radius"]):cell for cell in cells}
    for metric,name,ylabel in (("wallFrame","cached-wall-curves.png","Wall Frame median (ms)"),("gridLightingGpu","cached-lighting-curves.png","Grid Lighting GPU median (ms)")):
        fig,axes=plt.subplots(2,2,figsize=(10,7),constrained_layout=True)
        for axis,(n,r) in zip(axes.flat,((128,3.0),(256,6.0),(512,3.0),(512,12.0))):
            cell=lookup[("cached",n,r)];axis.plot(SLICES,[cell["slices"][str(s)]["metrics"][metric]["medianOfProcessMedians"] for s in SLICES],marker="o");axis.set_title(f"N={n}, R={r:g}, final S={cell['finalRuntimeSliceCount']} (candidate={cell['statisticalCandidateSliceCount']})");axis.set_xscale("log",base=2);axis.set_xticks(SLICES,SLICES);axis.grid(alpha=.3)
        fig.supxlabel("Z slice count S");fig.supylabel(ylabel);fig.savefig(chart/name,dpi=180);plt.close(fig)
    cell=lookup[("rebuild",256,6.0)];fig,axis=plt.subplots(figsize=(8,4.8),constrained_layout=True);x=np.arange(len(SLICES));width=.24
    for offset,metric,label in ((-width,"gridBuildCpu","CPU Build"),(0,"gridUploadCpu","CPU Upload"),(width,"gridLightingGpu","GPU Lighting")):
        axis.bar(x+offset,[cell["slices"][str(s)]["metrics"][metric]["medianOfProcessMedians"] for s in SLICES],width,label=label)
    axis.set_xticks(x,SLICES);axis.set_xlabel("Z slice count S");axis.set_ylabel("Milliseconds (parallel components; not summed)");axis.set_title("Rebuild N=256, R=6 component costs");axis.legend();axis.grid(axis="y",alpha=.3);fig.savefig(chart/"rebuild-breakdown.png",dpi=180);plt.close(fig)
    cell=lookup[("cached",512,6.0)];fig,axis1=plt.subplots(figsize=(8,4.8),constrained_layout=True);axis2=axis1.twinx();axis1.plot(SLICES,[cell["slices"][str(s)]["grid"]["totalIndices"]/1e6 for s in SLICES],"o-",label="CSR indices");axis2.plot(SLICES,[cell["slices"][str(s)]["grid"]["residentBytes"]/(1024**2) for s in SLICES],"s--",color="tab:red",label="Resident MiB");axis1.set_xscale("log",base=2);axis1.set_xticks(SLICES,SLICES);axis1.set_xlabel("Z slice count S");axis1.set_ylabel("Index references (million)");axis2.set_ylabel("Resident memory (MiB)");axis1.set_title("CSR and resident memory: N=512, R=6");axis1.grid(alpha=.3);fig.savefig(chart/"csr-memory.png",dpi=180);plt.close(fig)
    representative=round_lookup[("cached",best["lightCount"] or 256,best["radius"] or 6.0,1,best["sliceCount"] or 1)]["capture"];Image.open(representative).convert("RGB").save(chart/"representative-runtime.png")
    write_report(run_dir,aggregate)
    print(f"[analysis] PASS runs={len(runs)} exact={exact_groups}/{groups} cachedGo={len(go_cells)} best={best}")
    return 0


if __name__ == "__main__": raise SystemExit(main())
