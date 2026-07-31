# SSAO RenderDoc 人工捕获与验收步骤

本文只定义代表性 GPU 帧证据的人工捕获流程。RenderDoc 捕获属于诊断运行，
不能作为 SSAO 性能 Benchmark 的计时来源。

> 2026-07-31 更新：下述最初阻塞已经通过官方 RenderDoc 1.45 MSI payload
> 的用户级便携部署和默认关闭的 in-application API 捕获入口解除。
> Full-64 Legacy 与 Half-64 Bilateral 的真实 `.rdc` 已生成，并通过官方
> 命令行回放及程序化结构验收。真实结果、路径和仍待 GUI 人工确认的项目见
> [SSAO RenderDoc 1.45 真实帧捕获与结构验收](SSAO_RENDERDOC_ACCEPTANCE_20260731.md)。
> 本文其余部分保留为 GUI 人工复核步骤和历史阻塞记录。

## 1. 捕获前环境检查记录（已解除）

2026-07-31 对本机进行了只读检查，结果如下：

- `renderdoccmd.exe`、`qrenderdoc.exe` 和 `renderdocui.exe` 均不在 `PATH`；
- `C:\Program Files\RenderDoc`、`C:\Program Files (x86)\RenderDoc`、
  Scoop、Chocolatey 和用户本地程序的常见安装目录均不存在 RenderDoc；
- Windows 卸载注册表中没有 RenderDoc 项；
- 在 `C:\Users\Link` 下没有找到 `renderdoccmd.exe` 或
  `qrenderdoc.exe`；
- Python 环境中 `importlib.util.find_spec("renderdoc")` 返回 `None`；
- 项目生产源码中没有 RenderDoc in-application API 集成；
- 工作区中没有现存 `.rdc` 捕获文件。

因此，当前不能声称已经完成 RenderDoc 捕获。安装后的捕获必须按本文重新执行，
并保存真实 `.rdc` 和截图；不能用推测内容代替。

可用以下只读命令复核主要阻塞项：

```powershell
Get-Command renderdoccmd.exe, qrenderdoc.exe -ErrorAction SilentlyContinue
Test-Path "C:\Program Files\RenderDoc"
py -c "import importlib.util; print(importlib.util.find_spec('renderdoc'))"
rg -n "RENDERDOC_API|StartFrameCapture|EndFrameCapture" `
    OpenGL_Learn -g "*.cpp" -g "*.h"
