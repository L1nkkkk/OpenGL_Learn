# RenderDoc 点光源 GPU 瓶颈拆解（Legacy 基线）

> 结论：在 512 灯 representative 中，三次独立 replay 的分类事件中位数显示 Stencil clear 是最大单项（10.324 ms，47.4%），Lighting volume draw 次之（8.183 ms，37.5%），Stencil volume draw 最小（3.289 ms，15.1%）。本报告只诊断，不包含任何优化实现。

## 范围与固定条件

- 场景：固定 Sponza；Release x64；1920×1080；固定相机、seed `0x21D3F3A5` 与既有灯光生成器。
- 路径：Phong Deferred Legacy 点光体积；显式 `gPosition`；点光阴影、SSAO、Bloom 关闭；VSync 请求值 0。
- 窗口像素格式保留上一轮基线的 4× MSAA（`windowSamples=4`）；没有为本诊断改变 Legacy 状态。
- CPU：12th Gen Intel(R) Core(TM) i7-12700KF（20 logical processors）；OS：Microsoft Windows 11 专业版 10.0.26200 build 26200。
- GPU：NVIDIA GeForce RTX 5060 Ti/PCIe/SSE2；OpenGL `3.3.0 NVIDIA 591.86`；驱动来自 OpenGL 字符串。
- RenderDoc：`renderdoccmd x64 v1.45 built from 2fc0bc04cb95499635f63986a55bc6f67849dd9f`。

## 捕获与计时协议

每个配置保留 1 个独立 RDC；应用在约 30 帧后通过既有 in-app `StartFrameCapture/EndFrameCapture` 入口捕获。每个 RDC 随后由 3 个彼此独立的 QRenderDoc 进程重放。GPU Duration 与 pipeline statistics 分两次 `FetchCounters` replay pass 读取，避免统计查询本身污染 duration。下面所有 RenderDoc 汇总均为三次独立 replay 的中位数，不把单帧单次值当作稳定结果。

- captureExecutable：`AC1CAA71307B379B1898AFB83C33853A8EE8A40B779EA2D68964AC4D528CA798`；postCleanupExecutable：`78A27A1BBA48DB53F46FE614A53B2B7DED950C43CA4133E8ACBCFD8F297B3BFA`。两者分开记录，不把清理后的 EXE 伪装成四个既有 RDC 的捕获程序。

## 逐 replay 真实结果

| 配置 | Replay | 分类总和 ms | Clear ms | Stencil draw ms | Lighting draw ms |
|---|---:|---:|---:|---:|---:|
| representative / 16 | 1 | 1.334 | 0.250 | 0.669 | 0.415 |
| representative / 16 | 2 | 1.088 | 0.249 | 0.426 | 0.413 |
| representative / 16 | 3 | 1.132 | 0.250 | 0.464 | 0.418 |
| high-overlap / 16 | 1 | 1.770 | 0.254 | 0.156 | 1.360 |
| high-overlap / 16 | 2 | 1.755 | 0.254 | 0.156 | 1.345 |
| high-overlap / 16 | 3 | 1.770 | 0.255 | 0.156 | 1.359 |
| representative / 256 | 1 | 11.400 | 5.230 | 1.907 | 4.263 |
| representative / 256 | 2 | 11.059 | 4.995 | 2.019 | 4.044 |
| representative / 256 | 3 | 11.914 | 5.650 | 2.208 | 4.056 |
| representative / 512 | 1 | 22.544 | 11.184 | 3.508 | 7.852 |
| representative / 512 | 2 | 21.793 | 10.324 | 3.287 | 8.183 |
| representative / 512 | 3 | 21.179 | 9.343 | 3.289 | 8.547 |

## 按事件类别聚合

`总和中位数` 是每次 replay 先对该类全部事件求和、再对三次求中位数；`单事件中位数` 是三次 replay 各自事件中位数的中位数。占比以三类总和中位数之和为分母。

