# PBR-Renderer

A physically based renderer and game engine in C11 and OpenGL 4.6, built as a static library,
with a testbed.

- **Rendering**: Cook-Torrance GGX metallic-roughness shading, image-based lighting from HDRIs
  (multi-scatter, specular occlusion), cascaded shadow maps with filtered soft shadows, HDR
  rendering with ACES tone mapping and exposure, directional / point / spot lights.
- **Scene**: an entity-component world with generational handles, reflected components, a
  reference-counted asset cache, and JSON scene files.
- **Commands**: every scene change goes through one JSON command API, with validation, clear
  errors, and undo/redo grouped into transactions.
- **Remote control**: a localhost WebSocket server speaking JSON-RPC 2.0, with change
  notifications.

The project is a work in progress; see [docs/ROADMAP.md](docs/ROADMAP.md) for what's done and
what's next.

## Building

Requirements: CMake 3.10+, a C11 and C++17 compiler (C++ is only for Dear ImGui), and a GPU with
OpenGL 4.6.

```sh
git clone --recursive https://github.com/Mar-Dev-One/PBR-Renderer.git
cd PBR-Renderer
cmake -S . -B build
cmake --build build --config Release
```

If you cloned without `--recursive`, run `git submodule update --init` first (GLFW, cglm and
cimgui are submodules; the other dependencies are vendored in `engine/deps/`).

This builds the engine as one static library, `PBR_Engine`, and the **Testbed**
(`build/testbed/Release/Testbed.exe` with Visual Studio) on it. The testbed builds a scene in code
with the World API -- a ground plane, a grid of spheres from smooth to rough (dielectric and
metal), a polished Suzanne turning on a pedestal, a sun, a point light and a spot light under an
HDRI sky -- and draws it. Left-drag orbits, the wheel zooms, Esc quits. `--check PNG` renders it
once in a hidden window and saves the picture; `--view NAME` shows a debug view (albedo, normals,
roughness, metallic, ao, shadow, lighting, wireframe, lights). It finds the engine's shaders and
the sample project at runtime, so it can be started from any directory, and its
[main.c](testbed/main.c) is the shortest example of a program using the engine.

## Making an SDK

To use the engine from another project without CMake -- an include folder and `.lib` files, as
with any C library -- install it into a folder, once per configuration:

```sh
cmake --build build --config Debug   && cmake --install build --config Debug   --prefix PBR_SDK
cmake --build build --config Release && cmake --install build --config Release --prefix PBR_SDK
```

`PBR_SDK/` then holds `include/` (the engine's headers and the ones it needs from cglm, cJSON,
GLFW, glad and cimgui), `lib/Debug/` and `lib/Release/` (`PBR_Engine.lib` with `glfw3.lib`,
`glad.lib` and `cimgui.lib` beside it, each its own library), and `engine/shaders/`, which the
engine loads at runtime. Its [README](engine/sdk/README.md) lists the project settings, the
libraries to link and a minimal program.

## Using the engine

A program fills in an `App` ([engine/src/App.h](engine/src/App.h)) with `on_init`, `on_frame`,
`on_resize` and `on_shutdown` hooks and calls `run()`. In them it builds a `world`
([World/World.h](engine/src/World/World.h)) -- `world_create()`, then components such as
`world_add_mesh_renderer()` (a model from the asset cache, or `assets_builtin()` shapes) and
`world_add_light()` -- or loads one from a scene file (`deserialize_scene()`), and each frame calls
`world_update()` and `scene_renderer_draw()` ([Renderer/SceneRenderer.h](engine/src/Renderer/SceneRenderer.h)).
[testbed/main.c](testbed/main.c) does exactly that.

**Projects.** A project is a folder with a `project.json` (name, startup scene, window size,
input bindings) and `scenes/`, `models/`, `hdri/` and `textures/`; every path in a scene is
relative to it. [assets/](assets/) is the sample project, and [assets/scenes/](assets/scenes/)
holds two example scenes.

