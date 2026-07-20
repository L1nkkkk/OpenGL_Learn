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
