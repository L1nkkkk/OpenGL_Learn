# Classic Scene Validation

This document defines the repeatable acceptance suite for large, well-known
rendering scenes. It records where the assets came from, how they are prepared,
what constitutes a pass, and the latest measured result.

## Run the suite

From `OpenGL_Learn/`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Test-ClassicScenes.ps1
```

The first run verifies or downloads the archives, checks their byte sizes and
SHA-256 hashes, extracts them, prepares formats supported by this renderer,
builds Release x64, launches each scene in a separate process, writes captures
and telemetry, and opens the HTML report.

Useful repeat-run options:

```powershell
# Assets are already prepared; rebuild and run every scene.
.\tools\Test-ClassicScenes.ps1 -SkipPrepare

# Run one or more selected scenes.
.\tools\Test-ClassicScenes.ps1 -SkipPrepare -SceneIds sponza,san-miguel

# Regenerate the report from existing captures and JSON results.
.\tools\Test-ClassicScenes.ps1 -ReportOnly

# Do not open the generated HTML report.
.\tools\Test-ClassicScenes.ps1 -SkipPrepare -NoOpen
```

Generated assets, import caches, captures, logs, JSON results, and reports are
intentionally ignored by Git. The tracked manifest and tools are sufficient to
reproduce them.

## Asset provenance and integrity

| Package | Scenes used | Authoritative source | Usage terms | Archive SHA-256 |
|---|---|---|---|---|
| Crytek Sponza | Sponza | [McGuire Computer Graphics Archive](https://casual-effects.com/data) | Crytek public research/radiosity donation; retain the archive attribution | `DA005CBEE0BE2DF2ABC8513F3CEB61BCB6F69AAC112BABCD9C00169A27C2770C` |
| San Miguel 2.1 | Low-poly San Miguel | [McGuire Computer Graphics Archive](https://casual-effects.com/data) | Free for research and educational use with attribution | `85874077735808150E679B3C71D70A37A270CB8833F4911325AA1099DA3F7D4A` |
| Amazon Lumberyard Bistro | Exterior and interior | [NVIDIA ORCA Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) | Creative Commons Attribution 4.0 International | `0D50E3C724C6C5DA19F8EB99AD3F53E36FEC37FFA2DF9621F9CCF0603F3934E1` |

Credits and exact download URLs, archive sizes, camera presets, and expected
triangle counts are stored in `classic-scenes.manifest.json`. Original archives
remain under `classic-scenes/_archives/`.

### Reproducible preparation

- Sponza and San Miguel use the archive OBJ/MTL data directly.
- Bistro's official FBX files are exported to pre-transformed OBJ with Assimp,
  because the current runtime import path is most reliable for OBJ.
- Bistro DDS images are decoded once and stored as PNG payloads at their
  expected logical paths because `stb_image` does not decode DDS. The original
  official archive is retained.
- No source archive is accepted until both its exact byte length and SHA-256
  hash match the manifest.

## Acceptance protocol

- Configuration: Release x64.
- Resolution: 1440 x 900.
- Process isolation: one new renderer process per scene.
- Camera: fixed position, target, up vector, radius normalization, and FOV from
  the manifest.
- Presentation: hidden GLFW window with VSync disabled.
- Sample: 15 warm-up frames followed by 45 timed frames; capture occurs on
  rendered frame 60.
- Capture: one lossless PNG converted from the renderer's PPM framebuffer dump.
- Geometry gate: imported triangle count must exactly match the manifest.
- Memory gate: CPU mesh staging must be `0` after GPU upload.
- Lifetime gate: texture, CPU mesh, GPU mesh, and render-target telemetry must
  all return to `0` when the scene process exits.
- Visual gate: each final capture is manually reviewed for a valid camera,
  non-empty image, intact geometry, and visible material response.

`Load ready` depends heavily on import-cache state. The latest run below used a
warm generated Assimp cache. Keep cache state identical for comparisons.

Frame time and FPS in this suite are acceptance signals, not performance
optimization claims. Any renderer optimization still requires an isolated A/B
experiment with the same scene, cache state, build, resolution, camera, process
sampling order, and a recorded baseline/candidate delta as defined by the
project performance-testing policy.

## Latest accepted run

- Date: 2026-07-23
- Branch/base: `codex/classic-scene-suite`, `c85611f` plus working-tree changes
- GPU: NVIDIA GeForce RTX 5060 Ti
- Result: 4 / 4 passed

| Scene | Load ready | Avg. frame | FPS | Triangles | Texture estimate | Mesh GPU | Mesh CPU staging |
|---|---:|---:|---:|---:|---:|---:|---:|
| Crytek Sponza | 996.5 ms | 1.876 ms | 533.0 | 262,267 | 232.45 MiB | 14.32 MiB | 0.00 MiB |
| Bistro Exterior | 19,218.9 ms | 2.721 ms | 367.6 | 2,832,120 | 4,219.90 MiB | 198.41 MiB | 0.00 MiB |
| Bistro Interior | 8,618.9 ms | 1.983 ms | 504.3 | 1,046,609 | 2,209.14 MiB | 57.25 MiB | 0.00 MiB |
| San Miguel 2.1 low-poly | 5,675.1 ms | 4.845 ms | 206.4 | 5,617,451 | 442.34 MiB | 414.60 MiB | 0.00 MiB |

The texture column estimates uncompressed GPU mip-chain storage, not archive or
disk size. All four processes reported zero tracked resources after teardown.

## Defects found by the suite

1. A 703 x 1000 RGB San Miguel texture crashed during upload because OpenGL's
   default four-byte unpack alignment did not match its 2,109-byte row stride.
   Texture upload now temporarily uses `GL_UNPACK_ALIGNMENT = 1` and restores
   the previous state.
2. OBJ material texture paths failed when a Windows model path contained
   backslashes. Model parent paths now use `std::filesystem::path`.
3. Large production assets exposed missing normals, mixed primitives, invalid
   indices, and invalid material references. Import now guards each case.
4. Sponza uses separate OBJ `map_d` opacity images. Forward PBR, Phong, deferred
   geometry, and AO passes now sample the independent mask instead of assuming
   alpha is embedded in the diffuse image.

These are correctness and robustness fixes. No performance improvement is
claimed for them without a dedicated A/B experiment.

## Known visual gaps

- Bistro stores ambient occlusion, roughness, and metalness in the RGB channels
  of its Specular texture and uses DirectX-style normal maps. The renderer does
  not yet fully interpret that package convention, so both Bistro captures look
  cooler and grayer than the authored scene.
- The imported scenes use a simple deterministic acceptance light rig, not
  their original production lighting, probes, lightmaps, or post-processing.
- The suite currently validates static geometry and materials; animation,
  skinning, LOD transitions, streaming, and occlusion behavior are outside its
  scope.

The generated detailed report is
`benchmark-results/classic-scenes/report.html`.

The controlled before/after experiment for directional, point, and spot shadow
maps is recorded in `SHADOW_SYSTEM_BENCHMARK.md`.
