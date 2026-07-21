# PBR 与 IBL：P0 实现、性能与验收

本文档记录 metallic-roughness PBR 与 image-based lighting（IBL）的 P0 实现、验收方法、性能代价，以及在实现过程中保留或拒绝的优化实验。所有数据遵循 PERFORMANCE_OPTIMIZATION_PROTOCOL.md。

## 1. 范围与完成状态

P0 已覆盖：

- Cook-Torrance BRDF：GGX 法线分布、Smith 几何遮蔽、Schlick Fresnel。
- metallic / roughness / AO / emissive 因子与贴图。
- albedo、emissive 使用 sRGB 采样；normal、metallic、roughness、AO 使用线性采样。
- Assimp base-color、metalness、roughness、AO、emissive、normal 导入。
- OBJ 兼容：map_Bump 作为 normal fallback，并识别同目录 roughness、AO、metallic、emissive 常用文件名。
- 前向和延迟两条 PBR 路径。
- 基于现有天空盒的 irradiance cubemap、prefilter cubemap 和 BRDF LUT。
- 编辑器 Shader Type 中的 PBR 选项。
- PBR 专项 smoke test、前向/延迟截图对齐与资源释放检查。

未显式指定 shader 的文件模型默认使用 PBR；需要旧行为时应显式传入 Phong shader。场景存档中的 shader 名称仍会按原值恢复。

## 2. 材质约定

| 语义 | 材质属性 | 贴图属性 | 色彩空间 |
| --- | --- | --- | --- |
| Base color | albedo | texture_diffuse | sRGB |
| Normal | — | texture_normal | Linear |
| Metallic | metallic | texture_metallic | Linear |
| Roughness | roughness | texture_roughness | Linear |
| Ambient occlusion | ao | texture_ao | Linear |
| Emissive | emissive | texture_emissive | sRGB |
| Opacity / cutout | opacity、alphaCutoff、useAlphaCutoff | texture_diffuse alpha | Linear alpha |

当 metallicRoughnessPacked 为 true 时，按 glTF 约定读取 B 通道 metallic、G 通道 roughness。缺失数据使用稳定默认值：albedo 1、metallic 0、roughness 0.5、AO 1、emissive 0。

## 3. IBL 资源与生命周期

IBL 在场景首次出现 PBR 材质时初始化：

| Resource | Format | Size | Logical bytes |
| --- | --- | ---: | ---: |
| Irradiance cubemap | RGB16F | 32 × 32 × 6 | 36,864 |
| Prefilter cubemap | RGB16F | 128 base、5 mips、6 faces | 785,664 |
| BRDF LUT | RG16F | 256 × 256 | 262,144 |
| Total | — | — | 1,084,672 bytes（1.034 MiB） |

预计算使用 256 个 Hammersley samples。资源由 ImageBasedLighting 统一创建、绑定、记账和销毁；PBR smoke test 结束后 Texture、Mesh CPU、Mesh GPU、Render target 的 current bytes 必须全部归零。

## 4. 延迟渲染布局

| Attachment | Format | Legacy | PBR |
| --- | --- | --- | --- |
| gPosition | RGBA16F | world position + view depth | 同左 |
| gNormal | RGB16F | world normal | world normal |
| gAlbedoSpec | RGB8 | albedo | albedo |
| gMaterial | RGBA16F | ambient、diffuse、specular、shininess | metallic、roughness、AO、PBR sentinel |
| gEmissive | RGB16F | 0 | emissive |

在 1440 × 900 下，新增 gEmissive 使已分配的 Deferred render-target 遥测增加约 7.41 MiB。Forward 默认路径不分配该资源。

PBR 的 IBL 与 emissive 是每像素只合成一次的非加法项。当前若场景含 PBR 材质，即使 LIGHT_VOLUME 已开启，也会使用正确的 fullscreen deferred lighting；旧 Phong 场景继续使用 light-volume 路径。

## 5. 自动验收

从 OpenGL_Learn 项目目录运行 Release x64：

    ..\x64\Release\OpenGL_Learn.exe --pbr-smoke-test

验收条件与 2026-07-21 结果：