Get-ChildItem -Path . -Filter "*.rdc" -File -Recurse
```

## 2. 安装与捕获前准备

1. 从 [RenderDoc 官方构建页面](https://renderdoc.org/builds)安装或解压
   64-bit Windows 版本。不要从不明镜像获取二进制。
2. 确认以下命令能打印版本：

   ```powershell
   & "C:\Program Files\RenderDoc\renderdoccmd.exe" version
   ```

   使用便携版时，将路径替换为实际解压目录。
3. 先完成本项目最新的 Release x64 构建。记录待捕获 EXE 的 SHA-256：

   ```powershell
   Get-FileHash `
       "C:\Users\Link\Dev\OpenGL_Learn\x64\Release\OpenGL_Learn.exe" `
       -Algorithm SHA256
   ```

4. 两次捕获必须使用同一个 EXE，且使用以下固定启动位置：

   - Executable：
     `C:\Users\Link\Dev\OpenGL_Learn\x64\Release\OpenGL_Learn.exe`
   - Working directory：
     `C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn`

5. 建议把证据保存到一个新的、不会覆盖历史产物的目录，例如：

   ```text
   OpenGL_Learn/benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/
   ```

   执行任何命令前，必须把文中的所有 `<BATCH_ID>` 替换为本次唯一批次名；
   尖括号不是合法的 Windows 文件名字符。

本文选择 Sponza 的固定基线相机，以便 Full-64 与 Half-64 Bilateral 做同帧
对照。固定场景入口会创建隐藏窗口，因此较长的 warm-up 只用于给 QRenderDoc
Live Capture 留出连接和点击 `Capture Frame` 的时间；这两次运行不属于性能采样。
命令中的 `--classic-scene-radius 15` 是模型归一化半径；SSAO radius/bias 仍由
实验入口固定为 `0.35/0.025`。`--classic-scene-no-capture` 只关闭程序自身的
PPM 捕获，不会禁用 RenderDoc。

## 3. Full-64 捕获

### 3.1 QRenderDoc 图形界面

打开 QRenderDoc 的 `Launch Application`：

- 填入第 2 节的 Executable 和 Working directory；
- Capture File Path Template 设置为
  `<证据目录>\full64-sponza`；
- Command-line arguments 填入下面这一整行：

```text
--classic-scene-test classic-scenes/sponza/sponza.obj --classic-scene-name renderdoc-sponza-full64 --classic-scene-result benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/full64-diagnostic.json --classic-scene-camera -6 -1.5 0 --classic-scene-target 6 -0.8 0 --classic-scene-up 0 1 0 --classic-scene-radius 15 --classic-scene-world-scale 1 --classic-scene-fov 55 --classic-scene-render-path pbr-deferred --classic-scene-width 1920 --classic-scene-height 1080 --classic-scene-ssao-mode legacy-full --classic-scene-ssao-samples 64 --classic-scene-warmup-frames 12000 --classic-scene-capture-frame 12020 --classic-scene-no-capture
```

启动后在 QRenderDoc 的 Live Capture/活动目标中连接该进程。固定场景窗口默认
隐藏，不能依赖 overlay 或聚焦窗口按 F12；等待加载结束后在活动目标中点击一次
`Capture Frame`。不要捕获模型加载中的空白帧。

### 3.2 可选命令行注入

`renderdoccmd capture` 负责启动和注入，但不会自动触发捕获。执行下面命令后，
仍需在 QRenderDoc 的 Live Capture/活动目标中连接该隐藏窗口进程并点击一次
`Capture Frame`：

```powershell
$rdc = "C:\Program Files\RenderDoc\renderdoccmd.exe"
$exe = "C:\Users\Link\Dev\OpenGL_Learn\x64\Release\OpenGL_Learn.exe"
$workDir = "C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn"
$evidence = Join-Path $workDir `
    "benchmark-results\ssao-half-resolution\<BATCH_ID>\renderdoc"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

$argsFull64 = @(
    "--classic-scene-test", "classic-scenes/sponza/sponza.obj",
    "--classic-scene-name", "renderdoc-sponza-full64",
    "--classic-scene-result",
        "benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/full64-diagnostic.json",
    "--classic-scene-camera", "-6", "-1.5", "0",
    "--classic-scene-target", "6", "-0.8", "0",
    "--classic-scene-up", "0", "1", "0",
    "--classic-scene-radius", "15",
    "--classic-scene-world-scale", "1",
    "--classic-scene-fov", "55",
    "--classic-scene-render-path", "pbr-deferred",
    "--classic-scene-width", "1920",
    "--classic-scene-height", "1080",
    "--classic-scene-ssao-mode", "legacy-full",
    "--classic-scene-ssao-samples", "64",
    "--classic-scene-warmup-frames", "12000",
    "--classic-scene-capture-frame", "12020",
    "--classic-scene-no-capture"
)

& $rdc capture `
    --working-dir $workDir `
    --capture-file (Join-Path $evidence "full64-sponza") `
    --opt-disallow-vsync `
    --wait-for-exit `
    $exe @argsFull64
