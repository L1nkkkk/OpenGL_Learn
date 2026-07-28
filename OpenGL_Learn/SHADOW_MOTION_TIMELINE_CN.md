# Shadow Benchmark 确定性运动时间轴

状态：实现完成，四类路径已通过运行时烟测

## 1. 为什么保留两套 workload

原有 `move-point`、`move-caster` 等 workload 继续使用相邻帧两点微扰。它们的职责是把输入变量压缩到一个，精确验证某条 Cache 失效规则。

新增 Timeline workload 用于连续运动实验。它们共享同一套固定帧时间轴和归一化场景尺度，适合观察逐帧性能曲线、P95/P99、Cache Hit、更新灯数和 Point 六面提交数。

两者不是替代关系：

- 微扰 workload 回答“该不该失效”；
- Timeline workload 回答“连续运动时成本如何变化”。

## 2. 时间轴定义

- 默认固定频率：60 Hz；
- 默认周期：600 帧，即 10 秒；
- 时间只由测量帧号计算，不读取 `deltaTime`；
- 测量区间第一个样本固定对应 Timeline Frame 0，不会因 Warm-up 帧数改变而平移；
- 轨迹振幅按 `sceneRadius` 归一化，Sponza 与 San Miguel 使用相同的相对运动尺度；
- A/B 变体按相同帧号采样，因此最终截图与逐帧数据可直接配对。

## 3. 四种轨道组合

| Workload | Point | Caster | Camera | 主要用途 |
|---|---:|---:|---:|---|
| `timeline-point` | 开 | 关 | 关 | Per-Light Cache 的主收益案例 |
| `timeline-camera` | 关 | 关 | 开 | 验证 Camera-only 不误使阴影失效 |
| `timeline-caster` | 关 | 开 | 关 | 验证 Caster Revision 的保守全灯失效 |
| `timeline-mixed` | 开 | 开 | 开 | 连续运动压力与平稳退化路径 |

在 Directional、Point、Spot 各一盏、Point 使用 Six-Face 路径时，每帧预期账户如下：

| Workload | 无缓存更新灯数 | Per-Light 更新灯数 | Per-Light Cache Hit | Per-Light Point 提交 |
|---|---:|---:|---:|---:|
| `timeline-point` | 3 | 1 | 2 | 6 |
| `timeline-camera` | 3 | 0 | 3 | 0 |
| `timeline-caster` | 3 | 3 | 0 | 6 |
| `timeline-mixed` | 3 | 3 | 0 | 6 |

这张表也说明了优化边界：Point 移动时仍需更新完整 Cubemap；Per-Light Cache 节省的是没有失效的 Directional 与 Spot。Caster 移动影响三盏灯时，系统应自然退化到全灯更新。

## 4. 逐帧遥测

每个 Timeline 结果 JSON 都包含 `motionTimeline`：

- `fixedFramesPerSecond`、`cycleFrames`、`trackMask`；
- 每帧的 Timeline Frame、Cycle Frame、固定时间与归一化相位；
- Point、Caster、Camera Position 和 Camera Target；
- Wall Frame 时间；
- Shadow Update CPU 时间；
- Update Count、Cache Hit、Light Cache Hit；
- Directional / Point / Spot 更新数；
- Point Shadow Submission Pass；
- Caster Bounds Rebuild Count。

测试入口会验证逐帧计数之和与原有聚合计数完全一致，并验证 Timeline 样本与 Profiler Wall Frame 样本逐帧对齐。

## 5. 一键正式实验

在项目目录运行：

```powershell
.\tools\Test-ShadowMotionTimeline.ps1
```

默认配置：

- 1920×1080；
- Sponza 与 San Miguel；
- A 无缓存、B Per-Light Revision Cache；
- 每个变体三轮独立进程；
- 每轮 1,000 个测量帧；
- 100 帧外部 Warm-up、15 帧内部 Warm-up；
- Point、Camera、Caster、Mixed 四种 Profile；
- 自动生成中文 Markdown 报告、汇总 CSV、逐帧 CSV、曲线图、A/B 截图和差异热力图。

只运行主案例：

```powershell
.\tools\Test-ShadowMotionTimeline.ps1 -Profiles point
```

快速烟测：

```powershell
.\tools\Test-ShadowMotionTimeline.ps1 `
    -SkipBuild `
    -Width 640 `
    -Height 360 `
    -MeasuredFrames 4 `
    -ExternalWarmupFrames 4 `
    -InternalWarmupFrames 4 `
    -FormalRunsPerVariant 1 `
    -SkipExternalWarmup `
    -SceneIds sponza
```

正式结果默认写入：

- 原始实验：`benchmark-results/shadow-optimizations/`；
- 中文报告：`docs/benchmark-images/shadow-motion-timeline/<BatchId>/report.md`。

## 6. 当前烟测结论

640×360、Sponza、每变体 4 帧的工程烟测已覆盖四种 Profile，所有运行、计数校验、逐帧对齐、报告生成和像素对比均通过。四组 A/B 截图的最大通道差和变化像素均为 0。

这些烟测数据只证明实现与实验管线正确，不能替代 1920×1080、三轮独立运行的正式性能数据。
