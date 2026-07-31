# Draw Submission 压力场景

这个固定场景用于放大 CPU Draw Submission 成本，为后续 Static / Dynamic Retained Draw Submission 优化建立 A/B 基线。它不是普通游戏场景，也不用于证明画面复杂度。

## 固定条件

- Release、1920×1080、VSync 关闭；
- 默认 30,000 个独立 Model / Mesh Draw Item / Draw Call；
- 所有对象共享同一份 2 三角形 Quad 几何，避免模型资源和顶点量成为主要变量；
- 16 个材质，固定种子分配；
- 20% 对象按固定 60 Hz 时间轴运动，80% 保持静态；
- Forward + 最简 shader，无灯光、阴影、SSAO、Bloom；
- 相机自动适配网格，所有对象保持在视锥内；
- Benchmark 模式关闭重型编辑器面板，避免 30,000 项 UI 列表污染数据。
- Benchmark 模式将最终 FBO 直接 Blit 到窗口，保留可视画面而不重新引入编辑器面板成本。

默认的 30,000 对象规模是在当前机器上用短采样校准出的低于 60 FPS 档位。1,000 / 5,000 / 10,000 对象仍可通过参数作为规模曲线测试点。

## 运行

先构建 `Release | x64`，然后在项目目录运行正式协议：

```powershell
.\tools\run_submission_stress.ps1
```

快速验证：

```powershell
.\tools\run_submission_stress.ps1 `
  -WarmupFrames 60 `
  -SampleFrames 180 `
  -CapturePath benchmark-results/submission-stress/smoke.ppm `
  -Label submission-stress-smoke `
  -Output benchmark-results/submission-stress/smoke.json
```

规模曲线示例：

```powershell
.\tools\run_submission_stress.ps1 -ObjectCount 1000
.\tools\run_submission_stress.ps1 -ObjectCount 5000
.\tools\run_submission_stress.ps1 -ObjectCount 10000
.\tools\run_submission_stress.ps1 -ObjectCount 30000
```

## 重点读取

- `wallFrameMs`、`cpuFrameMs`、`gpuFrameMs` 的 Median / P95 / P99；
- `Build Draw Lists`；
- `Draw Item Collection`；
- `Opaque Draw Sorting`；
- `Transparent Draw Sorting`；
- `Submission Stress Motion`；
- `Submission Stress Present`；
- `activeModels`、`visibleModels`、`opaqueMeshes`、`drawCalls`。

判定前提：`activeModels == visibleModels == opaqueMeshes == ObjectCount`，Draw Call 数应约为对象数加固定后处理提交；同时 CPU 帧时间应明显高于 GPU 帧时间。否则这不是有效的 CPU Draw Submission 基线。

正式优化对照仍应遵循 300 帧预热、2,000 帧采样、三轮独立重复；短采样只用于功能验证和负载校准，不应写入简历。