**Commands.** Every scene change can also go through one JSON command API
([Commands/Commands.h](engine/src/Commands/Commands.h)), with validation, clear errors and
undo/redo grouped into transactions:

```json
{"cmd": "place_model", "args": {"model": "builtin:sphere", "size": 0.3, "on_ground": true}}
```

Commands cover entities and components (`entity.create`, `component.set`, ...), high-level
placement (`place_model`, `entity.place` relative to other entities), scene files (`scene.save`,
`scene.load`, `scene.list`), undo/redo (`history.*`) and batches; `scene.describe` shows the
current world. A program can add commands of its own (`command_context.host_commands`).

**Remote control.** `server_start()` ([Net/CommandServer.h](engine/src/Net/CommandServer.h)) opens
a command server on `127.0.0.1` and writes how to reach it (port and token) to
`~/.pbr-renderer/server.json`. Any WebSocket client can connect to
`ws://127.0.0.1:<port>/?token=<token>` and send the same commands as JSON-RPC 2.0; every change is
pushed back to connected clients as a `scene.changed` notification. See
[tests/server_test.py](tests/server_test.py) for a stdlib-only Python client.

## Models

`model_load(path)` ([engine/src/Scene/Model.h](engine/src/Scene/Model.h)) picks a loader by file
extension:

