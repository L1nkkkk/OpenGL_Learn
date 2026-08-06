# 显式 gPosition 与 Depth Position Reconstruction A/B 实验报告

**结论：NO-GO（质量/性能未通过）。** 候选未通过质量、性能的事前门槛；保持当前显式 gPosition 为默认。

- 决策门：显存 PASS；质量 FAIL；query/invariant PASS；生命周期 PASS；性能 FAIL。

## 实验身份与协议

- Batch：`gbuffer-position-formal-20260801-v3`；preset：`Formal`。
- Release x64 EXE SHA-256：`DAC2B05BCB69D638BACEB224D3B34E945D5BFDF56A2F78B399C4BB0A84B8D13A`。所有进程启动前后均复核该 hash。
- 源码 checkpoint：`ED481517D4DED319E04B6A73529F3E12388FF6ADAAF96EA4A6FE629C62578B86`；git HEAD `83f1a0eb0abf7f07a1ab4dbd001773d47f4b7252`。脏工作区使用逐文件 SHA-256 固化，每个进程前后复核，未提交、未重置用户修改。
- 分辨率 1920×1080，VSync off，PBR Deferred，Shadows/Bloom/自动热重载关闭。
- 每进程 300 帧预热、2000 帧测量；平衡顺序 `A/B/B/A/A/B`。
- SSAO Off 与 SSAO On（Legacy Full、64 samples、radius 0.35、bias 0.025）分别统计，性能进程没有图像读回。
- GPU zone 使用可嵌套的 GL_TIMESTAMP 起止对（GPU Frame ⊃ Deferred Pass ⊃ 子阶段），并对 GPU query/CPU zone 样本数逐进程做精确校验。
- Go/No-Go 阈值在任何正式数据生成前写入 `experiment-metadata.json`。

## 变体与显存

| 变体 | Color MRT | Depth/Stencil | Renderer-owned G-Buffer |
|---|---:|---|---:|
| A Control | 5（含 RGBA16F gPosition） | D24S8 renderbuffer | 35 B/px |
| B Candidate | 4（无 gPosition） | 可采样 D24S8 texture，depth/stencil 共用 | 27 B/px |

B 确定性减少 **8 B/px**；本分辨率为 **16,588,800 bytes（15.82 MiB）**。这项减少由每个结果中的 `rendererTrackedBytes` 与 attachment/format 自检共同确认。

## GPU 性能

下表先对每个独立进程求 Median/P95，再对三个配对差值取中位数；差值均为 B−A，负数更快。

| 场景 / 条件 | A Median | B Median | 配对 Median Δ | 配对 Median Δ% | P95 Δ | P95 Δ% |
|---|---:|---:|---:|---:|---:|---:|
| san-miguel / ssao-off | 2.791424 ms | 2.600576 ms | -0.1913 ms | -6.85% | -0.2327 ms | -7.60% |
| san-miguel / ssao-on | 5.71224 ms | 6.038304 ms | +0.3252 ms | +5.69% | +0.3819 ms | +6.32% |
| sponza / ssao-off | 0.78768 ms | 0.682368 ms | -0.1055 ms | -13.39% | -0.1039 ms | -11.22% |
| sponza / ssao-on | 3.151344 ms | 3.858144 ms | +0.7032 ms | +22.31% | +0.7473 ms | +21.42% |

SSAO Off 与 SSAO On 不混池；Go/No-Go 门槛逐个场景×条件应用。完整的 GPU/CPU 逐进程 Median/P95/P99 在 `process-summary.csv`，每一配对差值在 `paired-summary.csv`。

### Pass 归因

| 场景 / 条件 | G-Buffer Geometry Δ | Deferred Lighting Δ | SSAO Generate Δ | Deferred Pass Δ |
|---|---:|---:|---:|---:|
| sponza / ssao-off | -0.0973 ms | -0.0072 ms | n/a | -0.1054 ms |
| sponza / ssao-on | -0.0925 ms | -0.0068 ms | +0.8176 ms | +0.7036 ms |
| san-miguel / ssao-off | -0.3070 ms | -0.0019 ms | n/a | -0.1904 ms |
| san-miguel / ssao-on | -0.2109 ms | -0.0045 ms | +0.5211 ms | +0.3248 ms |

B 的 Geometry 少写一个 RGBA16F MRT；代价是 Lighting 每像素进行 inverseProjection、透视除法和 inverseView 两次矩阵向量变换。SSAO 直接在 View Space 结束，但为保持 Control 的 GL_LINEAR gPosition guide 语义，每个查询点进行 4 次 depth texelFetch、最多 4 次 inverseProjection/透视除法和一次双线性混合，不额外回到 World Space；Half-bilateral guide 同样先逐 texel 重建 view depth 再插值。所有矩阵 inverse 都在 CPU 每 pass 计算一次。整帧结论采用 GPU Frame，而不是只看 Geometry。

## 质量与正确性

