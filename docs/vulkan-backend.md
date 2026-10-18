# Vulkan render back end – design

Goal: replace the pushbuffer executor's CPU rasteriser with the GPU (target 60 fps; later higher resolution, AA,
HD textures, high frame rates). Vulkan rather than OpenGL: the user's choice (2026-10-18). The CPU rasteriser stays
available as the reference (`SSX3_RENDERER=cpu`) for comparison dumps; the Vulkan path is the one being maintained.

## Integration
- The runtime calls a registered back end through `xboxrecomp/src/kernel/nv2a_backend.h`: `clear`, `draw`, `flip`, all on
  the NV2A poll thread, one at a time. The executor still decodes the pushbuffer, runs vertex programs on the CPU
  (`nv2a_vsh_interp.c`), clips at the near plane and hands over triangles in surface pixels.
- Our back end lives in **`src/render/`** (own code, no submodule change) and is registered in `src/main.c` when
  `SSX3_RENDERER=vulkan` (the default in `scripts/run.sh`). The window and its event loop run on the main thread from
  process start (`ssx3_vk_host_main`); the guest runs on its own thread; rendering happens on the NV2A thread.
- Dependencies as pinned submodules: `third_party/Vulkan-Headers` (v1.4.363), `third_party/volk`
  (vulkan-sdk-1.4.363.0; loads `libvulkan.so`/`vulkan-1.dll` at run time → no import library or SDK, MinGW included).
- Shaders: GLSL in `src/render/shaders/`, compiled to SPIR-V C arrays at build time with `glslc -mfmt=c`.

## Rendering
- One offscreen colour image per guest surface (`color_va`) at full surface size (AA factor included), one
  depth/stencil image per `zeta_va` (a scratch one when the title has none). Dynamic rendering (Vulkan 1.3).
- `flip`: blit the surface the title last rendered into to the swapchain (linear filtering = AA downsample), present.
- State (blend/depth/stencil/cull/colour mask) as **dynamic state** (1.3 core + `VK_EXT_extended_dynamic_state3`), so no
  pipeline per NV2A state combination. Alpha test in the shader.
- Positions: surface pixels → NDC in the vertex shader (push constants: surface size); `w = 1/rhw` for perspective-
  correct interpolation; z normalised from the Z24 range.
- Textures: `nv2a_backend_decode_texture` → BGRA8, cached by (address, format, size); a hash of the raw guest bytes is
  checked once per frame and the texture re-decoded only when it changed (FMV frames are rewritten in place).
  Render-to-texture: a texture that points at a surface the GPU rendered samples that GPU image.

## What the interface does not (yet) provide → runtime patch, milestone 3
Only texture stage 0, one UV set, only the diffuse colour, no register combiners, no specular/fog. SSX 3 uses
combiners heavily (`nv2a_rc_eval` was the hottest function in the CPU profile). Plan: extend `Nv2aBatch` with an
optional extension pointer (4 texture stages, 4 UV sets, specular, fog, full combiner state); combiners as an
**uber-shader** (state in push constants/UBO, one precompiled SPIR-V – no run-time shader compiler, no pipeline flood).

## Milestones
1. ✅ Vulkan init, window, swapchain; `clear` + `flip`. Debug dump as BMP (`SSX3_VK_DUMP`).
2. ✅ Triangles with vertex colour + texture stage 0, blend/depth/stencil/alpha test. 60 fps title/menu.
3. Interface extension + combiner uber-shader + multitexturing.
4. Render-to-texture checks, AA/resolution, per-triangle culling, frames in flight, Windows (Win32 surface instead of SDL).
