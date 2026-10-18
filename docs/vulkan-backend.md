# Vulkan-Render-Back-End – Entwurf

Ziel: Den CPU-Rasterizer des Pushbuffer-Executors durch die GPU ersetzen (Ziel 60 fps; später höhere Auflösung,
AA, HD-Texturen, hohe fps – siehe Memory „performance-and-graphics-goals“). Entscheidung Vulkan statt OpenGL:
Nutzerwunsch (2026-10-18).

## Einbindung
- Die Runtime ruft ein registriertes Back-End über `xboxrecomp/src/kernel/nv2a_backend.h` auf: `clear`, `draw`, `flip`,
  alle auf dem NV2A-Poll-Thread, nacheinander. Der Executor dekodiert weiter den Pushbuffer, führt Vertex-Programme auf
  der CPU aus (`nv2a_vsh_interp.c`), clippt an der Near-Plane und liefert Dreiecke in Surface-Pixeln.
- Unser Back-End lebt in **`src/render/`** (eigener Code, kein Submodul-Eingriff) und wird in `src/main.c` registriert,
  wenn `SSX3_RENDERER=vulkan`. Dann kein `RECOMP_FB_WINDOW` (eigenes Fenster).
- Abhängigkeiten als gepinnte Submodule: `third_party/Vulkan-Headers` (v1.4.363), `third_party/volk`
  (vulkan-sdk-1.4.363.0, lädt `libvulkan.so`/`vulkan-1.dll` zur Laufzeit → kein Import-Lib/SDK nötig, auch MinGW).
- Shader: GLSL in `src/render/shaders/`, zur Build-Zeit mit `glslc -mfmt=c` nach SPIR-V als C-Array.

## Darstellung
- Pro Gast-Surface (`color_va`) ein Offscreen-Farbbild in voller Surface-Größe (inkl. AA-Faktor), pro `zeta_va` ein
  Depth/Stencil-Bild. Rendern mit Dynamic Rendering (Vulkan 1.3).
- `flip`: das zuletzt bezogene Surface per Blit (lineare Filterung = AA-Downsample) in die Swapchain, präsentieren,
  SDL-Events pumpen (Tastatur → `xbox_InputSdlEvent`).
- Zustände (Blend/Depth/Stencil/Cull/Colormask) als **Dynamic State** (1.3 core + `VK_EXT_extended_dynamic_state3`),
  damit nicht für jede NV2A-Zustandskombination eine Pipeline entsteht. Alpha-Test im Shader.
- Positionen: Surface-Pixel → NDC im Vertex-Shader (Push-Constants: Surface-Größe); `w = 1/rhw` für
  perspektivisch korrekte Interpolation; z aus dem Z24-Bereich normiert.
- Texturen: `nv2a_backend_decode_texture` → BGRA8, Cache nach (Adresse, Format, Größe, Inhalts-Hash pro Frame).
  Render-to-Texture: zeigt eine Textur auf ein Surface, das die GPU gerade gerendert hat, wird das GPU-Bild benutzt.

## Was die Schnittstelle (noch) nicht liefert → Runtime-Patch, Meilenstein 3
Nur Texturstufe 0, eine UV, nur Diffuse-Farbe, keine Register-Combiner, kein Specular/Fog. SSX 3 nutzt Combiner stark
(`nv2a_rc_eval` ist die heißeste Funktion im CPU-Profil). Plan: `Nv2aBatch` um einen optionalen Erweiterungszeiger
ergänzen (4 Texturstufen, 4 UV-Sätze, Specular, Fog, kompletter Combiner-Zustand); Combiner als **Uber-Shader**
(Zustand per Push-Constants/UBO, ein vorkompiliertes SPIR-V – kein Laufzeit-Shader-Compiler, keine Pipeline-Flut).

## Meilensteine
1. Vulkan-Init, Fenster, Swapchain; `clear` + `flip` (Fenster zeigt die Clear-Farben). Debug-Dump als BMP.
2. Dreiecke mit Vertex-Farbe + Textur Stufe 0, Blend/Depth/Stencil/Cull/Alpha-Test.
3. Schnittstellen-Erweiterung + Combiner-Uber-Shader + Mehrfachtexturen.
4. Render-to-Texture, AA, Performance (Frames in flight, Texture-Cache), Windows (Win32-Surface statt SDL).
