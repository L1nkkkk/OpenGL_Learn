# Tile16 / Cluster16 实际运行时性能边界报告

## 结论

本实验不是离线候选数估算，而是在同一 OpenGL 3.3 Deferred Lighting 路径中实际执行 Tile16 与 Cluster16。正式矩阵共 420 个独立进程，每个进程 300 帧预热、600 帧采样；所有配对截图逐字节一致。

- Cached/静态列表：Wall Frame 判定 Cluster/Tile/Tie = 22/0/28；GPU Frame = 29/0/21；仅点光 Lighting GPU = 28/0/22。
- Rebuild/每帧重建：Wall Frame 判定 Cluster/Tile/Tie = 0/20/0；仅点光 Lighting GPU = 9/0/11。
- 主结论必须看 Wall/GPU Frame 与 Lighting GPU 两层；CPU 与 GPU 并行，未把二者错误相加。
- 胜负规则在采集前冻结：3/3 独立配对同方向，同时绝对差至少 0.05 ms、相对差至少 3%；否则为 Tie。

## 边界表：Cached（Δ = Cluster16 − Tile16）

| N | R | Wall Δ / 判定 | GPU Frame Δ / 判定 | Lighting GPU Δ / 判定 | Tile indices | Cluster indices |
|---:|---:|---:|---:|---:|---:|---:|
| 32 | 1.5 | -0.0444 ms / tie | -0.0082 ms / tie | -0.0080 ms / tie | 23,177 | 95,323 |
| 32 | 2 | +0.0807 ms / tie | -0.0071 ms / tie | -0.0077 ms / tie | 45,429 | 233,895 |
| 32 | 2.5 | -0.0257 ms / tie | -0.0225 ms / tie | -0.0238 ms / tie | 84,632 | 435,606 |
| 32 | 3 | +0.0143 ms / tie | -0.0262 ms / tie | -0.0272 ms / tie | 111,802 | 681,665 |
| 32 | 4 | -0.0626 ms / tie | -0.0311 ms / tie | -0.0334 ms / tie | 148,935 | 1,010,902 |
| 32 | 5 | -0.0319 ms / tie | -0.0302 ms / tie | -0.0312 ms / tie | 184,790 | 1,383,713 |
| 32 | 6 | -0.0863 ms / tie | -0.0309 ms / tie | -0.0306 ms / tie | 208,779 | 1,616,500 |
| 32 | 8 | +0.0029 ms / tie | -0.0219 ms / tie | -0.0227 ms / tie | 241,412 | 2,163,502 |
| 32 | 10 | -0.0040 ms / tie | -0.0127 ms / tie | -0.0125 ms / tie | 256,224 | 2,685,932 |
| 32 | 12 | +0.0036 ms / tie | -0.0038 ms / tie | -0.0039 ms / tie | 260,032 | 3,020,832 |
| 64 | 1.5 | -0.0229 ms / tie | -0.0114 ms / tie | -0.0127 ms / tie | 51,734 | 226,690 |
| 64 | 2 | -0.0383 ms / tie | -0.0235 ms / tie | -0.0256 ms / tie | 92,340 | 489,533 |
| 64 | 2.5 | -0.0443 ms / tie | -0.0453 ms / tie | -0.0493 ms / tie | 152,566 | 803,068 |
| 64 | 3 | -0.0598 ms / cluster16 | -0.0507 ms / cluster16 | -0.0538 ms / cluster16 | 200,493 | 1,179,940 |
| 64 | 4 | -0.0797 ms / cluster16 | -0.0618 ms / cluster16 | -0.0614 ms / cluster16 | 297,196 | 2,010,490 |
| 64 | 5 | -0.0452 ms / tie | -0.0527 ms / cluster16 | -0.0526 ms / cluster16 | 374,378 | 2,871,814 |
| 64 | 6 | -0.0428 ms / tie | -0.0588 ms / cluster16 | -0.0550 ms / cluster16 | 419,885 | 3,459,650 |
| 64 | 8 | -0.0143 ms / tie | -0.0456 ms / tie | -0.0452 ms / tie | 482,864 | 4,481,346 |
| 64 | 10 | -0.0148 ms / tie | -0.0299 ms / tie | -0.0298 ms / tie | 510,748 | 5,293,392 |
| 64 | 12 | -0.0120 ms / tie | -0.0171 ms / tie | -0.0174 ms / tie | 519,724 | 5,950,204 |
| 128 | 1.5 | -0.0193 ms / tie | -0.0297 ms / tie | -0.0328 ms / tie | 95,168 | 404,104 |
| 128 | 2 | -0.0157 ms / tie | -0.0527 ms / cluster16 | -0.0572 ms / cluster16 | 167,271 | 833,549 |
| 128 | 2.5 | -0.0761 ms / cluster16 | -0.0911 ms / cluster16 | -0.0931 ms / cluster16 | 277,583 | 1,457,495 |
| 128 | 3 | -0.0579 ms / tie | -0.1006 ms / cluster16 | -0.1003 ms / cluster16 | 379,129 | 2,264,510 |
| 128 | 4 | -0.2199 ms / cluster16 | -0.1283 ms / cluster16 | -0.1263 ms / cluster16 | 610,845 | 4,175,827 |
| 128 | 5 | -0.1653 ms / cluster16 | -0.1110 ms / cluster16 | -0.1108 ms / cluster16 | 760,583 | 5,827,178 |
| 128 | 6 | -0.0822 ms / tie | -0.2477 ms / cluster16 | -0.1009 ms / cluster16 | 850,201 | 6,987,799 |
| 128 | 8 | -0.0623 ms / tie | -0.0799 ms / cluster16 | -0.0830 ms / cluster16 | 970,578 | 9,152,452 |
| 128 | 10 | -0.0496 ms / tie | -0.0658 ms / tie | -0.0628 ms / tie | 1,022,946 | 10,640,252 |
| 128 | 12 | -0.0460 ms / tie | -0.0415 ms / tie | -0.0336 ms / tie | 1,040,468 | 11,809,016 |
| 256 | 1.5 | -0.0802 ms / cluster16 | -0.0685 ms / cluster16 | -0.0747 ms / cluster16 | 185,724 | 746,810 |
| 256 | 2 | -0.0969 ms / cluster16 | -0.1147 ms / cluster16 | -0.1166 ms / cluster16 | 319,858 | 1,494,153 |
| 256 | 2.5 | -0.2449 ms / cluster16 | -0.1736 ms / cluster16 | -0.1736 ms / cluster16 | 515,226 | 2,638,334 |
| 256 | 3 | -0.3414 ms / cluster16 | -0.2093 ms / cluster16 | -0.2044 ms / cluster16 | 717,367 | 4,161,026 |
| 256 | 4 | -0.2795 ms / cluster16 | -0.3951 ms / cluster16 | -0.2566 ms / cluster16 | 1,160,873 | 7,637,102 |
| 256 | 5 | -0.2246 ms / cluster16 | -0.2790 ms / cluster16 | -0.4613 ms / cluster16 | 1,492,002 | 11,020,206 |
| 256 | 6 | -0.3312 ms / cluster16 | -0.3243 ms / cluster16 | -0.2491 ms / cluster16 | 1,689,528 | 13,485,714 |
| 256 | 8 | -0.2462 ms / cluster16 | -0.2148 ms / cluster16 | -0.2264 ms / cluster16 | 1,931,438 | 17,746,687 |
| 256 | 10 | -0.1671 ms / tie | -0.1733 ms / cluster16 | -0.1371 ms / tie | 2,040,606 | 20,884,884 |
| 256 | 12 | -0.0947 ms / tie | -0.1149 ms / tie | -0.1023 ms / tie | 2,081,208 | 23,488,832 |
| 512 | 1.5 | -0.1603 ms / cluster16 | -0.1649 ms / cluster16 | -0.1667 ms / cluster16 | 388,277 | 1,500,070 |
| 512 | 2 | -0.2634 ms / cluster16 | -0.2398 ms / cluster16 | -0.2371 ms / cluster16 | 648,797 | 3,001,507 |
| 512 | 2.5 | -0.4782 ms / cluster16 | -0.3345 ms / cluster16 | -0.3301 ms / cluster16 | 988,050 | 5,057,487 |
| 512 | 3 | -0.4266 ms / cluster16 | -0.5617 ms / cluster16 | -0.3835 ms / cluster16 | 1,367,128 | 7,915,772 |
| 512 | 4 | -0.6683 ms / cluster16 | -0.6441 ms / cluster16 | -0.5545 ms / cluster16 | 2,239,564 | 14,822,197 |
| 512 | 5 | -0.7412 ms / cluster16 | -0.6798 ms / cluster16 | -0.6957 ms / cluster16 | 2,940,892 | 21,596,216 |
| 512 | 6 | -0.6275 ms / cluster16 | -0.6217 ms / cluster16 | -0.5818 ms / cluster16 | 3,333,972 | 26,185,323 |
| 512 | 8 | -0.6514 ms / cluster16 | -0.6340 ms / cluster16 | -0.7307 ms / cluster16 | 3,822,570 | 34,344,226 |
| 512 | 10 | -0.4417 ms / cluster16 | -0.4759 ms / cluster16 | -0.4432 ms / cluster16 | 4,064,104 | 40,647,880 |
| 512 | 12 | -0.2298 ms / tie | -0.2266 ms / tie | -0.2226 ms / tie | 4,155,684 | 46,397,828 |

