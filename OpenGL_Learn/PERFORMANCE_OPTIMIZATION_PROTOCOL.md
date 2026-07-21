# Performance Optimization Protocol

本文档是本项目所有性能优化任务的强制执行规范。性能改动只有在完成可复现的 A/B 对比、正确性验证和结果记录后，才视为完成。

## 1. 核心原则

1. **一次只验证一个优化假设。** 每项优化使用独立改动和独立提交，不把无关优化、重构或用户文件混入同一次 A/B。
2. **先测量，再保留。** “理论上更快”不能替代数据。没有稳定对比数据的改动不得宣称取得性能收益。
3. **使用相邻版本作为对照。** A 版本原则上是优化前的直接提交，B 版本只包含当前优化和对应报告更新。必须记录完整或短提交哈希。
4. **测试条件必须一致。** A/B 使用相同硬件、构建配置、窗口尺寸、场景、相机、资源、渲染设置和采样时机。
5. **同时检查性能和正确性。** 任何画面错误、资源泄漏、生命周期回退或功能缺失都不能用性能提升抵消。
6. **回退也要记录。** 没有实质收益或发生回退的实验应撤销代码，但必须在性能报告的 `Rejected experiments` 中保留方法、数据和结论，避免重复尝试。

### 性能优先级

- 固定优先级为：**正确性 > 稳态运行时间 > 加载时间 > 内存占用**。
- 稳态运行时间优先看 CPU/GPU frame 与直接受影响 zone 的 median、P95、P99；受 Present、VSync 或外部限帧影响时，不得用 FPS 代替专项时间指标。
- 内存是约束指标，不再是默认优化目标。只要没有触发资源泄漏、OOM 风险或不可接受的常驻/峰值增长，可以用适量内存换取可重复的时间收益。
- 仅减少内存但导致稳定时间回退的方案原则上拒绝；只有为避免 OOM、显存抖动或资源生命周期故障时，才可明确记录理由后例外保留。
- 时间收益必须超出同条件自然波动范围。平均值变快但 P95/P99 明显恶化时，不得直接判定为优化成功。

## 2. Definition of Done

每项性能优化必须全部满足以下条件：

- 写清楚优化对象、原始成本、实现方法和预期影响指标。
- 建立可复现的 A/B 对照，并记录 A/B 提交哈希。
- 保存每次原始采样值，而不仅是最终平均值。
- 计算平均值、绝对变化、百分比变化，并判断变化是实质收益、回退还是测量噪声。
- 使用与优化类型匹配的 CPU、GPU、内存、加载或资源生命周期指标。
- 涉及运行时路径时，使用 `RUNTIME_BENCHMARK.md` 中的自动 benchmark 保存完整原始样本；短验证运行不能充当正式 A/B 数据。
- 完成 Release x64 构建和相关自动/手动回归验证。
- 将实验条件、数据和结论更新到项目性能报告。
- 每项优化独立提交；提交正文包含实现摘要、核心 A/B 数据和验证结果。
- 暂存和提交前检查文件范围，不覆盖或混入用户的无关改动。
- 未经用户明确要求，不 push。

缺少上述任一项时，优化任务仍处于未完成状态。

## 3. 标准 A/B 实验条件

除非某项优化需要专门场景，默认使用以下稳定基线：

- Build：Release x64
- Resolution：1440 x 900
- Scene：`saved/last_scene.json`
- Process：每次采样使用新进程
- Warm-up：每个 A/B 二进制各预热一次，预热结果不计入统计
- Samples：每个版本至少三次有效采样
- Order：使用平衡交错顺序 `A/B/B/A/A/B`
- Assets and settings：两版本使用相同资源、相机、渲染功能开关和驱动设置
- Timing point：异步模型队列清空后等待固定时间，再采集稳定状态数据

报告中还必须记录：

- 操作系统、CPU、GPU 和显卡驱动版本
- 测试日期
- A/B 提交哈希
- 与默认条件不同的所有设置
- 是否存在 VSync、帧率限制或后台负载等已知干扰

若三次结果波动较大，或者优化差值接近自然波动范围，应增加样本数并说明原因，不能选择性丢弃不利样本。

## 4. 按优化类型选择指标

### 4.1 启动与资源加载

至少记录：

- Load ready 时间
- 加载阶段 Working set 峰值
- 加载阶段 Private bytes 峰值
- 稳定后的 Working set / Private bytes
- Texture、Mesh CPU、Mesh GPU、Render target 等资源遥测
- 文件系统检查、缓存命中/未命中等相关计数

启动优化不应只看稳定内存；减少临时分配时，峰值内存和 Load ready 才是主要指标。

