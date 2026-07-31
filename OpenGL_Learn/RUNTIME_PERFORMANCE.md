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

## P2: cache Assets Browser directory snapshots

### Goal and problem

模型导入缓存解决重复启动时间后，默认场景已经更明显地受 CPU 和编辑器开销限制。P1 follow-up 的三个正式 B 样本中，CPU Frame median 平均为 1.520 ms，而 GPU Frame median 只有 0.494 ms。UI 相关 zone 的 median 分量合计约 0.549 ms；其中 `Viewport and Assets UI` 为 0.245 ms。

代码检查发现 `AssetsBrowser_UI` 在面板可见时每帧对 `models`、`materials` 和 `shaders` 调用 `std::filesystem::exists`。正式基线也稳定记录到每帧 3 次 filesystem checks；目录节点展开后，旧实现还会每帧重新运行 `directory_iterator`、扩展名解析和目录递归。

### Implementation

- 为 Models、Materials 和 Shaders 建立进程内目录快照；根目录存在性只检查一次，不再每帧访问磁盘。
- 每个目录的子节点在首次展开时按需加载并缓存。后续帧只遍历内存中的路径、名称、类型和 children。
- 扩展名过滤列表在缓存初始化时构建一次，不再为每个可见根节点重复解析 CSV。
- Assets 窗口聚焦时可按 F5 清空快照；刷新后的下一帧按需重建。
- 保留原有 `Viewport and Assets UI` 外层 CPU zone，新增 `Viewport UI` 和 `Assets Browser UI` 子 zone，便于直接比较外层 A/B 并诊断内部组成。
- Render telemetry 新增 `assetBrowserCacheHits` / `assetBrowserCacheMisses`。稳定正式样本的候选均为 1 hit / 0 misses。

### Control and method

- Date: 2026-07-21
- Control A: `d455056` (`perf(model): cache processed imports on disk`)
- Candidate B: 本节所在提交的未提交候选工作树
- Control binary SHA-256: `8BDE6D0EBD6881114593694104C95D0AC52EC8BCC0EAC136A14A2C3EBB1BC772`
- Measured candidate binary SHA-256: `6540C8EBA531EFABC5404347918216E5127448AAE76B96632097E3E616F08C19`
- Release x64、1440 x 900、4x default framebuffer MSAA、requested swap interval 0；场景和可见相机副本与 P1/P1 follow-up 相同。
- 固定默认 ImGui 布局，Assets tab 可见，三个资源根节点保持折叠。A 和 B 的 UI draw data 完全一致。
- A/B 各执行一次不计入结果的完整运行，再按 `A/B/B/A/A/B` 启动六个新进程；每次 300 帧 warm-up、1200 帧 sample。
- 六个 JSON 均为 `capture.valid == true`，wall/CPU/GPU、所有持续执行 zone、render stats 和 memory 均有完整 1200 个样本。
- 原始 JSON 和进程日志保留在本地忽略目录：

```text
benchmark-results/2026-07-21-p2-asset-browser/formal-ab/
```

### Per-run results

| Order | Variant | Wall median (ms) | CPU median (ms) | CPU P95 (ms) | Viewport + Assets median (ms) | Filesystem checks median |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - `d455056` | 1.6497 | 1.6175 | 3.0267 | 0.2462 | 3 |
| 2 | B - cached | 1.2769 | 1.2472 | 2.4362 | 0.0131 | 0 |
| 3 | B - cached | 1.3605 | 1.3301 | 2.4938 | 0.0136 | 0 |
| 4 | A - `d455056` | 1.6357 | 1.6077 | 2.8427 | 0.2443 | 3 |
| 5 | A - `d455056` | 1.6560 | 1.6258 | 2.8935 | 0.2477 | 3 |
| 6 | B - cached | 1.3600 | 1.3294 | 2.5647 | 0.0140 | 0 |