## 边界表：Rebuild Every Frame

| N | R | Wall Δ / 判定 | Build CPU Δ | Upload CPU Δ | Lighting GPU Δ / 判定 |
|---:|---:|---:|---:|---:|---:|
| 32 | 1.5 | +1.6345 ms / tile16 | +1.4627 ms | +0.0421 ms | -0.0060 ms / tie |
| 32 | 3 | +5.2817 ms / tile16 | +4.3012 ms | +0.3921 ms | -0.0192 ms / tie |
| 32 | 6 | +12.4195 ms / tile16 | +10.4608 ms | +0.8991 ms | -0.0239 ms / tie |
| 32 | 12 | +24.8279 ms / tile16 | +21.6230 ms | +1.6708 ms | +0.0043 ms / tie |
| 64 | 1.5 | +2.1798 ms / tile16 | +1.9808 ms | +0.0602 ms | -0.0082 ms / tie |
| 64 | 3 | +8.7918 ms / tile16 | +7.3829 ms | +0.6486 ms | -0.0439 ms / tie |
| 64 | 6 | +28.2741 ms / tile16 | +25.0743 ms | +1.7177 ms | -0.0441 ms / tie |
| 64 | 12 | +56.9779 ms / tile16 | +51.5546 ms | +3.0395 ms | -0.0034 ms / tie |
| 128 | 1.5 | +3.4134 ms / tile16 | +2.7744 ms | +0.2452 ms | -0.0251 ms / tie |
| 128 | 3 | +16.8432 ms / tile16 | +14.7648 ms | +1.0689 ms | -0.0862 ms / cluster16 |
| 128 | 6 | +63.8718 ms / tile16 | +57.6871 ms | +3.4436 ms | -0.0823 ms / cluster16 |
| 128 | 12 | +99.1004 ms / tile16 | +88.3443 ms | +5.9780 ms | -0.2427 ms / cluster16 |
| 256 | 1.5 | +5.2620 ms / tile16 | +4.4005 ms | +0.4127 ms | -0.0634 ms / cluster16 |
| 256 | 3 | +34.2946 ms / tile16 | +30.7627 ms | +1.9562 ms | -0.1708 ms / cluster16 |
| 256 | 6 | +118.8913 ms / tile16 | +107.3872 ms | +6.5232 ms | -0.4499 ms / cluster16 |
| 256 | 12 | +194.3979 ms / tile16 | +173.5777 ms | +11.7461 ms | -0.0605 ms / tie |
| 512 | 1.5 | +10.1116 ms / tile16 | +8.6825 ms | +0.6497 ms | -0.1475 ms / cluster16 |
| 512 | 3 | +71.0360 ms / tile16 | +64.6039 ms | +3.6373 ms | -0.3326 ms / cluster16 |
| 512 | 6 | +232.5768 ms / tile16 | +210.4179 ms | +12.5516 ms | -0.5100 ms / cluster16 |
| 512 | 12 | +384.5096 ms / tile16 | +342.7339 ms | +23.4218 ms | -0.2172 ms / tie |

