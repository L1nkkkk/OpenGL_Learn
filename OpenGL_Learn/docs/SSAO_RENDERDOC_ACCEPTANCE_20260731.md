# SSAO RenderDoc 1.45 真实帧捕获与结构验收

日期：2026-07-31
场景：Sponza
结论：真实捕获成功；程序化结构验收通过；GUI 人工截图仍待确认。

## 1. 结论

- RenderDoc 官方稳定版 `1.45.0` 的已签名 MSI payload 已部署为用户级便携运行时。
  Windows Installer 注册式安装未成功，因此不能表述为系统级安装完成。
- Full-64 Legacy 与 Half-64 Bilateral 使用同一个捕获版 Release EXE、
  同一个场景、相机、分辨率和 64 samples，各生成了一个真实、非空的 `.rdc`。
- 官方 `renderdoccmd replay --loops 1` 对两个 `.rdc` 的退出码均为 `0`；
  官方 thumbnail 提取和 `zip.xml` 转换也均成功。
- 对 RenderDoc XML 和 captured GL state 的程序化验收全部通过：
  Full 路径为
  `GBuffer -> SSAO Generate -> Deferred Lighting`，Half 路径为
  `GBuffer -> SSAO Generate -> SSAO Upsample -> Deferred Lighting`。
- Half 路径相对 Full 保持 393 个 geometry `glDrawElements` 不变，
  只多一个预期的 6-vertex Upsample `glDrawArrays`：
  总 Draw 从 `398` 变为 `399`。
- 捕获帧内没有 Texture/FBO 创建或重建，也没有 `glFinish`、`glFlush`、
  fence/wait 等显式同步。
- RenderDoc duration 没有进入正式计时。本次捕获不改变既有 SSAO 性能
  Go 结论，只补强了半分辨率链路确实被执行的结构证据。

机器可读总清单：
[evidence-manifest.json](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/manifest/evidence-manifest.json)

程序化结构验收：
[renderdoc-structural-validation.json](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/validation/renderdoc-structural-validation.json)

## 2. RenderDoc 部署状态

### 2.1 官方来源与版本

- 包管理器确认的包 ID：`BaldurKarlsson.RenderDoc`
- 版本：`1.45.0`
- 官方 MSI：
  [RenderDoc_1.45_64.msi](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/installer/RenderDoc_1.45_64.msi)
- MSI SHA-256：
  `228C03E3EBA017A80BB22E75C8F00EC7C04B6D7F31472C6396A878F701022605`
- Authenticode 验证：有效
- 签名者：
  `CN=Baldur Scott Karlsson, serialNumber=750811-1672, C=SE`
- 命令行版本输出：

  ```text
  renderdoccmd x64 v1.45 built from 2fc0bc04cb95499635f63986a55bc6f67849dd9f
  ```

