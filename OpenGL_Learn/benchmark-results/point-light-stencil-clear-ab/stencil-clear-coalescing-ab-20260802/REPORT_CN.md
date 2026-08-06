# Deferred 点光源相邻 Stencil Clear 合并 A/B

> 决策：**Go**；数据有效：是；生成于 2026-08-02T13:50:46.196184+00:00。

## 预定义通过条件

- 图像逐像素一致，scene/submission signature、Draw Call、submitted/culled 完全一致，日志无 GL error，点光阶段出口 stencil 为 0。
- Legacy 精确为 `2N` 次点光 clear；Coalesced 在 `N>0` 时精确为 `N+1`，`N=0` 时为 0；外围固定 clear 单独保持 3 次。
- representative/256 与 /512 的 pooled 点光 GPU Median 均至少改善 0.10 ms 且 2%，并且 3/3 对应进程的点光 GPU Median 都改善。

## 环境与二进制口径

- 正式 A/B 的 18 个进程全部使用 manifest 记录的同一 Release x64 二进制，SHA-256 `6B1D690B13BD7A6EAFF5310B1DBBC717EC35EECF3F04775EDA909DDDDE561A9D`。每个进程均显式指定模式，不依赖默认值。
- 1920×1080、固定 Sponza/相机、seed `0x21D3F3A5`、显式 gPosition、Deferred、VSync 请求 0，SSAO/Bloom/阴影关闭。启动次序完整记录在 `launch-order.jsonl` 与 `manifest.json`。

## 正式逐进程结果

| 灯数 | 模式 | 进程 | CPU Frame M/P95/P99 ms | GPU Frame M/P95/P99 ms | 点光 GPU/CPU M ms | Draw | submitted/culled | 点光/固定/总 clear |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 16 | coalesced-n-plus-one | 1 | 2.892/3.303/3.524 | 1.404/1.586/1.739 | 0.729/0.02460 | 428 | 16/0 | 17/3/20 |
| 16 | legacy-2n | 1 | 3.039/3.451/3.658 | 1.515/1.694/1.853 | 0.842/0.02530 | 428 | 16/0 | 32/3/35 |
| 16 | coalesced-n-plus-one | 2 | 2.504/2.960/3.198 | 1.402/1.589/1.800 | 0.729/0.02270 | 428 | 16/0 | 17/3/20 |
| 16 | legacy-2n | 2 | 2.632/3.015/3.241 | 1.514/1.698/1.845 | 0.840/0.02270 | 428 | 16/0 | 32/3/35 |
| 16 | coalesced-n-plus-one | 3 | 2.492/3.008/3.205 | 1.402/1.602/1.793 | 0.729/0.02260 | 428 | 16/0 | 17/3/20 |
| 16 | legacy-2n | 3 | 2.744/3.203/3.314 | 1.513/1.697/1.855 | 0.841/0.02350 | 428 | 16/0 | 32/3/35 |
| 256 | coalesced-n-plus-one | 1 | 10.949/11.485/11.761 | 9.672/10.312/10.547 | 8.798/0.29030 | 908 | 256/0 | 257/3/260 |
| 256 | legacy-2n | 1 | 13.169/13.885/14.494 | 11.923/12.484/12.716 | 11.180/0.29490 | 908 | 256/0 | 512/3/515 |
| 256 | coalesced-n-plus-one | 2 | 10.955/11.440/11.603 | 9.703/10.338/10.537 | 8.812/0.29015 | 908 | 256/0 | 257/3/260 |
| 256 | legacy-2n | 2 | 12.977/13.576/13.989 | 11.739/12.341/12.483 | 10.992/0.29330 | 908 | 256/0 | 512/3/515 |
| 256 | coalesced-n-plus-one | 3 | 10.954/11.541/11.739 | 9.680/10.361/10.530 | 8.825/0.28970 | 908 | 256/0 | 257/3/260 |
| 256 | legacy-2n | 3 | 12.956/13.622/14.069 | 11.728/12.317/12.520 | 10.966/0.29120 | 908 | 256/0 | 512/3/515 |
| 512 | coalesced-n-plus-one | 1 | 19.780/20.533/21.039 | 18.443/19.230/19.584 | 17.570/0.58290 | 1420 | 512/0 | 513/3/516 |
| 512 | legacy-2n | 1 | 23.892/24.849/25.190 | 22.586/23.339/23.675 | 21.807/0.58970 | 1420 | 512/0 | 1024/3/1027 |
| 512 | coalesced-n-plus-one | 2 | 19.470/20.381/20.947 | 18.207/18.872/19.250 | 17.343/0.57330 | 1420 | 512/0 | 513/3/516 |
| 512 | legacy-2n | 2 | 23.613/24.728/25.687 | 22.383/23.117/23.645 | 21.633/0.57900 | 1420 | 512/0 | 1024/3/1027 |
| 512 | coalesced-n-plus-one | 3 | 19.425/20.387/21.144 | 18.170/18.944/19.451 | 17.310/0.57630 | 1420 | 512/0 | 513/3/516 |
| 512 | legacy-2n | 3 | 23.605/24.572/25.346 | 22.336/23.146/23.633 | 21.587/0.57845 | 1420 | 512/0 | 1024/3/1027 |

