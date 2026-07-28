# 编辑器 UI 重设计说明

## 目标

这次调整不改变原有 Scene、Renderer、Material 与 Assets 的编辑能力，重点解决三个问题：

- 全局状态分散，无法快速判断当前管线与帧时间；
- Renderer 把高频配置、低频配置和调试项平铺在一起；
- 性能曲线、阴影缓存账户与运动控制互相割裂。

## 新的信息架构

默认 Dock 布局：

- 顶部：全局工具栏，显示 Forward / Deferred、CPU、GPU、FPS，并提供 `Reset layout`；
- 左侧：`Scene` 为主，`Overview` 显示项目状态与热重载；
- 中间：`Viewport`；
- 右侧：`Renderer` 与各类材质 Inspector；
- 底部中央：`Motion Timeline`；
- 底部右侧：`Profiler` 与 `Assets`。

编辑器启动时鼠标保持可见，可直接操作 UI。按 `M` 进入或退出相机视角控制；在文本框等控件捕获键盘时，WASD 不会误移动相机。

`Renderer` 内部按任务重新分组：

1. `Render Pipeline`：Forward / Deferred、Frustum Culling、SSAO、Light Volume；
2. `Shadows & Cache`：滤波方式、采样数、Cache 开关、Per-Light Dirty Cache；
3. `Post Processing`：HDR、Gamma、Bloom；
4. `Render Target Debug`：FBO 与 Attachment 查看；
5. `Editor & Hot Reload`、`Anti-aliasing`：低频选项默认收起。

`Profiler` 改为四个页签：

- `Frame`：CPU/GPU 帧曲线与 P95/P99；
- `Zones`：CPU/GPU Zone 明细；
- `Render`：Draw Call、三角形、可见性与状态缓存账户；
- `Memory`：工作集、私有内存和资源分类。

## Motion Timeline 的定位

`Motion Timeline` 是交互预览器，不是正式 Benchmark 启动器。它与命令行正式实验共享同一个确定性轨迹采样器，但实时预览会包含编辑器 UI、窗口事件和人工操作成本。

正式性能结论仍使用：

```powershell
.\tools\Test-ShadowMotionTimeline.ps1 -Profiles point
```

完整使用步骤和指标解释见 [SHADOW_MOTION_TIMELINE_CN.md](SHADOW_MOTION_TIMELINE_CN.md)。