- 所有常规 shader 编译并链接成功。
- Backpack PBR 材质存在 albedo、normal、roughness、AO 贴图，以及 metallic、emissive 因子。
- IBL 三类资源创建成功。
- Forward mean luminance：0.4773；Deferred：0.4772。
- Forward / Deferred 归一化 RGB MAE：0.000585（0.0585%，阈值 1%）。
- 两张截图均已视觉检查，材质、法线和环境反射一致。
- 退出时 textureBytes、meshCpuBytes、meshGpuBytes、renderTargetBytes 均为 0。

截图与原始输出：

- benchmark-results/pbr-ibl/pbr-forward.ppm
- benchmark-results/pbr-ibl/pbr-deferred.ppm

资源回归使用：

    ..\x64\Release\OpenGL_Learn.exe --resource-smoke-test

FBO 生命周期仍为 2 → 4 → 6 → 8 → 2；所有阶段 Mesh CPU 为 0.00 MiB，Mesh GPU 为 14.61 MiB。

## 6. 正式性能实验条件

- Date：2026-07-21
- Control commit：8c7e88b
- Candidate：codex/pbr-ibl 最终工作树
- Build：Release x64
- Resolution：1440 × 900
- OS：Windows 11 Pro，build 26200
- CPU：Intel Core i7-12700KF，12 cores / 20 logical processors
- GPU：NVIDIA GeForce RTX 5060 Ti，driver 591.86
- OpenGL：3.3.0 NVIDIA 591.86
- Swap interval：0
- 每种 variant 先运行一次不计入结果的 300-frame warm-up
- 每次正式运行：300 warm-up frames + 1200 sample frames
- 新进程顺序：A / B / B / A / A / B
- FPS 不作为结论；优先使用 CPU/GPU frame 与直接受影响的 pass

### 6.1 Retained optimization：共享 Assimp 材质实例

**Problem：** Backpack 的 80 个 mesh 使用同一个 Assimp material index，但原实现为每个 mesh 创建独立 Material。指针不同使 MaterialBatchScope 无法命中，每帧重复提交 PBR uniform 和纹理状态。

**Implementation：** 同一 Model 导入期间，以 Assimp material index 复用 shared Material；mesh geometry、可见性和 draw 数不变。专项控制开关 --benchmark-unshared-imported-materials 仅用于恢复旧行为。

**Method：** 同一个最终二进制与 builtin/backpack-pbr；A 关闭共享，B 开启共享。完整 JSON 位于 benchmark-results/pbr-ibl/material-sharing。

原始核心样本：

| Order | Variant | Load ready ms | CPU median ms | CPU P95 ms | Forward CPU median ms | Forward CPU P95 ms | Uniform updates |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | A unshared | 1179.238 | 1.8316 | 3.1560 | 0.6429 | 0.9432 | 3828 |
| 2 | B shared | 1336.643 | 1.3149 | 2.7199 | 0.2030 | 0.3024 | 162 |
| 3 | B shared | 1340.217 | 1.2983 | 2.6008 | 0.2001 | 0.3005 | 162 |
| 4 | A unshared | 1414.086 | 2.0282 | 4.1679 | 0.7033 | 1.0178 | 3828 |
| 5 | A unshared | 1378.987 | 1.8616 | 3.2450 | 0.6744 | 0.9555 | 3828 |
| 6 | B shared | 1344.845 | 1.3543 | 2.7616 | 0.2098 | 0.3148 | 162 |

汇总：

| Metric | A average | B average | Delta | Relative | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU Frame median | 1.9071 ms | 1.3225 ms | -0.5846 ms | -30.66% | Improvement |
| CPU Frame P95 | 3.5230 ms | 2.6941 ms | -0.8289 ms | -23.53% | Improvement |
| Forward CPU median | 0.6735 ms | 0.2043 ms | -0.4692 ms | -69.67% | Improvement |
| Forward CPU P95 | 0.9722 ms | 0.3059 ms | -0.6663 ms | -68.53% | Improvement |
| Uniform updates | 3828 | 162 | -3666 | -95.77% | Improvement |
| Material bind misses | 80 | 2 | -78 | -97.50% | Improvement |
| Material cache hits | 0 | 78 | +78 | — | Improvement |
| Texture state changes | 484 | 16 | -468 | -96.69% | Improvement |
| GPU Frame median | 0.2022 ms | 0.1988 ms | -0.0035 ms | -1.71% | No material change |
| GPU Frame P95 | 0.4523 ms | 0.4638 ms | +0.0115 ms | +2.53% | Noise |
| Load ready | 1324.10 ms | 1340.57 ms | +16.46 ms | +1.24% | Noise |
| Private bytes | 692.05 MiB | 691.90 MiB | -0.15 MiB | -0.02% | No change |

