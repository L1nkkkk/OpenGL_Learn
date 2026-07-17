# Memory optimization benchmark

## Test environment

- Build: Release x64
- Scene: `saved/last_scene.json`
- Resolution: 1440 x 900
- GPU: NVIDIA GeForce RTX 5060 Ti, driver 32.0.15.9186
- CPU: Intel Core i7-12700KF
- OS: Windows 11 Pro
- Method: three cold process launches per variant; wait until the async model queue is empty, wait two additional seconds, then sample process and Windows GPU-process counters. The CPU-staging phase also used one unmeasured warm-up per binary to exclude first-path executable scanning.
- Assimp import experiment date: 2026-07-18
- Assimp import method: one unmeasured warm-up per binary, then three fresh-process samples per variant in `A/B/B/A/A/B` order. Memory peaks cover process start through two seconds after load ready.
- Frame pacing: the application does not call `glfwSwapInterval` or implement an explicit FPS cap. The observed approximately 165 FPS may reflect external display/driver pacing, so FPS is not used as a primary import metric.
- Background load: no dedicated CPU affinity or machine-isolation step was applied. Balanced interleaving and fresh processes were used to reduce temporal drift; the complete raw spread is retained for noise assessment.

## Results

| Variant | Load ready (ms) | Dedicated GPU (MiB) | Shared GPU (MiB) | Working set (MiB) | Private bytes (MiB) | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline (`b3becc4`) | 2843.1 | 1449.52 | 47.77 | 304.10 | 1822.73 | 165 |
| Single-copy color-space-aware textures | 1792.1 | 891.33 | 49.10 | 305.30 | 1257.09 | 165 |
| Textures + on-demand render targets | 1739.2 | 786.75 | 47.77 | 303.86 | 1130.67 | 165 |
| Textures + on-demand targets + shared geometry | 1668.9 | 742.83 | 44.43 | 215.01 | 981.55 | 165.3 |
| Previous phases + released CPU mesh staging | 1977.7 | 742.83 | 45.77 | 158.41 | 941.93 | 165.0 |

Absolute rows are representative averages from each phase's benchmark session. The contemporaneous control tables below are authoritative for per-phase deltas.

## Texture optimization delta

- Dedicated GPU memory: **-558.19 MiB (-38.51%)**
- Private bytes: **-565.64 MiB (-31.03%)**
- Scene load-ready time: **-1051.0 ms (-36.97%)**
- FPS: **unchanged at 165**
- Working-set and shared-GPU changes were below 3 MiB and treated as measurement noise.

The optimized path uploads each texture once using its semantic color space. Albedo/diffuse and cubemap assets use sRGB storage, while normal/specular data remains linear. The legacy gamma-off view is reconstructed in shaders instead of keeping a second physical texture.

## On-demand render-target delta

This phase used an interleaved A/B/A test order against `12c9520` so both variants saw the same scene assets and similar filesystem-cache conditions. The matching control averaged 891.32 MiB dedicated GPU memory and 1261.90 MiB private bytes.

- Dedicated GPU memory: **-104.57 MiB (-11.73%)**
- Private bytes: **-131.23 MiB (-10.40%)**
- Load-ready time: **-8.0 ms (-0.46%, treated as noise)**
- FPS: **unchanged at 165**
- Working-set and shared-GPU changes were below 4 MiB and treated as measurement noise.

Relative to the original `b3becc4` baseline, the two retained memory phases now total:

- Dedicated GPU memory: **-662.77 MiB (-45.72%)**
- Private bytes: **-692.06 MiB (-37.97%)**
- Load-ready time: **-1103.9 ms (-38.83%)**
- FPS: **unchanged at 165**

The default forward path now owns only its HDR color/depth target and the final postprocess target. Deferred, SSAO, bloom, and disabled-light shadow targets are created on demand. Switching an effect off returns its old framebuffer storage instead of retaining every historical configuration in the pool.

## Shared mesh-geometry delta

This phase used an interleaved A/B/A test order against `75abe29`. Previously, copying a cached `Mesh` deep-copied its CPU vertex array and created another VAO/VBO. Cached and instantiated meshes now share immutable geometry ownership while retaining independent material and visibility state.

