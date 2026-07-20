# Runtime performance optimization report

本文档记录以稳态运行时间为第一目标的性能实验。所有实验仍遵守 `PERFORMANCE_OPTIMIZATION_PROTOCOL.md` 和 `RUNTIME_BENCHMARK.md`；每项优化必须保留相邻 A/B、原始运行汇总、绝对变化、百分比变化、正确性验证和最终决策。

## Test environment

- Date: 2026-07-20
- Build: Release x64
- OS: Windows 11 Pro
- CPU: Intel Core i7-12700KF
- GPU: NVIDIA GeForce RTX 5060 Ti
- Driver: NVIDIA 591.86 (`GL_VERSION` 为 `3.3.0 NVIDIA 591.86`)
- Resolution: 1440 x 900
- Default framebuffer: 4x MSAA
- Requested swap interval: 0
- Render path: Forward，Gamma 开启；Bloom、Deferred、SSAO、Forward normal buffer 和 shadow-casting lights 关闭
- Scene: `saved/last_scene.json` 的隔离副本；只把 camera front 从 `(0, 0, -1)` 改为 `(0, 0, +1)`，确保两个角色和环境实际进入 viewport。资产、模型、材质、灯光和其他设置不变。
- Background load: 未绑定 CPU affinity，也未建立专用隔离环境；使用完整预热、新进程和平衡交错顺序降低时间漂移。

原始 JSON 和视觉截图保留在本地忽略目录：

```text
benchmark-results/2026-07-20-p1-indexed-mesh/visible-ab/
benchmark-results/2026-07-20-p1-indexed-mesh/visual-visible-A2.png
benchmark-results/2026-07-20-p1-indexed-mesh/visual-visible-B2.png
```

## P1: tangent-aware vertex joining and indexed drawing

### Goal

减少默认 Forward 路径中的重复顶点读取和顶点着色工作，同时保持 UV、normal、tangent/bitangent 边界以及现有材质和裁剪行为正确。

### Problem

原导入只执行 triangulation 和 UV flip。Assimp 因此保留逐面角点格式，应用随后上传完整展开的 VBO，并使用 `glDrawArrays`。默认导入的 36 个 mesh 共保存 818,724 个顶点；其中两个角色的 812,964 个角点具有大量可复用的 position/UV/normal 组合。

旧的 `Rejected indexed-mesh experiment` 只给这份顺序展开数据增加 EBO，并没有先合并顶点，因此不能用于判断资产的真实索引复用潜力。

### Implementation

- 导入阶段启用 `aiProcess_CalcTangentSpace` 和 `aiProcess_JoinIdenticalVertices`。Assimp 先建立 tangent basis，再把 tangent/bitangent 也纳入顶点相等判断；UV、normal 或 tangent handedness 不兼容的边界不会被错误合并。
- `MeshGeometry` 同时拥有 VBO 和可选 EBO，索引 mesh 使用 `glDrawElements`；无索引数据仍保留 `glDrawArrays` 回退。
- 没有 Assimp tangent basis 的程序生成 mesh 使用按索引累计、正交化的 TBN 回退，不再为了计算 TBN 把索引展开回逐三角形顶点。
- VBO/EBO 上传后立即释放两份 CPU staging capacity；Mesh GPU 遥测现在同时统计 vertex 和 index buffer 字节。
- 非法索引会在上传前被拒绝并回退到非索引绘制，避免越界 EBO 访问。

### Control and method

- Control A: `139fb25` (`feat(profiler): add automated runtime benchmarks`)
- Candidate B: 本节所在提交的未提交候选工作树
- Control binary SHA-256: `040BC00FEF3B1648784834DDBDA91B0115B757D4F4D7843DEBE71EDD757FD624`
- Candidate binary SHA-256: `9FAFF1A6EF39E325EA7E4FC6D6E488F14B4E49A0FBE19F35BB567373C182DB27`
- 每个二进制先执行一次不计入结果的完整 300 warm-up / 1200 sample 运行。
- 正式运行均为新进程，每次 300 帧 warm-up、1200 帧 sample，顺序为 `A/B/B/A/A/B`。
- 六次报告均为 `capture.valid == true`；wall、CPU、GPU、Forward CPU/GPU、render stats 和 memory 均各有完整 1200 个样本。
- 六次环境和渲染设置完全一致。每帧均为 47 draw calls、815,946 submitted vertices/indices、271,982 triangles 和 557 uniform updates。

