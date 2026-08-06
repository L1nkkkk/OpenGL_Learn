# 点光源数量 × 有效半径控制变量全因子实验报告

结论：60 个离线正式组合均零漏灯、半径误差合格、Master Pool 前缀一致。20 个 N×R Cell 中，`5` 个为 Culling-friendly、`5` 个为 Saturated，其余为 Transition。

冻结协议 SHA-256：`99D647C5EBBC4BAABDB789A2EFB83C81F13D60D83C753AA7F575481796DAA988`。本报告严格区分离线候选代理和现有 `analytic-screen` 实测时间；没有 Tile/Cluster Runtime，也没有 GPU 加速百分比。
分析器执行 SHA-256：`F0F17DBDD7CA5140373C3A3E4F17AB9C08FD9E6DE4406B0799FB25EA774AAB5A`。采集后仅修复离线分析器的 uint64 计数类型转换；冻结协议、Release EXE 与 60 份原始采集未改动。

## 1. 主 Seed 二维结果

| N | R | GT mean 灯/像素 | overlap | Cluster/Tile candidate | removable capture | Cluster CSR MiB | Oracle GPU 点光 ms | 分类 |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 32 | 1.5 | 0.699 | 0.022 | 0.459× | 0.717 | 1.36 | 0.1334 | culling-friendly |
| 32 | 3 | 3.611 | 0.113 | 0.544× | 0.619 | 3.60 | 0.3734 | transition |
| 32 | 6 | 14.423 | 0.451 | 0.700× | 0.687 | 7.16 | 0.8698 | transition |
| 32 | 12 | 29.047 | 0.908 | 0.933× | 0.758 | 12.52 | 1.3249 | saturated |
| 64 | 1.5 | 1.400 | 0.022 | 0.446× | 0.711 | 1.86 | 0.2278 | culling-friendly |
| 64 | 3 | 6.488 | 0.101 | 0.540× | 0.625 | 5.50 | 0.6772 | transition |
| 64 | 6 | 30.585 | 0.478 | 0.740× | 0.641 | 14.19 | 2.0027 | transition |
| 64 | 12 | 56.534 | 0.883 | 0.911× | 0.791 | 23.69 | 2.8631 | saturated |
| 128 | 1.5 | 2.266 | 0.018 | 0.387× | 0.760 | 2.54 | 0.3711 | culling-friendly |
| 128 | 3 | 10.860 | 0.085 | 0.542× | 0.598 | 9.63 | 1.2998 | transition |
| 128 | 6 | 60.525 | 0.473 | 0.747× | 0.603 | 27.65 | 3.9115 | transition |
| 128 | 12 | 113.188 | 0.884 | 0.918× | 0.727 | 46.04 | 5.8844 | saturated |
| 256 | 1.5 | 3.748 | 0.015 | 0.378× | 0.745 | 3.84 | 0.6265 | culling-friendly |
| 256 | 3 | 19.489 | 0.076 | 0.525× | 0.611 | 16.87 | 2.7046 | transition |
| 256 | 6 | 111.963 | 0.437 | 0.717× | 0.616 | 52.44 | 7.8340 | transition |
| 256 | 12 | 224.619 | 0.877 | 0.911× | 0.746 | 90.60 | 11.8897 | saturated |
| 512 | 1.5 | 6.306 | 0.012 | 0.349× | 0.750 | 6.72 | 1.2411 | culling-friendly |
| 512 | 3 | 41.476 | 0.081 | 0.533× | 0.620 | 31.19 | 5.3831 | transition |
| 512 | 6 | 211.198 | 0.412 | 0.703× | 0.615 | 100.89 | 15.1608 | transition |
| 512 | 12 | 441.533 | 0.862 | 0.905× | 0.712 | 177.99 | 23.5771 | saturated |

## 2. 因果解释

- 固定 R 增加 N 时，GT interactions、Tile/Cluster candidates 和当前逐灯 Draw/CPU 提交近似随 N 增长；每盏灯的空间覆盖不因 N 变化。
- 固定 N 增加 R 时，单灯球覆盖更多 XY Tile 与 Z Slice，真实 overlap 上升；Cluster 能消除的候选空间缩小，同时同一灯被复制进更多 Cluster，CSR 内存增长。
- `removableWorkCapture` 衡量 Cluster 捕获 Tile 可剔除误收的能力；它和 `overlapFraction` 必须一起看。高 overlap 下，即使索引准确，真实必须计算的灯也不能被剔除。

## 3. Phase-B 边界