**Decision：retained。** 直接受影响的 CPU zone、P95 和提交计数均有大幅、同方向改善；GPU、加载和 Private bytes 无实质回退。

### 6.2 PBR 功能成本：Phong vs PBR

**Method：** 同一个最终二进制、同一个 Backpack、相机、灯光和 Forward 设置。A 使用 Phong；B 使用 PBR、roughness/AO maps 与 IBL。draw calls 固定为 82，submitted vertices 固定为 206,643。完整 JSON 位于 benchmark-results/pbr-ibl/feature-cost。

原始核心样本：

| Order | Variant | Load ready ms | CPU median ms | Forward CPU median ms | GPU median ms | Forward GPU median ms | Texture MiB |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | A Phong | 1235.909 | 1.2859 | 0.1905 | 0.4306 | 0.3663 | 272.00 |
| 2 | B PBR | 1356.630 | 1.3282 | 0.2022 | 0.1984 | 0.1307 | 315.70 |
| 3 | B PBR | 1334.417 | 1.2461 | 0.1953 | 0.1985 | 0.1307 | 315.70 |
| 4 | A Phong | 1238.198 | 1.2445 | 0.1879 | 0.4308 | 0.3664 | 272.00 |
| 5 | A Phong | 1186.176 | 1.3234 | 0.1988 | 0.4309 | 0.3665 | 272.00 |
| 6 | B PBR | 1364.579 | 1.2479 | 0.1878 | 0.1985 | 0.1309 | 315.70 |

汇总：

| Metric | Phong average | PBR average | Delta | Relative | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU Frame median | 1.2846 ms | 1.2741 ms | -0.0105 ms | -0.82% | Noise |
| CPU Frame P95 | 2.7258 ms | 2.6891 ms | -0.0367 ms | -1.35% | Noise |
| Forward CPU median | 0.1924 ms | 0.1951 ms | +0.0027 ms | +1.40% | No material change |
| GPU Frame median | 0.4307 ms | 0.1985 ms | -0.2323 ms | -53.92% | Workload-specific improvement |
| GPU Frame P95 | 0.7870 ms | 0.4516 ms | -0.3353 ms | -42.61% | Workload-specific improvement |
| Forward GPU median | 0.3664 ms | 0.1308 ms | -0.2357 ms | -64.32% | Workload-specific improvement |
| Load ready | 1220.09 ms | 1351.88 ms | +131.78 ms | +10.80% | PBR activation cost |
| Texture current | 272.00 MiB | 315.70 MiB | +43.70 MiB | +16.07% | Expected feature cost |
| Texture count | 6 | 9 | +3 | +50.00% | Two material maps + IBL resources |
| Private bytes | 648.60 MiB | 689.15 MiB | +40.55 MiB | +6.25% | Expected feature cost |
| Working set | 111.44 MiB | 112.88 MiB | +1.44 MiB | +1.29% | Small increase |
| Uniform updates | 128 | 162 | +34 | +26.56% | Expected PBR parameters |

本场景中 PBR GPU 更快是实测结果，但不能外推到更多灯光、阴影或不同材质覆盖率。当前 Phong shader 会为每个活动灯重复采样部分材质数据，两种模式也不是视觉等价 shader，因此该项只说明此固定 Backpack workload 没有 GPU 时间回退。

### 6.3 旧场景回归：8c7e88b vs PBR candidate

**Method：** saved/last_scene.json，控制与候选使用相同 scene hash 和相同用户模型/纹理副本。完整有效数据位于 benchmark-results/pbr-ibl/legacy-regression-v2。

