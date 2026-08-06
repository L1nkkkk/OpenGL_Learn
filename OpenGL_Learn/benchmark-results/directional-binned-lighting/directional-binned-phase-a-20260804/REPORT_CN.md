# Directional-Binned Tile/Cluster Lighting 可证伪实验报告

- 日期：2026-08-04
- 阶段：Phase A 离线诊断
- Control：当前默认 `analytic-screen` / 逐灯三维解析球 Oracle
- 抓取 EXE SHA-256：`EA40321A9CDE239AE223C1FBFFD43DCDE560CEC562B982CD4EF8069B4DBB4BB6`
- Phase 0 协议 SHA-256：`8C04EBE31B3A801BC6CAB47331C2E5D21BB3F99E058E3EC7608D9DB2CACE5326`
- 分辨率：640×360（feasibility，不是正式性能分辨率）
- 默认值：未改变，仍为固定 `analytic-screen`

## 1. 结论

| 方案 | Phase A 决策 | 静态质量 | 连续帧质量 | Work 门槛 | 1080p 内存门槛 |
|---|---|---:|---:|---:|---:|
| 原始 U8 | **No-Go** | Fail | Fail | Pass | Pass |
| Hybrid H8 | **No-Go** | Fail | Fail | Fail | Pass |

Phase B **未进入**：两个候选均未通过全部预设门槛；按阶段门控停止大规模 Runtime 改造，也没有为了实现而更改门槛。

关键可证伪结果（captured-specular）：

- U8 representative：Abs P99=0.118842，affected=88.296%；high-overlap：Abs P99=1.169863，affected=97.120%；
- H8 representative：Abs P99=0.014486，affected=27.546%，work ratio=1.066；high-overlap：Abs P99=0.309818，affected=65.838%，work ratio=0.633；
- H8 在 small-local 的零误差不是聚合成功：eligible=0，全部 1,057 个 cluster-light membership 走 exact，work 是 Oracle 球内 interaction 的 13.559×；
- U8 虽在 representative/high-overlap 的理论 work ratio 为 0.205/0.044，但质量先失败，因此这些比例不能被当成可实现的性能收益。

本轮不把 U8/H8 称为 exact、Adaptive 或已经完成的 Tiled/Clustered。没有 Runtime Candidate，因此没有可计时的 build/upload/resolve，也不产生伪造的 GPU 性能数字或 RenderDoc Candidate 帧。

## 2. Exact Oracle 与语义对齐

Oracle 读取本轮 Release 程序抓取的实际 explicit `gPosition`、validity alpha、normal、albedo、material RGB/alpha；天空先跳过。随后逐灯执行当前 shader 的三维球 predicate、distance attenuation、Lambert diffuse 和 Blinn-Phong specular，再在线性 HDR 中相加。压力场景点光 ambient、点阴影、SSAO、方向灯和聚光灯均为 0/off。

已验收的旧证据 `point-light-screen-routing/screen-bounds-scissor-analytic-20260804/REPORT_CN.md` 已证明当前 `analytic-screen` 对 per-light fullscreen Oracle 静态逐像素一致。本轮没有重新定义 Oracle，也没有用最终 8-bit 截图替代 HDR 比较。

CPU Oracle 另经过当前固定后处理（HDR off、gamma=2.2）与本轮 app Analytic Screen LDR 逐通道比对：

| Coverage | Channel mean / P95 / P99 / max（8-bit LSB） | 不同像素 | >1 LSB 通道 | Bulk sanity |
|---|---:|---:|---:|---:|
| small-local | 0.010503 / 0.000000 / 1.000000 / 1.000000 | 6931 / 3.008% | 0 / 0.000% | Pass |
| medium-local | 0.035185 / 0.000000 / 1.000000 / 4.000000 | 23449 / 10.178% | 2 / 0.000% | Pass |
| representative | 0.041808 / 0.000000 / 1.000000 / 1.000000 | 27732 / 12.036% | 0 / 0.000% | Pass |
| high-overlap | 0.073514 / 1.000000 / 1.000000 / 1.000000 | 46785 / 20.306% | 0 / 0.000% | Pass |

