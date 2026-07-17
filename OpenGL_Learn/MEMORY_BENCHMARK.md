# Memory optimization benchmark

## Test environment

- Build: Release x64
- Scene: `saved/last_scene.json`
- Resolution: 1440 x 900
- GPU: NVIDIA GeForce RTX 5060 Ti, driver 32.0.15.9186
- CPU: Intel Core i7-12700KF
- OS: Windows 11 Pro
- Method: three cold process launches per variant; wait until the async model queue is empty, wait two additional seconds, then sample process and Windows GPU-process counters. The CPU-staging phase also used one unmeasured warm-up per binary to exclude first-path executable scanning.

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