- Dedicated GPU memory: **-43.92 MiB (-5.58%)**
- Private bytes: **-145.44 MiB (-12.91%)**
- Working set: **-89.33 MiB (-29.35%)**
- Load-ready time: **-164.7 ms (-8.98%)**
- FPS: **unchanged at 165.3**

The profiler reports one live copy of the default scene geometry: **43.57 MiB Mesh CPU + 43.57 MiB Mesh GPU**. Cache hits no longer increase either category.

Relative to the original `b3becc4` baseline, the retained phases through shared geometry total:

- Dedicated GPU memory: **-706.69 MiB (-48.75%)**
- Private bytes: **-841.18 MiB (-46.15%)**
- Working set: **-89.09 MiB (-29.30%)**
- Load-ready time: **-1174.2 ms (-41.30%)**
- FPS: **165 -> 165.3 (no material change)**

## Released CPU mesh-staging delta

This phase used a balanced interleaved `A/B/B/A/A/B` order against `2889d7f`, after one unmeasured warm-up launch for each binary. Every measured run was a fresh process using the same scene and assets.

| Variant | Load ready (ms) | Dedicated GPU (MiB) | Shared GPU (MiB) | Working set (MiB) | Private bytes (MiB) | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Matched control (`2889d7f`) | 1971.8 | 742.82 | 44.43 | 200.19 | 981.18 | 165.1 |
| Release CPU staging after upload | 1977.7 | 742.83 | 45.77 | 158.41 | 941.93 | 165.0 |

- Live profiler Mesh CPU memory: **-43.57 MiB (-100.00%)**
- Process private bytes: **-39.25 MiB (-4.00%)**
- Working set: **-41.78 MiB (-20.87%)**
- Load-ready time: **+5.8 ms (+0.30%, treated as noise)**
- Dedicated GPU memory: **unchanged at two-decimal resolution**
- FPS: **-0.1 (-0.06%, treated as noise)**
- Shared-GPU change was 1.33 MiB and treated as measurement noise.

Mesh upload data now exists only while a VBO is being created. Each shared geometry retains its vertex count, AABB, and a compact conservative bounding sphere, then frees the `std::vector<Vertex>` capacity immediately after `glBufferData`. Cached model instances can therefore calculate safe frustum-culling bounds without retaining or reconstructing CPU vertex arrays.

Adding the matched per-phase deltas to the original baseline gives a normalized cumulative result of **-706.69 MiB dedicated GPU (-48.75%)**, **-880.43 MiB private bytes (-48.30%)**, and **-130.87 MiB working set (-43.03%)**. Normalized load-ready time remains **-1168.4 ms (-41.10%)**, with no material FPS change.

## Assimp face-copy import delta

`Model::processMesh` previously copied every `aiFace` before appending its indices. Assimp's `aiFace` copy constructor allocates and copies the index array, so the default scene paid for approximately **270,988 short-lived heap allocations, copies, and frees** while importing its two character models. The retained implementation reads each immutable face through `const aiFace&`; index output and rendering behavior are unchanged.

This phase compared the candidate against the matched `f781e27` control (binary-equivalent to `5078f30`). The candidate was the uncommitted working tree that became this commit. Both variants used Release x64 at 1440 x 900 with `saved/last_scene.json`. Each binary received one unmeasured warm-up, followed by fresh-process samples in the balanced `A/B/B/A/A/B` order. Load ready is the first frame after the asynchronous model queue becomes empty; memory peaks cover startup through two seconds after that point.

| Order | Variant | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - matched control | 2880.5 | 336.59 | 1084.45 | 224.84 | 1024.11 |
| 2 | B - face by reference | 2622.2 | 340.53 | 1122.44 | 220.77 | 1026.43 |
| 3 | B - face by reference | 2872.2 | 330.94 | 1080.72 | 225.52 | 1023.49 |
| 4 | A - matched control | 2783.7 | 326.93 | 1092.48 | 224.97 | 1025.56 |
| 5 | A - matched control | 2740.2 | 343.18 | 1114.23 | 220.88 | 1019.80 |
| 6 | B - face by reference | 2643.7 | 332.68 | 1081.02 | 224.92 | 1016.22 |