## 如何读这个边界

Cluster16 通过对数 Z 切片减少每个像素遍历的候选灯，但把逻辑网格从 8,160 个 Tile 扩大到 130,560 个 Cluster。列表可缓存时，这笔构建成本被摊销，候选减少可能转化为 GPU Lighting 收益；列表每帧重建时，额外 metadata、count/prefix/fill 与上传成本可能远大于像素阶段节省。

因此这里没有“Cluster 永远优于 Tile”的结论。边界由 N、有效半径 R、列表更新频率共同决定；图中的 Tie 是按预冻结门槛判定，并不等于数值完全相同。

## 正确性与公平性

- 正式配对：210/210 张 Tile/Cluster 配对截图逐字节一致。
- 两条路径共享同一灯数据、G-Buffer、材质/光照公式、精确逐像素球体谓词、TBO 格式与一个全屏 Draw；唯一变量是候选列表是否追加 16 个对数 Z 切片。
- CSR 无固定容量截断；超出纹理缓冲区/uint32 容量会使进程失败，而不是漏灯。
- Cached 中 Build/Upload zone 的采样是稳态 cache-hit 空作用域；一次性首帧构建发生在 300 帧预热内，不冒充每帧成本。Rebuild 子阶段才表示真实每帧 Bounds/Count/Prefix/Fill。

