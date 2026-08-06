# Deferred 点光源着色优化归档

> 归档日期：2026-08-07
> 当前默认路径：`analytic-screen`
> 范围：OpenGL 3.3 Deferred、无阴影点光源压力场景、1920×1080
> 原则：只记录已由正式 A/B、图像对照和 RenderDoc 支持的结论。

## 1. 最终结论

本轮真正落地的核心优化是：

**构建点光源保守投影 Screen Rect，以每灯一次带 Scissor 的矩形 Draw，替代每灯两次 Stencil Light Volume Draw；片元阶段仍读取真实 G-Buffer 三维位置并执行精确球体影响判定和相同光照公式。**

最终状态如下：

| 路径或实验 | 决策 | 当前状态 |
|---|---|---|
| 相邻 Stencil Clear 合并 | Go | 保留给显式 Volume 路径 |
| Scissored Coalesced Volume | Go | 保留为对照与回退实验路径 |
| Analytic Screen | **Go** | **正式默认路径** |
| Offscreen Culling | No-Go | 默认关闭 |
| Adaptive Volume/Screen Selector | Not-Implemented / No-Go | 不存在运行时自适应逻辑 |
| CPU CSR Tile S1 | 静态条件 Go、默认 No-Go | 仅实验路径 |
| Cluster / 深度切片 | Cached 局部 Go、Rebuild No-Go | 仅实验路径 |
| 八方向聚合光照 | No-Go | 未进入 Runtime |

## 2. 问题与性能基线

Legacy Deferred 对每盏点光源执行：

1. 绘制球体写入 Stencil；
2. 再绘制球体计算并累加光照；
3. 清理本灯产生的 Stencil 临时状态。

因此点光源增量 Draw 为 `2N`。在 representative / 512 灯正式基线中：

- GPU Frame Median：22.588 ms；
- 点光源阶段 GPU Median：21.809 ms，占 GPU Frame 约 96.5%；
- 总 Draw：1,420，其中点光源贡献 1,024 次 Volume Draw；
- 所有三个独立进程均稳定低于 60 FPS。

完整基线与复现入口见：[可复现多点光源压力基准](POINT_LIGHT_HEAVY_BASELINE_CN.md)。

## 3. 优化演进

### 3.1 相邻 Stencil Clear 合并

Legacy 路径每灯执行 ClearBefore 和 ClearAfter，共 `2N` 次点光 Clear。相邻两盏灯之间，上一灯的 ClearAfter 与下一灯的 ClearBefore 连续且等价，因此可以安全合并为：

```text
首灯前 1 次 Initial Clear + 每灯后 1 次 Clear = N + 1
```

不能只在整个阶段开头清一次，否则下一盏灯会继承上一盏灯的 Stencil Mask；最后一次 Clear 也必须保留，以保证 Pass 出口 Stencil 为零。

正式 A/B：

| 灯数 | 点光 Clear | 点光 GPU Median | 改善 |
|---:|---:|---:|---:|
| 256 | 512 → 257 | 11.061 → 8.810 ms | -2.251 ms / -20.35% |
| 512 | 1,024 → 513 | 21.675 → 17.418 ms | -4.257 ms / -19.64% |

Draw、提交灯数、灯序和图像均未变化。RenderDoc 在 512 灯下确认 512 次 Stencil Draw、512 次 Lighting Volume Draw 和 513 次点光 Clear。

证据：[Stencil Clear 合并正式报告](benchmark-results/point-light-stencil-clear-ab/stencil-clear-coalescing-ab-20260802/REPORT_CN.md)。

### 3.2 Scissored Coalesced Volume

为每盏灯计算保守 Screen Rect，将两次 Volume Draw 和灯后 Clear 限制在该矩形内；不改变双 Draw、Stencil 生命周期、灯序和光照公式。

正式 A/B：

| 灯数 | Coalesced Volume | Scissored Volume | 改善 |
|---:|---:|---:|---:|
| 256 | 9.0943 ms | 8.3548 ms | -0.7394 ms / -8.13% |
| 512 | 17.697 ms | 16.265 ms | -1.432 ms / -8.09% |