| Format | Loader | Notes |
|---|---|---|
| `.gltf` / `.glb` | [cgltf](https://github.com/jkuhlmann/cgltf) | Scene hierarchy, multiple meshes and materials, embedded or external textures, PBR metallic-roughness (base color, metal/rough, normal, occlusion, emissive), alpha mask/blend, double-sided; glass (`KHR_materials_transmission`, `_ior`, `_volume`), clear coat (`KHR_materials_clearcoat`) and specular (`KHR_materials_specular`), their factors. |
| `.obj` (+ `.mtl`) | [fast_obj](https://github.com/thisistherk/fast_obj) | Vertices are welded; `Kd`/`Ke`/`Ns`/`d` and `map_Kd`/`map_Ke` become a PBR material. |

Both are converted to the same `model` / `material` / `mesh` types and drawn with
`engine/shaders/pbr.*`. Not supported yet: skinning and animation, morph targets, Draco/meshopt
compression, `KHR_texture_transform`, and second UV sets.

To use a model, put it in the project's `models/` and load it with `assets_load_model()` (or name
it in a scene file). HDRIs work the same way: an equirectangular `.hdr` in the project's `hdri/`. [Poly Haven](https://polyhaven.com/hdris)
is a good source.

## Project layout

```
engine/src/
  App, Platform/   window, main loop, input
  RHI/             rendering hardware interface (OpenGL backend)
  Renderer/        scene renderer, lighting, shadows, IBL baking, post-processing
  Scene/           camera, lights, materials, meshes, model loaders
  World/           entity-component world, reflection, JSON serialization
  Assets/          shared, reference-counted asset cache
  Commands/        command API, undo/redo history, JSON-RPC
  Net/             localhost WebSocket command server
  UI/              ImGui layer, entity panel, inspector, command console, log
engine/shaders/    the engine's GLSL shaders
testbed/           builds a scene in code and draws it -- the engine used as a library
assets/            the default project: models, HDRIs, textures, scenes
tests/             headless C tests and end-to-end server tests
docs/              roadmap
```

## Rendering features in progress

Glass, depth of field, clearcoat and specular, built step by step on the branch
`features/glass-and-focus` as planned in [docs/RENDERING_FEATURES_ROADMAP.md](docs/RENDERING_FEATURES_ROADMAP.md).
Every new field defaults to off, so existing scenes render as before. The testbed shows each one:
`--scene glass`, `--scene focus`, `--scene coat`, with `--view transmission`, `behind_glass` or `focus`
and `--timings N` for the GPU cost.

| Feature | Step | Commit |
|---|---|---|
| 1. Transmission (glass) | 1a: material fields `transmission`, `ior`, `thickness`, `attenuation_color`, `attenuation_distance`, as MeshRenderer overrides and in scene files | Transmission 1a: glass material fields |
| | 1b: glTF loading of `KHR_materials_transmission`, `_ior` and `_volume` (factors; textures later) | Transmission 1b: read glass from glTF |
| | 1c: its own draw pass (opaque → glass → blend), the `transmission` debug view, the testbed's `--scene glass` | Transmission 1c: glass draws in its own pass |
| | 1d: a mipmapped copy of the scene behind the glass, only in frames with glass ("Glass copy" timing, `behind_glass` debug view) | Transmission 1d: copy the scene behind glass |
| | 1e: the shading: refraction by `ior` over `thickness`, blur by roughness, Beer–Lambert tint, Fresnel-weighted | Transmission 1e: glass shows what's behind it |
| | 1f: no SSAO on glass, no shadows from glass, TAA checked with a turning camera (`--aa taa --turn 1`) | Transmission 1f: glass with SSAO, shadows and TAA |
| | 1g: limits written down in `SceneRenderer.h` (glass behind glass, off-screen); an amber bottle behind the green one | Transmission 1g: glass behind glass |
| | 1h: cost, with `--timings N --size 1920x1080`: no glass passes in a scene without glass | Transmission 1h: glass timings |
| 2. Depth of field | 2a: `Camera.focus_distance` and `aperture` (f-number, 0 = off), a 24 mm sensor, `dof_taps` in the render settings | Depth of field 2a: camera focus and aperture |
| | 2b: circle of confusion per pixel from depth, the `focus` debug view | Depth of field 2b: circle of confusion |
| | 2c: the half-size gather blur, near and far kept apart; the testbed's `--scene focus` | Depth of field 2c: the blur |
| | 2d: before bloom, so bright spots blur into discs | Depth of field 2d: bokeh |
| | 2e: before TAA, which smooths in-focus edges (`--aa taa`, a still camera) | Depth of field 2e: stable with TAA |
| | 2f: cost ("Depth of field" timing); nothing at aperture 0 | Depth of field 2f: timings |
| 3. Clearcoat | 3a: material fields `clearcoat`, `clearcoat_roughness`, overrides, scene files, glTF loading | Clearcoat 3a: clear coat material fields |
| | 3b: a second GGX lobe from the lights and the environment, the base dimmed by the coat's Fresnel; the testbed's `--scene coat` | Clearcoat 3b: clear coat shading |
| | 3c: cost: skipped by a branch when `clearcoat` is 0 | Clearcoat 3c: no cost without a coat |
| 4. Specular | 4a: material fields `specular`, `specular_color`, overrides, scene files, glTF loading | Specular 4a: specular material fields |
| | 4b: a dielectric's F0 from `ior` × `specular_color`, its strength by `specular`; defaults render as before | Specular 4b: specular shading |

## Tests

```sh
ctest --test-dir build -C Release --output-on-failure
```

The C tests are headless (no window or GL context), so they run anywhere. They cover the world,
serialization, commands, undo history, placement, JSON-RPC and encoding. With Python 3 on the
path, ctest also runs an end-to-end test of the WebSocket server.

## Dependencies

[GLFW](https://github.com/glfw/glfw), [glad](https://github.com/Dav1dde/glad),
[cglm](https://github.com/recp/cglm), [Dear ImGui](https://github.com/ocornut/imgui) via
[cimgui](https://github.com/cimgui/cimgui), [stb_image / stb_image_write](https://github.com/nothings/stb),
[cgltf](https://github.com/jkuhlmann/cgltf), [fast_obj](https://github.com/thisistherk/fast_obj),
[cJSON](https://github.com/DaveGamble/cJSON).

## License

[MIT](LICENSE)
