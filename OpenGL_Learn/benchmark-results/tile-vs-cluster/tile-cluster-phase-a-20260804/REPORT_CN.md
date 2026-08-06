# Tile vs Cluster 精确点光源索引 Phase A 实验报告

结论：**No-Go for Phase B under this configuration**。这里的 Go/No-Go 只判断是否值得实现 benchmark-only runtime；本轮没有 Cluster Lighting runtime、GPU Timer Query 或 GPU 加速数据，默认路径未改变。

## 1. 实验回答的问题

Tile 与 Cluster 都只保存原始 `light index`，不压缩方向、颜色、深度或距离。未来像素阶段仍必须执行真实球形范围和原始点光公式。因此本轮比较的是空间索引候选工作量与列表代价，不是画质近似。

冻结协议 SHA-256：`3B3492ADBF51E80820A8E7CA1C407DF84723E85CC2DE5A2DA32CA8DB169E0B5E`。正式主配置为 1920×1080、16×16 Tile、Cluster16 对数 Z slices；8/24/32 只作敏感性，门槛在正式结果前冻结。

## 2. 正式主结果

| 场景 | 灯数 | Tile pixel-mean / P95 | Cluster16 pixel-mean / P95 | Candidate Tile → Cluster16 | 比例 | CSR 内存 Tile → Cluster16 | Miss |
|---|---:|---:|---:|---:|---:|---:|---:|
| small-local | 64 | 4.60 / 9 | 1.74 / 4 | 9,533,312 → 3,605,489 | 0.378× | 0.20 → 1.43 MiB | 0/0 |
| medium-local | 64 | 25.14 / 38 | 11.48 / 15 | 52,137,600 → 23,799,653 | 0.456× | 0.84 → 5.27 MiB | 0/0 |
| representative | 256 | 128.03 / 182 | 76.06 / 85 | 265,484,288 → 157,716,544 | 0.594× | 4.05 → 27.12 MiB | 0/0 |
| high-overlap | 512 | 500.31 / 512 | 388.60 / 512 | 1,037,439,360 → 805,802,304 | 0.777× | 15.64 → 152.86 MiB | 0/0 |

`pixel-mean/P95` 是每个有效 G-Buffer 像素实际查到的列表长度分布；Candidate interactions 是该长度对所有有效像素求和。它比全 logical-cell 的简单平均更接近未来 fragment loop 工作量，但仍不是 GPU shader invocation 或耗时。

## 3. 冻结门槛判定

| 门槛 | 实际 | 上限 | Pass |
|---|---:|---:|:---:|
| Zero miss / deterministic / no overflow | 0 failures | 0 failures | Yes |
| Representative candidate ratio | 0.594× | 0.70× | Yes |
| Representative pixel P95 ratio | 0.467× | 0.75× | Yes |
| High-overlap candidate ratio | 0.777× | 0.85× | Yes |
| High-overlap pixel P95 ratio | 1.000× | 0.90× | No |
| Representative+High candidate ratio | 0.740× | 0.75× | Yes |
| Max Cluster16 memory | 152.86 MiB | 64 MiB | No |

## 4. 为什么 Cluster 能缩短名单

Tile 只知道像素位于屏幕哪一格，同一格中近处和远处的灯都进入一张名单。Cluster 额外用线性 View depth 找到对数 Z slice；点光球只有和该截锥体保守相交才写入。它没有把球或光照改成立方体，Cluster 只是原始光源编号的三维通讯录。

球体只在完全落到任一 Tile/Cluster 平面外时才被排除，因此允许误收、不允许漏收。所有真实球内 pixel-light interaction 都由独立 Ground Truth 复核。

## 5. Slice 敏感性

| 场景 | Cluster8 / Tile | Cluster16 / Tile | Cluster24 / Tile | Cluster32 / Tile |
|---|---:|---:|---:|---:|
| small-local/64 | 0.477× | 0.378× | 0.360× | 0.347× |
| medium-local/64 | 0.520× | 0.456× | 0.438× | 0.432× |
| representative/256 | 0.646× | 0.594× | 0.571× | 0.562× |
| high-overlap/512 | 0.819× | 0.777× | 0.769× | 0.762× |

## 6. 正确性、截图和可视化

四个场景全部方案的 miss 总数为 `0`；CSR 按 light index 升序、无重复、无静默截断，Tile/Cluster16 七次进程内重建各只有一个 hash。

运行截图来自 Release renderer 的共同 exact `analytic-screen` Oracle，不是离线 Tile/Cluster 着色截图。Heatmap 仅把同一有效像素未来会遍历的列表长度映射回屏幕，用于解释候选工作量。

- `screenshots/representative-0256-exact-analytic-screen.png`、`screenshots/high-overlap-0512-exact-analytic-screen.png`：真实 1920×1080 renderer 运行截图；
- `heatmaps/*-renderer-tile-cluster.png`：运行截图、Tile list count、Cluster16 list count 并排，相同色标；
- `charts/list-length-mean-p95.png`、`candidate-interactions.png`、`csr-memory.png`、`slice-sensitivity.png`：正式图表。

## 7. CPU/GPU 时间的解释边界

`offlineTiming` 是同一 Python 进程中的 NumPy wall time（1 warmup + 7 samples），拆分 membership/count/prefix/fill。它不包含引擎对象访问、C++ allocator、GL buffer upload、driver 或 GPU，未进入 Go 门槛，也不能写成 CPU Frame 或 GPU Lighting 提升。

只有 Phase B benchmark-only runtime 才能测 Release C++ 构建/上传、GPU Timer Query、RenderDoc 和最终 HDR 一致性；Phase A 即使 Go，也不授权切换默认路径。

## 8. 有效像素、天空与局限

正式捕获的 invalid/sky pixel 数依次为 `[0, 0, 0, 0]`。无效像素在 Position 读取和列表统计前排除。若这些值均为 0，本轮固定相机没有真实天空覆盖，只验证了代码的 skip 条件和全有效 Sponza 画面，不能外推为天空场景验证。

结论仅适用于固定 Sponza、固定相机/seed、1920×1080、16×16 XY 与本轮 8/16/24/32 对数切片。动态相机/灯导致的每帧重建和上传尚未测量；透明 Forward、阴影、PBR 后端也不在本轮范围。

## 9. 可复现与原始证据

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_tile_cluster_phase_a.ps1 -Mode All
```

- `PHASE0_FROZEN_PROTOCOL_CN.md`：结果前冻结定义与门槛；
- `capture-manifest.json`、`captures/`、`logs/`：EXE/source hash、场景 JSON、G-Buffer PFM、原始 PPM 与运行日志；
- `aggregate.json`、`summary.csv`、`scenes/*.json`、`cells/*.csv`、`per-light/*.csv`：聚合和逐层证据；
- `csr/*.npz`：所有正式方案的原始 CSR；
- `verification/independent-verification.json`：独立验证器从 PFM + CSR 复算 miss/candidate/hash；
- `artifact-manifest.json`：产物大小与 SHA-256。

## 10. 最终 Release 与默认 Smoke

- Release build：**Pass**，exit code=0；
- EXE SHA-256：`EA40321A9CDE239AE223C1FBFFD43DCDE560CEC562B982CD4EF8069B4DBB4BB6`；
- 默认 smoke：**Pass**，renderMode=`analytic-screen`，explicit=false；
- 默认改变：**false**；GL error：0；遗留 renderer 进程：0。
