# Rendering features roadmap

New rendering features for the engine: **glass** (transmission), **depth of field**, and, after them,
**clearcoat** and **specular**. Each is built in steps; each step ends with a check that must pass
before the next one starts.

| Feature | What it gives |
|---|---|
| 1. Transmission | Glass and clear liquids that show what's behind them, bent and tinted, with reflections on top (today transparency is alpha blending only: glass looks like tinted plastic) |
| 2. Depth of field | A camera focus: the subject sharp, the foreground and background soft, round bokeh on highlights; animatable (focus pulls) |
| 3. Clearcoat | A second glossy layer over a material: lacquer, car paint, coated plastic |
| 4. Specular | The strength and colour of a dielectric's reflections: coloured glass, liquids, fabrics |

## Working rules

- All the work on one branch, `features/glass-and-focus`, in small commits.
- A section in the README listing each feature and its commits.
- **Additive only:** every existing scene, model and material renders exactly as before. Every new field
  defaults to "off" (`transmission` 0, `aperture` 0, `clearcoat` 0, specular as glTF's defaults).
- New fields go through the reflection table and the scene file, so commands, the inspector and saved
  scenes all see them.
- Each feature gets a testbed scene that shows it, and a GPU timing scope (`Renderer/GpuProfiler.h`).

## Where things are

- Materials: `engine/src/Scene/Material.h` / `Material.c`: `material` (base colour, metallic, roughness,
  emissive, alpha mode), `material_override` with its `MATERIAL_OVERRIDE_*` bits.
- glTF loading: `engine/src/Scene/ModelGltf.c` (reads `KHR_materials_emissive_strength` already).
- Draw passes: `MODEL_PASS_OPAQUE` and `MODEL_PASS_BLEND` (`Scene/Model.h`); `draw_entities()` in
  `Renderer/SceneRenderer.c` draws all opaque submeshes, then all blended ones.
- A frame, in `SceneRenderer.c`: depth pre-pass (opaque) and SSAO → sky → the PBR pass (opaque, then
  blend) into the HDR target (`post_process.scene_target`, RGBA16F) → TAA → auto-exposure → bloom →
  `post_end_scene_to()` (tone mapping, FXAA).
- Shaders: `engine/shaders/` (`pbr.vert` / `pbr.frag`, `taa.frag`, `bloom_*.frag`, `tonemap.frag`, ...).
- Camera component: `c_camera` in `World/Components.h` (`fov_degrees`, `near_plane`, `far_plane`).
- Per-entity material tweaks: `c_mesh_renderer.material` (a `material_override`).
- Reflection: `World/Reflect.c`; saving: `World/Serialize.c`; tests: `tests/`.

---

## 1. Transmission (glass)

Screen-space transmission, as in glTF's `KHR_materials_transmission` + `KHR_materials_volume` +
`KHR_materials_ior`: a transmissive surface shows the opaque scene behind it, refracted by its index of
refraction over its thickness, absorbed by its volume's colour, blurred by its roughness, with the usual
Fresnel-weighted reflections on top.

| Step | Delivers | Checked by |
|---|---|---|
| 1a | **Material fields:** `transmission` (0..1), `ior` (default 1.5), `thickness` (meters), `attenuation_color` (linear RGB, default white), `attenuation_distance` (meters, 0 = no absorption). Override bits and fields in `material_override` (`MATERIAL_OVERRIDE_TRANSMISSION`, `_IOR`, `_THICKNESS`, `_ATTENUATION`). In the reflection table (MeshRenderer) and the scene file | A test sets, reads, saves and reloads them; a scene saved before this loads and saves unchanged |
| 1b | **glTF:** `ModelGltf.c` reads the three extensions (factor, thickness, attenuation colour and distance, ior; textures can wait) | Khronos's sample models *TransmissionTest* and *DragonAttenuation* load with the right values |
| 1c | **Its own pass:** `MODEL_PASS_TRANSMISSION`. A submesh with `transmission > 0` draws in it, not in opaque or blend; order: opaque → transmission → blend | A debug colour for transmissive submeshes shows the right ones |
| 1d | **The scene behind:** after the opaque geometry and the sky, a copy of the HDR target into a texture with a mip chain (a downsample chain like bloom's). Made only in frames that draw something transmissive | A debug view shows the copy and its mips; the GPU timings show no new work in a scene without glass |
| 1e | **The shading** (`pbr.frag`, or a variant compiled with a define): the transmitted light is the copy sampled at the screen position offset by the view-space normal × `thickness` × (1 − 1/`ior`), at a mip chosen from roughness; tinted by Beer–Lambert absorption, `attenuation_color ^ (thickness / attenuation_distance)`; it replaces the diffuse term in proportion to `transmission`. Specular (IBL and lights) as for any surface, weighted by Fresnel. The pass writes depth | Testbed: a glass sphere and a glass bottle in front of a checkerboard and a bright light. The board shows through, bent and tinted; the edges reflect the environment; at roughness 0.3 what's behind is blurred |
| 1f | **Around it:** no SSAO on transmissive surfaces (they're not in the depth pre-pass); their shadows lightened by `transmission` and tinted by the attenuation colour (or not cast at all, if much simpler: write down which); TAA reprojects them using the depth they write | The test scene with TAA on and off; no ghosting when the camera moves |
| 1g | **Limits, written down:** glass seen through glass shows only the opaque scene behind both; only what's on screen can be refracted | Two bottles, one behind the other: the result is stable and documented |
| 1h | **Cost** | At 1920×1080: under ~1.5 ms for the copy and the pass; nothing without glass |

## 2. Depth of field

A thin-lens camera: `focus_distance` and `aperture` on the camera, a blur per pixel from its depth, a
gather blur that keeps a sharp subject from bleeding into a soft background.

| Step | Delivers | Checked by |
|---|---|---|
| 2a | **Camera fields:** `c_camera.focus_distance` (meters) and `aperture` (f-number; 0 = off, the default). Focal length from `fov_degrees` and a nominal 24 mm sensor height (full frame), so f/2 looks like f/2 on a real camera. In the reflection table (so they can be animated: focus pulls) and the scene file. `scene_render_settings`: a quality setting (taps) | Tests: set, read, save, reload; old scenes load with `aperture` 0 |
| 2b | **Circle of confusion** per pixel from depth: `c = A · f · |d − s| / (d · (s − f))`, converted to pixels, signed (near / far), clamped to a maximum radius, into a half-resolution target | Debug view: black at the focus, brighter with distance in front and behind |
| 2c | **The blur:** a half-resolution gather over a disc (24–48 taps on a golden-angle spiral). Near and far handled apart: a sample spreads onto a pixel only if its own blur reaches it, so a sharp subject doesn't halo into the background and a blurred foreground covers it softly. Composited back at full resolution by the blur amount | Testbed: a bottle sharp at f/2 with a wall behind it soft; no halo round the bottle; an object in front blurred over it |
| 2d | **Its place in the frame:** in `finish_frame()`, after TAA and before auto-exposure and bloom, on the HDR target, so bright highlights blur into round bokeh | A bright point behind the subject becomes a disc |
| 2e | **Stability:** still with TAA and with renders that restart TAA and converge over several sub-frames | A still camera over 3 s: no shimmer on the subject's edges |
| 2f | **Cost** | At 1920×1080: under ~2 ms; nothing at `aperture` 0 |

## 3. Clearcoat

glTF's `KHR_materials_clearcoat`: a thin, glossy dielectric layer over the base material.

| Step | Delivers | Checked by |
|---|---|---|
| 3a | Material fields `clearcoat` (0..1) and `clearcoat_roughness`; override bits; reflection and the scene file; glTF loading (factors first, textures later) | Khronos's *ClearCoatTest* loads with the right values |
| 3b | Shading: a second specular lobe (GGX, IOR 1.5) from the lights and the IBL, the base layer dimmed by the coat's Fresnel | Testbed: a red rough sphere with a clear coat shows sharp highlights over a soft base |
| 3c | Cost | Negligible with `clearcoat` 0 (a branch or a shader variant) |

## 4. Specular

glTF's `KHR_materials_specular`: the strength and colour of a dielectric's reflection at normal
incidence.

| Step | Delivers | Checked by |
|---|---|---|
| 4a | Material fields `specular` (0..1, default 1) and `specular_color` (default white); override bits; reflection and the scene file; glTF loading | Khronos's *SpecularTest* loads with the right values |
| 4b | Shading: F0 for dielectrics from `ior` (feature 1) × `specular_color` × `specular`, instead of the fixed 0.04 | Testbed: a row of spheres from specular 0 to 1 and a tinted one; defaults render exactly as before |

---

## Order and time

| Step | Time |
|---|---|
| 1a–1e (glass shows through) | about a day |
| 1f–1h | half a day |
| 2a–2f (depth of field) | half a day |
| 3, 4 | half a day each, when needed |

## Risks

- **Screen space:** glass refracts only what's on screen. Good for objects in front of a set; wrong
  for, say, a window looking out of frame.
- **Depth of field and TAA:** shimmer on edges. 2e checks for it; reducing the blur's sample jitter or
  running it before TAA are the options if it shows.
- **Shader size:** transmission and clearcoat add work to `pbr.frag`; use shader variants (defines) so
  materials without them pay nothing.

## Later

- Transmission and clearcoat textures (`transmissionTexture`, `thicknessTexture`, `clearcoatTexture`, ...).
- Motion blur (needs per-pixel velocity; TAA reprojects with the camera's matrices today).
- Sheen and anisotropy.