该 sanity 只用于排除 CPU Oracle 的整体实现偏差，不是候选质量门槛：要求通道误差 P99≤1 LSB、>1 LSB 通道≤0.01%、max≤8 LSB。捕获 JSON 将灯参数序列化为六位小数，极少数恰在解析球边界的像素可因命中集合移动而超过 1 LSB；原始计数与 max 均保留在 `aggregate.json`。

无效/天空语义 smoke：从 representative G-Buffer 额外标记并以 NaN 毒化 21023 个像素；Exact/U8/H8 在这些像素的最大绝对输出均为 0，完整输出 finite，结果 **Pass**。固定相机本身恰好没有天空像素，因此这项 smoke 只验证‘先跳过再读取’代码契约，不冒充真实天空画质场景。原始结果见 [`invalid-pixel-semantic-smoke.json`](invalid-pixel-semantic-smoke.json)。

## 3. 静态 HDR 质量

误差均为 tone mapping 前。Abs 是逐像素 RGB 最大通道绝对误差；Rel 是 Oracle luminance>1e-3 像素的相对亮度误差。

| Coverage / 灯数 | 材质 | 方案 | Abs mean / P95 / P99 / max | Rel P95 / P99 | Affected | Miss / Leak | 判定 |
|---|---|---|---|---|---:|---:|---:|
| small-local / 64 | diffuse-only | U8 | 0.000808 / 0.004457 / 0.008089 / 0.036166 | 100.000% / 263.352% | 19.013% | 0.008% / 0.000% | Fail |
| small-local / 64 | diffuse-only | H8 | 0.000000 / 0.000000 / 0.000000 / 0.000000 | 0.000% / 0.000% | 0.000% | 0.000% / 0.000% | Pass |
| small-local / 64 | captured-specular | U8 | 0.000809 / 0.004464 / 0.008102 / 0.036866 | 100.000% / 246.399% | 19.056% | 0.009% / 0.000% | Fail |
| small-local / 64 | captured-specular | H8 | 0.000000 / 0.000000 / 0.000000 / 0.000000 | 0.000% / 0.000% | 0.000% | 0.000% / 0.000% | Pass |
| medium-local / 64 | diffuse-only | U8 | 0.004510 / 0.015074 / 0.019055 / 0.088113 | 230.021% / 317.248% | 81.488% | 1.302% / 0.012% | Fail |
| medium-local / 64 | diffuse-only | H8 | 0.000221 / 0.001353 / 0.001790 / 0.004828 | 23.349% / 56.121% | 20.491% | 0.000% / 0.000% | Fail |
| medium-local / 64 | captured-specular | U8 | 0.004498 / 0.015086 / 0.019083 / 0.089686 | 229.164% / 315.127% | 81.567% | 1.323% / 0.012% | Fail |
| medium-local / 64 | captured-specular | H8 | 0.000214 / 0.001343 / 0.001785 / 0.004841 | 23.224% / 56.119% | 20.175% | 0.000% / 0.000% | Fail |
| representative / 256 | diffuse-only | U8 | 0.033544 / 0.090291 / 0.118560 / 0.341185 | 111.711% / 157.422% | 88.683% | 12.606% / 0.000% | Fail |
| representative / 256 | diffuse-only | H8 | 0.002749 / 0.009149 / 0.014642 / 0.026345 | 12.743% / 22.066% | 28.448% | 0.000% / 0.000% | Fail |
| representative / 256 | captured-specular | U8 | 0.033560 / 0.090463 / 0.118842 / 0.346504 | 111.651% / 157.245% | 88.296% | 12.589% / 0.000% | Fail |
| representative / 256 | captured-specular | H8 | 0.002607 / 0.009102 / 0.014486 / 0.026119 | 12.675% / 21.723% | 27.546% | 0.000% / 0.000% | Fail |
| high-overlap / 256 | diffuse-only | U8 | 0.213856 / 0.598142 / 1.164642 / 3.045362 | 263.827% / 312.566% | 97.099% | 9.355% / 0.000% | Fail |
| high-overlap / 256 | diffuse-only | H8 | 0.102562 / 0.281077 / 0.311690 / 0.379650 | 211.584% / 1298.035% | 79.178% | 0.000% / 0.103% | Fail |
| high-overlap / 256 | captured-specular | U8 | 0.212937 / 0.598433 / 1.169863 / 3.081505 | 261.818% / 309.449% | 97.120% | 9.337% / 0.000% | Fail |
| high-overlap / 256 | captured-specular | H8 | 0.095261 / 0.279701 / 0.309818 / 0.377261 | 209.625% / 1293.376% | 65.838% | 0.000% / 0.103% | Fail |