### Per-run results

| Order | Variant | Load ready (ms) | GPU median (ms) | GPU P95 (ms) | GPU P99 (ms) | Forward GPU median (ms) | Forward GPU P95 (ms) | Forward GPU P99 (ms) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | A - `139fb25` | 1488.522 | 0.551104 | 0.977632 | 1.223168 | 0.486016 | 0.903648 | 1.123232 |
| 2 | B - indexed | 1639.309 | 0.495904 | 0.901216 | 1.094944 | 0.431360 | 0.834272 | 1.024352 |
| 3 | B - indexed | 1642.194 | 0.500832 | 0.916000 | 1.192544 | 0.436160 | 0.846656 | 1.086336 |
| 4 | A - `139fb25` | 1449.960 | 0.549664 | 1.013472 | 1.236544 | 0.484320 | 0.904640 | 1.117664 |
| 5 | A - `139fb25` | 1476.347 | 0.546112 | 0.967936 | 1.233376 | 0.480960 | 0.895424 | 1.108896 |
| 6 | B - indexed | 1631.211 | 0.493664 | 0.907136 | 1.137184 | 0.428896 | 0.836512 | 1.029472 |

### Average result

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| GPU Frame median | 0.548960 ms | 0.496800 ms | **-0.052160 ms** | **-9.50%** | improvement |
| GPU Frame P95 | 0.986347 ms | 0.908117 ms | **-0.078229 ms** | **-7.93%** | improvement |
| GPU Frame P99 | 1.231029 ms | 1.141557 ms | **-0.089472 ms** | **-7.27%** | improvement |
| Forward GPU median | 0.483765 ms | 0.432139 ms | **-0.051627 ms** | **-10.67%** | improvement |
| Forward GPU P95 | 0.901237 ms | 0.839147 ms | **-0.062091 ms** | **-6.89%** | improvement |
| Forward GPU P99 | 1.116597 ms | 1.046720 ms | **-0.069877 ms** | **-6.26%** | improvement |
| Wall frame median | 0.856867 ms | 0.857700 ms | +0.000833 ms | +0.10% | noise |
| Wall frame P95 | 1.171633 ms | 1.147900 ms | -0.023733 ms | -2.03% | ranges overlap; no claim |
| CPU Frame median | 0.835800 ms | 0.836567 ms | +0.000767 ms | +0.09% | noise |
| Forward CPU median | 0.052733 ms | 0.054533 ms | +0.001800 ms | +3.41% | 1.8 microseconds; ranges overlap |
| Load ready | 1471.609 ms | 1637.571 ms | **+165.962 ms** | **+11.28%** | regression |
| Mesh GPU | 43.574 MiB | 14.607 MiB | **-28.966 MiB** | **-66.48%** | secondary improvement |
| Private bytes median | 983.977 MiB | 955.467 MiB | -28.509 MiB | -2.90% | secondary improvement |
| Working set median | 172.867 MiB | 149.533 MiB | -23.335 MiB | -13.50% | secondary improvement |

GPU timing is the primary result. Every B run was lower than every A run for GPU Frame median/P95/P99 and Forward GPU median/P95/P99. Wall and CPU medians did not materially change because this scene remains CPU-bound at the full-frame level.

### Geometry and resource delta

| Metric | A | B | Delta |
| --- | ---: | ---: | ---: |
| Imported unique vertices | 818,724 | 217,220 | **-601,504 (-73.47%)** |
| Character unique vertices | 812,964 | 213,180 | **-599,784 (-73.78%)** |
| Imported index references | 818,724 | 818,724 | unchanged |
| Mesh GPU bytes | 45,690,624 | 15,317,056 | **-30,373,568 (-66.48%)** |
| Mesh resource count | 45 | 45 | unchanged |
| Draw calls per frame | 47 | 47 | unchanged |
| Submitted triangles per frame | 271,982 | 271,982 | unchanged |

Tangent-aware joining keeps 6,862 more character vertices than the theoretical position/UV/normal-only minimum of 206,318. This is intentional: those additional splits preserve incompatible tangent bases instead of maximizing merge rate at the cost of normal-map correctness.

