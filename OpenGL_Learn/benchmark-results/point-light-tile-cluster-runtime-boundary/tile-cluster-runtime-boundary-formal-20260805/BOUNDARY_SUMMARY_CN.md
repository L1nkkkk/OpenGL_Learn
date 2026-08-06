# Tile16 / Cluster16 边界结论（精简版）

## 结论先行

在本项目当前的 OpenGL 3.3 CPU 构表 + TBO 上传实现里，Cluster16 **不是无条件更快**：

- 候选列表可缓存时，Cluster16 在“灯足够多、半径尚未大到跨越多数 Z slice”的区域有真实收益；
- 候选列表每帧重建时，Cluster16 在本次测试域 `N=32..512, R=1.5..12` 内没有端到端胜区，20/20 个格点均由 Tile16 获胜；
- 大半径端会重新回到 Tie，因为灯球跨越更多 Z slice，Cluster 的深度选择性下降，但 16 倍逻辑 cell 和更大 CSR 仍然存在。

胜负使用采集前冻结规则：三个独立进程配对 3/3 同方向，且绝对差至少 0.05 ms、相对差至少 3%；否则为 Tie。

## Cached：测得的离散胜区

下表只列 Cluster16 胜出的正式采样点；未列出的点均为 Tie，没有 Tile16 获胜点。这里的区间表示所列离散 R 点连续胜出，不外推到未测半径。

| N | Point-Light Lighting GPU | Whole GPU Frame | Wall Frame |
|---:|---|---|---|
| 32 | 无 | 无 | 无 |
| 64 | R=3,4,5,6 | R=3,4,5,6 | R=3,4 |
| 128 | R=2,2.5,3,4,5,6,8 | 同左 | R=2.5,4,5（R=3 为 Tie） |
| 256 | R=1.5,2,2.5,3,4,5,6,8 | 再含 R=10 | R=1.5..8 |
| 512 | R=1.5,2,2.5,3,4,5,6,8,10 | 同左 | 同左 |

代表数据：

- `N=64,R=3`：Lighting GPU `0.250752 → 0.196928 ms`，差 `-0.053760 ms / -21.44%`；
- `N=512,R=8`：Lighting GPU `7.822736 → 7.050800 ms`，差 `-0.730672 ms / -9.34%`；
- `N=512,R=5`：Wall Frame `6.49095 → 5.75215 ms`，差 `-0.74115 ms / -11.42%`。

## Rebuild Every Frame：测试域内无 Cluster 胜区

Cluster 的像素阶段在 9/20 个格点仍然更快，但其 CPU 构表与上传成本使 Wall/GPU Frame 在 20/20 个格点全部由 Tile16 获胜。

- 较轻的 `N=32,R=1.5`：Cluster Wall 增加 `1.6345 ms / 73.3%`；
- `N=256,R=3`：Wall `18.0767 → 52.3713 ms`，增加 `34.2946 ms / 189.7%`；
- 最重的 `N=512,R=12`：Wall `92.8707 → 478.0900 ms`，增加 `384.5096 ms / 415.3%`。

这不是“Clustered Shading 在理论上不行”，而是当前实现边界：串行 CPU 构建完整 130,560-cell CSR，并使用 `glBufferData` 全量上传。Compute Shader、GPU prefix/fill、紧凑 active-cluster、增量更新或更低 slice 数可能改变动态边界，不能把本结果外推到这些实现。

## 为什么边界呈中间宽、两头窄

- 低 N：候选灯本来很少，Cluster 的绝对节省不足 0.05 ms。N=32 所有 Cached 点均为 Tie。
- 中等 R：Z slice 最能排除不在当前深度段的灯，是 Cluster 的主胜区。
- 很大 R：灯球跨越更多 slice，Cluster 每像素候选数重新接近 Tile；例如 `N=512,R=12`，Tile/Cluster 平均每 cell 灯数为 `509.3/355.4`，Lighting 只快 2.18%，按冻结规则为 Tie。
- Cluster 的构建数据始终更大：`N=512,R=3` 常驻 CSR 为 `5.31/31.22 MiB`（Tile/Cluster）；`N=512,R=12` 为 `15.95/178.02 MiB`。这对 Cached 主要是内存代价，对 Rebuild 则直接变成每帧 count/fill/upload 代价。

## 可信度

- 420 个正式性能进程，300 帧预热 + 600 帧采样，3 个独立进程/路径，平衡交错顺序；
- 210/210 对 Tile/Cluster 截图逐字节一致；
- 独立重建并核对 100 份 CSR FNV；
- `N=512,R=12` 枚举 915,561,903 个真实 light-pixel interaction，Tile/Cluster miss 均为 0；
- RenderDoc replay 确认 Cluster 路径只有 1 个全屏 Draw、0 个 Stencil/Volume Draw，并实际绑定 3 个 grid TBO；
- 默认渲染路径仍为 `AnalyticScreen`。
