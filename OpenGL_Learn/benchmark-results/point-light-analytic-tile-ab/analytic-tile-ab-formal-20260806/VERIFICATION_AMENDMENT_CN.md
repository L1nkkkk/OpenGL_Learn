# 独立复核采集后修正说明

记录时间：正式 60 进程采集与主分析完成后、最终独立复核完成前。

## 不变的证据

- 不修改 `PHASE0_FROZEN_PROTOCOL_CN.md`、`pre-capture-manifest.json`、`capture-manifest.json`。
- 不修改冻结的 Release 二进制、60 个原始 JSON、60 张 PPM、60 份日志。
- 不修改采集前冻结的 `verify_analytic_tile_ab.py`；其 SHA-256 仍由 pre-capture manifest 约束。

## 原复核器为什么停止

原复核器在完成原始文件哈希、运行语义、配对 Wall Median、30 组图像误差与 15 组运动相机轨迹检查后，执行一项附加 Shader 源码顺序检查：

```text
grid_shader.find("discard") < grid_shader.find("gridMetadata")
```

该表达式错误地命中了 Shader 顶部的 `uniform isamplerBuffer gridMetadata;` 声明，所以得到 False。真正需要验证的契约是：无效 G-Buffer 像素的 `discard` 发生在第一次 `texelFetch(gridMetadata, ...)` 之前。运行时逻辑与图像结果没有因此变化。

## 修正方式

新增采集后复核器 `tools/verify_analytic_tile_ab_postcapture.py`，不改写冻结工具。它：

1. 首先再次运行冻结复核器，并要求它只以已知的源码检查错误退出，把完整输出保存为 `verification/frozen-verifier-known-failure.log`。
2. 重新校验协议、pre/capture manifest、二进制及 180 个原始文件哈希。
3. 从 60 个原始 JSON 独立重算 10 个同轮配对比较、胜负门槛与最终决策。
4. 重新比较 30 组截图，并逐帧比较 15 组 Moving Camera Position/Target。
5. 将源码顺序检查修正为 `discard` 位于第一次 `texelFetch(gridMetadata, ...)` 之前。
6. 输出自身 SHA-256、冻结复核器 SHA-256和本说明 SHA-256，形成可审计的采集后修正链。

该修正只影响验证工具的静态断言，不改变任何实验输入、实现、计时数据、截图或 Go/No-Go 门槛。
