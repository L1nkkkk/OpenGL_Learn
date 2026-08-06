# Analytic Screen 与 Tile S1 正式 A/B 实验报告

## 结论

**Tile S1 仅条件路径 Go，默认路径 No-Go。** 正式实验包含 60 个独立进程，每进程 300 帧预热 + 600 帧采样。主判据为同轮配对的 Wall Frame，而不是单独的 Lighting GPU Zone。

- 静止相机：Tile 明确获胜 4/5 个锚点；Analytic 明确获胜 0/5 个锚点。
- 运动相机：Tile 明确获胜 0/5 个锚点；Analytic 明确获胜 5/5 个锚点。
- 质量：30/30 组截图通过冻结门槛；最坏 Max=2 LSB，Mean=0.066386 LSB，P99=1.000 LSB。

Tile 的收益依赖 CSR 跨帧复用：静止 View/Light Set 下存在明确收益，但运动相机下不满足默认路径门槛。因此它是条件优化，不是通用替换；当前默认继续保留 Analytic Screen。

## 端到端结果

下表中的变化均为 `Tile - Analytic`；负数表示 Tile 更快。只有 3/3 方向一致、绝对差 ≥0.05 ms 且相对差 ≥3% 才判定赢家。

| 场景 | 相机 | Analytic Wall | Tile Wall | 配对变化 | 判定 |
|---|---|---:|---:|---:|---|
| N16/R1.5 | 静止 | 1.7082 ms | 1.6689 ms | -0.0370 ms / -2.17% | Tie |
| N16/R1.5 | 运动 | 1.6962 ms | 1.8409 ms | +0.1456 ms / +8.58% | Analytic |
| N64/R1.5 | 静止 | 1.8537 ms | 1.7018 ms | -0.1519 ms / -8.19% | Tile |
| N64/R1.5 | 运动 | 1.8316 ms | 2.7991 ms | +0.9676 ms / +52.83% | Analytic |
| N64/R12 | 静止 | 4.3826 ms | 2.7760 ms | -1.6052 ms / -36.54% | Tile |
| N64/R12 | 运动 | 4.3497 ms | 11.9998 ms | +7.6076 ms / +174.46% | Analytic |
| N256/R6 | 静止 | 9.0143 ms | 4.4790 ms | -4.5353 ms / -50.31% | Tile |
| N256/R6 | 运动 | 8.9106 ms | 36.0085 ms | +27.0980 ms / +304.11% | Analytic |
| N512/R12 | 静止 | 23.6334 ms | 11.4580 ms | -12.1787 ms / -51.48% | Tile |
| N512/R12 | 运动 | 23.3173 ms | 92.4921 ms | +69.1938 ms / +295.95% | Analytic |

## 为什么静止与运动结果会分叉

Analytic Screen 每盏灯执行一次解析屏幕包围、Scissor 和矩形 Draw。Tile S1 则把所有灯压入一次全屏 Draw，并让像素只遍历所在 Tile 的候选灯，因此通常能减少 Draw Call 和 Lighting GPU 工作。

但 Tile 的 CSR 是 View/Projection 相关数据。相机静止且灯不变时，它只在预热阶段构建一次，正式采样只承担很小的 Cache Check；相机运动时，矩阵每帧变化，CSR 每帧重建并上传。若 Build/Upload超过 GPU 侧节省，端到端 Wall Frame 就会变慢。CPU 和 GPU Zone 可能并行，报告不把二者简单相加，最终只用 Wall Frame 做 Go/No-Go。

## 正确性与边界

- 两条路径使用相同灯光数据、精确球体影响判定、BRDF/衰减公式和逐灯累加顺序；Tile 的矩形覆盖只扩大候选集合，不删除真实受光像素。
- Moving A/B 的 600 个采样相机状态逐帧一致；所有 Scene/Submission Signature 一致。
- Static 的一次性 CSR Build 位于预热阶段，因此静态数据表示稳定运行时上限，不表示首次进入场景成本。
- 该结果属于 OpenGL 3.3、CPU 构表 + TBO 上传实现；不能外推为 GPU Compute Tiled/Clustered Shading。
- 未修改默认路径；若要利用静态收益，需要未来实现可靠的 View/Light Revision 策略或 GPU 构表，再重新实验。

## 证据与复现

- `aggregate.json`：完整聚合、配对比较与判定。
- `summary.csv`：每组每项指标的 pooled Median/P95/P99。
- `charts/wall-frame-static-moving.png`：静止/运动端到端对照。
- `charts/paired-relative-change.png`：Tile 相对变化。
- `charts/moving-cost-breakdown.png`：GPU 节省与 CPU 构表/上传分解。
- `charts/representative-heavy-moving-analytic.png`、`...-tile.png`、`...-difference-x96.png`：代表性运行截图和放大误差图。
- `pre-capture-manifest.json` 与 `capture-manifest.json`：冻结协议、二进制/源码哈希及 60 个原始进程证据链。
- 复现入口：`tools/run_analytic_tile_ab.ps1`。

## 独立复核

独立验证脚本结论：PASS；重算 10 个配对比较，校验 180 个文件哈希，相机逐帧配对 15 组。
