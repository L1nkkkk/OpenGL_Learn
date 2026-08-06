# Deferred 点光源自适应路径：从 Stencil Volume 到 Screen-Space Analytic 的技术原理

- 状态：保守 Bounds、Scissored Volume、Analytic Screen 已实现并完成正式实验；Adaptive 因无稳定交叉点判定 No-Go
- 日期：2026-08-04
- 适用项目：OpenGL_Learn，OpenGL 3.3，Phong Deferred Light Volume
- 当前默认：固定 Analytic Screen；Coalesced `N+1`、Scissored Volume、Analytic Volume 与 Legacy2N 均保留显式复现开关

> 本文最初是实现前设计，现已按 2026-08-04 的代码和正式实验修订。完整原始数据、逐进程统计、图像差异、RenderDoc replay 与最终决策以新报告为准；未实现的 Adaptive 仍不得写成已落地能力。

相关材料：

- [点光源压力场景与基线](POINT_LIGHT_HEAVY_BASELINE_CN.md)
- [Legacy RenderDoc GPU 瓶颈拆解](docs/POINT_LIGHT_RENDERDOC_BREAKDOWN_CN.md)
- [Stencil Clear 合并正式 A/B](benchmark-results/point-light-stencil-clear-ab/stencil-clear-coalescing-ab-20260802/REPORT_CN.md)
- [Screen Bounds / Scissor / Analytic 正式报告](benchmark-results/point-light-screen-routing/screen-bounds-scissor-analytic-20260804/REPORT_CN.md)
- [项目性能优化实验协议](PERFORMANCE_OPTIMIZATION_PROTOCOL.md)
- [2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere](https://research.nvidia.com/publication/2013-08_2d-polyhedral-bounds-clipped-perspective-projected-3d-sphere)
- [GPU Gems 3：Deferred Shading in Tabula Rasa](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-19-deferred-shading-tabula-rasa)

---

## 1. 一句话概括

优化前 Deferred 点光源使用同一种 Stencil Light Volume 路径处理所有灯：

```text
每盏灯：
    绘制球体生成 Stencil Mask
    → 再绘制一次球体计算 Lighting
    → 清理本灯留下的 Stencil
```

它能正确限制光照范围，但每盏灯固定承担两次几何 Draw、Stencil 状态维护和 Clear。不同灯在屏幕上的投影面积、相机关系和重叠程度差异很大，因此不存在尚未验证就可以假定对所有灯都最优的单一路径。

本轮实际按四个门控阶段执行：

```text
A：Legacy Stencil Volume，2N 次点光 Clear
    ↓ 已完成：合并相邻冗余 Clear
B：Coalesced Stencil Volume，N+1 次点光 Clear
    ↓ 已完成：计算保守屏幕包围与遥测
C1：Scissored Stencil Volume，已实现并判定 Go
C2：Analytic Screen Rect，已实现并判定固定路径 Go
    ↓ 四档 coverage 均为 Screen 胜出，没有稳定交叉点
D：Adaptive Selector，按预设门槛停止，Not-Implemented / No-Go
```

这里的“自适应”不是为了听起来高级而随意加入分支，而是回答一个具体问题：

> 对当前这盏灯，继续支付 Stencil Volume 的固定成本更便宜，还是直接在它的屏幕矩形内执行一次解析球体判断更便宜？

如果正式实验显示某一条路径在全部代表场景中都占优，就不应该强行保留 Adaptive Selector；此时正确结论是选择全局固定赢家，并将“自适应”判为 No-Go。

---

## 2. 当前路径、已完成工作与剩余问题

### 2.1 当前 Stencil Light Volume 在做什么

当前 `DeferRenderPass::DrawPointLightVolumesDeferred()` 对每盏 Active Point Light 计算有效半径，将 Point Light 的球体代理缩放到该半径，然后执行两次球体绘制。

第一遍只写 Stencil：

```text
ColorMask = false
DepthMask = false
DepthFunc = LESS
Cull      = disabled
Stencil   = back INCR_WRAP / front DECR_WRAP on depth fail
Draw      = point-light sphere
```

它通过球体与已有场景深度的关系，将可能处于光体内部的可见像素标为非零。

第二遍执行 Lighting：

```text
Blend       = ONE + ONE
ColorMask   = true
StencilFunc = NOTEQUAL 0
CullFace    = FRONT
DepthFunc   = GEQUAL
Draw        = the same point-light sphere
```

Fragment Shader 从 G-Buffer 读取 Position、Normal、Albedo、Material 和可选 SSAO，再累加当前点光贡献。

因此，这里的 Stencil 不是点光阴影算法。它是 Deferred Lighting 的像素覆盖掩码，用来避免在光体外运行昂贵的点光 Fragment Shader。

### 2.2 Legacy 为什么是 `2N` 次点光 Clear

Legacy 对每盏灯都执行：

```text
ClearBefore(i)
→ StencilDraw(i)
→ LightingDraw(i)
→ ClearAfter(i)
```

对于相邻两盏灯：

```text
ClearAfter(i)
→ ClearBefore(i + 1)
```

中间没有任何 Stencil 写入，所以两次 Clear 中有一次是冗余的。

### 2.3 已完成的 B：Coalesced `N+1`

Coalesced 显式 Control 保留首灯前一次 Clear，并保留每盏灯后的 Clear，删除第二盏及以后重复的 `ClearBefore`；它已不再是最终默认 Render Mode：

```text
InitialClear
→ [StencilDraw(0) → LightingDraw(0) → ClearAfter(0)]
→ [StencilDraw(1) → LightingDraw(1) → ClearAfter(1)]
→ ...
```

因此：

```text
N = 0：0 次点光 Clear
N > 0：N + 1 次点光 Clear
```

正式 A/B 已证明这一生命周期变化有效：

| 灯数 | Legacy 点光 GPU Median | Coalesced 点光 GPU Median | 变化 | 点光 Clear |
|---:|---:|---:|---:|---:|
| 16 | 0.841 ms | 0.729 ms | -13.31% | 32 → 17 |
| 256 | 11.061 ms | 8.810 ms | -20.35% | 512 → 257 |
| 512 | 21.675 ms | 17.418 ms | -19.64% | 1024 → 513 |

该结果应解释为 Stencil 生命周期优化，不能扩大成“多光源算法已经完成”。因为它没有改变：

- 两次球体 Draw；
- Lighting Fragment 数量；
- 屏幕覆盖范围；
- Point Light 提交数量；
- G-Buffer 读取次数；
- Shadow CubeMap 采样方式。

### 2.4 Coalesced 以后还剩什么

对 `N` 盏提交的点光源，当前点光阶段仍近似为：

```text
Geometry Draw = 2N
Point-light Clear = N + 1
Lighting Shader Work ≈ 所有 Light Volume 覆盖像素的总和
```

Legacy RenderDoc 证据也表明，Clear 并不是唯一成本：

- representative / 512 的 Lighting Volume Draw 为 8.183 ms；
- representative / 512 的 Stencil Volume Draw 为 3.289 ms；
- high-overlap / 16 的 Lighting PS Invocations 是 representative / 16 的 3.54 倍。

Clear 合并删除的是确定的重复状态工作；下一步必须处理“空间覆盖”和“逐灯两次 Draw”，否则只是在继续压缩同一项固定开销。

---

## 3. 为什么固定一种点光路径不一定总是最好

### 3.1 小而局部的灯

当光球只覆盖屏幕很小区域时：

- 屏幕矩形面积小；
- 解析球外拒绝只处理少量像素；
- 一次矩形 Draw 可能比 Stencil Draw + Lighting Draw + Clear 更便宜。

这类灯是 Analytic Screen Rect 的主要候选。

### 3.2 大范围或高度重叠的灯

当灯光覆盖大部分屏幕时：

- 矩形四角会包含大量不在球内的像素；
- 每个候选像素至少要读取 Position 并执行距离判断；
- 多盏灯高度重叠时，矩形路径可能重复检查大量像素。

Stencil Volume 虽然多一个 Pass 和 Clear，但球体光栅覆盖可能比矩形更紧，因此设计阶段不能假设 Screen Rect 一定更快。正式实验最终推翻了“当前测试 GPU 上大覆盖会反转”的候选假设：high-overlap / 256 的 Analytic Volume 为 26.327 ms，Analytic Screen 为 11.276 ms，Screen 改善 15.051 ms / 57.17%，五个独立进程全部同向。这个结论只适用于本轮硬件、场景、分辨率与实现，不能外推所有 GPU。

### 3.3 相机位于光体内或光体穿过 Near Plane

这是屏幕投影最容易出错的情况：

- 普通切线公式可能不存在或数值不稳定；
- 只投影球心并估算半径会漏掉屏幕边缘；
- 球体可能覆盖整个视口；
- 几何 Volume 的 Front/Back Face、Depth 与 Stencil 行为也会进入边界分支。

第一版不能在这里冒险生成偏小矩形。可靠策略只能是：

1. 使用能够处理 Near Plane Clipping 的保守投影算法；或
2. 退化为 Fullscreen Analytic；或
3. 保留当前已经验证过的 Volume Path。

任何无法证明边界正确的情况都不能直接剔除。

### 3.4 完全离屏的灯

Coalesced Control 仍只检查 `Active`；新的 Bounds 路径已经能将点光分类为 `Outside / ConservativeRect / FullscreenFallback`。正式默认没有启用离屏剔除，因此完全位于视锥外的灯只有在显式 `--point-light-offscreen-culling on` 时才跳过提交。

- Radius 计算；
- 两次球体提交；
- Uniform 与纹理绑定；
- Stencil 清理。

一旦保守球体测试能够证明光体与视锥没有交集，这盏灯可以完全跳过。这里的关键字是“证明”：边界不可靠时必须提交，不能用漏光换性能。

### 3.5 所以真正需要比较的是成本模型

对第 `i` 盏灯，可以粗略写成：

```text
T_volume(i)
    = CPU 两次提交
    + Stencil Volume Raster
    + Stencil Clear
    + 受 Stencil/Depth 约束的 Lighting Fragment

T_screen(i)
    = CPU 一次提交
    + Screen Rect Raster
    + Rect 内 Position Load
    + Analytic Sphere Reject
    + 球内 Lighting Fragment
```

两式都不是仅由灯数 `N` 决定。主要输入包括：

- 屏幕矩形面积；
- 球投影与矩形的面积比；
- G-Buffer 中实际有几何的像素比例；
- 相机是否在光体内；
- 是否穿过 Near Plane；
- 是否启用 Shadow、SSAO、Bloom；
- GPU 对 Clear、Stencil、Early-Z、动态分支和纹理带宽的具体实现。

因此 Adaptive Selector 的阈值必须来自 A/B，而不能凭经验写死一个“10% 屏幕面积”。

---

## 4. 两条路径必须共享的光照语义

### 4.1 有效半径来自同一公式

项目当前使用：

```cpp
ComputePointLightStencilVolumeRadius(
    constant,
    linear,
    quadratic,
    diffuse,
    cutoffScale,
    radiusScale)
```

令：

```text
c = constant
l = linear
q = quadratic
Lmax = max(diffuse.r, diffuse.g, diffuse.b)
s = cutoffScale
```

当前代码求解：

```text
q r² + l r + c = (256 / 5) · s · Lmax
```

在判别式有效且 `q` 非零时取正向根：

```text
r = (-l + sqrt(l² - 4q(c - (256 / 5)sLmax))) / (2q)
r_final = r · radiusScale
```

点光衰减函数数学上不会在有限距离严格变成零。Light Volume Radius 本身就是“贡献低于阈值后不再计算”的现有截断语义。Screen Path 不能再定义另一套半径，否则 A/B 同时改变算法范围与提交方式，无法归因。

### 4.2 半径必须只计算一次并进入分类结果

建议建立每帧临时描述：

```text
PointLightScreenProxy {
    lightIndex
    worldCenter
    viewCenter
    radius
    radiusSquared
    classification
    pixelRect
    coverageRatio
    selectedPath
}
```

Radius、Frustum、Screen Rect、Shader Uniform 和遥测都消费同一个 `radius`。不能在 CPU Bounds、Volume Scale 和 Fragment Reject 中各自复制公式。

### 4.3 Lighting Evaluation 必须共用

实现前 `lightVolumeFragment.glsl` 与 `lightVolumeFullscreenFragment.glsl` 是两份独立实现。正式 A/B 前已经抽出 `shaders/pointLightLighting.glsl`，Volume、Screen 与 Fullscreen Oracle 共用 G-Buffer validity、Diffuse、Specular、Attenuation、SSAO、Point Shadow、Bloom/HDR 输出和光照公式。

- 两份 Shader 可能逐步出现公式漂移；
- Fullscreen 版本目前没有与 Volume 版本完全一致的 SSAO 接口；
- G-Buffer 采样顺序不同会改变 Candidate 的真实成本；
- 修复一条路径时可能遗漏另一条路径。

共享实现的结构等价于：

```glsl
PointLightResult EvaluatePointLight(
    PointLightData light,
    vec3 fragPos,
    vec3 normal,
    vec3 albedo,
    vec4 material,
    float ao,
    vec3 viewPos);
```

两条路径只允许在“候选像素怎样生成”上不同：

```text
Volume Path：Stencil + Sphere Raster 生成候选像素
Screen Path：Screen Rect + distance² 判断生成候选像素
```

真正进入 Lighting Evaluation 后，光照、Shadow、SSAO 和 Bloom 语义相同；正式主时序仍按协议固定 Explicit gPosition、SSAO/Bloom/Point Shadow off，资源 smoke 另行覆盖这些开关的回归。

### 4.4 当前适用范围

第一版只覆盖现有 `LIGHT_VOLUME && !scene->UsesPbrMaterials()` 路径，即 Phong Deferred Point Lights。

以下内容不能顺带宣称已经支持：

- PBR Deferred 的 IBL / Emissive 合成；
- Spot Light；
- Forward Transparent Lighting；
- 多灯单 Draw 批处理；
- Tiled / Clustered Light List。

先在当前已存在的 Point Light Volume 路径上完成可归因 A/B，再讨论扩展。

---

## 5. 保守屏幕包围：怎样把三维光球映射到二维矩形

### 5.1 为什么不能只投影球心再加固定半径

透视投影下，偏离视轴的球体轮廓不是简单的屏幕圆。球越靠近屏幕边缘，透视畸变越明显；Near Plane 截断还会进一步改变投影边界。

Mara 与 McGuire 的工作专门讨论了“被 Near Plane 裁剪的透视投影球体”的二维保守包围，并将 Deferred Light Classification 列为直接应用。第一版如果不实现论文中的完整裁剪版本，也必须在 Near Plane 边界退化到安全路径。

### 5.2 普通情况的切线推导

假设 OpenGL View Space 中相机朝 `-Z`，将球心写为：

```text
Cview = (x, y, z)
d = -z > 0
radius = r
```

先考虑 X-Z 平面。屏幕横向投影比例为：

```text
u = x' / d'
```

从相机原点向二维圆作两条切线，可以得到横向极值：

```text
u± = (x·d ± r·sqrt(x² + d² - r²)) / (d² - r²)
x_ndc± = P00 · u±
```

纵向同理：

```text
v± = (y·d ± r·sqrt(y² + d² - r²)) / (d² - r²)
y_ndc± = P11 · v±
```

其中 `P00`、`P11` 来自对称透视投影矩阵。计算两组解后分别取最小值与最大值。

这套简化公式要求：

- 使用对称 Perspective Projection；
- 球体不包含相机；
- 球体没有穿过 Near Plane；
- 中间值全部有限；
- `d² - r²` 远离零。

只要任一条件不满足，就不能继续使用普通分支。

### 5.3 Near Plane 与 Camera-Inside 的处理

建议把投影结果设计成三态，而不是只返回 `bool`：

```text
Outside
    已证明球体不影响视口，可以剔除

ConservativeRect
    得到了可靠且非空的像素矩形

FullscreenFallback
    球体可能可见，但普通矩形公式不可靠
```

第一版保守规则：

```text
球体完全在任一 Frustum Plane 外：Outside
相机在球内：FullscreenFallback
球体与 Near Plane 相交：FullscreenFallback
普通切线计算非有限或退化：FullscreenFallback
其他情况：ConservativeRect
```

后续如果采用论文中的 Near Plane Clipped Sphere Bounds，可以将部分 `FullscreenFallback` 收紧成 `ConservativeRect`。这应作为独立优化，不与第一版同时混入。

### 5.4 NDC 到 OpenGL Scissor 的保守取整

对 NDC 矩形：

```text
[minX, maxX] × [minY, maxY]
```

先映射到像素坐标：

```text
px = (ndcX · 0.5 + 0.5) · width
py = (ndcY · 0.5 + 0.5) · height
```

再进行向外取整：

```text
x0 = floor(minPx) - guardPixels
y0 = floor(minPy) - guardPixels
x1 = ceil (maxPx) + guardPixels
y1 = ceil (maxPy) + guardPixels
```

最后 Clamp 到：

```text
0 ≤ x0 ≤ x1 ≤ width
0 ≤ y0 ≤ y1 ≤ height
```

第一版建议保留至少一个像素的 Guard Band，用于覆盖浮点误差、投影边界和 Rasterization Rule。矩形可以略大，不能略小。

`glScissor` 使用左下角原点，与 OpenGL NDC 的 Y 方向一致；实现时仍要确认项目截图和窗口坐标没有额外翻转。

### 5.5 不变量

Screen Rect 必须满足：

```text
所有可能受该点光影响的当前可见样本
    ⊆ Conservative Screen Rect
```

它允许包含额外像素，但不允许漏掉有效像素。

如果该不变量不能证明，正确行为是扩大到 Fullscreen 或回退 Volume，而不是继续使用一个“看起来差不多”的矩形。

### 5.6 为什么 AABB 八角点只能作为保守原型

将球体包在 View-Space AABB 中、裁剪 AABB 与 Near Plane、投影所有顶点，也可以得到明显保守的屏幕矩形。它实现简单，但可能比球体真实投影大很多。

它适合：

- 验证 Scissor 生命周期；
- 建立第一版 Bounds Telemetry；
- 作为解析公式失败时的中间保守方案。

它不适合未经数据就作为最终实现，因为过大的矩形会直接增加：

- Scissored Clear 面积；
- Analytic Path 的 Position Load；
- 球外 Fragment Reject 数量。

---

## 6. C1：Scissored Stencil Volume

### 6.1 这条路径改变什么

Scissored Volume 不改变现有 Stencil 和 Lighting 算法，只将当前灯的所有相关工作限制在保守屏幕矩形内：

```text
SetScissor(light.pixelRect)
→ Stencil Volume Draw
→ Lighting Volume Draw
→ Scissored Stencil Clear
→ RestoreScissorState
```

它仍然保留两次球体 Draw，因此是低风险、像素一致优先的阶段。

### 6.2 为什么局部 Clear 是安全的

阶段开始前，Stencil 为零。对第 `i` 盏灯，设它的保守矩形为 `R_i`。

执行时启用 Scissor：

```text
StencilDraw(i) 只能修改 R_i 内的像素
LightingDraw(i) 只消费当前灯的 Mask
ClearAfter(i)   将 R_i 恢复为 0
```

因为：

1. `R_i` 外在本灯开始前为零；
2. 本灯的 Stencil Draw 无法写出 `R_i`；
3. 本灯结束后 `R_i` 被清零；

所以整张 Stencil 在灯结束后重新为零。归纳到所有灯，阶段出口仍满足：

```text
∀ pixel，Stencil(pixel) = 0
```

前提是 Screen Rect 真正保守，并且 Clear 时：

- `GL_SCISSOR_TEST` 已启用；
- Scissor Rect 仍是当前灯的矩形；
- `StencilMask = 0xFF`；
- Clear 后恢复原有 Scissor 与 Stencil Mask。

### 6.3 工作量怎样变化

设视口面积为：

```text
A_view = width · height
```

Legacy Coalesced 的点光 Clear 理论覆盖量近似为：

```text
(N + 1) · A_view
```

Scissored Volume 变成：

```text
A_initial + Σ A_rect(i)
```

其中 `A_initial` 当前最多为一次全屏初始化，`A_rect(i)` 是第 `i` 盏灯的矩形面积。

注意：

- Clear 调用数量未必下降；
- GPU 时间不会按像素面积严格线性缩放；
- 驱动可能对 Full Clear 与 Scissored Clear 采用不同快路径；
- 大灯的 `A_rect` 接近全屏时几乎没有收益。

所以“清理面积减少”是机制证据，不是性能结论。

### 6.4 这条路径的价值边界

它最适合：

- 大量小范围灯；
- 屏幕矩形面积明显小于全屏；
- 当前 Clear 仍是显著成本；
- 要求与现有 Volume 路径逐像素一致。

Screen Bounds 也能证明部分灯完全离屏，但“缩小 Scissor”和“跳过 Draw”会改变不同的机制指标，正式实验应分开归因：先保持 Draw 数不变验证 Scissor，再单独打开 Offscreen Culling。

它不能解决：

- 两次球体 Draw；
- Lighting Shader 内部成本；
- 高重叠造成的 Fragment Invocations；
- 每灯独立 Shadow CubeMap 绑定；
- G-Buffer 重复读取。

因此 C1 是安全的空间约束，不应被描述成完整的多光源扩展算法。

---

## 7. C2：Analytic Screen Rect

### 7.1 基本思想

Screen Path 不再通过球体 Mesh + Stencil 生成像素 Mask，而是：

```text
保守屏幕矩形
    → 绘制一个矩形代理
    → 每个候选像素读取 G-Buffer Position
    → 判断该位置是否位于 Point Light 有效球内
    → 球内才执行完整 Lighting
```

数学判断为：

```text
delta = light.position - fragPos
distanceSquared = dot(delta, delta)

if distanceSquared > radiusSquared:
    reject
else:
    evaluate point lighting
```

这样每盏 Screen Light 理论上只需要一次 Draw，不写 Stencil，也不需要本灯 Stencil Clear。

### 7.2 Fragment Shader 的正确执行顺序

必须先执行最便宜、拒绝范围最大的判断：

```glsl
void main()
{
    vec2 uv = gl_FragCoord.xy / screenSize;

    vec3 fragPos;
    if (!LoadWorldPosition(uv, fragPos)) {
        discard;
    }

    vec3 toLight = pointLight.position - fragPos;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared > pointLight.radiusSquared) {
        discard;
    }

    // 只有通过球内判断后，才读取其余 G-Buffer。
    vec3 normal = LoadNormal(uv);
    vec3 albedo = LoadAlbedo(uv);
    vec4 material = LoadMaterial(uv);
    float ao = LoadAOIfEnabled(uv);

    PointLightResult result = EvaluatePointLight(...);
    WriteLightingResult(result);
}
```

实现已按上述顺序先读取/重建 Position、检查有效性和三维球内条件，再进入共享 Lighting Evaluation。这个顺序同时用于 Analytic Screen 与 Fullscreen Oracle。

这种 Shader 重排必须同时应用于对照所共享的 Lighting Evaluation，或者单独完成图像回归，避免把“代理路径变化”和“Shader 微优化”混入同一个 A/B。

### 7.3 为什么使用 `discard` 而不是写黑色

点光结果使用 Additive Blend。球外像素应当像“没有产生 Fragment”一样不修改任何目标。

如果写：

```glsl
FragColor = vec4(0, 0, 0, 1);
```

RGB 在 `ONE + ONE` 下虽然不变，Alpha 与其他 MRT 仍可能变化。`discard` 更接近当前 Volume Path 对球外像素“不产生 Lighting Fragment”的语义。

是否使用真正的 `discard`、分支后零输出或其他写法，仍需通过 GPU A/B 验证。GPU Gems 也强调动态分支只有在能够跳过足够多昂贵工作、且邻近像素分支具有局部一致性时才可能获益。

### 7.4 Draw 和 Clear 数量

设：

```text
N_v = 选择 Volume Path 的灯数
N_s = 选择 Screen Path 的灯数
N_c = 被保守剔除的灯数
N_active = N_v + N_s + N_c
```

混合路径的点光工作量为：

```text
Point-light Draw = 2N_v + N_s

Point-light Stencil Clear =
    0,                 if N_v = 0
    N_v + 1,           if N_v > 0 and current lifecycle is retained
```

如果后续正式建立“上游 Lighting Pass 已经发布 Clean Stencil”的入口契约，还可以讨论复用上游 Clear；这只节省一个固定 Clear，不属于本轮核心目标。

### 7.5 Shadow、SSAO、Bloom 与 Position Source

Screen Path 要成为真实替代路径，必须覆盖 Volume Path 的全部行为：

| 能力 | 统一 Volume | Analytic Screen / Fullscreen | 验证状态 |
|---|:---:|:---:|:---:|
| 显式 `gPosition` | 支持 | 支持 | 正式 A/B 固定并通过 |
| Depth Reconstruction | 支持 | 使用共用 Position Source | 未纳入本轮主时序，默认仍为 Explicit |
| Normal / Albedo / Material | 支持 | 支持 | 共用 Lighting 实现 |
| SSAO Ambient | 支持 | 支持 | 资源 smoke 无回归 |
| Point Shadow CubeMap | 支持 | 支持 | 资源 smoke 无回归 |
| Bloom BrightColor | 支持 | 支持 | 资源 smoke 无回归 |
| Radius Reject | 统一路径显式三维判断 | 显式三维判断 | `dot(P-L,P-L) <= R²` |

正式性能 A/B 可以先固定：

```text
Explicit gPosition
SSAO = off
Bloom = off
Point Shadow = off
```

但生产回归仍必须覆盖这些开关。不能因为压力测试关闭某项能力，就宣称新路径已经具备完整功能等价性。

### 7.6 代理球语义统一与逐像素结果

项目实际点光代理是 `models/sphere/sphere.obj`，顶点半径约为 1；`Global.h` 中半径 0.5 的内建球约定不用于这条路径。该 OBJ 的最小三角形面平面半径为 0.99043723，因此旧代理是理想单位球的内接离散近似。理论外包比例为 1.0096551；实现采用 1.02 倍代理，并在 Fragment 中用同一个 `radius²` 做精确三维球内判断，以覆盖光栅规则和数值边界。

旧 Coalesced Control 与理想球 Oracle 确实存在可量化的边缘差异；这被作为独立语义变量保留，没有混入 Screen 收益。例如 representative / 16 的旧 Control 对 Oracle：最大通道误差 39、平均误差 0.070459、不同像素 50,108（2.416474%）。

```text
旧 Control：由内接离散代理 Mesh 的投影/Stencil 决定
统一 Volume / Screen / Fullscreen Oracle：由同一个理想球体半径方程决定
```

正式验收区分了：

1. **C1 Scissored Volume**：应要求逐像素完全一致；
2. **语义统一**：旧 Scissored Volume 对 Analytic Volume 单独做图像与性能 A/B；
3. **C2 Analytic Screen**：对 Fullscreen per-light Oracle 要求逐像素一致；
4. **路径切换**：Analytic Volume 与 Analytic Screen 也要求逐像素一致。

五档 coverage 中，统一 Volume、Screen 与 Fullscreen Oracle 均达到 `max=mean=P95=0`、不同像素 0。Adaptive 的 No-Go 原因不是画质差异，而是固定路径实验没有出现稳定交叉点。

---

## 8. D：Adaptive Selector 怎样做决策

### 8.1 每盏灯的输入特征

第一版只使用容易解释、可稳定复现的特征：

```text
classification    = Outside / ConservativeRect / FullscreenFallback
coverageRatio     = rectArea / viewportArea
cameraInside      = distance(camera, center) <= radius
nearPlaneIntersect
shadowed
lastSelectedPath  = optional，只有实现 Hysteresis 时需要
```

不要第一版就引入复杂机器学习或在线 GPU 预测。当前任务需要的是可解释的渲染成本模型。

### 8.2 最简单的候选规则

```text
if light inactive:
    Skip

else if sphere is provably outside frustum:
    Culled

else if bounds are unreliable:
    SafeFallback

else if coverageRatio <= crossoverThreshold:
    AnalyticScreenRect

else:
    ScissoredStencilVolume
```

`SafeFallback` 第一版可选择：

- 已验证的 Full-Viewport Volume；或
- 完成行为对齐后的 Fullscreen Analytic。

不能选择“矩形计算失败就 Skip”。

### 8.3 阈值不是拍脑袋常量

应先对固定路径做独立 A/B：

```text
Mode V：全部可见灯使用 Scissored Volume
Mode S：全部可见灯使用 Analytic Screen Rect
```

再构造不同 Coverage Distribution：

- 多个小型局部灯；
- representative；
- high-overlap；
- 少量接近全屏的大灯；
- camera-inside / near-plane / offscreen。

只有数据出现稳定交叉关系时，才从结果拟合 `crossoverThreshold`。例如：

```text
小 coverage：Screen 明显更快
大 coverage：Volume 明显更快
```

若实验结果是：

```text
Screen 在全部代表场景都更快
```

则直接默认 Screen；不需要 Adaptive。

若结果是：

```text
Volume 在全部代表场景都更快
```

则保留 Volume；Screen 与 Adaptive 都 No-Go。

### 8.4 更完整但仍可解释的成本模型

若单阈值不足，可以拟合两个简单模型：

```text
EstimatedVolumeCost
    = a_v
    + b_v · rectArea
    + c_v · projectedSphereArea

EstimatedScreenCost
    = a_s
    + b_s · rectArea
```

最终选择：

```text
selectedPath = argmin(EstimatedVolumeCost, EstimatedScreenCost)
```

但模型参数必须来自固定场景训练，并在未用于拟合的场景验证。若模型只在当前 RTX 5060 Ti 和一个 Seed 上有效，就不能宣称它是通用引擎策略。

### 8.5 为什么可能需要 Hysteresis

当 `coverageRatio` 在阈值附近波动时，路径可能逐帧来回切换：

```text
Volume → Screen → Volume → Screen
```

即使两条路径理论输出相同，这也可能造成：

- GPU 时间抖动；
- Render State 切换抖动；
- 若边界不完全一致，则出现可见闪烁。

可使用双阈值：

```text
Volume → Screen：coverage < thresholdLow
Screen → Volume：coverage > thresholdHigh
thresholdLow < thresholdHigh
```

不过这要求每盏灯具有稳定身份和 `lastSelectedPath`。如果 Point Light 存在 Vector 增删、排序或对象复用，状态不能仅按当前数组下标绑定。第一版静态 Benchmark 不需要立刻实现 Hysteresis；生产默认启用 Adaptive 前必须解决这一生命周期问题。

### 8.6 每帧伪代码

```text
BuildPointLightScreenProxies(scene, camera):
    proxies.clear()

    for each active pointLight:
        radius = ComputePointLightStencilVolumeRadius(...)

        if radius is not finite or projection is unsupported:
            proxies.push(VolumeFallback)
            continue

        classification, rect = ProjectSphereConservatively(
            pointLight.position,
            radius,
            view,
            projection,
            viewport)

        if classification == Outside:
            countCulled++
            continue

        coverage = rect.area / viewport.area

        if classification == FullscreenFallback:
            path = SafeFallback
        else:
            path = SelectPathFromMeasuredCost(coverage, otherFeatures)

        proxies.push({light, radius, rect, path})


RenderPointLights(proxies):
    if any proxy selects Volume:
        EstablishCleanStencil()

    for proxy in original deterministic light order:
        if proxy.path == Volume:
            RenderScissoredStencilVolume(proxy)
        else:
            RenderAnalyticScreenRect(proxy)

    RestorePointLightPhaseState()
```

为了保持 Additive Lighting 的确定性和便于图像对比，第一版建议保留原灯序，不为了减少状态切换而重新排列灯。浮点加法不满足严格结合律，重排大量灯可能改变低位结果。

---

## 9. GL 状态与生命周期不变量

### 9.1 为什么这部分和算法本身同样重要

当前点光路径同时修改：

- Blend；
- Color Mask；
- Depth Mask / Depth Func；
- Stencil Test / Func / Op / Mask；
- Cull Face；
- Scissor；
- Active Texture 与 CubeMap Binding；
- Shader Program 与 VAO。

Adaptive Path 在同一循环内交替执行两套状态。如果只关注投影数学，不明确状态入口和出口，很容易出现“单独运行两种模式都正确，混合运行错误”的问题。

### 9.2 建议的状态矩阵

| 状态 | Scissored Volume Stencil | Scissored Volume Lighting | Analytic Screen |
|---|---|---|---|
| Blend | Off | `ONE, ONE` | `ONE, ONE` |
| Color Mask | False | True | True |
| Depth Write | Off | Off | Off |
| Depth Test | `LESS` | `GEQUAL` | Off 或经独立证明的约束 |
| Stencil Test | On | `NOTEQUAL 0` | Off |
| Cull | Off | Front | Off |
| Scissor | Light Rect | Light Rect | Light Rect / Fullscreen |
| Geometry | Sphere | Sphere | Quad/Rect |

Screen Path 不应继承上一条 Volume Path 的 `StencilFunc(NOTEQUAL)` 或 `CullFace(FRONT)`。Volume Path 也不能继承 Screen Path 的 Depth Disabled。

### 9.3 阶段入口与出口

建议定义明确契约：

```text
Entry：
    Output FBO 已绑定
    Depth/Stencil 已由 G-Buffer 拷贝
    Stencil 内容已知或会在首个 Volume 前建立为 Clean

Exit：
    Blend disabled
    ColorMask = true
    DepthMask = true/由后续阶段期望值
    DepthFunc = LESS
    StencilMask 恢复
    Stencil 对所有 Point-Light-owned 像素为 0
    CullFace = BACK 或 disabled，符合后续 Forward Extras
    Scissor 恢复进入阶段前状态
```

项目已有 `POINT_LIGHT_STENCIL_LIFECYCLE_CHECK` readback，可继续作为非计时正确性证据。

### 9.4 Scissor 必须像资源一样成对管理

不能假设阶段入口一定关闭 Scissor。正确做法是：

```text
Capture previous scissor enabled state and box
→ Set light rect
→ Draw / Clear
→ Restore previous box and enabled state
```

若项目 `GLState` Cache 尚未完整跟踪 Scissor，需要先补最小封装或在阶段内建立唯一所有权。不要让 Raw `glEnable(GL_SCISSOR_TEST)` 与 Cache 记录失配。

### 9.5 Shadowed Point Light

当前每盏 Shadowed Point Light 绑定自己的 CubeMap。Analytic Screen Rect 仍然逐灯 Draw，因此可以保留这套语义。

第一版不能把所有 Screen Lights 直接 Instancing 成一个 Draw，因为不同灯可能拥有不同：

- Shadow CubeMap；
- Shadow Enable；
- Far Plane；
- Light 参数。

无阴影灯批处理可以作为后续独立实验，不能混入本轮路径 A/B。

### 9.6 Resize 与投影变化

Screen Rect 依赖：

- View Matrix；
- Projection Matrix；
- Width / Height；
- Near Plane；
- Light Position / Radius。

因此不能跨帧无条件缓存像素矩形。窗口 Resize、FOV、Camera 或 Light 变化后必须重算。第一版每帧 `O(N_active)` 重算最容易保证正确，后续只有 CPU 数据证明它成为瓶颈时才讨论缓存。

---

## 10. 分阶段实现计划

### 10.1 Phase 0：冻结现有 Coalesced 基线

目标：确保后续所有候选都以已经通过正式 A/B 的 Coalesced 路径为对照，而不是回到 Legacy `2N`。

保留模式：

```text
legacy-2n                 历史诊断对照
coalesced-n-plus-one      当前生产对照
```

不修改现有正式结果目录。

### 10.2 Phase 1：只计算 Bounds，不改变渲染

目标：先验证分类数学和 CPU 成本。

新增遥测但仍全部走当前 Volume Path：

- outside count；
- conservative rect count；
- fullscreen fallback count；
- rect area sum；
- coverage P50 / P95 / Max；
- near-plane / camera-inside count；
- bounds CPU Median / P95 / P99。

这一阶段不能声称 GPU 优化。

### 10.3 Phase 2：Scissored Volume

只改变：

```text
Volume Draw 与 Clear 的 Scissor 范围
```

保持：

- Light 顺序；
- Shader；
- Sphere Mesh；
- Uniform；
- Draw 数；
- Lighting 公式；
- Shadow / SSAO / Bloom 开关。

这是最适合要求逐像素一致的阶段。

Phase 2 的首轮只设置 Scissor，不跳过完全离屏灯：离屏灯使用空 Scissor，仍保留原 Draw 计数，以便只验证 Clear / Raster 影响区域这一变量。

### 10.4 Phase 2B：Offscreen Culling

在 Scissored Volume 已经通过后，再单独令 `Outside` 灯不提交：

```text
A = Scissored Volume，Outside 使用空 Scissor 但仍提交
B = Scissored Volume，Outside 完全跳过提交
```

这一轮预期 Draw 和 submitted 改变，但 LightInput Signature 必须相同，并满足：

```text
active = submitted + culled
```

若代表场景没有离屏灯，这一轮没有性能收益是正常结果，不能人为修改灯分布后再外推到一般场景。

### 10.5 Phase 3：显式 Analytic Screen 模式

先实现全局显式模式：

```text
--point-light-render-path volume-scissored
--point-light-render-path screen-analytic
```

不要一开始就做逐灯 Adaptive。先证明两条固定路径：

- 各自正确；
- 各自性能稳定；
- 在不同 Coverage Distribution 下是否存在交叉点。

### 10.6 Phase 4：Adaptive Selector

只有 Phase 3 数据满足以下条件才进入：

```text
存在可解释、可复现的路径交叉点
AND
两条路径切换时画质稳定
AND
Selector CPU 成本没有吃掉 GPU 收益
```

否则停止在固定赢家，不实现 Adaptive。

### 10.7 Phase 5：默认值与回归

只有正式 A/B 和正确性门槛通过后：

- 切换默认路径；
- 保留显式 Control；
- 跑 Forward / Deferred、PBR fallback、SSAO、Bloom、Resize、Resource release smoke；
- 更新 README、架构图和简历候选表述。

---

## 11. 遥测与可观测性

### 11.1 必须新增的 CPU/GPU Zone

建议阶段级而不是逐灯插入 Timer Query，避免测量本身放大开销：

```text
CPU Point Light Bounds
CPU Deferred Point Lights

GPU Deferred Point Lights
GPU Point Light Stencil/Clear
GPU Point Light Volume Lighting
GPU Point Light Screen Lighting
```

逐灯事件使用 RenderDoc Marker，不在正式 600 帧计时中默认启用字符串格式化。

### 11.2 必须记录的机制计数

```text
totalPointLights
activePointLights
culledPointLights
submittedPointLights
volumePathLights
screenPathLights
fullscreenFallbackLights
nearPlaneFallbackLights
cameraInsideLights

pointLightDrawCalls
stencilVolumeDraws
lightingVolumeDraws
screenRectDraws
pointLightStencilClears

screenRectPixelAreaSum
screenRectCoverageMedian/P95/Max
estimatedStencilClearPixelArea
```

基本不变量：

```text
active = culled + volume + screen
submitted = volume + screen
pointLightDraw = 2·volume + screen
```

若存在 Fullscreen Fallback，它仍必须归属于 Volume 或 Screen 中的一条实际提交路径，不能重复计数。

### 11.3 为什么 Submission Signature 需要扩展

旧 Clear A/B 要求 Draw 和 submitted 完全相同，因为它只改变 Clear 生命周期。

新优化会主动改变：

- submitted；
- culled；
- Draw Call；
- Path Classification。

因此不能继续把“Draw 完全相同”当成正确性条件。应拆成：

```text
LightInputSignature
    证明两种模式输入的灯、参数、顺序相同

DecisionSignature
    证明同一模式、同一输入的分类结果可重复

RenderedPathCounters
    证明 Draw/Clear 改变量符合理论公式
```

图像和状态不变量负责证明“少提交没有漏光”，而不是要求提交数量不变。

---

## 12. 正式实验设计

### 12.1 A/B 必须逐层分离变量

第一轮只验证 Scissor：

```text
A = Coalesced Volume
B = Scissored Coalesced Volume
```

第二轮只验证 Offscreen Culling：

```text
A = Scissored Volume，Outside 仍提交
B = Scissored Volume，Outside 跳过提交
```

第三轮只比较代理路径；两边使用相同的 Bounds 与 Culling 结果：

```text
A = Scissored Volume + Common Culling
B = Analytic Screen Rect + Common Culling
```

Adaptive 是第四轮：

```text
A = 当前场景中更快的固定路径
B = Adaptive Selector
```

不能一次把 Clear 合并、Scissor、Analytic Shader 和 Adaptive 全部打开，然后把总收益归到某一个方法上。

### 12.2 固定实验协议

- Release x64；
- 1920×1080；
- VSync 请求 0；
- 固定 Sponza、相机与 Seed；
- Explicit `gPosition`；
- 300 帧预热；
- 每进程 600 帧正式采样；
- 每配置至少 3 个独立进程；
- 同一可执行文件内显式选择路径；
- 启动顺序平衡交错；
- 保存 EXE SHA-256、参数、日志、JSON 和截图。

### 12.3 场景矩阵

| Coverage | 灯数 | 目的 |
|---|---:|---|
| representative | 16 / 256 / 512 | 延续现有正式压力口径 |
| high-overlap | 16 / 64，必要时扩展 | 验证大覆盖和 Fragment 压力 |
| small-local | 64 / 256 / 512 | 验证小矩形是否适合 Screen Path |
| edge-cases | 16 | Near Plane、Camera Inside、Offscreen |
| zero-light | 0 | 状态和 Clear 空路径 |

`small-local` 若新增，必须使用固定生成器版本、Seed、相机和半径分布，并记录 Scene / LightInput Signature。

### 12.4 性能指标

主指标：

- GPU Deferred Point Lights Median / P95 / P99；
- GPU Frame Median / P95 / P99；
- CPU Deferred Point Lights Median / P95 / P99；
- CPU Point Light Bounds Median / P95 / P99。

机制指标：

- Point Light Draw；
- Clear Count；
- Estimated Clear Area；
- Lighting PS Invocations；
- Stencil Samples / Primitives；
- Culled / Volume / Screen / Fallback Count；
- Coverage Distribution。

控制指标：

- LightInput Signature；
- 固定场景最终图像；
- GL Error；
- Stencil Exit Nonzero Pixels；
- Resource Count；
- Forward / PBR fallback smoke。

### 12.5 正确性门槛

#### Scissored Volume

- 固定截图逐像素完全一致；
- `maxError = 0`；
- Scene / LightInput Signature 一致；
- Draw 数与 Coalesced Volume 相同；
- 首轮只验证 Scissor，`submitted` 与 Control 相同；
- Stencil 阶段出口非零像素为 0；
- 0 GL Error。

#### Offscreen Culling

- 固定截图逐像素完全一致；
- LightInput Signature 与不剔除路径一致；
- `active = submitted + culled`；
- Draw 的减少量与被剔除灯的路径理论 Draw 数一致；
- Edge fixture 中 Offscreen 灯必须被剔除，Near Plane 与 Camera Inside 灯不能误剔除；
- 0 GL Error。

#### Analytic Screen

- 预先定义全图与边界带质量门槛，不能看完结果再改；
- 球内主体区域不得出现候选漏光；
- Near Plane / Camera Inside / Offscreen fixture 全部通过；
- Shadow / SSAO / Bloom 各至少一组功能回归；
- 相机运动序列不能出现路径边界闪烁；
- 0 GL Error。

#### Adaptive

- 同一输入下 Decision Signature 可重复；
- 路径计数满足理论公式；
- 性能应接近不同 Coverage 场景中固定路径的较优者；
- Selector CPU 成本不得抵消主要 GPU 收益；
- 若不存在稳定交叉点，直接 No-Go。

### 12.6 性能通过门槛必须在正式运行前冻结

本轮在正式脚本执行前冻结并实际使用了以下门槛：

```text
fixedPathMinimumRelativeImprovementPercent = 3%
requiredProcessDirection = 3/3；差异 <3% 或方向不一致则扩展到 5 个进程
adaptiveMinimumAbsoluteImprovementMs = 0.10 ms
adaptiveMinimumRelativeImprovementPercent = 3%
adaptiveRequiredProcessDirection = 3/3
qualityThreshold = 对 Fullscreen Oracle 逐像素完全一致
```

阈值必须结合当前 Point Light GPU 基线和测量噪声设置，写入后不能为了让结果 Go 而回改。

---

## 13. RenderDoc 要回答什么

### 13.1 Scissored Volume

确认：

- Clear 事件仍为预期数量；
- Clear 时 Scissor Rect 正确；
- Stencil / Lighting 两次球体 Draw 使用相同 Light Rect；
- Scissor-only A/B 中完全离屏灯仍保留 Draw，但其 Scissor Area 为 0；
- Stencil Exit 为 Clean；
- Scissor 没有泄漏到后续 Forward Extras。

Offscreen Culling 的独立捕获再确认 `Outside` 灯没有 Draw，并且 Draw 减少量与遥测一致。

### 13.2 Analytic Screen

确认：

- 每盏 Screen Light 只有一次 Draw；
- 无 Point-Light Stencil Draw / Clear；
- Shader 先采 Position，再做 Radius Reject，再读取其他 G-Buffer；
- Lighting PS Invocations 与 Rect Area / Coverage 变化一致；
- Shadowed Light 正确绑定自己的 CubeMap；
- Blend、MRT 与最终目标一致。

### 13.3 Adaptive

确认事件树中的：

```text
PointLightPhase
    → Culled count marker/telemetry
    → Volume lights
        → StencilDraw
        → LightingDraw
        → LocalClear
    → Screen lights
        → AnalyticDraw
```

正式 GPU 收益仍以应用内 Timer Query 的多进程数据为主。RenderDoc Duration 用于解释机制，不把一次 Replay 当成稳定性能结论。

---

## 14. 主要失败模式与保守降级

### 14.1 Screen Rect 偏小

现象：屏幕边缘或光球边界漏光。

处理：

- 向外取整；
- Guard Band；
- Near Plane 走 Fullscreen / Volume Fallback；
- 使用覆盖 Mask Debug；
- 任何非有限值禁止剔除。

### 14.2 Bounds 正确但太松

现象：画面正确，但 Screen Path 或 Scissored Clear 没有收益。

处理：

- 对比 `rectArea / actual shaded pixels`；
- 从 AABB 原型升级到解析切线或论文的 clipped sphere bounds；
- 作为独立 A/B，不与 Shader 优化混合。

### 14.3 Screen Shader 与 Volume Shader 漂移

现象：主体区域也出现明显色差。

处理：

- 共享 Lighting Evaluation；
- 对所有开关建立行为矩阵；
- Shader 热重载同时编译两条路径；
- 禁止复制后分别维护公式。

### 14.4 路径切换闪烁

现象：相机移动时光球边缘亮度跳变。

处理：

- 先定位是否来自 Mesh Sphere 与 Analytic Sphere 边界差异；
- 加 Transition Motion Test；
- 需要时加入 Hysteresis；
- 若仍不可接受，取消逐灯 Adaptive，只保留固定模式。

### 14.5 Selector CPU 成本过高

现象：GPU 降低，但 CPU Frame 或 P99 上升。

处理：

- Bounds 保持无分配、线性写入；
- 不做逐灯 Timer Query；
- 避免每帧字符串；
- 仅在 CPU 数据证明需要时缓存；
- 最终按 CPU/GPU 双侧数据决定 Go/No-Go。

### 14.6 Scissor 状态泄漏

现象：天空盒、透明物体、Outline 或后处理被裁掉。

处理：

- 阶段入口捕获、出口恢复；
- 添加 Forward Extras 截图回归；
- GLState Cache 与 Raw API 不混用失配。

### 14.7 GPU/驱动相关交叉点

现象：当前 NVIDIA GPU 上 Screen 更快，另一设备上 Volume 更快。

处理：

- 不把单机阈值描述成通用常量；
- 将阈值保留为配置；
- 简历只报告测试平台与场景；
- 若没有第二设备，不外推跨 GPU 结论。

### 14.8 光照半径异常

现象：负半径、NaN、Inf 或极大范围。

处理：

- 本轮不悄悄改变现有 Radius 语义；
- 异常值走已验证 Volume / Fullscreen Fallback；
- 单独记录 fallback 原因；
- 若要修正 Radius 公式，作为独立正确性变更。

---

## 15. 为什么暂时不选择其他方案

### 15.1 整个点光阶段只清一次 Stencil

错误。不同灯共用同一 Stencil Attachment，若不在灯之间恢复零，后一盏灯会继承前一盏灯的 Mask。当前 `N+1` 已是维持逐灯独立性的安全合并；进一步减少必须依赖局部 Clear、独立 Stencil 表示或完全取消 Stencil Path。

### 15.2 只做 Frustum Culling

有价值，但只解决完全离屏灯。当前 representative 正式场景 `culled=0`，所以不能把 Frustum Culling 预设成主要收益。它应作为 Screen Bounds 的自然副产品，而不是单独包装成旗舰优化。

### 15.3 只加 Scissor，不做路径比较

Scissor 能限制 Clear 范围；同一套 Bounds 还可以在独立阶段剔除完全离屏灯。但现有 Sphere Volume 已经约束了大量 Lighting Fragment，所以它更像安全的中间阶段，不能回答“两次 Draw 和 Stencil 是否仍值得”的核心问题。

### 15.4 直接 Instancing 所有点光源

当前每盏灯可能绑定不同 Shadow CubeMap，且 Stencil Mask 必须逐灯隔离。直接把全部 Volume 合成一个 Instanced Draw 会让不同灯的 Stencil 结果混在一起。只有 Analytic、无阴影且数据布局统一的灯，才可能进一步做 Instanced Screen Rect；这属于后续 CPU Submission 优化。

### 15.5 直接做 Tiled / Clustered Deferred

Tiled / Clustered 从根本上改变 Lighting Backend：构建 Tile/Cluster Light List，并让每个像素只遍历局部灯。它对更多灯更具扩展性，但当前 OpenGL 3.3 项目需要额外处理 Buffer Texture、CPU/GPU Light List、容量、上传、Shadowed Light 和调试。

在当前目标下，应先用 Screen Bounds 和两条固定路径证明剩余成本。只有逐灯路径仍无法满足规模目标，才进入 Tiled 设计；否则会把“优化现有路径”扩大成“新写一个 Lighting Backend”。

### 15.6 Light Volume Mesh LOD

降低球体网格面数可以减少 Stencil / Lighting Draw 的 Vertex Work，但不会减少 Draw、Clear 和主要 Lighting Fragment。只有新的 Coalesced RenderDoc 证明 Vertex/Primitive 成本成为主要项时才值得做。

---

## 16. 理论收益与不能提前声称的内容

### 16.1 可以提前证明的机制变化

若所有 `N` 盏灯都走 Screen Path：

```text
Point-light Draw：2N → N
Point-light Stencil Draw：N → 0
Point-light Stencil Clear：N+1 → 0
```

若使用混合路径：

```text
Point-light Draw：2N_v + N_s
Point-light Clear：N_v + 1（N_v > 0）
Culled Light：N_c 不提交
```

这些是工作量公式，不是 GPU 时间公式。

### 16.2 不能提前声称

在正式 A/B 前不能写：

- “GPU 提升 XX%”；
- “Screen Path 一定优于 Stencil Volume”；
- “自适应阈值为某个固定百分比”；
- “画质完全一致”；
- “支持 PBR / Transparent / Spot”；
- “适用于所有 GPU”；
- “已经实现 Tiled / Clustered”。

### 16.3 最可能出现的三种实验结论

#### 结论一：存在稳定交叉点

```text
小覆盖 Screen 更快
大覆盖 Volume 更快
Adaptive 接近各场景较优固定路径
```

此时 Adaptive Go，形成完整的多路径成本选择案例。

#### 结论二：Screen 全面占优

此时默认 Screen，Adaptive No-Go。简历可以写“以解析屏幕代理替代逐灯 Stencil Volume”，但不能再强调自适应。

#### 结论三：Volume 全面占优或 Screen 画质不过关

此时保留 Scissored Volume 或当前 Coalesced Volume，Screen / Adaptive No-Go。实验仍可以作为方案比较证据，但不能包装成成功优化。

---

## 17. 这项工作怎样形成技术故事

如果最终通过，完整脉络应是：

```text
1. 通过 Timer Query 与 RenderDoc 发现多点光阶段随灯数近线性增长。

2. 将 GPU 成本拆成：
   full-target clear、stencil volume raster、lighting fragment。

3. 先删除相邻重复 Clear：
   2N → N+1，保持 Draw 与图像不变。

4. 发现剩余成本仍包含逐灯两次 Draw 与覆盖相关 Fragment Work。

5. 从点光有效范围是球体这一事实出发，计算保守屏幕投影：
   Outside / Rect / FullscreenFallback。

6. 建立两条显式候选：
   Scissored Volume 与 Analytic Screen Rect。

7. 用 Coverage Distribution 找交叉点，而不是拍脑袋选路径。

8. 只有存在稳定交叉点且切换无画质问题时，才启用 Adaptive。

9. 用相同输入签名、阶段计时、RenderDoc 事件树、图像和 Stencil
   生命周期共同证明收益没有来自漏画或状态泄漏。
```

这比“减少了几个 API 调用”更完整，因为它覆盖：

- 场景与指标；
- Profile 定位；
- 根因拆分；
- 空间数学；
- 方案比较；
- 状态生命周期；
- 质量边界；
- 性能交叉点；
- 保守降级。

---

## 18. 当前数据允许的简历事实

本轮数据只允许陈述固定 Screen 路径与门控实验，不能写成“已实现 Adaptive”：

```text
多点光 Deferred 优化：构建保守投影球 Screen Rect 与共享三维球内光照判定，
以每灯一次 Analytic Screen Draw 替代两次 Stencil Volume Draw；在 1080p
固定 Sponza 的 256 灯四档 coverage 中，点光 GPU Median 相对统一
Analytic Volume 降低 51.45%～76.21%，五个独立进程全部同向，且对
Fullscreen per-light Oracle 的逐像素误差为 0。
```

最终措辞必须根据真实结果选择：

- 可以写固定 Analytic Screen、保守 Bounds、共享三维 predicate、逐像素 Oracle 和正式五进程 A/B；
- Scissor 可单独写 256/512 灯约 8.1% 的阶段性机制收益；
- 不写 Adaptive Selector、Tiled/Clustered、Instancing 或跨 GPU 普适结论。

---

## 19. 面试中如何讲五分钟

### 第 0–1 分钟：问题证据

说明 512 灯 Legacy 中 Clear、Stencil Draw、Lighting Draw 的 RenderDoc 拆分，以及 Coalesced 已将 `2N` Clear 降为 `N+1`。

### 第 1–2 分钟：为什么继续优化

说明 Clear 合并没有减少两次球体 Draw 和 Lighting Fragment；high-overlap 的 PS Invocations 证明覆盖率是另一条独立成本轴。

### 第 2–3 分钟：两条路径原理

说明保守投影球矩形、Scissored Volume 与 Analytic `distance² <= radius²`。

### 第 3–4 分钟：为什么需要实验决定

小灯可能适合 Screen，大灯可能适合 Volume；Near Plane 和 Camera Inside 必须 Fullscreen / Volume Fallback。若没有稳定交叉点，就不做 Adaptive。

### 第 4–5 分钟：正确性与结果

说明 LightInput Signature、Path Counters、Stencil Exit、图像边界带、RenderDoc 事件树和多进程 A/B。只有此时才给出真实性能数字。

---

## 20. 关键代码索引

- 当前点光阶段入口：`DeferRenderPass.cpp::DrawPointLightVolumesDeferred`
- 当前 Lighting Path 选择：`DeferRenderPass.cpp::Render`
- 当前 Radius 公式：`Global.h::ComputePointLightStencilVolumeRadius`
- 当前 Volume Shader：`shaders/lightVolumeFragment.glsl`
- 当前 Screen / Fullscreen Shader：`shaders/lightVolumeFullscreenFragment.glsl`
- 共用点光语义：`shaders/pointLightLighting.glsl`
- Position Source 共用逻辑：`shaders/positionReconstruction.glsl`
- 压力场景与 Edge Fixtures：`PointLightStressBenchmark.h`
- Stencil Lifecycle Readback：`DeferRenderPass.cpp`
- Point Light 正式结果输出：`test.cpp`
- 现有 Clear A/B 脚本：`tools/run_point_light_stencil_clear_ab.ps1`
- Screen Routing 正式脚本：`tools/run_point_light_screen_routing.ps1`
- 正式聚合器：`tools/analyze_point_light_screen_routing.py`
- 最终 RenderDoc 捕获：`tools/capture_point_light_screen_renderdoc.ps1`

实现已将 Render Mode 与 Clear Mode 分成两个正交枚举：

```text
PointLightRenderPathProperty {
    CoalescedVolume,
    BoundsVolume,
    ScissoredVolume,
    AnalyticVolumeFull,
    AnalyticVolume,
    AnalyticScreen,
    AnalyticFullscreen
}
```

Clear Mode 与 Render Path 是两个正交维度。实现阶段可以为了控制组合数量规定：

```text
CoalescedVolume / ScissoredVolume 固定使用安全的 Coalesced Lifecycle
AnalyticScreen 不使用 Point-Light Stencil Clear
Adaptive 根据实际 Volume Count 建立 Stencil Lifecycle
```

不要让一个枚举同时承担“怎样清 Stencil”和“怎样生成 Lighting Candidate Pixels”两种语义。

---

## 21. 最终技术判断

当前可以确定的事实是：

1. Legacy 的相邻 Stencil Clear 存在可删除的重复工作；
2. Coalesced `N+1` 已通过正式 A/B，并在 256 / 512 灯下降低约 20% 点光 GPU Median；
3. 保守 Bounds 已实现；Near Plane、Camera Inside、无效投影退化为 Fullscreen，只有可证明 Outside 才允许显式剔除；
4. Scissored Volume 保持 `2N` Draw，在 representative / 256、512 分别改善 8.13%、8.09%，五进程全部同向，因此判 Go；
5. Analytic Screen 每灯一次 Quad Draw、0 次本灯 Stencil Draw/Clear；四档 256 灯 coverage 相对统一 Analytic Volume 改善 51.45%～76.21%，五进程全部同向且逐像素等同 Oracle，因此固定 Screen 判 Go；
6. high-overlap 也由 Screen 稳定胜出，没有 Volume 反转，所以 Adaptive 没有成立所需的交叉点，Not-Implemented / No-Go；
7. 正常 Deferred 默认使用 Analytic Screen；旧 Coalesced、Scissored、Analytic Volume 与 Legacy2N 均可显式复现。

本轮已经执行并完成以下可证伪链：

```text
建立保守 Bounds
→ 验证像素一致的 Scissored Volume
→ 隔离代理语义统一
→ 对 Fullscreen Oracle 验证 Analytic Volume / Screen
→ 用四档 coverage 的固定路径 A/B 检验交叉点
→ 无交叉点，固定 Screen，停止 Adaptive
```

这项方案真正有价值的部分，不是路径数量，而是建立了一个可证伪的决策链：

> 只有当空间包围正确、两条路径行为等价或质量受控、性能存在可重复交叉点，并且选择器成本低于收益时，自适应才是优化；否则它只是额外复杂度。