```

### 3.3 Full-64 验收

在捕获的 Event Browser、Pipeline State、Texture Viewer 和 Resource Inspector 中
逐项确认：

1. 事件顺序为 GBuffer 写入、SSAO Generate 全屏 Draw、Deferred Lighting；
   `legacy-full` 不应出现 SSAO Upsample Draw。
2. SSAO Generate 是 `glDrawArrays(GL_TRIANGLES, 0, 6)`，即 6 vertices、
   2 triangles 的全屏 Draw。
3. Generate 的颜色 attachment 为 `1920 x 1080`、单通道
   `GL_R16F`；RenderDoc 可能将它显示为 `R16_FLOAT`。
4. Generate 使用以下输入：

   - `gPosition`：`1920 x 1080`、`GL_RGBA16F`；
   - `gNormal`：`1920 x 1080`、`GL_RGB16F`；
   - noise texture：`4 x 4`、`GL_RGB16F`。

5. 该全分辨率 AO texture 随后被 Deferred Lighting 采样。
6. Texture Viewer 中用 `[0, 1]` 范围查看 R 通道，保存 AO attachment 的
   原尺寸截图。

`PERF_CPU_SCOPE` 和 `PERF_GPU_SCOPE` 不会自动变成 RenderDoc 的 OpenGL debug
group。如果 Event Browser 中没有 `SSAO Generate` 文本标签，应通过 SSAO shader、
attachment 格式/尺寸和 6-vertex 全屏 Draw 共同定位，不能只凭事件序号猜测。

## 4. Half-64 Bilateral 捕获

使用新的独立进程，保持相同 EXE、场景、相机、分辨率、64 samples、radius、
bias、灯光和其他状态，仅将模式改为 `half-bilateral`。

QRenderDoc 的 Command-line arguments 填入：

```text
--classic-scene-test classic-scenes/sponza/sponza.obj --classic-scene-name renderdoc-sponza-half64-bilateral --classic-scene-result benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/half64-bilateral-diagnostic.json --classic-scene-camera -6 -1.5 0 --classic-scene-target 6 -0.8 0 --classic-scene-up 0 1 0 --classic-scene-radius 15 --classic-scene-world-scale 1 --classic-scene-fov 55 --classic-scene-render-path pbr-deferred --classic-scene-width 1920 --classic-scene-height 1080 --classic-scene-ssao-mode half-bilateral --classic-scene-ssao-samples 64 --classic-scene-warmup-frames 12000 --classic-scene-capture-frame 12020 --classic-scene-no-capture
```

按第 3 节相同方式，在加载结束后从 QRenderDoc 活动目标点击一次
`Capture Frame`。捕获模板使用 `<证据目录>\half64-bilateral-sponza`。

命令行注入时可复用第 3.2 节脚本，只需进行以下替换：

```powershell
$argsHalf64Bilateral = $argsFull64.Clone()
$argsHalf64Bilateral[
    [Array]::IndexOf($argsHalf64Bilateral, "renderdoc-sponza-full64")
] = "renderdoc-sponza-half64-bilateral"
$argsHalf64Bilateral[
    [Array]::IndexOf($argsHalf64Bilateral, "legacy-full")
] = "half-bilateral"
$argsHalf64Bilateral[
    [Array]::IndexOf(
        $argsHalf64Bilateral,
        "benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/full64-diagnostic.json")
] = "benchmark-results/ssao-half-resolution/<BATCH_ID>/renderdoc/half64-bilateral-diagnostic.json"

& $rdc capture `
    --working-dir $workDir `
    --capture-file (Join-Path $evidence "half64-bilateral-sponza") `
    --opt-disallow-vsync `
    --wait-for-exit `
    $exe @argsHalf64Bilateral
```

### 4.1 Half-64 Bilateral 验收

逐项确认：

1. 事件顺序为 GBuffer 写入、SSAO Generate、SSAO Upsample、Deferred
   Lighting。
2. Generate 是 6-vertex 全屏 Draw，其颜色 attachment 为
   `960 x 540 GL_R16F`。通用尺寸规则是
   `ceil(width / 2) x ceil(height / 2)`。
3. Generate 仍读取全分辨率 `gPosition`、`gNormal` 和 `4 x 4` noise，
   但只向半分辨率 attachment 写 AO。