### Average result

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU Frame median | 1.617000 ms | 1.302233 ms | **-0.314767 ms** | **-19.47%** | improvement; ranges do not overlap |
| CPU Frame P95 | 2.920967 ms | 2.498233 ms | **-0.422733 ms** | **-14.47%** | improvement; ranges do not overlap |
| CPU Frame P99 | 3.763267 ms | 3.550867 ms | -0.212400 ms | -5.64% | ranges overlap; no tail claim |
| Wall frame median | 1.647133 ms | 1.332467 ms | **-0.314667 ms** | **-19.10%** | improvement; ranges do not overlap |
| Wall frame P95 | 2.955233 ms | 2.537800 ms | **-0.417433 ms** | **-14.13%** | improvement; ranges do not overlap |
| Wall frame P99 | 3.797100 ms | 3.582667 ms | -0.214433 ms | -5.65% | ranges overlap; no tail claim |
| Viewport + Assets CPU median | 0.246067 ms | 0.013567 ms | **-0.232500 ms** | **-94.49%** | direct target improvement |
| Viewport + Assets CPU P95 | 0.354133 ms | 0.024767 ms | **-0.329367 ms** | **-93.01%** | direct target improvement |
| Viewport + Assets CPU P99 | 0.431767 ms | 0.035467 ms | **-0.396300 ms** | **-91.79%** | direct target improvement |
| Filesystem checks median / P95 / P99 | 3 / 3 / 3 | 0 / 0 / 0 | **-3 / -3 / -3** | **-100%** | expected cache behavior |
| GPU Frame median | 0.495723 ms | 0.493461 ms | -0.002261 ms | -0.46% | ranges overlap; noise |
| GPU Frame P95 | 0.864533 ms | 0.867296 ms | +0.002763 ms | +0.32% | noise |
| GPU Frame P99 | 0.965184 ms | 0.997376 ms | +0.032192 ms | +3.34% | ranges overlap; no regression claim |
| Forward GPU median | 0.431317 ms | 0.429024 ms | -0.002293 ms | -0.53% | ranges overlap; noise |
| Load ready | 1470.204 ms | 1482.520 ms | +12.316 ms | +0.84% | ranges overlap; unrelated noise |
| Working set median | 136.823 MiB | 135.443 MiB | -1.380 MiB | -1.01% | noise; not a retention criterion |
| Private bytes median | 920.453 MiB | 931.525 MiB | +11.072 MiB | +1.20% | noise; not a retention criterion |
| Mesh GPU / Mesh CPU | 15,317,056 / 0 bytes | 15,317,056 / 0 bytes | 0 / 0 | 0% | unchanged |

CPU Frame median 的 A 范围为 1.6077–1.6258 ms，B 为 1.2472–1.3301 ms；CPU P95 的 A 范围为 2.8427–3.0267 ms，B 为 2.4362–2.5647 ms，两项均完全不重叠。直接目标 zone 的 median 从 0.2443–0.2477 ms 降至 0.0131–0.0140 ms。

候选内部拆分后，`Assets Browser UI` median 平均为 0.005667 ms，`Viewport UI` 为 0.007233 ms。外层 zone 还包含两个子 scope 的 profiling 和作用域开销，因此略高于二者之和。

### Correctness

- Release x64 构建成功；resource smoke test 通过，FBO 生命周期保持 `2 -> 4 -> 6 -> 8 -> 2`，所有阶段均为 `0.00 MiB Mesh CPU` 和 `14.61 MiB Mesh GPU`。
- Smoke test 前后用户 `imgui.ini` SHA-256 相同；正式 benchmark 的隔离运行目录也没有生成 `imgui.ini`。
- 所有六次运行均保持 47 draws、815,946 submitted indices/vertices、271,982 triangles、45 mesh resources、4 model-import cache hits。
- A/B 的 ImGui draw data 完全一致：13 draw calls、2,258 vertices、4,845 indices。缓存没有改变正式布局中的可见 UI 几何。
- 稳定 B 样本全部为 1 Assets Browser cache hit / 0 misses。Filesystem check 最大值仍可能达到 43，这是低频 shader/material hot-reload polling，不是 Assets Browser 每帧扫描。

### Cache cost and behavior

- 缓存只保存已访问目录的路径、显示名称、类型和 children vector；未展开目录不会递归扫描或占用完整树内存。
- 第一次显示 Assets 面板时仍检查三个根目录，工作量与旧实现该帧的三个根检查相当；这发生在正式 300 帧 warm-up 内，不计入稳定样本。
- 首次展开某个目录会产生一次 `directory_iterator` 和条目检查；后续帧复用快照。F5 会主动清空快照并重复这一过程。
- 当前固定 benchmark 禁用键鼠并保持三个根节点折叠，因此本报告没有把完全展开目录树的潜在收益量化为正式数字。

### Decision

**Retained.** 该改动以很小的按需路径缓存消除稳定帧磁盘访问，使直接目标 zone median 降低 94.49%，并让完整 CPU Frame median 降低 19.47%、P95 降低 14.47%。GPU、加载、渲染资源和 UI 几何没有实质变化。结果符合时间优先策略，内存变化不作为保留理由。