- 推荐未来 Runtime A/B Anchor：`N=512, R=1.5`，分类 `culling-friendly`。主 Seed 与至少 2/3 Seed 均通过离线门槛；这只表示允许进入下一轮。
- 失败/饱和边界：`N=32, R=12.0`，分类 `saturated`。
- Culling-friendly 门槛：三 Seed 至少 2/3 同时满足 candidate ratio≤0.70、removable capture≥0.70、Cluster16≤64 MiB、零 miss。

## 4. 实测时间与离线代理

主 Seed 的 20 Cell 均使用同一 Release EXE，3 个独立进程，每进程 300 帧预热 + 600 帧采样。下列时间是当前精确逐灯 `analytic-screen` Oracle，不是 Tile/Cluster：

| N | R | CPU Frame Median ms | CPU Point Lights ms | GPU Point Lights ms | Draw Calls |
|---:|---:|---:|---:|---:|---:|
| 32 | 1.5 | 1.9347 | 0.0404 | 0.1334 | 428 |
| 32 | 3 | 2.2008 | 0.0411 | 0.3734 | 428 |
| 32 | 6 | 2.9040 | 0.0413 | 0.8698 | 428 |
| 32 | 12 | 3.3847 | 0.0407 | 1.3249 | 428 |
| 64 | 1.5 | 1.9911 | 0.0741 | 0.2278 | 460 |
| 64 | 3 | 2.4760 | 0.0748 | 0.6772 | 460 |
| 64 | 6 | 3.9870 | 0.0766 | 2.0027 | 460 |
| 64 | 12 | 4.8971 | 0.0766 | 2.8631 | 460 |
| 128 | 1.5 | 2.2187 | 0.1398 | 0.3711 | 524 |
| 128 | 3 | 3.4029 | 0.1417 | 1.2998 | 524 |
| 128 | 6 | 6.0213 | 0.1438 | 3.9115 | 524 |
| 128 | 12 | 7.9675 | 0.1420 | 5.8844 | 524 |
| 256 | 1.5 | 2.4745 | 0.2716 | 0.6265 | 652 |
| 256 | 3 | 4.6749 | 0.2769 | 2.7046 | 652 |
| 256 | 6 | 9.9864 | 0.2787 | 7.8340 | 652 |
| 256 | 12 | 13.9898 | 0.2740 | 11.8897 | 652 |
| 512 | 1.5 | 3.2681 | 0.5343 | 1.2411 | 908 |
| 512 | 3 | 7.4293 | 0.5442 | 5.3831 | 908 |
| 512 | 6 | 17.2219 | 0.5429 | 15.1608 | 908 |
| 512 | 12 | 25.5913 | 0.5343 | 23.5771 | 908 |

## 5. 正确性、硬件与限制

- Tile/Cluster miss 总数：`0`；确定性失败：`0`；半径失败：`0`；前缀失败：0。
- 正式 G-Buffer 有效像素：`2073600`；无效/天空像素：`0`。若后者为 0，本轮没有真实天空覆盖，只完成了 invalid skip 契约 smoke。
- GPU `GL_MAX_TEXTURE_BUFFER_SIZE`：`134217728` texels；它只用于未来上传可行性判断。
- 全量 Cluster CSR 是逻辑内存；Active-only、uint16 和 Global/Local 分流均为离线 what-if，不是实现或实测收益。
- 结论仅适用于固定 Sponza、相机、Uniform representative 灯位分布、1920×1080、16×16 Tile、16 个对数 Z Slice。聚集分布、移动相机/灯、阴影、透明 Forward、PBR、动态构建与上传未进入本矩阵。

## 6. 图表、截图与复现

- `charts/gt-mean-heatmap.png`、`candidate-ratio-heatmap.png`、`cluster-csr-memory-heatmap.png`、`analytic-screen-gpu-heatmap.png`、`removable-work-capture-heatmap.png`；
- `charts/fixed-radius-candidate-ratio.png`、`charts/fixed-count-candidate-ratio.png`；
- `charts/seed-overlap-median-min-max.png`、`seed-candidate-ratio-median-min-max.png`、`seed-cluster-memory-median-min-max.png`：三 Seed Median [Min, Max]；
- `screenshots/*-exact-analytic-screen.png`：真实 Release renderer；`heatmaps/*-renderer-tile-cluster.png`：离线列表长度解释图；
- `cases/*.json`、`summary.csv`、`cells/*.csv.gz`、`csr/*.npz`、`pixel-counts/*.npz`、`timing/`、`verification/`、`artifact-manifest.json`；
- 复现：`powershell -ExecutionPolicy Bypass -File .\tools\run_count_radius_factorial.ps1 -Mode All`。
