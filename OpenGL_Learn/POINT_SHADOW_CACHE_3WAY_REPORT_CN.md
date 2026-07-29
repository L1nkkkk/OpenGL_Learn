# Point Light 空间关联与 Per-Face 阴影缓存三档实验报告

> 本报告由同一 Release 可执行文件自动生成。三档仅切换阴影缓存策略；场景、Shader、FBO、阴影分辨率、运动轨迹及渲染路径保持一致。

## 1. 实验结论

- **Crytek Sponza**：Point Shadow GPU 中位数 `0.588 → 0.588 → 0.496 ms`；Point Face 提交 `6.00 → 4.00 → 3.06 次/帧`。
- **San Miguel 2.1 (low-poly)**：Point Shadow GPU 中位数 `2.669 → 2.664 → 1.931 ms`；Point Face 提交 `6.00 → 4.00 → 2.91 次/帧`。

这组数据应按两层优化解读：A→B 消除未变化灯光的重绘；B→C 保留 Point Cubemap 未失效的 Face。Receiver Demand 在部分场景仍会覆盖 5～6 面，因此 C 的主要收益来自局部 Caster 运动阶段，而不是假设相机永远只需要一两个面。

![三档性能对比](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/three-way-performance.png)

![三档工作量对比](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/three-way-work.png)

## 2. 三档定义与统一轨迹

| 档位 | 缓存粒度 | 典型行为 |
|---|---|---|
| A 全局重绘 | 无缓存控制路径 | 每帧更新 Directional、Point、Spot；Point 固定 6 Face |
| B Per-Light | 当前灯光级 Revision Cache | Point 移动时只更新 Point，但仍固定 6 Face；Caster Revision 仍可能使多灯失效 |
| C Per-Light + Per-Face | 空间 Caster 签名、Face Dirty/Valid/Required Mask | Point 只绘制 `required & stale` 的 Face，未需求 Face 延迟物化 |

确定性周期分为三段：`Point+Camera`、`Local Caster+Camera`、`Camera-only`。独立运动 Caster 使用同一小球模型和固定轨迹，避免移动整座 Sponza/San Miguel 导致所有 Face 天然失效。

## 3. 正式性能结果

### Crytek Sponza

| 指标 | A 全局重绘 | B Per-Light | C Per-Face | A→B | B→C | A→C |
|---|---:|---:|---:|---:|---:|---:|
| GPU 帧时间中位数 (ms) | 1.574 | 1.460 | 1.212 | -7.20% | -16.98% | -22.95% |
| GPU 帧时间 P95 (ms) | 1.724 | 1.653 | 1.546 | -4.14% | -6.47% | -10.34% |
| Shadow Update GPU 中位数 (ms) | 0.760 | 0.753 | 0.523 | -0.97% | -30.51% | -31.18% |
| Point Shadow GPU 中位数 (ms) | 0.588 | 0.588 | 0.496 | +0.02% | -15.62% | -15.60% |
| 整帧墙钟时间中位数 (ms) | 3.915 | 3.491 | 3.272 | -10.83% | -6.27% | -16.41% |

| 工作量（每帧） | A | B | C |
|---|---:|---:|---:|
| 更新阴影灯 | 3.00 | 1.33 | 1.33 |
| Point 更新次数 | 1.00 | 0.67 | 0.67 |
| Point Face 提交 | 6.00 | 4.00 | 3.06 |
| Point Face 实际绘制 | 6.00 | 4.00 | 3.06 |
| Point Face 缓存命中 | 0.00 | 0.00 | 2.94 |
| Caster Draw | 1376.52 | 653.86 | 533.16 |
| Caster Triangle | 1048245.92 | 521237.26 | 407224.99 |

![Crytek Sponza 时间线](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/sponza-timeline.png)

![Crytek Sponza 三档截图](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/sponza-three-way-screenshot.png)

### San Miguel 2.1 (low-poly)

| 指标 | A 全局重绘 | B Per-Light | C Per-Face | A→B | B→C | A→C |
|---|---:|---:|---:|---:|---:|---:|
| GPU 帧时间中位数 (ms) | 7.609 | 5.452 | 5.088 | -28.35% | -6.67% | -33.13% |
| GPU 帧时间 P95 (ms) | 8.308 | 7.731 | 6.735 | -6.94% | -12.89% | -18.93% |
| Shadow Update GPU 中位数 (ms) | 4.861 | 3.982 | 2.583 | -18.09% | -35.12% | -46.85% |
| Point Shadow GPU 中位数 (ms) | 2.669 | 2.664 | 1.931 | -0.21% | -27.51% | -27.67% |
| 整帧墙钟时间中位数 (ms) | 11.337 | 8.899 | 8.821 | -21.50% | -0.88% | -22.19% |

| 工作量（每帧） | A | B | C |
|---|---:|---:|---:|
| 更新阴影灯 | 3.00 | 1.33 | 1.33 |
| Point 更新次数 | 1.00 | 0.67 | 0.67 |
| Point Face 提交 | 6.00 | 4.00 | 2.91 |
| Point Face 实际绘制 | 6.00 | 4.00 | 2.91 |
| Point Face 缓存命中 | 0.00 | 0.00 | 2.94 |
| Caster Draw | 4911.41 | 2366.07 | 1881.89 |
| Caster Triangle | 22209686.89 | 11325482.89 | 8692771.49 |