4. Upsample 是第二个 6-vertex 全屏 Draw，其颜色 attachment 为
   `1920 x 1080 GL_R16F`。
5. Upsample 使用 `ssaoUpsampleFragment.glsl`，并同时绑定：

   - texture unit 0，`halfAO`：`960 x 540 GL_R16F`；
   - texture unit 1，`gPosition`：`1920 x 1080 GL_RGBA16F`，
     alpha 保存 linear view depth；
   - texture unit 2，`gNormal`：`1920 x 1080 GL_RGB16F`。

6. 在 shader source/resource access 中确认它执行邻域 `halfAO` 读取，并用
   full-resolution depth 与 normal 计算权重。只看到普通 bilinear sampling
   不能通过本项验收。
7. Deferred Lighting 读取的是全分辨率 Upsample 输出，而不是直接读取
   `960 x 540` 的 half AO。
8. 保存半分辨率 Generate 输出、全分辨率 Upsample 输出及其输入资源绑定的
   原尺寸截图。

## 5. 必须保存的证据

每次捕获后至少保存：

- `full64-sponza.rdc`；
- `half64-bilateral-sponza.rdc`；
- Full-64 Event Browser 事件顺序截图；
- Full-64 Generate attachment 与输入纹理截图；
- Half-64 Bilateral Event Browser 事件顺序截图；
- Half Generate 的 `960 x 540 R16F` attachment 截图；
- Bilateral Upsample 的 `1920 x 1080 R16F` attachment 截图；
- Upsample 的 half AO、full position/depth、full normal 绑定截图；
- Deferred Lighting 最终读取 AO texture 的 Resource Inspector 截图。

同时记录以下元数据：

- 捕获 UTC 时间与 RenderDoc 版本；
- 完整启动命令；
- Git HEAD、dirty 状态和源码检查点/patch 路径；
- Release EXE SHA-256；
- 每个 `.rdc` 的文件大小与 SHA-256；
- Generate、Upsample、Deferred Lighting 的 RenderDoc event ID；
- 每个关键 attachment 的尺寸和格式；
- 输入 texture unit、资源尺寸和格式；
- 捕获中出现的 GL debug/error 信息。

可用以下命令生成哈希记录：

```powershell
$evidence = "C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\ssao-half-resolution\<BATCH_ID>\renderdoc"
Get-ChildItem -LiteralPath $evidence -File |
    Where-Object { $_.Extension -in @(".rdc", ".png") } |
    ForEach-Object {
        [pscustomobject]@{
            file = $_.Name
            bytes = $_.Length
            sha256 = (
                Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256
            ).Hash.ToLowerInvariant()
        }
    } |
    ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $evidence "evidence-sha256.json") `
        -Encoding UTF8
```

## 6. 与性能实验严格隔离

RenderDoc 会注入 hook、跟踪资源与 API 调用，并在捕获帧保存额外状态。这些行为
会改变 CPU 和 GPU 执行成本。因此：

- 不得把 RenderDoc 运行产生的 CPU Frame、GPU Frame、SSAO Pass、
  SSAO Generate 或 SSAO Upsample 时间写入正式性能表；
- 不得把 RenderDoc Event Browser 中显示的单帧 duration 与未注入的正式
  Benchmark 数字混用；
- 不得让正式 300 warm-up、2000 measured、3-process 脚本通过 RenderDoc
  启动；
- `.rdc` 仅证明实际 attachment 尺寸/格式、输入绑定、事件顺序和 Draw；
- 正式性能数字必须来自未注入 RenderDoc、严格 query 样本计数通过的 Release
  Benchmark JSON；
- 可以要求诊断捕获与正式 Benchmark 使用同一 EXE SHA-256，但两者仍是两类
  独立证据。

若安装后仍无法可靠捕获，应在技术报告中明确记录 RenderDoc 版本、失败步骤、
错误日志和未完成项，不能把本文的预期检查项表述为已经观察到的事实。
