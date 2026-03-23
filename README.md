# OpenGL Learn

基于 OpenGL 3.3 的 3D 渲染学习项目，支持前向渲染与延迟渲染，包含完整的编辑器界面和常用光照、后处理功能。

## 功能特性

### 渲染管线
- **前向渲染 / 延迟渲染**：可切换，延迟渲染写入 GBuffer（位置、法线、漫反射、材质）
- **光体积优化**：点光源采用模板缓冲 + 球体几何体，仅对光体积内像素计算光照
- **多 Pass 架构**：Geometry Pass → Lighting Pass → Forward（天空盒、透明、轮廓线）

### 光照
- 平行光、点光源、聚光灯
- PCF / PCSS 软阴影
- 点光源立方体贴图阴影

### 后处理
- HDR
- 伽马校正
- 泛光（Bloom）
- 抗锯齿（Default / MSAA）
- AO（Ambient Occlusion）
  - 当前实现思路：在正向渲染的主 FBO 里通过 MRT 输出用于后处理的场景信息（depth + normal）。
  - `ForwardRenderPass` 的 `attachment2 (Color 2)` 作为 normal buffer，供后续 SSAO/GTAO 等屏幕空间 AO 采样使用。
  - 透明物体在需要后处理时也会参与 normal 输出，以保证同屏法线一致性。

### 编辑器界面（ImGui + Docking）
- 场景面板：光源、模型、材质管理
- 设置面板：渲染模式、光照、后处理参数
- 视口：多 FBO 调试、宽高比选择
- 资源浏览器：Models / Materials / Shaders
- 材质 XML 编辑器

### 模型与材质
- Assimp 加载 OBJ 模型
- XML 材质系统，支持热重载
- Phong / Mirror / Grass / Explode 等着色器
- 材质面板支持按 Mesh 实时切换贴图（系统文件浏览器）

### 场景存档与加载优化
- Scene JSON 按模型来源区分：`file`（文件模型）/`generated`（程序生成模型）
- 持久化并恢复模型状态：transform、active、shader、outline、材质参数
- 持久化并恢复灯光参数：Point/Direction/Spot（含阴影开关与衰减参数）
- 分帧异步恢复文件模型，降低启动卡顿峰值
- 启动与 UI 显示模型加载进度（Settings + 窗口标题）
- 纹理缓存与模型网格缓存，减少重复加载带来的内存占用

## 项目结构

```
OpenGL_Learn/
├── OpenGL_Learn/           # 主工程
│   ├── shaders/            # GLSL 着色器
│   ├── models/             # 模型资源
│   ├── materials/          # 材质 XML
│   ├── DeferRenderPass.*   # 延迟渲染
│   ├── ForwardRenderPass.* # 前向渲染
│   ├── PostprocessRenderPass.*
│   ├── Scene.*             # 场景管理
│   ├── Model.* / Light.*   # 模型与光源
│   ├── mygui.h             # ImGui 界面
│   └── ...
├── includes/               # 第三方头文件
├── libs/                   # 第三方库
└── OpenGL_Learn.sln
```

## 依赖

- **OpenGL 3.3**：GLAD 加载
- **GLFW**：窗口与输入
- **GLM**：数学库
- **Assimp**：模型加载
- **ImGui (Docking)**：编辑器 UI
- **stb_image**：纹理加载

## 构建

### 环境要求
- Windows 10+
- Visual Studio 2022（v143 工具集）
- C++17

### 配置
1. 将 `glm`、`assimp` 头文件放入 `includes/`
2. 将 `glad.c`、`glfw3.lib`、`assimp-vc143-mtd.lib` 等库放入 `libs/`
3. 确认 vcxproj 中 `AdditionalIncludeDirectories`、`AdditionalLibraryDirectories` 指向正确路径
4. ImGui 路径：项目引用 `..\..\imgui-docking\`，请根据本地路径调整

### 编译
```bash
# 使用 MSBuild
msbuild OpenGL_Learn.sln /p:Configuration=Release /p:Platform=x64
```

或在 Visual Studio 中打开 `OpenGL_Learn.sln` 直接构建。

### 运行
编译完成后，从 `OpenGL_Learn/x64/Release/` 或 `Debug/` 运行 `OpenGL_Learn.exe`。程序需在可执行文件所在目录或项目根目录找到 `shaders/`、`models/`、`materials/` 等资源路径。

## 操作说明

- **WASD**：摄像机移动
- **鼠标**：视角控制（按 M 切换鼠标锁定）
- **ESC**：退出

## 许可证

学习项目，仅供个人使用。