512 灯的点光 Clear Pixel Area 减少 49.70%，但 GPU 只改善约 8.1%，说明覆盖面积只能作为机制解释，不能直接换算成 GPU 时间。

该阶段证明解析 Screen Bounds 有效，但仍保留每灯两次 Volume Draw 和 Stencil 工作，因此没有成为最终方案。

### 3.3 Analytic Screen：最终默认方案

Analytic Screen 对每盏灯执行：

1. 根据点光球解析切线计算保守 Screen Rect；
2. 对异常半径、相机位于球内、Near Plane 相交或退化投影执行安全 Fullscreen Fallback；
3. 设置 Scissor；
4. 绘制一次矩形；
5. 在 Fragment Shader 中读取或重建实际三维位置；
6. 执行与 Oracle 相同的精确球体谓词、衰减和光照累加。

Screen Rect 只负责扩大候选像素集合，不决定最终是否受光。因此允许 False Positive，不允许 False Negative；画质并未使用二维距离近似。

256 灯、四档覆盖场景的正式结果：

| 场景 | Analytic Volume | Analytic Screen | 点光 GPU Median 改善 |
|---|---:|---:|---:|
| small-local | 2.5553 ms | 0.6080 ms | -76.21% |
| medium-local | 5.7146 ms | 2.6308 ms | -53.96% |
| representative | 8.5391 ms | 4.1458 ms | -51.45% |
| high-overlap | 26.3267 ms | 11.2762 ms | -57.17% |

四档均为五个独立进程同向。机制变化为：

| 256 灯点光命令 | Analytic Volume | Analytic Screen |
|---|---:|---:|
| Stencil Draw | 256 | 0 |
| Lighting Volume Draw | 256 | 0 |
| Screen Draw | 0 | 256 |
| 点光 Stencil Clear | 257 | 0 |

质量验证：

- Fullscreen per-light Oracle、Analytic Volume、Analytic Screen 在五档质量场景中逐像素完全一致；
- high-overlap、edge-cases、Resize 和资源生命周期 smoke 通过；
- RenderDoc representative / 512 捕获确认 512 次 Screen Draw、0 次 Stencil Volume Draw、0 次点光 Clear；
- Pass 退出时 Stencil 保持全零，GL Debug Message 为 0。

完整数据、实现事实和适用边界见：[Screen Bounds / Scissor / Analytic Screen 正式报告](benchmark-results/point-light-screen-routing/screen-bounds-scissor-analytic-20260804/REPORT_CN.md)。

## 4. 实验性 Tile / Cluster 为什么没有成为默认

### 4.1 CPU CSR Tile S1

Tile S1 将全部点光源放入一次全屏 Draw，并让像素只遍历所在 Tile 的候选灯。静态 View/Light Set 下可以复用 CSR，因此重负载场景有明确收益：

- N512/R12 静止相机：23.6334 → 11.4580 ms，改善 51.48%。

但相机运动会使 View/Projection 相关 CSR 每帧失效：

- N512/R12 运动相机：23.3173 → 92.4921 ms，恶化 295.95%；
- 重负载构表 CPU Median：85.9226 ms；
- CSR 含 4,155,684 个索引，驻留约 15.95 MiB。

当前实现还包含逐受影响 Tile 重建侧平面、Count/Prefix/Fill、多份中间数据和完整 CSR Hash。这是当前 CPU 构表实现的成本，不代表 GPU Compute Tiled Lighting 的固有成本。

结论：仅静态条件路径 Go，不满足通用默认路径门槛。

证据：[Analytic Screen 与 Tile S1 正式 A/B](benchmark-results/point-light-analytic-tile-ab/analytic-tile-ab-formal-20260806/REPORT_CN.md)。

### 4.2 Cluster / 深度切片

Cluster 能借助 Z 切片缩短部分像素的候选列表，但会显著增加 Cell 和 CSR 规模。实际 Runtime 中：

- Cached：部分 N/R 场景 Cluster 获胜；
- Rebuild Every Frame：Cluster 在全部正式场景中都无法通过端到端门槛；
- 单独的 Lighting GPU 变快不等于 Wall Frame 变快；
- 透明 Forward、阴影和 PBR 后端未包含在结论中。