### Correctness

- Release x64 build succeeded for the control and candidate.
- Resource smoke test passed: FBO lifecycle remained `2 -> 4 -> 6 -> 8 -> 2`; all stages reported `0.00 MiB Mesh CPU` and `14.61 MiB Mesh GPU` for B.
- Automated runs did not modify `imgui.ini`.
- A/B screenshots used the same visible camera fixture. Both show both characters, transparent quads, floor, walls and skybox with no missing mesh, crack, material reassignment or culling failure.
- A fixed 777 x 595 viewport crop differed at 165 of 462,315 pixels (0.03569%). The small differences are confined to character detail and are consistent with the retained tangent-aware interpolation changes; no structural difference was observed.
- Render invariants were identical in all six runs: 47 draws, 815,946 submitted indices/vertices, 271,982 triangles, 557 uniform updates and 45 live mesh resources.

### Decision

**Retained.** P1 reduces GPU Frame median by 9.50% and Forward GPU median by 10.67%, with consistent P95/P99 improvements and no correctness failure. It increases Load ready by 165.962 ms (11.28%) because Assimp now computes tangent data and performs tangent-aware joining during import. Under the project priority `correctness > steady-state time > load time > memory`, the stable runtime gain justifies this one-time load regression. The large GPU/process-memory reductions are useful secondary effects, not the retention criterion.

### Limitations

- The checked-in `saved/last_scene.json` camera faces away from the models. Formal P1 measurements therefore use an isolated copy with only the camera direction changed; the black-viewport measurements made before detecting this issue are invalid and excluded.
- GPU timestamp queries measure the complete pass but do not expose hardware vertex-shader invocation or post-transform-cache counters. Vertex reduction is verified from imported geometry counts, EBO contents, unchanged submitted index counts and GPU buffer telemetry.
- Results cover the default Forward feature set on one GPU/driver. Deferred, shadow and optional post-process configurations require their own A/B before making broader claims.

## P1 follow-up: persistent Assimp import cache

### Goal and problem

P1 的索引化绘制让稳态 GPU Frame median 降低 9.50%，但 `aiProcess_CalcTangentSpace` 和 `aiProcess_JoinIdenticalVertices` 每次启动都重新处理相同 OBJ，令 Load ready 增加 165.962 ms（11.28%）。本轮目标是在不撤销 P1 稳态收益和切线正确性的前提下，优先收回重复启动的加载时间。

### Implementation

- 首次导入仍使用与 P1 完全相同的 Assimp flags；处理后的 `aiScene` 以 `assbin` 写入 `model-cache/`，后续进程直接读取处理结果，不再重复 triangulation、切线生成和顶点合并。
- 缓存 key 包含规范化源文件绝对路径、文件大小和修改时间、导入 flags、缓存格式版本、`sizeof(Vertex)`、Assimp 版本；OBJ 还包含前 256 行内首个 `mtllib` 所引用 MTL 的大小和修改时间。
- 缓存写入临时文件后再发布。缺失、损坏或不可读取的条目会回退到原始模型导入并重建，不能创建缓存目录时也不影响模型加载。
- 材质纹理仍从源资产目录加载；缓存不会内嵌或长期持有纹理像素。
- Profiler 和 benchmark JSON 新增 `modelImportCacheHits` / `modelImportCacheMisses`，用于验证每次实验实际走到的导入路径。
- 运行时缓存目录加入 `.gitignore`，不会作为源码或 benchmark 结果提交。

### Control and method

- Control A: `2bc6bb4` (`perf(mesh): draw joined vertices with indices`)
- Candidate B: 本节所在提交的未提交候选工作树
- Control binary SHA-256: `9FAFF1A6EF39E325EA7E4FC6D6E488F14B4E49A0FBE19F35BB567373C182DB27`
- Measured candidate binary SHA-256: `CE239F99A45AA673AC33ACF206886816AE7A76DE76A7F9EAA3E0FD9DBBF434F0`
- 环境、可见相机副本和渲染设置与 P1 相同。每次进程仍执行 300 帧 warm-up 和 1200 帧 sample；所有正式 JSON 均为 `capture.valid == true`。
- 热缓存：A/B 各先执行一次不计入结果的完整运行，随后按 `A/B/B/A/A/B` 启动六个新进程。三个 B 均为 4 hits / 0 misses。
- 冷缓存：仍按 `A/B/B/A/A/B`，但三个 B 分别使用独立、初始为空的运行目录。每个 B 都为 1 hit / 3 misses：同进程第二次加载 sphere 命中，三个唯一模型各生成一次缓存。
- 原始 JSON 保留在本地忽略目录：