## Pooled 样本与进程中位数范围

| 灯数 | 模式 | 样本 | CPU Frame pooled M/P95/P99；进程 M 范围 | GPU Frame pooled M/P95/P99；进程 M 范围 | 点光 GPU pooled M/P95/P99；进程 M 范围 | 点光 CPU pooled M/P95/P99；进程 M 范围 |
|---:|---|---:|---:|---:|---:|---:|
| 16 | legacy-2n | 3×600 | 2.797/3.314/3.520；2.632–3.039 | 1.514/1.698/1.850；1.513–1.515 | 0.841/1.003/1.069；0.840–0.842 | 0.02370/0.02910/0.04980；0.02270–0.02530 |
| 16 | coalesced-n-plus-one | 3×600 | 2.646/3.189/3.410；2.492–2.892 | 1.403/1.590/1.793；1.402–1.404 | 0.729/0.891/1.100；0.729–0.729 | 0.02310/0.02901/0.04990；0.02260–0.02460 |
| 256 | legacy-2n | 3×600 | 13.037/13.723/14.309；12.956–13.169 | 11.811/12.387/12.610；11.728–11.923 | 11.061/11.552/11.751；10.966–11.180 | 0.29310/0.37864/0.49521；0.29120–0.29490 |
| 256 | coalesced-n-plus-one | 3×600 | 10.954/11.483/11.748；10.949–10.955 | 9.686/10.334/10.547；9.672–9.703 | 8.810/9.453/9.633；8.798–8.825 | 0.29010/0.34822/0.46390；0.28970–0.29030 |
| 512 | legacy-2n | 3×600 | 23.694/24.735/25.497；23.605–23.892 | 22.436/23.204/23.669；22.336–22.586 | 21.675/22.354/22.677；21.587–21.807 | 0.58070/0.72993/0.91394；0.57845–0.58970 |
| 512 | coalesced-n-plus-one | 3×600 | 19.549/20.479/21.068；19.425–19.780 | 18.283/19.016/19.451；18.170–18.443 | 17.418/18.046/18.405；17.310–17.570 | 0.57655/0.72226/0.87892；0.57330–0.58290 |

## A/B 收益与判定

| 灯数 | 点光 GPU Legacy→Coalesced | 节省 | GPU Frame 节省 | CPU Frame 节省 | 点光 CPU 节省 | clear 下降 | 进程方向 | 稳定门槛 |
|---:|---:|---:|---:|---:|---:|---:|---:|:---:|
| 16 | 0.841→0.729 ms | 0.112 ms / 13.31% | 0.112 ms / 7.37% | 0.151 ms / 5.39% | 0.00060 ms / 2.53% | 32→17（-46.88%） | 3/3 | 通过 |
| 256 | 11.061→8.810 ms | 2.251 ms / 20.35% | 2.125 ms / 17.99% | 2.083 ms / 15.98% | 0.00300 ms / 1.02% | 512→257（-49.80%） | 3/3 | 通过 |
| 512 | 21.675→17.418 ms | 4.257 ms / 19.64% | 4.153 ms / 18.51% | 4.145 ms / 17.49% | 0.00415 ms / 0.71% | 1024→513（-49.90%） | 3/3 | 通过 |

clear 数量接近减半是机制工作量，不等同于 GPU 时间必然减半；表中的 GPU 收益全部来自本轮同一新二进制、显式模式参数的应用内 Timer Query。旧 RenderDoc 的 10.324 ms 仅作为历史定位证据，未被计入本轮收益。

在 512 灯下，本轮真实 A/B 点光 GPU 节省 4.257 ms。若仅把旧 breakdown 的 10.324 ms clear 总时长作为线性机制量级参考，删除约一半 clear 的粗略预期是 5.162 ms；本轮收益约为该参考的 82.5%，量级接近但并不等同。差异符合驱动批次、clear 固定开销及 replay 与应用内 Timer 口径不同的预期。

点光 CPU Median 的变化很小：16/256/512 分别仅 0.00060/0.00300/0.00415 ms（2.53%/1.02%/0.71%）。因此不把 CPU Frame 的较大下降解释成算法侧 CPU 优化；它更可能来自更少 GPU clear 后的驱动提交阻塞/队列节奏变化。

## 正确性

