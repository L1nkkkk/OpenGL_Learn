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

## Texture optimization delta

- Dedicated GPU memory: **-558.19 MiB (-38.51%)**
- Private bytes: **-565.64 MiB (-31.03%)**
- Scene load-ready time: **-1051.0 ms (-36.97%)**
- FPS: **unchanged at 165**
- Working-set and shared-GPU changes were below 3 MiB and treated as measurement noise.

The optimized path uploads each texture once using its semantic color space. Albedo/diffuse and cubemap assets use sRGB storage, while normal/specular data remains linear. The legacy gamma-off view is reconstructed in shaders instead of keeping a second physical texture.