| 配置 | 类别 | 事件数/Replay | 总和中位数 ms | 单事件中位数 ms | 点光分类阶段占比 |
|---|---|---:|---:|---:|---:|
| representative / 16 | Stencil clear | 32 | 0.250 | 0.007792 | 22.1% |
| representative / 16 | Stencil volume draw | 16 | 0.464 | 0.012160 | 41.1% |
| representative / 16 | Lighting volume draw | 16 | 0.415 | 0.019488 | 36.8% |
| high-overlap / 16 | Stencil clear | 32 | 0.254 | 0.007856 | 14.4% |
| high-overlap / 16 | Stencil volume draw | 16 | 0.156 | 0.006944 | 8.8% |
| high-overlap / 16 | Lighting volume draw | 16 | 1.359 | 0.102928 | 76.8% |
| representative / 256 | Stencil clear | 512 | 5.230 | 0.007808 | 46.3% |
| representative / 256 | Stencil volume draw | 256 | 2.019 | 0.004576 | 17.9% |
| representative / 256 | Lighting volume draw | 256 | 4.056 | 0.004928 | 35.9% |
| representative / 512 | Stencil clear | 1024 | 10.324 | 0.007808 | 47.4% |
| representative / 512 | Stencil volume draw | 512 | 3.289 | 0.004512 | 15.1% |
| representative / 512 | Lighting volume draw | 512 | 8.183 | 0.004832 | 37.5% |

## 与应用内 OpenGL Timer Query 分开对照

| 配置 | 应用内点光阶段 Median ms | RenderDoc 分类事件总和 Median ms | RenderDoc / App |
|---|---:|---:|---:|
| representative / 16 | 0.844 | 1.132 | 1.34× |
| high-overlap / 16 | 1.966 | 1.770 | 0.90× |
| representative / 256 | 11.073 | 11.400 | 1.03× |
| representative / 512 | 21.809 | 21.793 | 1.00× |

RenderDoc 是 replay 时对单个 API event 插入 counter 的分类总和；应用内 Timer Query 是原进程中包围整个点光阶段的连续区间，包含状态切换/间隙但没有 RenderDoc replay 插桩。两者不要求完全一致。256/512 的两种口径非常接近；16 灯固定开销占比更高。趋势一致：灯数上升与 high-overlap 都增加点光成本。

## 覆盖率计数与 high-overlap 定位

| 配置 | Lighting PS Invocations | Lighting duration ms | Stencil Samples Passed | Rasterized primitives（Stencil） |
|---|---:|---:|---:|---:|
| representative / 16 | 7,924,110 | 0.415 | 13,156,970 | 5,919 |
| high-overlap / 16 | 28,058,194 | 1.359 | 1,101,394 | 4,246 |
| representative / 256 | 79,694,395 | 4.056 | 91,638,459 | 129,463 |
| representative / 512 | 155,388,872 | 8.183 | 168,598,770 | 262,693 |

16 灯 high-overlap 相对 representative：Lighting PS Invocations 从 7,924,110 增至 28,058,194（3.54×），Lighting duration 从 0.415 ms 增至 1.359 ms（3.27×）。应用内点光阶段是 2.33×；RenderDoc 分类总和是 1.56×，差异被 high-overlap 更低的 Stencil volume draw 成本部分抵消。因此 2.33× 的主要增量明确落在 Lighting volume fragment shading，而不是 clear。

## 三个决策问题

1. **512 灯主成本**：full-target stencil clear。三次 replay 的类总和中位数为 10.324 ms（47.4%）；Lighting fragment shading 为 8.183 ms（37.5%）；Stencil volume raster 为 3.289 ms（15.1%）。
2. **16 灯 high-overlap 的差异**：主要落在 Lighting volume fragment shading；PS Invocations 和 duration 的同步增长给出直接证据。
3. **下一项正式 A/B**：主候选只验证“点光 stencil clear 频率/标记复用”这一变量，A 保持 Legacy 2N clear，B 仅改变 clear 策略并保持灯、球体 draw 与顺序不变。备选只验证 Lighting draw 的屏幕覆盖约束（例如 scissor）并以 PS Invocations 为机制指标。本轮未实现二者。

## 正确性与重放证据