### 3.1 深度不连续与点光球边缘

| Coverage / 材质 / 方案 | Depth P99 / affected | Sphere-edge P99 / affected |
|---|---:|---:|
| small-local / diffuse-only / U8 | 0.011941 / 16.062% | 0.009795 / 41.819% |
| small-local / diffuse-only / H8 | 0.000000 / 0.000% | 0.000000 / 0.000% |
| small-local / captured-specular / U8 | 0.011992 / 16.199% | 0.009815 / 41.976% |
| small-local / captured-specular / H8 | 0.000000 / 0.000% | 0.000000 / 0.000% |
| medium-local / diffuse-only / U8 | 0.031344 / 74.894% | 0.017455 / 84.926% |
| medium-local / diffuse-only / H8 | 0.000000 / 0.000% | 0.001770 / 23.576% |
| medium-local / captured-specular / U8 | 0.031437 / 74.947% | 0.017479 / 84.983% |
| medium-local / captured-specular / H8 | 0.000000 / 0.000% | 0.001768 / 23.470% |
| representative / diffuse-only / U8 | 0.145936 / 86.788% | 0.119015 / 88.422% |
| representative / diffuse-only / H8 | 0.001230 / 0.000% | 0.014770 / 29.602% |
| representative / captured-specular / U8 | 0.146504 / 86.703% | 0.119280 / 88.035% |
| representative / captured-specular / H8 | 0.000000 / 0.000% | 0.014605 / 28.662% |
| high-overlap / diffuse-only / U8 | 1.556794 / 97.225% | 0.955027 / 97.097% |
| high-overlap / diffuse-only / H8 | 0.037278 / 14.088% | 0.312160 / 81.542% |
| high-overlap / captured-specular / U8 | 1.561194 / 97.267% | 0.959100 / 97.120% |
| high-overlap / captured-specular / H8 | 0.000610 / 0.000% | 0.310428 / 67.804% |

每个静态组合都保存了 exact/candidate HDR PFM、absolute HDR diff PFM、tone-mapped PNG、固定量程 heatmap 和 signed luminance 伪彩图。图像目录为 [`images`](images/)，HDR 目录为 [`hdr`](hdr/)。原程序 Analytic Screen LDR 与完整 G-Buffer 抓取保留在 [`captures`](captures/)。

## 4. 压缩、分类与方向量化

Work-equivalent 不是 GPU 时间：Exact 计实际球内 shading interaction；U8 为每个有效像素固定 8 次；H8 为固定 8 次加 cluster exact-list 长度。只有质量先通过时，该数值才有资格作为进入 Runtime 的可行性门槛。

| Coverage / 材质 | Exact interactions | U8 work / ratio | H8 work / ratio | H8 partial / full / far / eligible / exact |
|---|---:|---:|---:|---:|
| small-local / diffuse-only | 149839 | 1843200 / 12.301 | 2031724 / 13.559 | 821 / 236 / 0 / 0 / 1057 |
| small-local / captured-specular | 149839 | 1843200 / 12.301 | 2031724 / 13.559 | 821 / 236 / 0 / 0 / 1057 |
| medium-local / diffuse-only | 1164658 | 1843200 / 1.583 | 2939700 / 2.524 | 2868 / 4436 / 1098 / 1098 / 6206 |
| medium-local / captured-specular | 1164658 | 1843200 / 1.583 | 2949805 / 2.533 | 2868 / 4436 / 1098 / 1046 / 6258 |
| representative / diffuse-only | 8983583 | 1843200 / 0.205 | 9482927 / 1.056 | 15063 / 39645 / 11410 / 11410 / 43298 |
| representative / captured-specular | 8983583 | 1843200 / 0.205 | 9573761 / 1.066 | 15063 / 39645 / 11410 / 10865 / 43843 |
| high-overlap / diffuse-only | 41885657 | 1843200 / 0.044 | 24742093 / 0.591 | 14999 / 230129 / 107933 / 107933 / 137195 |
| high-overlap / captured-specular | 41885657 | 1843200 / 0.044 | 26504043 / 0.633 | 14999 / 230129 / 107933 / 98026 / 147102 |