| Average | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A - matched control | 2801.5 | 335.57 | 1097.05 | 223.56 | 1023.16 |
| B - face by reference | 2712.7 | 334.72 | 1094.73 | 223.74 | 1022.05 |
| Delta | **-88.8 (-3.17%)** | -0.85 (-0.25%) | -2.33 (-0.21%) | +0.17 (+0.08%) | -1.11 (-0.11%) |

- Load-ready time improved by **88.8 ms (3.17%)** on average. Two of the three candidate launches were faster than every control launch; one candidate launch overlapped the control range, so the full raw spread is retained above rather than presenting the mean alone.
- All observed memory changes were below 0.3% and are treated as measurement noise. This change removes transient allocator work, not retained mesh or GPU storage.
- Release x64 built successfully. The resource smoke test passed with the expected FBO sequence **2 -> 4 -> 6 -> 8 -> 2**, while every stage remained at **0.00 MiB Mesh CPU** and **43.57 MiB Mesh GPU**.

## Removed unused Assimp tangent generation

The importer previously requested `aiProcess_CalcTangentSpace`, but `Model::processMesh` never copied Assimp's tangent or bitangent arrays. `Mesh` instead expands indexed triangles and computes the TBN basis with `ComputeTBNVertices`. Removing the unused Assimp post-process avoids generating temporary per-vertex tangent data without changing the vertex data uploaded by this application.

This phase used parent commit `f29a557` as the matched control; its executable is binary-equivalent to the originally measured `d4bc160` control because the rewrite changed documentation only. The candidate was the uncommitted working tree that became this commit. The Release x64, 1440 x 900 scene, warm-up, fresh-process order, load-ready signal, and memory sampling window were identical to the preceding experiment.

| Order | Variant | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - `f29a557` control | 2781.9 | 333.19 | 1117.49 | 224.94 | 1024.57 |
| 2 | B - no Assimp TBN | 2994.6 | 326.68 | 1080.38 | 228.96 | 1026.14 |
| 3 | B - no Assimp TBN | 2640.4 | 327.47 | 1043.93 | 220.79 | 1007.71 |
| 4 | A - `f29a557` control | 2883.0 | 322.54 | 1086.78 | 224.95 | 1021.08 |
| 5 | A - `f29a557` control | 2810.4 | 325.74 | 1086.72 | 220.90 | 1027.36 |
| 6 | B - no Assimp TBN | 2620.6 | 326.42 | 1081.65 | 224.80 | 1025.91 |

| Average | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A - `f29a557` control | 2825.1 | 327.16 | 1097.00 | 223.60 | 1024.34 |
| B - no Assimp TBN | 2751.9 | 326.86 | 1068.65 | 224.85 | 1019.92 |
| Delta | **-73.2 (-2.59%)** | -0.30 (-0.09%) | **-28.34 (-2.58%)** | +1.25 (+0.56%) | -4.42 (-0.43%) |

- Peak private bytes fell by **28.34 MiB (2.58%)** on average, and every candidate peak was below every control peak. This is the primary retained-memory-window result for the removed temporary tangent arrays.
- Load-ready time improved by **73.2 ms (2.59%)** on average. Two candidate launches were faster than every control launch, while the first candidate launch was a high outlier; the raw values are retained above.
- Peak working-set and both stable-memory changes were below 0.6% and are treated as noise. No steady-state memory or GPU-memory reduction is claimed.
- Release x64 built successfully, and the resource smoke test again passed the **2 -> 4 -> 6 -> 8 -> 2** FBO lifecycle with **0.00 MiB Mesh CPU** and **43.57 MiB Mesh GPU** throughout.

## Moved mesh staging vectors into construction

`Model::processMesh` fills one vertex vector and one index vector per Assimp mesh, then previously passed both as lvalues to `Mesh`'s by-value constructor. That copied the complete staging arrays immediately before `ComputeTBNVertices` expanded them. All return paths now transfer the two vectors with `std::move`; the constructor receives the same contents and remains their sole owner.

