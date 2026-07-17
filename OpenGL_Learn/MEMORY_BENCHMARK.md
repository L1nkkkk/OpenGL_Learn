# Memory optimization benchmark

## Test environment

- Build: Release x64
- Scene: `saved/last_scene.json`
- Resolution: 1440 x 900
- GPU: NVIDIA GeForce RTX 5060 Ti, driver 32.0.15.9186
- CPU: Intel Core i7-12700KF
- OS: Windows 11 Pro
- Method: three cold process launches per variant; wait until the async model queue is empty, wait two additional seconds, then sample process and Windows GPU-process counters.

## Results

| Variant | Load ready (ms) | Dedicated GPU (MiB) | Shared GPU (MiB) | Working set (MiB) | Private bytes (MiB) | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline (`b3becc4`) | 2843.1 | 1449.52 | 47.77 | 304.10 | 1822.73 | 165 |
| Single-copy color-space-aware textures | 1792.1 | 891.33 | 49.10 | 305.30 | 1257.09 | 165 |
| Textures + on-demand render targets | 1739.2 | 786.75 | 47.77 | 303.86 | 1130.67 | 165 |
| Textures + on-demand targets + shared geometry | 1668.9 | 742.83 | 44.43 | 215.01 | 981.55 | 165.3 |

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

Relative to the original `b3becc4` baseline, all retained memory phases total:

- Dedicated GPU memory: **-706.69 MiB (-48.75%)**
- Private bytes: **-841.18 MiB (-46.15%)**
- Working set: **-89.09 MiB (-29.30%)**
- Load-ready time: **-1174.2 ms (-41.30%)**
- FPS: **165 -> 165.3 (no material change)**

## Resource lifecycle smoke test

Run the built-in OpenGL lifecycle test from the project directory:

```powershell
.\x64\Release\OpenGL_Learn.exe --resource-smoke-test
```

The test enables each large target group, checks the number of live FBOs, disables everything again, and exits without modifying the saved scene.

| Stage | Busy FBOs | Render targets (MiB) | Mesh CPU (MiB) | Mesh GPU (MiB) |
| --- | ---: | ---: | ---: | ---: |
| Forward default | 2 | 24.72 | 43.57 | 43.57 |
| Forward + bloom | 4 | 64.27 | 43.57 | 43.57 |
| Deferred + SSAO + bloom | 6 | 107.53 | 43.57 | 43.57 |
| All effects + point/directional shadows | 8 | 135.53 | 43.57 | 43.57 |
| Reclaimed forward default | 2 | 24.72 | 43.57 | 43.57 |

## Rejected indexed-mesh experiment

An indexed-mesh implementation was tested and reverted. These character assets are already heavily split at material, normal, and UV boundaries, so index reuse is too low to offset the EBO and tangent-accumulation overhead.

| Variant | Load ready (ms) | Dedicated GPU (MiB) | Shared GPU (MiB) | Working set (MiB) | Private bytes (MiB) | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Texture-only control | 1809.1 | 891.33 | 49.10 | 306.98 | 1257.18 | 165 |
| Indexed mesh experiment | 1770.7 | 897.32 | 46.43 | 319.65 | 1297.26 | 165 |

The experiment increased dedicated GPU memory by **5.99 MiB (+0.67%)** and private bytes by **40.08 MiB (+3.19%)**. Its 38.4 ms load-time change did not compensate for the steady-state regression, so none of the indexed-mesh code is included in the retained implementation.