官方入口：
[RenderDoc v1.45 release](https://github.com/baldurk/renderdoc/releases/tag/v1.45)、
[in-application API 文档](https://renderdoc.org/docs/in_application_api.html)。

### 2.2 实际部署结果

系统 MSI 安装遇到 Windows Installer `2503/2502`，没有生成卸载注册项或
`C:\Program Files\RenderDoc`。失败日志：
[msiexec-admin-extract.log](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/installer/msiexec-admin-extract.log)

随后只使用同一份已签名官方 MSI 的 File/Component/Directory 表和内嵌 CAB，
还原了用户级便携运行时：

```text
C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\
benchmark-results\ssao-renderdoc-evidence\renderdoc-1.45.0\
portable\RenderDoc
```

关键文件：

| 文件 | SHA-256 |
|---|---|
| `renderdoccmd.exe` | `273352017E23E890FE9134DE0157D1FE556676A4C6004BFE3265DB1A4648ED07` |
| `qrenderdoc.exe` | `C9904905FE380B2869D48C7A4209C2331370E0BFDA502A24DA26EC4031CF885B` |
| `renderdoc.dll` | `313F450BFA1F7F7D5F50CC7EE80D9F99B68D3FE873E25EB865FF0433E359F7B3` |

便携提取日志：
[portable-extract.log](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/installer/portable-extract.log)

`qrenderdoc --python` 在进入脚本入口前挂起，因此本轮没有声称完成 GUI
Event Browser 人工检查。命令行 capture、thumbnail、convert 和 replay
均真实成功。

## 3. 源码与正式数据隔离

Git HEAD：

```text
12c04af1a9c82d886c30f37e513a705bcd2d27ef
```

捕获时工作区为 dirty；manifest 记录了 88 行 `git status --short`。
没有 reset、clean、checkout 或提交。

正式性能 EXE：

```text
C:\Users\Link\Dev\OpenGL_Learn\x64\Release\OpenGL_Learn.exe
SHA-256:
95456C15E767724EA4ECA7901F236A146D1EEEE8F12B9B5DE0F1F136B78B9071
```

该文件已保存检查点，哈希完全一致：
[OpenGL_Learn-formal-95456c15.exe](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/checkpoint/OpenGL_Learn-formal-95456c15.exe)

捕获版 Release EXE：

```text
C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\x64\Release\OpenGL_Learn.exe
SHA-256:
612A4E1CA8C7CBFDC31B6CD4D86269BEA2EA40388D821474E93F0B563D59E687
```

两个 `.rdc` 均使用这个捕获版 EXE。它只新增默认关闭的诊断开关：

```text
--classic-scene-renderdoc-capture-frame
--classic-scene-renderdoc-capture-template
```

只有同时显式传入这两个参数并通过 RenderDoc 注入启动时，程序才加载
`RENDERDOC_GetAPI`，在指定帧调用 `StartFrameCapture`/`EndFrameCapture`。
SSAO RenderPass、shader 和算法没有为本次捕获修改。

正式结果目录包含 164 个文件，最新写入时间仍为
`2026-07-31 04:24:05 +08:00`：

```text
C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\
benchmark-results\ssao-half-resolution\
ssao-half-formal-20260731\formal
```

本次没有重跑或改写正式性能采样。

## 4. 固定捕获协议

两次进程除 SSAO mode 和输出文件名外保持一致：

| 项目 | 固定值 |
|---|---|
| Scene | Sponza |
| Model | `classic-scenes/sponza/sponza.obj` |
| Render path | `pbr-deferred` |
| Resolution | `1920 x 1080` |
| Camera position | `(-6, -1.5, 0)` |
| Camera target | `(6, -0.8, 0)` |
| Camera up | `(0, 1, 0)` |
| FOV | `55 degrees` |
| Normalized radius | `15` |
| World scale | `1` |
| SSAO samples | `64` |
| SSAO radius/bias | `0.35 / 0.025` |
| VSync | off |
| RenderDoc capture frame | `120` |
| Diagnostic warm-up | `150` |
| Diagnostic measured frames | `30` |

RenderDoc 捕获帧位于诊断 measurement 之前。整个进程仍然受到 RenderDoc
注入影响，因此后续 30 帧也只用于入口完整性校验，不作为性能数据。

实际有效命令可用下面的 PowerShell 形式复现：

```powershell
$base = "C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\" +
    "benchmark-results\ssao-renderdoc-evidence\renderdoc-1.45.0"
$rdc = Join-Path $base "portable\RenderDoc\renderdoccmd.exe"
$exe = "C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\" +
    "x64\Release\OpenGL_Learn.exe"
$work = "C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn"

$common = @(
    "--classic-scene-test", "classic-scenes/sponza/sponza.obj",
    "--classic-scene-camera", "-6", "-1.5", "0",
    "--classic-scene-target", "6", "-0.8", "0",
    "--classic-scene-up", "0", "1", "0",
    "--classic-scene-radius", "15",
    "--classic-scene-world-scale", "1",
    "--classic-scene-fov", "55",
    "--classic-scene-render-path", "pbr-deferred",
    "--classic-scene-width", "1920",
    "--classic-scene-height", "1080",
    "--classic-scene-ssao-samples", "64",
    "--classic-scene-warmup-frames", "150",
    "--classic-scene-capture-frame", "180",
    "--classic-scene-renderdoc-capture-frame", "120",
    "--classic-scene-no-capture"
)

$fullTemplate = Join-Path $base "captures\sponza-full64-legacy"
$full = $common + @(
    "--classic-scene-name", "renderdoc-sponza-full64",
    "--classic-scene-result",
        "benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/" +
        "diagnostics/sponza-full64-legacy-diagnostic.json",
    "--classic-scene-ssao-mode", "legacy-full",
    "--classic-scene-renderdoc-capture-template", $fullTemplate
)
& $rdc capture --working-dir $work --capture-file $fullTemplate `
    --opt-disallow-vsync --wait-for-exit $exe @full

$halfTemplate = Join-Path $base "captures\sponza-half64-bilateral"
$half = $common + @(
    "--classic-scene-name", "renderdoc-sponza-half64-bilateral",
    "--classic-scene-result",
        "benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/" +
        "diagnostics/sponza-half64-bilateral-diagnostic.json",
    "--classic-scene-ssao-mode", "half-bilateral",
    "--classic-scene-renderdoc-capture-template", $halfTemplate
)
& $rdc capture --working-dir $work --capture-file $halfTemplate `
    --opt-disallow-vsync --wait-for-exit $exe @half
```

捕获 stdout/stderr：

- [Full stdout](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/capture-full64-stdout-run1.log)
- [Full stderr](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/capture-full64-stderr-run1.log)
- [Half stdout](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/capture-half64-bilateral-stdout-run1.log)
- [Half stderr](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/capture-half64-bilateral-stderr-run1.log)

## 5. 真实捕获文件

| Capture | Bytes | SHA-256 |
|---|---:|---|
| [Full-64 Legacy RDC](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/captures/sponza-full64-legacy_capture.rdc) | 361,430,633 | `08C1C549F5F6CEE206580FEDE380BFD15316B85513DB1B16E7A00FA3EB187847` |
| [Half-64 Bilateral RDC](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/captures/sponza-half64-bilateral_capture.rdc) | 362,302,304 | `0935E22D8EAC4C1EB517FBE451511EAC7086714BD8CD2C7BAD7FA5A93B5E702C` |

官方命令行回放：

```powershell
& $rdc replay --width 640 --height 360 --loops 1 `
    (Join-Path $base "captures\sponza-full64-legacy_capture.rdc")
& $rdc replay --width 640 --height 360 --loops 1 `
    (Join-Path $base "captures\sponza-half64-bilateral_capture.rdc")
```

两次退出码均为 `0`：

- [Full replay log](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/replay-validation-full64.log)
- [Half replay log](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/logs/replay-validation-half64-bilateral.log)

官方提取的捕获缩略图：

### Full-64 Legacy

![Full-64 Legacy capture thumbnail](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/thumbnails/sponza-full64-legacy.png)

### Half-64 Bilateral

![Half-64 Bilateral capture thumbnail](../benchmark-results/ssao-renderdoc-evidence/renderdoc-1.45.0/thumbnails/sponza-half64-bilateral.png)

## 6. 程序化结构验收

验收来源是官方 `renderdoccmd convert --convert-format zip.xml` 导出的原始
OpenGL chunks、shader source、initial resource contents 和 captured
`GLRenderState`。下列数字是 capture-local `chunkIndex`，不是 GUI Event ID。

### 6.1 事件链与 Draw

| Path | GBuffer | Generate | Upsample | Deferred Lighting |
|---|---:|---:|---:|---:|
| Full-64 | chunks `7776..10197`, 393 Draws | `10286`, 1 Draw | 无 | `10367`, 1 Draw |
| Half-64 Bilateral | chunks `7806..10227`, 393 Draws | `10316`, 1 Draw | `10333`, 1 Draw | `10414`, 1 Draw |

| Path | `glDrawElements` | `glDrawArrays` | Total |
|---|---:|---:|---:|
| Full-64 | 393 | 5 | 398 |
| Half-64 Bilateral | 393 | 6 | 399 |

Half 只多一个预期的 `glDrawArrays(GL_TRIANGLES, count=6)` Upsample Draw。

### 6.2 Full-64 Legacy

Generate：

- FBO `1486`
- Viewport `1920 x 1080`
- `GL_COLOR_ATTACHMENT0`：
  texture `1487`，`1920 x 1080 GL_R16F`
- `gPosition`：
  unit 0，texture `1448`，`1920 x 1080 GL_RGBA16F`
- `gNormal`：
  unit 1，texture `1449`，`1920 x 1080 GL_RGB16F`
- `texNoise`：
  unit 2，texture `1489`，`4 x 4 GL_RGB16F`

Deferred Lighting：

- `ssaoMap`：
  unit 8，texture `1487`
- 即直接读取 Full Generate 输出。

### 6.3 Half-64 Bilateral

Generate：

- FBO `1489`
- Viewport `960 x 540`
- `GL_COLOR_ATTACHMENT0`：
  texture `1490`，`960 x 540 GL_R16F`
- `gPosition`：
  unit 0，texture `1448`，`1920 x 1080 GL_RGBA16F`
- `gNormal`：
  unit 1，texture `1449`，`1920 x 1080 GL_RGB16F`
- `texNoise`：
  unit 2，texture `1492`，`4 x 4 GL_RGB16F`

Upsample：

- FBO `1486`
- Viewport `1920 x 1080`
- `GL_COLOR_ATTACHMENT0`：
  texture `1487`，`1920 x 1080 GL_R16F`
- `halfAO`：
  unit 0，texture `1490`，`960 x 540 GL_R16F`
- `gPosition`：
  unit 1，texture `1448`，`1920 x 1080 GL_RGBA16F`
- `gNormal`：
  unit 2，texture `1449`，`1920 x 1080 GL_RGB16F`
- captured shader source 同时包含 `relativeDepthDelta`、`depthWeight` 和
  `normalWeight`；不是普通 bilinear 路径。
- captured uniforms：
  `depthSigma = 0.02`，`normalPower = 32.0`

Deferred Lighting：

- `ssaoMap`：
  unit 8，texture `1487`
- 即读取全分辨率 Bilateral 输出，而不是直接读取 texture `1490`。

### 6.4 生命周期与同步

在两个 capture 的 `Internal::Beginning of Capture` 与
`Internal::End of Capture` 之间：

- Texture/FBO 创建或 attachment 重建：`0`
- `glFinish`/`glFlush`/fence/wait：`0`
- Application diagnostic JSON：两者 `success = true`
- 捕获进程结束后 tracked texture、mesh CPU/GPU 和 render-target bytes：
  均为 `0`

## 7. 仍需 GUI 人工确认的项目

本轮没有完成 QRenderDoc GUI 的人工 Event Browser/Pipeline State 截图，
也没有把命令行或 XML 检查冒充 GUI 目视检查。

用户在可正常启动 GUI 的 RenderDoc 环境中打开两个 `.rdc` 后，最短检查步骤：

1. Full capture 中定位使用 SSAO shader 且 `count=6` 的 Generate Draw。
   查看 color output 为 `1920 x 1080 R16F`，并确认输入为
   `gPosition/gNormal/noise`。
2. 确认 Full 的 Deferred Lighting `ssaoMap` 指向同一个 Full AO resource，
   且中间没有 Upsample Draw。
3. Half capture 中依次定位 Generate 和紧随其后的 Upsample Draw。
   确认 Generate output 为 `960 x 540 R16F`。
4. 在 Upsample Pipeline State 中确认 unit 0/1/2 分别为
   `halfAO/gPosition/gNormal`，output 为 `1920 x 1080 R16F`。
5. 打开 Upsample fragment shader，目视确认 depth 与 normal 权重代码。
6. 在 Deferred Lighting 中确认 `ssaoMap` 指向全分辨率 Upsample output。
7. 查看 Resource History，确认稳态帧中没有 AO Texture/FBO 重建。
8. 保存 Event Browser、Generate output、Upsample bindings/output 和
   Deferred `ssaoMap` 的 GUI 截图。

Portable `qrenderdoc.exe` 路径：

```text
C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\
benchmark-results\ssao-renderdoc-evidence\renderdoc-1.45.0\
portable\RenderDoc\qrenderdoc.exe
```

若该 GUI 仍在启动时挂起，最短人工步骤是在不受当前沙箱限制的管理员桌面会话中，
用同一官方 MSI 完成系统安装，然后直接打开这里已经生成的两个 `.rdc`；无需重新
运行性能 Benchmark。

## 8. 对现有 Go 结论的影响

现有正式数据来自未注入 RenderDoc 的 Release EXE，保持不变：

- Sponza：Full-64 SSAO total `2.209 ms`，
  Half-64 Bilateral `0.815 ms`
- San Miguel：Full-64 SSAO total `2.626 ms`，
  Half-64 Bilateral `0.905 ms`

本次 RenderDoc 证据不提供新的性能数字，也不重新计算 Go/No-Go。
它证明了以下先前只能由应用日志和 FBO 校验支持的事实：

- Half Generate 实际写入 `960 x 540 R16F`
- Bilateral 实际读取 half AO、full position/depth 和 full normal
- Bilateral 实际输出 `1920 x 1080 R16F`
- Deferred Lighting 实际读取全分辨率 Bilateral 输出
- Half 相对 Full 只增加预期的一个全屏 Upsample Draw
- 稳态捕获帧没有意外资源重建或显式同步

因此现有 SSAO 优化 Go 结论保持成立，且实现链路的证据更完整。RenderDoc
capture duration 不得写入正式报告或简历数字。

## 9. 已知限制

- 系统注册式安装没有完成；当前是官方已签名 payload 的用户级便携部署。
- `qrenderdoc --python` 自动化没有进入脚本入口，未产出 GUI 截图。
- XML 的 `chunkIndex` 不等于 GUI Event ID；GUI 打开后应记录实际 Event ID。
- 两个 capture 使用同一个捕获版 EXE，但该 EXE 与正式性能 EXE 的哈希不同；
  差异和两个哈希都已明确记录。
- 本轮不重新评估 AO 质量、SSIM/PSNR 或边缘 crop；这些证据继续使用正式
  Half-Resolution SSAO 报告中的既有 PFM/图像产物。