This phase used parent commit `0c0da2d` as the matched control; its executable is binary-equivalent to the originally measured `d5a87a7` control because the rewrite changed documentation only. The candidate was the uncommitted working tree that became this commit. The Release x64, 1440 x 900 scene, warm-up, fresh-process `A/B/B/A/A/B` order, load-ready signal, and memory sampling window were unchanged.

| Order | Variant | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | A - `0c0da2d` control | 2626.7 | 314.32 | 1076.37 | 224.94 | 1023.81 |
| 2 | B - moved vectors | 2730.3 | 310.83 | 1066.24 | 224.84 | 1022.47 |
| 3 | B - moved vectors | 2595.6 | 326.85 | 1067.36 | 224.23 | 1023.20 |
| 4 | A - `0c0da2d` control | 2637.4 | 328.21 | 1111.87 | 224.83 | 1028.46 |
| 5 | A - `0c0da2d` control | 2750.6 | 311.19 | 1112.41 | 224.80 | 1026.16 |
| 6 | B - moved vectors | 2567.0 | 325.07 | 1067.80 | 224.81 | 1027.42 |

| Average | Load ready (ms) | Peak working set (MiB) | Peak private bytes (MiB) | Stable working set (MiB) | Stable private bytes (MiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A - `0c0da2d` control | 2671.6 | 317.91 | 1100.22 | 224.86 | 1026.14 |
| B - moved vectors | 2631.0 | 320.92 | 1067.13 | 224.63 | 1024.36 |
| Delta | **-40.6 (-1.52%)** | +3.01 (+0.95%) | **-33.08 (-3.01%)** | -0.23 (-0.10%) | -1.78 (-0.17%) |

- Peak private bytes fell by **33.08 MiB (3.01%)** on average, and every candidate peak was below every control peak. This matches the eliminated duplicate vertex and index allocations.
- Load-ready time improved by **40.6 ms (1.52%)** on average. Two candidate launches were faster than every control launch, while the first candidate launch was slower; all raw samples remain visible above.
- Peak working-set and stable-memory changes were below 1% and are treated as noise. The vectors were always temporary, so no steady-state or GPU-memory reduction is claimed.
- Release x64 built successfully, and the resource smoke test passed the **2 -> 4 -> 6 -> 8 -> 2** FBO lifecycle with **0.00 MiB Mesh CPU** and **43.57 MiB Mesh GPU** throughout.

## Resource lifecycle smoke test

Run the built-in OpenGL lifecycle test from the project directory:

```powershell
.\x64\Release\OpenGL_Learn.exe --resource-smoke-test
```

The test enables each large target group, checks the number of live FBOs, disables everything again, and exits without modifying the saved scene.

| Stage | Busy FBOs | Render targets (MiB) | Mesh CPU (MiB) | Mesh GPU (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Forward default | 2 | 24.72 | 0.00 | 43.57 |
| Forward + bloom | 4 | 64.27 | 0.00 | 43.57 |
| Deferred + SSAO + bloom | 6 | 107.53 | 0.00 | 43.57 |
| All effects + point/directional shadows | 8 | 135.53 | 0.00 | 43.57 |
| Reclaimed forward default | 2 | 24.72 | 0.00 | 43.57 |

## Rejected indexed-mesh experiment

An indexed-mesh implementation was tested and reverted. These character assets are already heavily split at material, normal, and UV boundaries, so index reuse is too low to offset the EBO and tangent-accumulation overhead.

| Variant | Load ready (ms) | Dedicated GPU (MiB) | Shared GPU (MiB) | Working set (MiB) | Private bytes (MiB) | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Texture-only control | 1809.1 | 891.33 | 49.10 | 306.98 | 1257.18 | 165 |
| Indexed mesh experiment | 1770.7 | 897.32 | 46.43 | 319.65 | 1297.26 | 165 |

The experiment increased dedicated GPU memory by **5.99 MiB (+0.67%)** and private bytes by **40.08 MiB (+3.19%)**. Its 38.4 ms load-time change did not compensate for the steady-state regression, so none of the indexed-mesh code is included in the retained implementation.