| Metric | 8c7e88b average | Candidate average | Delta | Relative | Assessment |
| --- | ---: | ---: | ---: | ---: | --- |
| CPU Frame median | 1.1729 ms | 1.1678 ms | -0.0051 ms | -0.43% | Noise |
| CPU Frame P95 | 2.2363 ms | 2.2239 ms | -0.0124 ms | -0.56% | Noise |
| Forward CPU median | 0.2557 ms | 0.2498 ms | -0.0059 ms | -2.30% | Small / within variation |
| GPU Frame median | 0.6208 ms | 0.6378 ms | +0.0169 ms | +2.73% | Noise; run ranges overlap |
| GPU Frame P95 | 1.0238 ms | 1.0439 ms | +0.0201 ms | +1.96% | Noise |
| Load ready | 1423.62 ms | 1543.81 ms | +120.19 ms | +8.44% | High-variance startup result |
| Texture current | 451.00 MiB | 451.00 MiB | 0 | 0% | No change |
| Mesh GPU | 14.61 MiB | 14.61 MiB | 0 | 0% | No change |
| Render target | 24.72 MiB | 24.72 MiB | 0 | 0% | No change |
| Draw calls / vertices / triangles | 44 / 815,928 / 271,976 | Same | 0 | 0% | Matched workload |

**Decision：legacy steady-state accepted。** CPU/GPU 差异小于运行间波动，核心 draw 与资源计数一致。Load ready 的两组范围较宽，因此记录为启动噪声，不宣称改善；PBR 真正启用时的一次性成本以 6.2 的同二进制专项实验为准。

第一次 legacy-regression 运行因干净 worktree 缺少被 .gitignore 排除的 OBJ，控制组出现 Assimp missing-file 错误，几何计数不匹配；该目录只保留诊断用途，未混入任何汇总。v2 在镜像相同资产并核对 hash 后完整重跑。

### 6.4 Rejected experiment：PBR shader 按需编译

**Hypothesis：** 旧 Phong 场景不在 ShaderManager::Init 中编译 PBR，可缩短启动。

**Method：** 临时增加同二进制 eager/lazy 控制开关与 shader-initialization 直接计时，执行标准 A/B/B/A/A/B；完整 JSON 位于 benchmark-results/pbr-ibl/lazy-pbr-shader-v3。

| Order | Variant | Shader init ms | Load ready ms |
| ---: | --- | ---: | ---: |
| 1 | A eager | 16.0518 | 1443.315 |
| 2 | B lazy | 18.9154 | 1535.218 |
| 3 | B lazy | 14.4165 | 1467.189 |
| 4 | A eager | 18.1091 | 1513.413 |
| 5 | A eager | 16.8742 | 1364.128 |
| 6 | B lazy | 17.2236 | 1397.273 |

Shader initialization 平均 17.0117 → 16.8518 ms，只有 -0.1599 ms（-0.94%）；Load ready 为 1440.28 → 1466.56 ms（+26.27 ms，+1.82%）。两项均落在运行波动内，没有可重复收益。

**Decision：rejected。** 按需编译实现、专项开关和计时字段均已撤销，最终代码保留 eager shader validation；本节保留以避免重复尝试。

## 7. 已知限制与后续方向

- 当前 IBL 输入是现有 LDR sRGB cubemap，不是 HDR equirectangular environment；高动态范围环境导入属于后续功能。
- 尚未提供运行时更换天空盒后的 IBL 重建。
- PBR + Deferred light-volume 暂时回退 fullscreen lighting；后续可增加独立 ambient/IBL/emissive composition pass。
- Smoke asset 是 OBJ + 常用贴图命名；Assimp glTF metallic-roughness packed path 已实现，但仍需要专门 glTF 资产回归。
- 未实现纹理压缩、流式加载或 PBR map 分辨率策略；当前 +43.70 MiB 主要来自两个额外高分辨率材质贴图和 1.034 MiB IBL 逻辑资源。
- 透明 PBR、clear-coat、transmission、anisotropy 和 area lights 不在本 P0 范围。