![San Miguel 2.1 (low-poly) 时间线](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/san-miguel-timeline.png)

![San Miguel 2.1 (low-poly) 三档截图](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/san-miguel-three-way-screenshot.png)

## 4. Face 行为与 CPU 代价

![Face 更新分布](docs/benchmark-images/shadow-optimizations/point-shadow-cache-3way-1080p-release/point-face-update-histogram.png)

| 场景 | C 需求 Face/帧 | C 实际绘制 Face/帧 | Face 命中/帧 | 缓存检查 CPU | 需求分析 CPU | Face 签名 CPU |
|---|---:|---:|---:|---:|---:|---:|
| Crytek Sponza | 6.000 | 3.065 | 2.935 | 0.0062 ms | 0.0033 ms | 0.0014 ms |
| San Miguel 2.1 (low-poly) | 5.843 | 2.908 | 2.935 | 0.0444 ms | 0.0408 ms | 0.0015 ms |

## 5. 正确性与资源一致性

性能模式允许未被当前 Receiver 采样的 Face 暂时保留旧内容，但这些 Face 带有无效签名，未来进入 Required Mask 时必须先重建。报告同时运行独立 PCSS 审计：强制六面全部物化，验证 Per-Face 路径最终与 Six-face 基准路径完全收敛。

独立进程截图审计还暴露并修复了一个与阴影缓存无关的可重复性问题：Opaque 批次原先按 Shader/Material 内存地址排序，不同进程可能改变少量共面像素的先后覆盖。现在改为按场景首次出现顺序生成稳定批次键，以下屏幕比较继续使用严格 `0` 像素差门禁，没有放宽容差。

| 场景 | A/B 屏幕像素 | B/C 屏幕像素 | B 重复运行截图 | PCSS 六面深度 Hash |
|---|---|---|---|---|
| Crytek Sponza | 完全一致 | 完全一致 | 完全一致 | 六面完全一致 |
| San Miguel 2.1 (low-poly) | 完全一致 | 完全一致 | 完全一致 | 六面完全一致 |

| 场景 | B 主实验 GPU 帧中位数 | B 独立重测 | 重测差异 |
|---|---:|---:|---:|
| Crytek Sponza | 1.460 ms | 1.460 ms | +0.00% |
| San Miguel 2.1 (low-poly) | 5.452 ms | 5.487 ms | +0.64% |

| 场景 | 纹理内存 A/B/C（字节） | Mesh GPU A/B/C（字节） | Render Target A/B/C（字节） |
|---|---:|---:|---:|
| Crytek Sponza | 243742611/243742611/243742611 | 15135820/15135820/15135820 | 87609344/87609344/87609344 |
| San Miguel 2.1 (low-poly) | 463827497/463827497/463827497 | 434863012/434863012/434863012 | 87609344/87609344/87609344 |

## 6. 实验条件

- 分辨率：`1920×1080`。
- 每档独立进程：`3` 轮；每轮测量 `600` 帧。
- 渲染：Release、PBR Forward、Hard Shadow 性能隔离；另以 PCSS 执行完整六面正确性审计。
- Point Shadow：显式 Six-face 路径、逐面 Caster Culling；三档使用相同 Shader、FBO 与分辨率。
- GPU：`NVIDIA Corporation / NVIDIA GeForce RTX 5060 Ti/PCIe/SSE2`；OpenGL `3.3.0 NVIDIA 591.86`。

## 7. UI 手动验证与一键复现

- 打开 `Motion Timeline`，点击 `Prepare 3-light test`。它会临时建立一组 Directional/Point/Spot 阴影灯和一个红色局部运动 Caster；退出测试时可恢复原场景。
- `Profile` 选择 `Cache 3-way phases` 后，轨迹依次执行“点光源+相机 / 局部遮挡物+相机 / 仅相机”。右侧实时显示 Required、Rendered、Face hits 与 Deferred。
- 顶部 A/B/C 按钮分别切换全局重绘、Per-Light、Per-Light + Per-Face。正式测试仍以脚本的独立进程数据为准。

```powershell
.\tools\Test-PointShadowCache3Way.ps1 -SkipBuild -BatchId point-shadow-cache-3way-1080p -Width 1920 -Height 1080 -MeasuredFrames 600 -ExternalWarmupFrames 100 -InternalWarmupFrames 15 -FormalRunsPerVariant 3 -SceneIds sponza,san-miguel
```

## 8. 边界与结论

1. 当前场景的 Receiver Demand 并不稀疏：Sponza 常需六面，San Miguel 常需五面。仅靠 Camera Demand 不能保证大收益。
2. 局部 Caster 运动时，Per-Face Signature 能稳定把 Point 更新从六面压到约 2～3 面；这才是 C 档的主要案例。
3. Directional/Spot/Point 的 Auto-fit 投影仍保留全局 Caster 依赖，空间 Per-Light 失效会保守退化；报告不会把这部分包装成已经完成的局部化收益。
4. C 档不增加 Shadow Texture 或 Render Target；额外状态只有 CPU 侧 Face Mask、签名和版本数据。
5. Layered Geometry Shader 仍仅保留诊断用途；当前驱动上曾出现五面未写入，因此生产路径继续使用已验证的 Six-face。

最终判断：这项优化值得保留，但应把简历结论写成“局部 Caster 变化下的 Point Per-Face 增量更新”，而不是笼统声称所有 Point Light 更新都会从六面降到一面。