证据：

- [Tile vs Cluster Phase A](benchmark-results/tile-vs-cluster/tile-cluster-phase-a-20260804/REPORT_CN.md)
- [Tile16 / Cluster16 实际边界](benchmark-results/point-light-tile-cluster-runtime-boundary/tile-cluster-runtime-boundary-formal-20260805/REPORT_CN.md)
- [深度切片数 Runtime 实验](benchmark-results/point-light-grid-slice-count/grid-slice-count-formal-20260805/REPORT_CN.md)

### 4.3 八方向聚合近似

将多灯聚合为固定方向与强度虽然可以降低理论循环次数，但无法充分表达：

- 每像素深度方向；
- 点光距离衰减；
- 球体影响边界；
- 高光 Half Vector。

原始 U8 与带 Exact Fallback 的 H8 均未同时通过质量和工作量门槛，因此没有实现 Runtime，也不能陈述为完成的优化。

证据：[Directional-Binned 可证伪实验](benchmark-results/directional-binned-lighting/directional-binned-phase-a-20260804/REPORT_CN.md)。

## 5. 当前代码入口

- 默认模式与实验开关：[Global.h](Global.h)
- Screen Bounds、路由和点光 Pass：[DeferRenderPass.cpp](DeferRenderPass.cpp)
- 共用精确光照函数：[shaders/pointLightLighting.glsl](shaders/pointLightLighting.glsl)
- Analytic Screen Fragment：[shaders/lightVolumeFullscreenFragment.glsl](shaders/lightVolumeFullscreenFragment.glsl)
- 实验性 Tile/Cluster Runtime：[PointLightGridRuntime.cpp](PointLightGridRuntime.cpp)
- Tile/Cluster Fragment：[shaders/pointLightGridFragment.glsl](shaders/pointLightGridFragment.glsl)

## 6. 对外陈述边界

当前数据支持的项目/简历事实：

> 构建保守投影球 Screen Rect 与共享三维球内光照判定，以每灯一次 Analytic Screen Draw 替代两次 Stencil Volume Draw；在 1080p 固定 Sponza 的 256 灯四档 coverage 中，点光 GPU Median 相对统一 Analytic Volume 降低 51.45%～76.21%，五个独立进程全部同向，并对 Fullscreen per-light Oracle 达到逐像素完全一致。

不能陈述：

- 已实现 Adaptive Selector；
- Tiled/Clustered 已成为正式路径；
- 已完成 Instancing 或多灯单 Draw 批处理；
- 结果能跨 GPU、驱动、分辨率和场景直接外推；
- 透明物体已经由 Deferred 点光路径覆盖。

## 7. 后续若重新研究 Tile

当前 OpenGL 3.3 项目中，更合理的后续方向不是继续扩展 CPU CSR，而是单独立项验证：

```text
Analytic Screen Bounds
        ↓
Tile Bitmask + Log-Z Bin
        ↓
Fragment 中 TileMask & ZBinMask
        ↓
位扫描候选灯 + 精确球体谓词
```

该方案接近 Unity URP 当前的 CPU Jobified Cluster Light Loop；若升级到支持 Compute Shader 的图形接口，则可研究 HDRP / GPUOpen 风格的 GPU Light List Build。任何后续实现仍必须以运动相机下的 Wall Frame、构表/上传、GPU Lighting、内存和图像 Oracle 共同决定 Go/No-Go。

业界参考：

- [Unity HDRP Fine-Pruned Tiled Light List](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting/LightLoop/lightlistbuild.compute)
- [Unity URP Forward+/Cluster CPU Jobs](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.universal/Runtime/ForwardLights.cs)
- [Unity URP TileMask 与 ZBin Shader](https://github.com/Unity-Technologies/Graphics/blob/master/Packages/com.unity.render-pipelines.universal/ShaderLibrary/Clustering.hlsl)
- [Microsoft MiniEngine GPU Light Grid](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Model/Shaders/FillLightGridCS.hlsli)