- 图像：12 对 Legacy/Coalesced 截图全部逐字节比较；完全一致对数 12/12，全局 max/mean channel error 为 0/0.000000000。
- signature / Draw：所有正式配置 scene/submission signature 与逐帧 Draw Call 一致；submitted 保持 N，culled 保持 0。
- 生命周期：high-overlap/16、edge-cases/16 和 0 灯的两种模式均执行一次非计时 readback，非零 stencil 像素为 0；edge-cases 的近裁面、相机在光体内、完全离屏 fixture 均已验证。
- GL：扫描日志未发现 OpenGL error / GL_INVALID；JSON success 均通过。

## RenderDoc 机制证据

- 新捕获的 representative/512 Coalesced RDC 已由独立 QRenderDoc 进程成功打开并 replay；fatal status 为 `<Result: 'Success'>`，debug message 数为 0。
- 事件树：Light marker=512，Stencil volume draw=512，Lighting volume draw=512；clear initial/before/after=1/0/512，点光 clear 总计=513。
- RDC：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\renderdoc\captures\representative-0512-coalesced_capture.rdc`；replay JSON：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\renderdoc\replay\representative-0512-coalesced-replay.json`；事件树文本：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\renderdoc\replay\representative-0512-coalesced-replay.event-tree.txt`。
- 最终默认值切换后的 capture 二进制 SHA-256 为 `76DEBC76105F756DFFF3050C5C635F3F75DC3B7207C9012333583CCC3327AFA5`。它与正式 A/B 二进制分开记录；事件计数验证使用显式 Coalesced 参数，不冒充正式 A/B 时序样本。
- 本轮 RenderDoc 只做可重放性与事件树/计数验证，未 Fetch duration counter；收益口径始终是正式 A/B 的应用内 Timer Query。旧 Legacy RDC 仅保留作历史机制对照，未伪装成同二进制 A/B。

## 最终默认值与常规回归 smoke

- 最终 Release 重建后，显式 Legacy/Coalesced、不带模式参数的默认 representative/16，以及 post-final-build 关键 smoke 均成功；默认实际模式为 `coalesced-n-plus-one`，`stencilClearModeExplicit=false`。
- 最终二进制中 Legacy/Coalesced 点光 clear 为 32/17；三张截图 SHA-256 相同，生命周期 readback 非零像素均为 0。
- Resource smoke 覆盖 Forward、Deferred+SSAO+Bloom、resize/restore 与资源释放；PBR smoke 覆盖 Forward/Deferred，二者退出码均为 0，最终资源计数归零。PBR smoke 固定路径的历史 PPM 已从未修改 PNG 无损还原，本轮图像另存于 final-smoke/images。

## 为什么是 N+1，而不是只清一次

Stencil volume draw 会把当前灯的非零掩码写入同一附件。若整个点光阶段只在开头清一次，后一盏灯会继承前一盏灯的掩码，`GL_NOTEQUAL` lighting draw 将使用错误的并集/抵消结果。安全合并只能删除“上一盏 ClearAfter 与下一盏 ClearBefore”中的一个；首灯前一次加每灯后一次得到 N+1。最后一次 clear 仍有必要，它恢复阶段出口 stencil=0，避免 Forward extras / outline 等后续消费者继承最后一盏灯的掩码。

## 副作用与边界

- 不改变灯、灯序、Uniform、两次 volume draw、Blend/Depth/Cull/Stencil 状态；只跳过第二盏及以后与前一盏尾 clear 相邻的 ClearBefore。
- 收益随 active/submitted 灯数增长；N=0 不发点光 clear，N=1 与 Legacy 都是 2 次，因此无 clear 数量收益。
- 未混入 Scissor、剔除、批处理、Instancing、GBuffer、Shader、网格或 Uniform 缓存优化。

## 产物与复现

- 绝对目录：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802`
- 原始 JSON / 日志 / 截图：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\raw`、`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\logs`、`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\images`
- 聚合：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\aggregate.json`、`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\per-process.csv`、`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\aggregate.csv`、`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\comparisons.csv`
- A/B 图：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\images\point-light-stencil-clear-ab.svg`；逐像素证据：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\image-diff.json`
- RenderDoc：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\renderdoc`
- 可重复脚本快照：`C:\Users\Link\Dev\OpenGL_Learn\OpenGL_Learn\benchmark-results\point-light-stencil-clear-ab\stencil-clear-coalescing-ab-20260802\scripts`（项目内源文件位于 `tools/`）。

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_point_light_stencil_clear_ab.ps1 -Mode All
```

RenderDoc 512 Coalesced 机制捕获（在 A/B 完成后执行）：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\capture_point_light_stencil_clear_renderdoc.ps1 -RunDirectory <A/B绝对目录>
```