```text
benchmark-results/2026-07-20-p2-model-import-cache/formal-ab/
benchmark-results/2026-07-20-p2-model-import-cache/cold-formal-ab/
```

### Warm-cache per-run results

| Order | Variant | Load ready (ms) | Import cache hit/miss | GPU median (ms) | GPU P95 (ms) | GPU P99 (ms) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - `2bc6bb4` | 2197.134 | n/a | 0.507872 | 0.869920 | 0.952288 |
| 2 | B - cache hit | 1604.547 | 4 / 0 | 0.486656 | 0.843072 | 0.904224 |
| 3 | B - cache hit | 1494.091 | 4 / 0 | 0.495040 | 0.861920 | 0.918848 |
| 4 | A - `2bc6bb4` | 1951.214 | n/a | 0.488224 | 0.888480 | 0.984928 |
| 5 | A - `2bc6bb4` | 2091.999 | n/a | 0.492864 | 0.859456 | 0.988288 |
| 6 | B - cache hit | 1543.364 | 4 / 0 | 0.500064 | 0.904448 | 0.987552 |

### Warm-cache average result

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| Load ready | 2080.116 ms | 1547.334 ms | **-532.782 ms** | **-25.61%** | improvement; ranges do not overlap |
| GPU Frame median | 0.496320 ms | 0.493920 ms | -0.002400 ms | -0.48% | ranges overlap; noise |
| GPU Frame P95 | 0.872619 ms | 0.869813 ms | -0.002805 ms | -0.32% | ranges overlap; noise |
| GPU Frame P99 | 0.975168 ms | 0.936875 ms | -0.038293 ms | -3.93% | ranges overlap; no runtime claim |
| Forward GPU median | 0.432192 ms | 0.429728 ms | -0.002464 ms | -0.57% | ranges overlap; noise |
| Forward GPU P95 | 0.800992 ms | 0.800480 ms | -0.000512 ms | -0.06% | noise |
| Forward GPU P99 | 0.878187 ms | 0.857387 ms | -0.020800 ms | -2.37% | ranges overlap; no runtime claim |
| Wall frame median | 1.648633 ms | 1.551733 ms | -0.096900 ms | -5.88% | ranges overlap; no claim |
| CPU Frame median | 1.616000 ms | 1.519967 ms | -0.096033 ms | -5.94% | ranges overlap; no claim |
| Working set median | 132.364 MiB | 133.196 MiB | +0.832 MiB | +0.63% | ranges overlap; noise |
| Private bytes median | 930.346 MiB | 936.779 MiB | +6.433 MiB | +0.69% | ranges overlap; noise |
| Mesh GPU | 15,317,056 bytes | 15,317,056 bytes | 0 | 0% | unchanged |
| Mesh CPU | 0 bytes | 0 bytes | 0 | 0% | unchanged |

热缓存 Load ready 的 A 范围为 1951.214–2197.134 ms，B 范围为 1494.091–1604.547 ms，完全不重叠。后续帧的代码路径没有变化，GPU/CPU/内存的重叠波动均不作为收益。

### Cold-cache results and amortization

| Order | Variant | Load ready (ms) | Import cache hit/miss |
| ---: | --- | ---: | ---: |
| 1 | A - `2bc6bb4` | 2233.561 | n/a |
| 2 | B - empty cache | 2256.298 | 1 / 3 |
| 3 | B - empty cache | 2228.070 | 1 / 3 |
| 4 | A - `2bc6bb4` | 2136.273 | n/a |
| 5 | A - `2bc6bb4` | 2140.455 | n/a |
| 6 | B - empty cache | 2206.441 | 1 / 3 |

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| First Load ready | 2170.096 ms | 2230.270 ms | **+60.174 ms** | **+2.77%** | first-write cost; ranges overlap |
| Cache files after first load | 0 | 3 | +3 | — | expected |
| Cache disk footprint | 0 | 15,044,204 bytes (14.347 MiB) | +14.347 MiB | — | persistent disk cost |

