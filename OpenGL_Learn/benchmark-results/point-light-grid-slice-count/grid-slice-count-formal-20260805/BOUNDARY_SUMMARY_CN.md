# Point-Light Grid Z 切片数边界结论

## 一句话结论

Tile 可以加入深度维度，但不能默认固定为 16 层。当前 OpenGL 3.3、CPU 构建 CSR/TBO 的实现中，Z 切片只在 **Grid 可缓存且灯数/覆盖关系合适** 时带来端到端收益；若每帧重建，正式矩阵内全部场景都应保持 `S=1`。

本轮没有修改产品默认路径：默认仍为 `analytic-screen`。以下结论只适用于实验用的 Grid 路径，不外推到 GPU Compute clustered shading。

## 冻结实验

- Release x64，1920×1080，VSync off，固定 Sponza、相机和 seed。
- `S={1,2,4,8,16}`，XY tile 始终为 16×16 像素；五档共享 builder、CSR/TBO、light payload、shader、逐像素精确球体谓词和灯累加顺序，唯一变量是 Z 切片数。
- Cached：25 个 `(N,R)` 场景；Rebuild Every Frame：16 个场景。每档 3 个独立进程，每进程 300 帧预热 + 600 帧采样，共 615 个性能进程。
- Go 规则：三组配对进程同方向、配对差值中位数至少 0.05 ms、配对相对差中位数至少 3%。统计 Tie 时优先更小 S；未通过相对 S=1 的 Go 门槛时，最终运行选择仍为 S=1。

## Cached 的最终运行选择

表中数字是通过 Go 门槛后的最终 S；`1` 包含无收益或统计 Tie 的场景。

| 灯数 N \ 半径 R | 1.5 | 3 | 6 | 8 | 12 |
|---:|---:|---:|---:|---:|---:|
| 32  | 1 | 1 | 1 | 1 | 1 |
| 64  | 1 | 1 | 2 | 1 | 1 |
| 128 | 1 | 1 | 2 | 1 | 1 |
| 256 | 2 | 8 | 2 | 1 | 1 |
| 512 | 8 | 8 | 8 | 2 | 1 |

因此 Cached 为 **9/25 Go**。边界不是“灯越多、S 越大”：灯覆盖半径过大时会跨越更多 Z slice，候选引用复制和 TBO 常驻内存迅速增长；例如 `N=512,R=12` 的数值最低点是 S=8，但相对 S=1 未过冻结门槛，最终仍选 S=1。

## Rebuild Every Frame

16/16 个正式场景最终均为 `S=1`。以 `N=256,R=6` 为例：

| S | Wall Frame | Grid Build CPU | Upload CPU | Lighting GPU | 常驻内存 |
|---:|---:|---:|---:|---:|---:|
| 1  | 36.2683 ms | 33.2871 ms | 0.9338 ms | 2.6718 ms | 6.523 MiB |
| 2  | 49.2143 ms | 45.1366 ms | 1.5662 ms | 2.2708 ms | 11.085 MiB |
| 4  | 61.9142 ms | 56.2910 ms | 2.2866 ms | 2.2780 ms | 15.893 MiB |
| 8  | 93.2301 ms | 84.2560 ms | 4.0725 ms | 2.2282 ms | 28.326 MiB |
| 16 | 156.5669 ms | 141.7283 ms | 7.5318 ms | 2.1900 ms | 52.456 MiB |

深度切片确实降低了 Lighting GPU，但 CPU 构表与上传增长远大于这部分收益。CPU/GPU 是流水并行关系，上表的分项不可直接相加；最终判定只看 Wall Frame。

## 代表性 Cached 场景

`N=512,R=6` 的统计顶层集合是 `{8,16}`。S=16 数值上更低，但相对 S=8 未形成冻结规则下的显著胜出，所以工程选择 S=8，以避免近一倍的常驻内存。

| S | Wall Frame | Lighting GPU | CSR 引用数 | 常驻内存 |
|---:|---:|---:|---:|---:|
| 1  | 8.0176 ms | 5.8115 ms | 3.334 M | 12.812 MiB |
| 2  | 7.5874 ms | 5.3836 ms | 5.550 M | 21.328 MiB |
| 4  | 7.5990 ms | 5.4165 ms | 8.019 M | 30.870 MiB |
| 8  | 7.4560 ms | 5.2883 ms | 14.210 M | 54.737 MiB |
| 16 | 7.3485 ms | 5.1886 ms | 26.185 M | 100.916 MiB |

最终 S=8 相对 S=1：Wall Frame `-0.5616 ms / -7.00%`，Lighting GPU 的三轮配对中位差为 `-0.5232 ms / -9.93%`。三轮 Wall Frame 均同向：`-0.5530/-0.5616/-0.6064 ms`。

正式矩阵中最大相对 Wall 改进为 `N=256,R=3,S=8` 的 `-13.16%`；最大绝对改进为上面的 `N=512,R=6,S=8`，二者不要混写。

`N=512,R=8,S=2` 的独立 median-of-medians 显示为 `-2.79%`，但冻结判据使用同 round 配对：三轮均同向，配对差中位数 `-0.3560 ms`，配对相对差中位数 `-3.62%`，因此判为 Go。这是统计口径差异，不是修改门槛。

## 正确性证据

- 123/123 组正式截图中，五个 S 逐字节一致。
- 独立验证器重建 125 组唯一 CSR，logical/non-empty cells、indices、内存、max/average 与 streaming FNV-1a 全部一致。
- 低/边界/高覆盖三组真实 G-buffer 全图枚举，共 1,160,805,552 个 light-pixel 真值交互，五个 S 的漏灯数均为 0。
- 相对 `analytic-screen` Oracle：三组均通过 `max≤2 LSB、mean≤0.1 LSB、P99≤1 LSB`；最重组为 max=2、mean=0.0664、P99=1。
- 正式 1920×1080 相机下 Sponza 填满了画面，不能单靠该矩阵声称验证了 sky/invalid；补充的超宽视口实际落到 5758×64，其中 13,504 个像素（3.66%）为无效 G-buffer，S=1/S=8 的最终图像、Position 和有效性缓冲仍逐字节一致。
- N=0、N=1、near-plane、camera-inside、fully-offscreen、Z 边界、S 切换缓存失效均通过；旧 Tile16/S1 与 Cluster16/S16 的 CSR 和图像端点兼容通过。
- 同一正式二进制的单进程 Resize 冒烟从 640×360 经两次窗口调整后得到 1008×537 客户区；Grid `buildCount=3`、`uploadCount=3`、`cacheHitCount=7998`，最终 63×34×8 个 cell 与新分辨率严格对应，且无 GL/Shader 错误。
- RenderDoc 的 `N=512,R=6,S=8` 捕获可回放：点光阶段 1 次全屏 Draw、0 次 stencil/volume Draw，实际片元 uniform `gridSliceCount=8`，metadata/index/light 三张 TBO 均绑定，debug message=0。

## 可落地结论

这不是“Cluster 一定优于 Tile”，而是一个明确的成本模型：

1. 增加 S 会减少像素实际遍历的候选灯；
2. 同时会复制跨 slice 的灯引用，并线性增加 cell metadata；
3. Grid 静态可缓存时，前者可能胜出；
4. Grid 每帧重建时，后两项在当前 CPU builder 上占主导。

若后续做运行时策略，先只把本轮结果表述为“离线选择/边界实验”，不要宣称已经实现自适应：当前代码支持参数化 S，但产品默认仍为 `analytic-screen`，也没有把这张场景相关、硬件相关的相图硬编码成通用策略。
