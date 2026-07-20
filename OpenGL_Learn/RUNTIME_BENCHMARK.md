# Runtime performance benchmark

本文档说明项目运行时性能基准工具的使用方式。正式优化结果仍需按 `PERFORMANCE_OPTIMIZATION_PROTOCOL.md` 的要求，以相邻版本执行独立 A/B、保留原始样本并记录到对应性能报告；已完成的稳态时间实验汇总在 `RUNTIME_PERFORMANCE.md`。

## Benchmark command

从 `OpenGL_Learn` 项目目录运行 Release x64：

```powershell
.\x64\Release\OpenGL_Learn.exe `
  --performance-benchmark `
  --benchmark-label A-<commit>-1 `
  --benchmark-output benchmark-results\<experiment>\01-A.json
```

默认行为：

- 场景：`saved/last_scene.json`
- 加载队列清空后开始预热
- Warm-up：300 帧，不计入结果
- Samples：1200 帧
- 请求 `glfwSwapInterval(0)`；报告同时记录实际默认 framebuffer sample 数，驱动级限帧仍需在实验说明中注明
- 冻结相机与 ImGui 输入，保持场景和渲染开关不变
- 不保存场景，不写入 `imgui.ini`
- 采样结束后排空未决 GPU timestamp query、写入 JSON 并自动退出

可选参数：

```text
--benchmark-warmup-frames <N>  非负整数
--benchmark-sample-frames <N>  正整数
--benchmark-label <text>       A/B 版本、提交和样本编号
--benchmark-output <path>      JSON 输出路径
```

## Metrics and semantics

JSON 使用 schema version 1，并包含以下信息：

- 环境：构建类型、架构、OpenGL vendor/renderer/version、分辨率、默认 framebuffer MSAA、请求的 swap interval。
- 固定设置：Forward/Deferred、Bloom、SSAO、Gamma、Forward normal buffer、热重载和灯光数量。
- `wallFrameMs`：相邻主循环边界之间的完整墙钟时间，包含 profiler 帧尾统计等主循环开销。
- `cpuFrameMs`：`PerformanceProfiler::BeginFrame` 完成内部预处理后，到帧作用域结束前的 CPU 时间。
- `gpuFrameMs`：包围场景、后处理和 ImGui GPU 命令的 timestamp query 时间。
- CPU/GPU zones：每个实际执行 zone 的原始时间样本及 mean、min、max、median、P95、P99。
- Render stats：每帧 draw、vertex/triangle、uniform、material/state/cache、文件检查和 UI 计数。
- Memory：每帧进程 working set/private bytes 快照，Texture、Mesh CPU、Mesh GPU、Render target 的 current/peak/count，以及 texture/model import cache 的 hit/miss 计数。

百分位使用 nearest-rank 方法。Zone 只在实际执行的帧中产生样本，因此必须同时检查 zone 的 `count`；例如每 0.25 秒触发一次的热重载轮询不会拥有与总帧数相同的样本量。

正式结果至少应满足：

- `capturedWallFrames == requestedSampleFrames`
- `capturedCpuFrames == requestedSampleFrames`
- GPU timing 支持时，`capturedGpuFrames == requestedSampleFrames`
- `capture.valid == true`；采样不完整时仍保留 JSON 供诊断，但进程返回非零且报告不得用于 A/B
- A/B 的对应 zone、render counters 和渲染设置一致；如果优化目标本身改变某个计数，必须在报告中解释

## Standard A/B sequence

每个二进制先执行一次不计入统计的完整预热运行，然后用新进程执行：

```text
A / B / B / A / A / B
```

六次运行必须使用不同输出文件和清晰 label，例如：

```text
01-A.json  02-B.json  03-B.json  04-A.json  05-A.json  06-B.json
```

正式性能报告需要保留每次运行的核心原始样本或完整汇总、A/B 平均值、绝对变化、百分比变化、样本范围和噪声判断。FPS 只作辅助信息。

### Cache-sensitive experiments

文件缓存、着色器缓存或其他跨进程缓存会改变加载路径时，冷缓存和热缓存必须分开测量和报告，不能混为同一组平均值：

- 冷缓存：每次 B 运行使用独立且初始为空的缓存目录；记录首次生成时间、hit/miss 和落盘体积。
- 热缓存：正式采样前用一次不计入结果的完整运行填充缓存；每次 B 必须核对预期 hit/miss。
- A/B 仍使用新进程和平衡交错顺序。冷缓存 B 不能先做会填充同一目录的额外导入预热，但进程内的 300 帧渲染 warm-up 保持不变。
- 报告必须同时给出首次使用代价、后续命中收益和回本次数；如果失效策略或缓存清理有边界，也必须记录。

## P0 infrastructure validation

2026-07-20 使用 Release x64、1440 x 900 和 `saved/last_scene.json` 执行了短流程验证：5 帧 warm-up、20 帧 sample。进程以 code 0 自动退出，JSON 可解析，wall/CPU/GPU frame 均完整捕获 20 个样本，各默认 Forward CPU/GPU zone 也得到预期样本，报告记录到默认 framebuffer 为 4x MSAA。

随后使用默认规模再次验证：300 帧 warm-up、1200 帧 sample。进程以 code 0 自动退出，`capture.valid == true`，wall/CPU/GPU frame、render stats 和 memory snapshot 均完整捕获 1200 个样本，持续执行的 4 个 GPU zone 也各有 1200 个样本。非法的零 sample 参数在创建窗口前以 code 4 拒绝。资源 smoke test 仍通过，FBO 生命周期为 `2 -> 4 -> 6 -> 8 -> 2`，且自动流程没有改写 `imgui.ini`。

视觉检查随后发现，存档恢复的是 `cameraFront`，旧的 view matrix 却读取只会由鼠标回调更新的 `cameraDirection`。自动模式冻结输入后，这会让基准相机无效并产生黑色 viewport。P0 因此改为直接用已恢复的 `cameraFront` 构造 view matrix；此前黑色 viewport 的运行只保留作采集链路验证，不能作为任何渲染性能基线。正式优化 A/B 必须基于包含该修复的 P0 提交，并确认所用场景夹具确实包含可见几何；如果当前存档相机本身朝向空场景，应使用隔离副本调整相机并在报告中记录差异。

这些运行仅验证 benchmark 生命周期、GPU query 排空、JSON schema、参数校验和既有资源回归，不是性能 A/B 基线，不得用于宣称任何运行时收益。