首次生成的 60.174 ms 代价小于一次后续命中的 532.782 ms 收益，因此第二次启动即回本。按两次启动的两组平均值组合，A 共 4250.212 ms，B 共 3777.604 ms，累计减少 **472.608 ms（11.12%）**。

### Correctness

- Release x64 构建成功；resource smoke test 通过，FBO 生命周期保持 `2 -> 4 -> 6 -> 8 -> 2`，所有阶段均为 `0.00 MiB Mesh CPU` 和 `14.61 MiB Mesh GPU`。
- Smoke test 前后 `imgui.ini` SHA-256 相同，没有覆盖用户配置。
- 六次热缓存和六次冷缓存运行的 render invariants 完全一致：47 draws、815,946 submitted indices/vertices、271,982 triangles、45 live mesh resources、15,317,056 Mesh GPU bytes 和 0 Mesh CPU bytes。
- 缓存命中截图与 P1 control 使用相同可见相机。固定 777 x 596 viewport crop 只有 164 / 463,092 像素不同（0.035414%，平均通道绝对差 0.020461），差异局限于角色上方的小块细节；未观察到网格、材质、透明或裁剪错误。

### Decision

**Retained.** 该改动保持 P1 的索引、GPU 内存和稳态渲染路径不变；首次运行以 60.174 ms 和 14.347 MiB 磁盘换取后续启动 532.782 ms（25.61%）的稳定加载收益，第二次启动即回本。按照 `correctness > steady-state time > load time > memory`，这是面向重复开发运行和重复打开场景的净时间收益。

### Limitations

- 当前 OBJ 依赖跟踪只读取前 256 行内的首个 `mtllib`。其他模型格式的外部几何/材质依赖不会自动进入 key；这些依赖变化时需要清空 `model-cache/` 或提升缓存版本。
- 旧 hash 条目不会自动清理，资产反复修改会累积磁盘文件；后续可增加容量上限或 LRU 清理，但必须单独测量启动开销。
- 绝对源路径属于 key，移动工作目录会安全地产生新条目，但旧目录对应的缓存不会复用。
- 结果覆盖当前三个唯一 OBJ 和当前机器的本地文件系统；慢盘、只读目录和更多资产规模需要独立测量。

## Rejected load experiment: application-side tangent reconstruction

### Attempt

为直接消除 P1 的 Assimp tangent 处理时间，候选曾关闭 `aiProcess_CalcTangentSpace`，并在应用侧按 45 度不连续阈值对共享顶点进行 tangent/bitangent 聚类和必要拆分，再继续 indexed draw。该实现通过构建与画面检查，但增加了复杂的导入时几何重写。

### Formal comparison

Control 仍为 `2bc6bb4`。执行两轮平衡顺序、共 12 个新进程（A/B 各 6 次），每次 300 帧 warm-up / 1200 帧 sample，所有 capture 和 render invariants 有效。

| Order | Variant | Load ready (ms) |
| ---: | --- | ---: |
| 1 | A | 2179.693 |
| 2 | B | 2220.251 |
| 3 | B | 2081.115 |
| 4 | A | 2118.199 |
| 5 | A | 1865.462 |
| 6 | B | 2074.258 |
| 7 | B | 2000.540 |
| 8 | A | 1991.593 |
| 9 | A | 2130.526 |
| 10 | B | 2094.245 |
| 11 | B | 2036.271 |
| 12 | A | 2098.700 |

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| Load ready | 2064.029 ms | 2084.447 ms | +20.418 ms | +0.99% | no improvement |
| GPU Frame median | 0.500917 ms | 0.498592 ms | -0.002325 ms | -0.46% | ranges overlap; noise |
| GPU Frame P99 | 0.954501 ms | 0.992229 ms | +0.037728 ms | +3.95% | ranges overlap; no gain |
| Mesh GPU | 15,317,056 bytes | 15,120,216 bytes | -196,840 bytes | -1.29% | secondary, not target |

### Decision

**Rejected and fully reverted.** 候选没有收回 Load ready，稳态时间也没有改善；仅减少 1.29% Mesh GPU 不足以用更多导入逻辑和切线边界风险交换。持久化已处理场景缓存随后作为独立候选实施和测量。