U8 每 tile 的 exact light count、occupied bin count 和方向误差在各 `*-u8-tile-stats.csv`。H8 每 cluster 的 partial/full/far/eligible/exact、specular fallback 和方向误差在各 `*-h8-cluster-stats.csv`。汇总入口为 [`static-summary.csv`](static-summary.csv)。

H8 的 `full` 是完整 Tile×depth-slice frustum 八角点都在解析光球内，不是二维 Screen Rect 覆盖。`far` 还要求距离≥4×cluster radius 且八角 attenuation max/min≤1.10。captured specular 中 `material.b>0.02` 的 cluster 走 exact fallback；质量恢复如果伴随 exact pool 膨胀，会在 Work 与内存门槛中如实失败。

| Coverage | U8 exact lights/tile P50/P95/max | U8 bins/tile P50/P95/max | H8 exact lights/cluster P50/P95/max | H8 bins/cluster P50/P95/max | U8 dir error P50/P95/max | H8 reconstructed dir error P50/P95/max |
|---|---:|---:|---:|---:|---:|---:|
| small-local | 1.00/3.00/4.00 | 1.00/3.00/4.00 | 0.00/3.00/4.00 | 0.00/0.00/0.00 | 44.28/114.39/159.41 | 0.00/0.00/0.00 |
| medium-local | 6.00/10.00/20.00 | 4.00/7.00/8.00 | 5.00/8.00/14.00 | 0.00/5.00/7.00 | 38.16/121.87/177.73 | 15.92/37.83/41.78 |
| representative | 45.00/60.05/111.00 | 8.00/8.00/8.00 | 34.00/48.00/69.00 | 5.00/8.00/8.00 | 43.32/157.62/179.61 | 12.71/36.08/44.69 |
| high-overlap | 176.00/256.00/256.00 | 8.00/8.00/8.00 | 101.50/256.00/256.00 | 6.00/7.00/8.00 | 31.14/74.95/152.77 | 19.68/37.52/44.97 |

## 5. 连续移动灯序列

representative 场景前 64 灯在固定实际 G-Buffer 上按冻结正弦轨迹移动 33 个连续样本。它是离线连续帧诊断，不伪称 GPU runtime 动画。

| 材质 | 方案 | 全帧质量 | Jump P99 / max | Bin switches | Membership transitions | 最坏帧 |
|---|---|---:|---:|---:|---:|---:|
| diffuse-only | U8 | Fail | 0.005460 / 0.017710 | 13770 | 10304 | 3 |
| diffuse-only | H8 | Fail | 0.001705 / 0.007405 | 3323 | 8388 | 8 |
| captured-specular | U8 | Fail | 0.005526 / 0.017708 | 13770 | 10304 | 3 |
| captured-specular | H8 | Fail | 0.001605 / 0.007428 | 3228 | 7858 | 8 |

逐帧原始统计在 [`temporal-frames.csv`](temporal-frames.csv) 和 [`aggregate.json`](aggregate.json)；每个方案/材质的最坏帧 HDR 与伪彩图在 `hdr/`、`images/`。Bin switch 只统计同一 tile/cluster、同一灯连续两帧都处于 aggregate 状态而主 bin 改变；进入/离开 aggregate 另列 membership transition。

## 6. 原始想法为何成立或不成立

U8 的动机成立在“把多灯循环压成固定八方向”这一复杂度目标上，但 RGB+二维方位没有携带每像素深度方向、距离衰减和球边界。tile 中只要一个样本被灯命中，整个 tile 都会收到该 bin，天然产生 leak；反过来，固定方向和代表点 attenuation 又会造成 miss/幅值错误。Specular 还依赖 half-vector，二维量化误差会被高光指数放大。只要任一冻结质量门槛失败，U8 即为 No-Go，而不是通过改阈值或偷加 per-light 数据挽救原定义。
H8 用三维包含、far 限制和 exact fallback 修复 U8 最危险的语义漏洞，但修复存在硬交换：保守 partial/near/specular-critical 会回到逐灯 exact；允许 aggregate 的部分仍只有八个固定 3D 方向和 cluster-center attenuation。若质量不通过，说明八方向本身仍不足；若质量通过而 Work/内存失败，说明 exact fallback 已吃掉压缩收益。两种情况都不应进入大规模 Runtime。