### 4.2 CPU 运行时

至少记录：

- CPU Frame 中位数、P95，必要时记录 P99
- 直接受影响的 CPU profiler zone
- draw calls、uniform updates、material binds、状态切换和缓存命中等相关计数
- 帧时间抖动和最差帧是否改善

如果 Present/VSync 占据主要 CPU Frame 时间，应使用具体 CPU zone 或关闭限制后的专项实验，不能仅比较总 FPS。

### 4.3 GPU 与 shader

至少记录：

- GPU Frame 中位数和 P95
- 直接受影响的 GPU pass，例如 Forward、Deferred、SSAO、Bloom 或 Shadow Maps
- draw calls、submitted vertices/triangles 和相关纹理采样/目标带宽变化
- 相同相机和渲染设置下的截图回归

当前环境中 FPS 可能稳定在约 165 并受显示刷新率限制，因此 FPS 只作为辅助信息，不能作为 shader/GPU 优化的唯一证据。

### 4.4 内存与资源生命周期

至少记录：

- Dedicated GPU memory
- Shared GPU memory
- Working set
- Private bytes
- 各资源分类的 current / peak bytes 和 resource count
- 启用功能、切换功能和关闭功能后的资源生命周期

对于按需资源，必须验证关闭功能后资源是否回收，而不仅是首次启用时的占用。

## 5. 数据记录与计算

必须先记录原始数据，再计算汇总结果。建议使用以下结构：

| Order | Variant | Metric 1 | Metric 2 | Metric 3 | Notes |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | A |  |  |  |  |
| 2 | B |  |  |  |  |
| 3 | B |  |  |  |  |
| 4 | A |  |  |  |  |
| 5 | A |  |  |  |  |
| 6 | B |  |  |  |  |

汇总表：

| Metric | A average | B average | Absolute delta | Relative delta | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| Example |  |  | `B - A` | `(B - A) / A * 100%` | improvement / regression / noise |

判定规则：

- 对“越小越好”的指标，负 delta 表示改善。
- 对“越大越好”的指标，应在报告中明确说明方向。
- 差值落在运行间自然波动范围内时，标记为 `noise` 或 `no material change`，不能宣称收益。
- 若主要指标回退，应增加样本确认；确认后撤销优化或清楚记录接受回退的理由。
- 不得只报告百分比。必须同时给出原始单位和绝对变化。

## 6. 每项优化的报告内容

每个保留或拒绝的实验都应包含以下内容：

### Optimization: `<name>`

- **Goal：** 要减少的具体成本。
- **Problem：** 原实现为何产生 CPU、GPU、内存、加载或资源生命周期成本。
- **Implementation：** 修改了什么，以及为什么不会改变预期行为。
- **Control：** A 版本提交哈希。
- **Candidate：** B 版本提交哈希或未提交工作树说明。
- **Method：** 构建、场景、分辨率、预热、顺序、样本数和专项设置。
- **Raw samples：** 完整的 A/B 原始数据。
- **Result：** 平均值、绝对 delta、百分比 delta 和噪声判断。
- **Correctness：** 构建、自动测试、资源生命周期和截图/画面验证。
- **Decision：** retained 或 rejected，以及原因。
- **Limitations：** VSync、驱动计数误差、场景覆盖不足等限制。

内存相关实验继续记录在 `MEMORY_BENCHMARK.md`。如果后续出现大量 CPU/GPU 帧时间实验，可新增对应的运行时性能报告，但仍必须遵守本文档。

## 7. 验证清单

每项优化至少执行与其风险相称的验证：

- [ ] Release x64 构建成功
- [ ] 相关 smoke test / 自动测试通过
- [ ] `git diff --check` 通过
- [ ] 暂存区只包含当前优化和报告文件
- [ ] 默认场景正常加载和退出
- [ ] 受影响功能的开/关切换正常
- [ ] 资源数量和内存在功能关闭后回到预期状态
- [ ] shader、纹理格式或渲染目标修改完成截图对比
- [ ] 非均匀缩放、透明/裁剪材质、阴影边缘等相关边界场景已检查
- [ ] A/B 原始数据、汇总和结论已写入报告

## 8. 提交要求

推荐提交结构：

```text
perf(<scope>): <optimization summary>

Explain the original cost and implementation.

Control: <commit>
Method: <A/B method>
Primary metric: <before> -> <after> (<absolute>, <percent>)
Other metrics: <summary>
Validation: <build/tests/visual/resource checks>
```

性能报告应与对应代码改动处于同一提交，确保将来检出任意优化提交时，都能找到该版本的实验方法和结果。