## 独立验证与 RenderDoc

- 独立验证器按 JSON 中的灯和相机重新构建 50 组 Tile/Cluster、共 100 份 CSR；`logicalCells`、index 数、内存、每 cell 最大/平均灯数与 streaming FNV-1a 均和 C++ 运行时完全一致。
- 在 `N=512, R=12` 上枚举 2,073,600 个 G-Buffer 有效像素与全部 512 盏灯，共 915,561,903 个真实 light-pixel interaction；Tile16 与 Cluster16 的候选漏失均为 0。
- Oracle 三组质量门槛全部通过：`N512/R1.5` max/mean=1/0.00271 LSB，`N128/R10` 为 1/0.03775，`N512/R12` 为 2/0.06639；三组 Tile/Cluster 仍逐字节一致。
- RenderDoc 1.45 在 `Cached, N=512, R=3, Cluster16` 成功 capture/replay：点光阶段只有 1 个全屏 Draw，0 个 Tile/Legacy/Stencil/Volume Draw；`gridMetadata`、`gridIndices`、`gridLights` 三个 Texture Buffer 均实际绑定，Replay Debug Message=0。

## 图表

- `charts/cached-wall-delta.png`：Cached 端到端 Wall 边界。
- `charts/cached-gpu-frame-delta.png`：Cached 整帧 GPU 边界。
- `charts/cached-lighting-gpu-delta.png`：Cached 点光像素阶段边界。
- `charts/rebuild-wall-delta.png`：每帧重建时的端到端边界。
- `charts/csr-index-ratio.png`：Cluster/Tile CSR index 数量比。

## 可复现入口

- `pre-capture-manifest.json`：采集前冻结的矩阵、协议/二进制/源码哈希。
- `capture-manifest.json`：每个进程的结果、截图与日志哈希。
- `aggregate.json` / `summary.csv`：完整聚合数据。
- `verification/independent-verification.json`：独立验证器输出。
- `tools/run_tile_cluster_runtime_boundary.ps1`：正式复现实验入口。