## 7. 内存与 overflow

- U8 1080p 物理估算：1.027 MiB；
- H8 按 640×360 非空 cluster/exact pool 随 tile 数外推的最坏 1080p 估算：6.590 MiB；
- Phase A 使用动态容器和 float64 累加；所有 PFM 写出前检查 finite；没有固定 N 截断；
- 若未来实现 Runtime，必须 count/prefix-sum 精确分配 exact pool；不足时重分配、整 cluster 回退 Analytic Screen 或明确失败，禁止静默丢灯。

## 8. 可复现与证据

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_directional_binned_phase_a.ps1 `
  -Mode All `
  -RunDirectory .\benchmark-results\directional-binned-lighting\directional-binned-phase-a-20260804 `
  -PythonExecutable C:\Users\Link\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe
```

关键证据：

- [`run_directional_binned_phase_a.ps1`](../../../tools/run_directional_binned_phase_a.ps1)、[`analyze_directional_binned_phase_a.py`](../../../tools/analyze_directional_binned_phase_a.py)：抓取编排与离线 Oracle/U8/H8 分析源码；
- [`PHASE0_FROZEN_PROTOCOL_CN.md`](PHASE0_FROZEN_PROTOCOL_CN.md)：结果前冻结的定义和门槛；
- [`capture-manifest.json`](capture-manifest.json)：EXE/协议/源文件哈希与场景抓取；
- [`aggregate.json`](aggregate.json)：全部静态、连续帧与门控原始聚合；
- [`static-summary.csv`](static-summary.csv)、[`temporal-frames.csv`](temporal-frames.csv)：扁平统计；
- [`captures`](captures/)：app LDR、G-Buffer PFM、逐灯参数与 bounds telemetry；
- [`csv`](csv/)：每 tile/cluster 明细；
- [`hdr`](hdr/)、[`images`](images/)：HDR exact/candidate/diff 与可视化；
- [`artifact-manifest.json`](artifact-manifest.json)：产物大小和 SHA-256。

## 9. 限制与适用边界

- Phase A 是 640×360、固定 RTX 主机上由 Release 程序抓取 G-Buffer 后的 CPU/NumPy 重放；Python wall time 不是渲染性能；
- 静态覆盖 small/medium/representative/high-overlap；连续帧是移动灯、固定几何，不包含移动相机导致的新遮挡；
- 压力场景点阴影关闭；设计上 shadowed 灯只能 exact；
- H8 运行时若改 depth slice、tile size、方向集合、specular 阈值或 far 条件，必须视为新候选重新冻结并重测；
- 结论不外推其他 GPU，也不证明透明 Forward 或 PBR backend；
- Phase B 未进入时没有 Candidate RenderDoc；旧 Analytic Screen RenderDoc 证据仍保留在旧 benchmark 目录，未覆盖。

## 10. Release 构建与默认路径 Smoke

- Release x64 构建：**Pass**，exit code=0；
- EXE SHA-256：`EA40321A9CDE239AE223C1FBFFD43DCDE560CEC562B982CD4EF8069B4DBB4BB6`；
- 不显式传 `--point-light-render-mode` 的 640×360 Smoke：**Pass**，result success=true，renderMode=`analytic-screen`，renderModeExplicit=false；
- 默认值改变：**false**。本实验没有新增 Runtime Candidate，也没有切换当前 Analytic Screen 默认路径。
- 产物完整性复算：**Pass**；重算 204 个清单哈希，解析 10 个 JSON、14 个 CSV（14420 行），检查 88 个 PFM（47923200 个 finite 值）并解码 85 个 PNG/PPM。
- 数值产物交叉校验：**Pass**；从 exact/candidate PFM 重算 16 个静态组合与 4 个连续序列最坏帧的 diff/P99，并验证 H8 分类恒等式、8-bin 上限与灯数上限。

机器可读结果与运行输出见 [`verification/verification.json`](verification/verification.json)、[`verification/smoke.json`](verification/smoke.json) 和 [`verification/smoke.log`](verification/smoke.log)。