- `representative / 16`：signature `0x28cdb6b119b52795` / `0xff25d7196616c895`；RDC SHA-256 `B6DEC114B369729EED681B122471CDE8E65FE973AFC20D1CE43BF44CDB4995D8`；3/3 replay 成功、0 debug message、fatal status Success；事件计数 clear=32、stencil draw=16、lighting draw=16；应用截图 non-black ratio 0.9274。
- `high-overlap / 16`：signature `0x1d9822b67d99e5aa` / `0xeaa95faf1a53aa75`；RDC SHA-256 `3CCFC06648F82F7AFF54EEFBE1880B615F3EBF3CD3BE86A49C4D6605C0385D61`；3/3 replay 成功、0 debug message、fatal status Success；事件计数 clear=32、stencil draw=16、lighting draw=16；应用截图 non-black ratio 0.9049。
- `representative / 256`：signature `0x53c86b88620f757d` / `0xda0284a310591088`；RDC SHA-256 `E82879F736CF56589BBB09B8732CD41AA9B6A821ED48B66C36F171F19C026791`；3/3 replay 成功、0 debug message、fatal status Success；事件计数 clear=512、stencil draw=256、lighting draw=256；应用截图 non-black ratio 0.9999。
- `representative / 512`：signature `0x93b2fb98f925264f` / `0xee449aa19906ca3b`；RDC SHA-256 `C9FFAD94BF22CB8B7AED78087D7C6016A400DE06AF6830D62E8D42CBBD804212`；3/3 replay 成功、0 debug message、fatal status Success；事件计数 clear=1024、stencil draw=512、lighting draw=512；应用截图 non-black ratio 1.0000。

## Instrumentation cleanup 验收

父任务发现 marker 默认关闭时仍逐灯格式化名称。清理后，`snprintf` 只在 `renderDocMarkers=true` 分支执行；clear、draw、Uniform 和逐灯顺序没有变化。

| 进程 | Warmup | Samples | CPU Deferred Point Lights Median ms | GPU Median ms |
|---:|---:|---:|---:|---:|
| 1 | 300 | 600 | 0.02450 | 0.842368 |
| 2 | 300 | 600 | 0.02410 | 0.842400 |
| 3 | 300 | 600 | 0.02315 | 0.842384 |

三进程 CPU Median 的中位数为 **0.02410 ms**，上一轮 representative/16 run01 为 0.0235 ms；父任务清理前新测值为 0.1713 ms。三进程均为 signature `0x28cdb6b119b52795` / `0xff25d7196616c895`、submitted=16、culled=0、clear=32、non-black ratio 0.927415。

- marker-enabled 短 smoke：成功、`renderDocMarkersEnabled=true`、同一 signature、submitted=16、culled=0、clear=32、非黑；按要求没有创建新 RDC，也没有采正式 counter。
- marker 语义复核：清理后源码仍按 Phase → Light → ClearBefore → StencilDraw → LightingDraw → ClearAfter 顺序保留相同标签；既有 representative/16 RDC 事件树仍为 phase=1、light=16、clear=32、stencil draw=16、lighting draw=16。
- 四个原始 RDC、12 个原始 replay JSON 与 GPU 数值均保留；cleanup 只新增 CPU/default-off 和 marker-enabled app smoke 证据。

每个 replay JSON 内含精确 marker/action Event ID、逐事件 counter、Stencil 与 Lighting 关键 Pipeline State。每配置的 `*-lighting-target.png` 是在最后一个 Lighting volume draw 事件通过 RenderDoc `SaveTexture` 导出的目标纹理；`*-thumbnail.png` 是 RDC 内嵌最终帧缩略图。

## 工具限制与已知限制

- NVIDIA 扩展硬件 counter 不可用：RenderDoc 明确报告缺少 Nsight Perf SDK `nvperf_grfx_host.dll`。本报告只使用可用的通用 OpenGL GPU Duration、Samples/Primitives/Shader Invocations counters，没有估算。
- Duration 是 RenderDoc replay 口径，不是原始 capture 进程的原位帧时间；分类总和不包含 marker、状态设置和事件间隙。应用 Timer Query 继续作为独立口径保留。
- 每配置为 1 个独立捕获 + 3 个独立 replay 进程，不声称有 3 个独立 RDC。
- representative/16 的 Stencil draw replay 波动高于其他配置；报告使用三进程中位数并保留逐 replay 原始 JSON，不隐藏波动。
- 本轮未修改上一轮正式基线 JSON，也未实现 Scissor、剔除、批处理、clear 减少、单 Pass 或 Stencil 策略切换。

## 机器可读产物

- `aggregate.json`：聚合、签名、捕获哈希、决策值。
- `per-replay.csv`、`per-class-replay.csv`、`per-event.csv`：逐 replay / 类 / 事件数据。
- `point-light-gpu-breakdown.svg`：拆解图。
- `instrumentation-cleanup/summary.json`：清理前后 EXE、三进程 CPU/GPU Median、marker 语义和原证据保留状态。
- `instrumentation-cleanup/per-process.csv`：三个 300+600 default-off 进程结果。
