# 可复现多点光源压力基准（Legacy Deferred）

> 状态：通过；生成时间：2026-08-01T12:53:37.068544+00:00。这是基准建设，不包含渲染优化。

## 场景与隔离变量

- 场景：项目自带 Crytek Sponza，模型归一化半径 15；固定相机 `(-6, -1.5, 0)` 看向 `(6, -0.8, 0)`，FOV 55°。
- 路径：`phong-deferred-volume`，沿用逐灯 stencil volume + lighting volume 的 Legacy 顺序。
- 固定条件：Release x64、1920×1080、请求 VSync 关闭、显式 gPosition、Bloom/SSAO/所有阴影关闭。点光源调试球和完整编辑器 UI 在基准模式关闭。
- 默认 framebuffer：sample buffers=1，samples=4；HDR/后处理、天空盒和场景透明材质仍沿用项目现状。
- 生成器：固定 xorshift32 v1，seed `0x21D3F3A5`；灯光顺序、位置、8 色固定调色板、强度与衰减均进入 scene/submission 签名。
- representative 半径约 3.686；high-overlap 半径约 9.813。edge-cases 使用 representative 衰减，并显式包含近裁面相交、相机位于光体内和完全离屏灯。

## 环境与协议

- CPU：12th Gen Intel(R) Core(TM) i7-12700KF。
- GPU：NVIDIA GeForce RTX 5060 Ti/PCIe/SSE2；驱动：32.0.15.9186。
- OpenGL：3.3.0 NVIDIA 591.86（NVIDIA Corporation）。
- OS：Microsoft Windows 11 专业版 10.0.26200 build 26200。
- 正式协议：每配置 3 个独立进程；每进程预热 300 帧、采样 600 帧。百分位按进程原始样本合并后线性插值计算；每进程值仍单独保留。

## 每进程正式结果

| 灯数 | 进程 | CPU Median/P95/P99 ms | GPU Median/P95/P99 ms | Deferred GPU ms | 点光源 GPU ms | Draw | 提交/剔除 | Stencil clear |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 1 | 2.743/3.129/3.293 | 1.516/1.696/1.766 | 1.433 | 0.844 | 428 | 16/0 | 35 |
| 16 | 2 | 2.781/3.279/3.490 | 1.517/1.692/1.857 | 1.434 | 0.844 | 428 | 16/0 | 35 |
| 16 | 3 | 2.734/3.180/3.329 | 1.515/1.708/1.886 | 1.432 | 0.843 | 428 | 16/0 | 35 |
| 64 | 1 | 4.799/5.344/5.588 | 3.635/4.027/4.332 | 3.548 | 2.890 | 524 | 64/0 | 131 |
| 64 | 2 | 4.776/5.336/5.571 | 3.603/4.025/4.160 | 3.508 | 2.882 | 524 | 64/0 | 131 |
| 64 | 3 | 4.725/5.253/5.492 | 3.565/3.962/4.146 | 3.481 | 2.877 | 524 | 64/0 | 131 |
| 256 | 1 | 13.066/13.996/14.391 | 11.835/12.579/12.929 | 11.743 | 11.076 | 908 | 256/0 | 515 |
| 256 | 2 | 13.044/13.947/14.389 | 11.825/12.616/12.888 | 11.735 | 11.060 | 908 | 256/0 | 515 |
| 256 | 3 | 13.134/14.019/14.381 | 11.865/12.696/13.018 | 11.758 | 11.086 | 908 | 256/0 | 515 |
| 512 | 1 | 23.855/25.387/26.604 | 22.542/23.754/24.231 | 22.442 | 21.748 | 1420 | 512/0 | 1027 |
| 512 | 2 | 23.886/25.181/25.824 | 22.600/23.783/24.128 | 22.504 | 21.815 | 1420 | 512/0 | 1027 |
| 512 | 3 | 23.907/25.462/26.217 | 22.628/23.825/24.357 | 22.525 | 21.873 | 1420 | 512/0 | 1027 |

## 跨进程聚合与 60 FPS 阈值

| 灯数 | 进程×样本 | CPU Median/P95/P99 ms | GPU Median/P95/P99 ms | Deferred GPU ms | 点光源 GPU ms | 3/3 稳定低于 60 FPS |
|---:|---:|---:|---:|---:|---:|:---:|
| 16 | 3×600 | 2.751/3.203/3.403 | 1.516/1.700/1.852 | 1.433 | 0.844 | 否（0/3） |
| 64 | 3×600 | 4.763/5.303/5.588 | 3.586/4.009/4.263 | 3.498 | 2.881 | 否（0/3） |
| 256 | 3×600 | 13.079/13.996/14.391 | 11.843/12.643/12.950 | 11.745 | 11.073 | 否（0/3） |
| 512 | 3×600 | 23.886/25.379/26.118 | 22.588/23.801/24.292 | 22.497 | 21.809 | 是（3/3） |