### Limitations

- 外部工具新增、删除或重命名资产后，Assets Browser 不会自动轮询刷新；用户需要聚焦 Assets 窗口并按 F5。避免自动轮询是消除稳定帧磁盘访问的有意取舍。
- 当前正式结果覆盖默认折叠根节点。目录完全展开时旧实现预期会产生更多重复访问，但在建立可重复的展开状态 fixture 前不作精确收益声明。
- 缓存重建仍在主线程执行。当前资产规模很小；大目录的首次展开如果出现可见卡顿，应另行比较后台扫描或分帧构建，不能直接以更多复杂度替换。

## P3: share imported material instances

### Goal and problem

PBR Backpack 的 80 个 mesh 使用同一个 Assimp material index，但旧导入路径为每个 mesh 创建独立 Material。MaterialBatchScope 按指针识别材质，因而每帧产生 80 次 material bind miss、3828 次 uniform update 和 484 次 texture-state change。

### Implementation and method

- Model 在一次 Assimp 导入期间按 material index 复用 shared Material；geometry、draw 顺序和材质内容不变。
- 最终二进制提供仅限 benchmark 的 --benchmark-unshared-imported-materials 控制开关，用于恢复旧行为。
- Release x64、1440 × 900、builtin/backpack-pbr、Forward、相同相机和灯光。
- A/B 各执行一次不计入结果的完整运行，再按 A/B/B/A/A/B 启动六个新进程；每次 300 帧 warm-up、1200 帧 sample。
- 六个 JSON 均 capture.valid == true，draw calls 固定为 82，submitted vertices 固定为 206,643。
- 原始 JSON：benchmark-results/pbr-ibl/material-sharing。

### Per-run results

| Order | Variant | CPU median (ms) | CPU P95 (ms) | Forward CPU median (ms) | Forward CPU P95 (ms) | Uniform updates |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - unshared | 1.8316 | 3.1560 | 0.6429 | 0.9432 | 3828 |
| 2 | B - shared | 1.3149 | 2.7199 | 0.2030 | 0.3024 | 162 |
| 3 | B - shared | 1.2983 | 2.6008 | 0.2001 | 0.3005 | 162 |
| 4 | A - unshared | 2.0282 | 4.1679 | 0.7033 | 1.0178 | 3828 |
| 5 | A - unshared | 1.8616 | 3.2450 | 0.6744 | 0.9555 | 3828 |
| 6 | B - shared | 1.3543 | 2.7616 | 0.2098 | 0.3148 | 162 |

### Average result

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU Frame median | 1.9071 ms | 1.3225 ms | **-0.5846 ms** | **-30.66%** | improvement |
| CPU Frame P95 | 3.5230 ms | 2.6941 ms | **-0.8289 ms** | **-23.53%** | improvement |
| Forward CPU median | 0.6735 ms | 0.2043 ms | **-0.4692 ms** | **-69.67%** | direct improvement |
| Forward CPU P95 | 0.9722 ms | 0.3059 ms | **-0.6663 ms** | **-68.53%** | direct improvement |
| Uniform updates | 3828 | 162 | **-3666** | **-95.77%** | expected |
| Material bind misses / hits | 80 / 0 | 2 / 78 | **-78 / +78** | — | expected |
| Texture-state changes | 484 | 16 | **-468** | **-96.69%** | expected |
| GPU Frame median | 0.2022 ms | 0.1988 ms | -0.0035 ms | -1.71% | no material change |
| GPU Frame P95 | 0.4523 ms | 0.4638 ms | +0.0115 ms | +2.53% | noise |
| Load ready | 1324.10 ms | 1340.57 ms | +16.46 ms | +1.24% | noise |
| Private bytes | 692.05 MiB | 691.90 MiB | -0.15 MiB | -0.02% | unchanged |

### Correctness and decision

- Release x64 构建、PBR smoke 和 resource smoke 均通过。
- Forward / Deferred PBR 截图归一化 RGB MAE 为 0.000585；材质共享没有改变像素输出。
- PBR smoke 退出后 Texture、Mesh CPU、Mesh GPU、Render target current bytes 全部为 0。
- FBO 生命周期保持 2 → 4 → 6 → 8 → 2。

**Retained.** 直接目标 zone、完整 CPU Frame median/P95 和提交计数均有大幅且同方向的改善；GPU、加载与内存没有实质回退。PBR 功能成本、旧场景回归和 rejected 按需 shader 实验见 PBR_IBL.md。