| 场景 / 条件 | LDR MAE | LDR P99 | LDR Max | SSIM | AO MAE | World Pos MAE/P95/Max | View Pos MAE/P95/Max | 结果 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| sponza / ssao-off | 0.000048 | 0.003922 | 0.011765 | 0.999997 | n/a | 0.001814/0.003657/0.004419 | 0.001814/0.003658/0.004419 | PASS |
| sponza / ssao-on | 0.000342 | 0.003922 | 0.047059 | 0.999971 | 0.001847 | 0.001814/0.003657/0.004419 | 0.001814/0.003658/0.004419 | FAIL |
| san-miguel / ssao-off | 0.000081 | 0.003922 | 0.309804 | 0.999992 | n/a | 0.001193/0.002052/0.004319 | 0.001193/0.002052/0.004319 | FAIL |
| san-miguel / ssao-on | 0.002295 | 0.023529 | 0.313725 | 0.999293 | 0.009430 | 0.001193/0.002052/0.004319 | 0.001193/0.002052/0.004319 | FAIL |

失败项（使用事前阈值）：
- `sponza / ssao-on`：ldr, AO。
- `san-miguel / ssao-off`：ldr。
- `san-miguel / ssao-on`：ldr, AO。

- Normal/Albedo/Material RGB/Material Alpha/Emissive 捕获 hash：**全部一致**。
- 背景 foreground-mask 不一致像素合计：**0**。
- 位置误差按 Control 前景 View-depth 的 near/mid/far 三分位分桶，阈值在正式数据前固定；逐桶实际深度边界、count、MAE、P95、P99、max 在 `quality-summary.json`，不会用近景平均值掩盖最深三分之一几何的误差。
- 每组输出 A 的 World/View 显式位置与 B 的 World/View 重建位置 PFM；边缘和背景 LDR 统计也写入质量 JSON。

## 生命周期与路径覆盖

- A/B lifecycle smoke（Forward→Deferred+SSAO→Shadows→Forward、1024×640 resize 后恢复、最终释放）：**PASS**。
- A/B Half-resolution bilateral smoke 强制校验 SSAO Generate、Upsample、Deferred Lighting 的 GPU query 与 CPU zone 样本数，覆盖 depth/normal guide：**PASS**（若失败批处理会中止）。
- 正式主实验固定 PBR Deferred。Forward 不读取 G-Buffer，候选开关对 Forward 无影响；旧 `Scene::DrawDefferedModels` 路径会显式警告并安全回退到 gPosition，不会静默错读 attachment。
- 资源 target 在 Resize 时由 FramebuffersManager 重建；每个质量/性能进程结束均确认 Texture/Mesh/RenderTarget tracked bytes 回到 0。

## 限制

- 本轮只决策 Position Reconstruction；没有压缩 Normal/Material/Albedo/Emissive，没有改变 SSAO 参数或样本数，也没有继续 Oct Normal 或 Material/Emissive packing。
- D24S8 texture 与 D24S8 renderbuffer 的物理驱动布局可能有厂商差异；报告中的确定性字节结论是 renderer-owned logical RenderTarget accounting，并由 attachment storage 结构佐证。
- PBR Lighting 保持 World Space，保留了两次 mat4×vec4 的候选 ALU；这使本实验保守地反映当前消费者迁移，而不是同时重构整个 lighting 坐标空间。

## 产物

- `experiment-metadata.json`：事前协议、阈值与 Go/No-Go 规则。
- `run-manifest.json`：每个进程、顺序、pair、退出码，以及 EXE/源码 checkpoint 前后 hash。
- `raw/`：正式逐进程原始 JSON；`quality/`：独立捕获和重建位置。
- `process-summary.csv`、`paired-summary.csv`、`group-summary.csv`、`quality-summary.csv/json`、`summary.json`。
- `charts/`：GPU Frame 配对、pass 归因、深度分桶位置误差图。

## 实际修改文件（checkpoint 范围）

- `OpenGL_Learn/Global.h`
- `OpenGL_Learn/FramebufferManager.cpp`
- `OpenGL_Learn/ShaderManager.h`
- `OpenGL_Learn/ShaderManager.cpp`
- `OpenGL_Learn/DeferRenderPass.h`
- `OpenGL_Learn/DeferRenderPass.cpp`
- `OpenGL_Learn/SSAORenderPass.cpp`
- `OpenGL_Learn/Material.h`
- `OpenGL_Learn/Model.cpp`
- `OpenGL_Learn/Scene.cpp`
- `OpenGL_Learn/Profiler.h`
- `OpenGL_Learn/test.cpp`
- `OpenGL_Learn/OpenGL_Learn.vcxproj`
- `OpenGL_Learn/shaders/deferProcessReconstructFragment.glsl`
- `OpenGL_Learn/shaders/positionReconstruction.glsl`
- `OpenGL_Learn/shaders/deferFragment.glsl`
- `OpenGL_Learn/shaders/deferDirLightVolumeFragment.glsl`
- `OpenGL_Learn/shaders/lightVolumeFragment.glsl`
- `OpenGL_Learn/shaders/lightVolumeFullscreenFragment.glsl`
- `OpenGL_Learn/shaders/ssaoFragment.glsl`
- `OpenGL_Learn/shaders/ssaoUpsampleFragment.glsl`
- `OpenGL_Learn/tools/run_gbuffer_position_reconstruction.ps1`
- `OpenGL_Learn/tools/analyze_gbuffer_position_reconstruction.py`