首次稳定低于 60 FPS 的配置是 **representative / 512 灯**：所有独立进程的 CPU 或 GPU Frame Median 均超过 16.67 ms。

## 正确性与复现性

- 所有结果均要求 `success=true`、GL error-free、GPU/CPU/zone 样本数完整、截图非黑屏。
- 同一覆盖形态和灯数的 scene/submission 签名必须跨进程完全一致；不同灯数不要求截图逐像素一致。
- Legacy 路径没有点光源视锥/Scissor 剔除，因此完全离屏灯仍真实提交，正式结果中的剔除数应为 0。

| 分类/形态 | 灯数 | Scene signature | Submission signature | 进程一致 |
|---|---:|---|---|:---:|
| formal/representative | 16 | `0x28cdb6b119b52795` | `0xff25d7196616c895` | 是（3/3） |
| formal/representative | 64 | `0xcd6256a94b8fc73c` | `0xd250ffea61878314` | 是（3/3） |
| formal/representative | 256 | `0x53c86b88620f757d` | `0xda0284a310591088` | 是（3/3） |
| formal/representative | 512 | `0x93b2fb98f925264f` | `0xee449aa19906ca3b` | 是（3/3） |
| smoke/edge-cases | 16 | `0xc668f218476e0afc` | `0xfb6f89484981fe81` | 是（1/1） |
| smoke/high-overlap | 16 | `0x1d9822b67d99e5aa` | `0xeaa95faf1a53aa75` | 是（1/1） |
| smoke/representative | 16 | `0x28cdb6b119b52795` | `0xff25d7196616c895` | 是（1/1） |

| Smoke | 非黑屏 | 近裁面 | 相机在光体内 | 完全离屏 |
|---|:---:|:---:|:---:|:---:|
| edge-cases / 16 | 是 | 是 | 是 | 是 |
| high-overlap / 16 | 是 | 不适用 | 不适用 | 不适用 |
| representative / 16 | 是 | 不适用 | 不适用 | 不适用 |

## 产物

- 精简基准数据：[legacy-baseline-20260801](benchmark-results/point-light-heavy/legacy-baseline-20260801/)（仓库仅保留聚合数据与清单，逐帧原始数据按保留策略清理）。
- 曲线图：[legacy-point-light-scaling.svg](docs/benchmark-images/point-light-heavy/legacy-baseline-20260801/legacy-point-light-scaling.svg)。
- 固定相机截图：[PNG 便览](docs/benchmark-images/point-light-heavy/legacy-baseline-20260801/)；PPM 原始证据仅在本地精简归档中保留代表样本。

## 复现命令

在项目目录 `OpenGL_Learn` 中运行以下命令，会重建 Release 并执行 16/64/256/512 各 3 个正式进程及三种覆盖形态 smoke：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_point_light_heavy.ps1 -Mode All
```

## 已知限制与候选瓶颈

- 点光源 stencil 与 lighting 在 Legacy 循环内逐灯交错。为避免为每盏灯插入大量 timestamp query 并污染基线，本次只导出完整 `Deferred Point Lights` GPU 区间；`Deferred Lighting` 包含方向光全屏与点光源阶段，未伪造 stencil/lighting 子阶段数据。
- CPU Frame 是提交侧范围，GPU Frame 是 timestamp 范围；两者与 wall frame 语义不同，阈值判断只使用用户要求的 CPU/GPU Frame Median。
- 基准显式调用 `glfwSwapInterval(0)` 并在 JSON 记录 `requestedSwapInterval=0`；OpenGL/GLFW 没有可移植的驱动强制 VSync 回读接口，因此控制面板级覆盖无法在进程内独立确认。
- 从真实计数看，点光源阶段每灯固定 2 个 draw，point-light stencil clear 固定为每灯 2 次，总 stencil clear 为 `2N+3`；这只是 Legacy 工作量证据，不代表任何优化收益。
- 最大正式配置 512 灯的 GPU Frame Median 为 22.588 ms，其中点光源阶段 Median 为 21.809 ms（两个 Median 的比值约 96.5%）；这把 Legacy 点光源阶段列为首要候选瓶颈，但不代表任何优化收益。
- 同为 16 灯、428 Draw、35 次 stencil clear 的 smoke 中，high-overlap 点光源 GPU Median 为 1.966 ms，representative 为 0.843 ms（约 2.33 倍）；因此光体积像素覆盖与 overdraw 是有实测依据的候选调查项。
- 结合线性增长的每灯 draw/clear 计数，可把逐灯状态/Uniform 更新与模板清理列为后续候选调查项；本任务未实现 Scissor、灯光剔除、批处理、减少 clear 或 Stencil 策略切换。
