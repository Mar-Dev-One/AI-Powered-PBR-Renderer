# AI Powered PBR-Renderer

A physically based renderer and game engine in C11 and OpenGL 4.6, built as a static library, with
an editor, a game runtime and an AI scene builder: describe a scene in Claude Desktop, Claude Code
or the editor's chat and watch it get built in the running app.

- **Rendering**: Cook-Torrance GGX metallic-roughness shading, image-based lighting from HDRIs
  (multi-scatter, specular occlusion), cascaded shadow maps with filtered soft shadows, spot and
  point light shadows, clustered lighting, reflection probes, glass (transmission), clear coat and
  specular, depth of field, HDR rendering with ACES / AgX / PBR Neutral tone mapping,
  auto-exposure, bloom, SSAO and FXAA / TAA.
- **Scene**: an entity-component world with generational handles, reflected components, a
  reference-counted asset cache, and JSON scene files.
- **Gameplay**: input actions over keyboard, mouse and gamepads, a walk / fly PlayerController,
  and SceneTriggers that fade over to another scene.
- **Commands**: every scene change goes through one JSON command API, with validation, clear
  errors, and undo/redo grouped into transactions. The in-app console, the GUI and AI clients all
  use it.
- **Remote control**: a localhost WebSocket server speaking JSON-RPC 2.0, with change
  notifications and offscreen screenshots.
- **AI**: a Python package (`pbr_agent`) with an async engine client, 36 tool definitions, an
  MCP server so any MCP client can build scenes, and an in-app chat panel backed by a Claude agent.

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

This builds the engine as one static library, `PBR_Engine`, and three programs on it. All of them
find their files at runtime (the engine's shaders, and a project folder), so they can be started
from any directory.

- **Editor** (`build/editor/Release/Editor.exe` with Visual Studio): the projects, panels, Viewport,
  command console, command server and in-app chat described below.
- **Game** (`build/game/Release/Game.exe`): the game runtime, what an export ships. It plays a
  project's scene with no UI, console or command server:
  `Game [--width N] [--height N] [--fullscreen] [--project DIR] [--check PNG] [scene] [hdri]`, where
  `scene` is a name under the project's `scenes/` (default: the project's startup scene) or a path
  to a `.json` file, and `hdri` is a `.hdr` under the project that overrides the scene's own. The
  project is `--project`, else a `project.json` next to the program (an exported game), else
  `assets/`. The scene file carries its environment (HDRI, exposure, ambient strength, skybox), so
  it looks as it did in the editor. It views the scene through its first Camera entity; without
  one, the camera frames the scene's objects (not its ground plane), left-drag orbits and the
  wheel zooms. The window's title and size come from the project. The game runs the scene's
  behaviours, as the editor's Play mode does: a `Spin` component turns its entity at
  `degrees_per_second`, a camera with a `PlayerController` is walked (click to take the mouse,
  then WASD / the mouse or a gamepad; Esc frees it, then quits), and walking into a `SceneTrigger`
  fades over to the scene it names. A Debug build is a console program, so its log shows in the
  terminal; a Release build opens no console and writes `log.txt` next to itself, and if it has to
  close (no OpenGL 4.6, a startup scene that doesn't load) it says why in a message box first.
- **Testbed** (`build/testbed/Release/Testbed.exe`): builds a scene in code with the World API
  and draws it -- the shortest example of a program using the engine
  ([testbed/main.c](testbed/main.c)). `--scene` picks one: `materials` (the default: a grid of
  spheres from smooth to rough, a polished Suzanne turning on a pedestal), `glass`, `focus`,
  `coat`, `showcase` (the renderer at its best) or `features` (every engine feature in a plaza you
  walk through, with a SceneTrigger into the showcase). Left-drag orbits, the wheel zooms, Esc
  quits. `--check PNG` renders it once in a hidden window and saves the picture; `--view NAME`
  shows a debug view (albedo, normals, roughness, metallic, ao, shadow, lighting, wireframe,
  lights, transmission, behind_glass, focus); `--timings N` prints each pass's GPU time.

**Exporting a game.** **Export** (Viewport toolbar, or File > Export Game) makes a folder a player
can run: `<project name>.exe`, the C++ runtime DLLs it needs, the engine's shaders, `project.json`,
every saved scene, and only the models and HDRIs those scenes use. Double-clicking the `.exe`
plays the startup scene in a window titled and sized from Project Settings; its log goes to
`log.txt` beside it. Only saved scenes are exported. After copying, the editor runs the exported
game once in a hidden window (`--check`) and shows what it rendered, so a broken export shows up
in the editor rather than on a player's PC. Build the Release game first so the export runs on PCs
without Visual Studio (the dialog warns otherwise):

```sh
cmake --build build --config Release --target Game
```

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

## Using the editor

**Projects.** The editor works on a project: a folder with a `project.json` (name, startup scene,
window size, default HDRI, input bindings) and `scenes/`, `models/`, `hdri/` and `textures/`. Every model and HDRI
path in a scene is relative to it. Without `--project` the editor opens the sample project,
`assets/`: its startup scene `model_viewer` is the spinning model on a ground plane, and
`material_grid` is the metallic × roughness sphere grid for checking IBL and the BRDF.

- **File > New Project** creates a folder (by default in `Documents/PBR Projects`) with a starter
  scene — ground, sun, a cube and a camera — and opens it. **Open Project** takes a project folder;
  **Recent Projects** lists the last ten.
- **New Scene / Open Scene / Save (Ctrl+S) / Save As** work on `scenes/` in the project. The title
  bar shows `*` while there are unsaved changes; opening something else or quitting asks first.
  Opening a scene starts a fresh undo history.
- **Import Asset** copies a `.glb`, `.gltf` (with the buffers and images it names), `.obj` (with its
  `.mtl` and textures) or `.hdr` into the project.
- **Project Settings** sets the name, the startup scene the game plays, its window size, the
  HDRI new scenes start with, and under **Input** the game's actions (move, look, jump...) and
  their keys, mouse and gamepad bindings -- type one in or **Press to bind**.

**Panels.** Every panel can be closed, re-docked or pulled out into its own window; **View** turns
each one back on. The first time, they're arranged around the Viewport:

- **Scene**: the project and scene, **Save** and **Frame all**; **Add** puts a model from the
  project, a cube, sphere or plane (at the size given), a point light, spot light or sun, a camera
  matching the current view, a reflection probe or an empty entity at the centre of the view; and
  the scene's **Environment**, saved with it: HDRI, IBL, skybox and its blur, ambient strength,
  exposure and auto-exposure, tone mapping (ACES, AgX, PBR Neutral), bloom, and ambient occlusion
  with its radius.
- **Renderer**: how the editor views the scene (not saved with it): anti-aliasing (off, FXAA, TAA),
  the debug view, shadows on or off, where the sun's cascades split, tinting by cascade, and
  unclustered lighting (the reference clustered lighting must match). **Reload shaders** rebuilds
  every shader now; **Bake reflections** captures the reflection probes again. Below, what the
  last frame drew: lights (and how full the cluster lists are), which lights got shadow maps,
  reflection probes, the environment and the GPU frame time.
- **Entities**: the scene as a tree, with each entity's kind (sun, spot light, camera, probe,
  empty) and its ID. Type in the filter to list only matching names. Drag an entity onto another
  to parent it there, or onto *scene root* to unparent it; it stays where it is in the world.
  Right-click for Frame, Duplicate, Unparent and Delete.
- **Inspector**: the selected entity's fields (see Editing below).
- **Assets**: the project's models (**Add** places one) and HDRIs (**Use** lights the scene with
  one), **Import...**, and the models in memory with their reference counts, triangles and
  materials; **Unload unused** frees the ones nothing uses.
- **History**: every undo step, newest last; click one to go back (or forward) to it.
- **Console** (F1), **Chat**, **Log** (opens by itself on an error) and **GPU timings**.

The status bar along the bottom shows the scene (`*` when unsaved), whether it's playing, the
selection, the command server and its clients, and the frame time.

**Viewport:** the scene is drawn in the **Viewport** panel, docked in the middle the first time;
the image always matches its size. Orbit, zoom, picking and the gizmo work inside it. Its toolbar
holds Play / Pause / Step, the gizmo's mode, space and snapping; then the camera, the debug view,
**Overlays** and **Export**.

- **Camera:** look through the editor's own camera or any Camera entity, to see what a player
  sees from it. Dragging or zooming goes back to the editor camera from that view. **Game camera**
  makes Play look through the scene's first Camera, as the exported game does.
- **Overlays**, drawn over the picture: a marker per light in its color (a sun has rays), with
  the selected light's reach — a point light's range as three rings, a spot light's cone out to
  its range, a sun's direction; each Camera's view as a small pyramid; each reflection probe's
  box; and, when turned on, a 1 m grid on the ground.

**Play:** **Play** (Ctrl+P) runs a copy of the scene in the Viewport — Spin components turn — with
a green border (amber while paused; Ctrl+Shift+P pauses, F10 steps one frame). While playing
you can still select, inspect and move things, and the agent works on the copy too; **Stop**
throws the copy away, environment changes included, and the edited scene is exactly as it was.
There's no undo while playing, and saving waits until you stop. With **Game camera** on and a
PlayerController on that camera, click the view to play it: the keys and mouse walk it (the
project's input actions, set in **Project Settings > Input**), SceneTriggers fade over to other
saved scenes, and Esc gives the mouse back.

**Selecting:** click an object in the view (a short click; dragging still orbits) or its name in
the **Entities** panel. The selection is outlined in orange with its name; lights, cameras and
probes are picked by clicking their marker. **F** frames the selection, **Esc** clears it (and
quits when nothing is selected), and clicking empty space deselects.

**Editing:** the **Inspector** shows the selected entity's name, parent and every component as
editing fields — drags for numbers and vectors (within each field's range), color pickers,
checkboxes, dropdowns for enums and models, and a checkbox per material override. **Add
component...** adds a MeshRenderer, Light, Camera, Spin, ReflectionProbe, PlayerController or SceneTrigger; the **X** on a
component's header removes it; **Delete** removes the entity. **Edit** (and the shortcuts below)
also undoes and redoes (naming the step), duplicates the selection with its children beside it
(**Ctrl+D**), deletes it (**Delete**), unparents it and selects its parent. Every edit runs as a
command, exactly like the console or the agent: it is validated (a rejected value shows in red
and changes nothing), reaches connected clients, and is undoable — a whole drag, a duplicate or a
reparent is one step.

**Gizmo:** the selection gets move / rotate / scale handles in the view —
**W** / **E** / **R** switch between them. The Viewport toolbar has the **Local / World** toggle (scale
always uses the object's own axes) and **Snap** with its step (meters, degrees or scale factor);
holding **Ctrl** while dragging flips snapping. A drag is one undoable step, like an Inspector edit,
and works on parented entities (the result is stored relative to the parent).

Shortcuts work whichever panel has focus, except while typing in a text field (Help > Keyboard
shortcuts lists them):

| Input | Action |
|---|---|
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
| W / E / R | Move / rotate / scale gizmo |
| F | Frame the selection |
| Ctrl+D / Delete | Duplicate / delete the selection |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+S | Save the scene |
| Ctrl+P / Ctrl+Shift+P / F10 | Play or stop / pause or resume / step one frame |
| F1 | Command console |
| Chat window | Ask the AI agent to build or change the scene |
| Esc | Clear the selection, or quit |

### Command console

The console (F1) runs the same JSON commands that remote clients send:

```json
{"cmd": "place_model", "args": {"model": "builtin:sphere", "size": 0.3, "on_ground": true}}
```

Commands cover entities and components (`entity.create`, `component.set`, ...), high-level
placement (`place_model`, `entity.place` relative to other entities), scene files (`scene.save`,
`scene.load`, `scene.list`), undo/redo (`history.*`), batches, and app-level settings such as the
camera and environment. Whatever the panels can set, a command can too: `view.look_through` picks
the camera, `render.set` the render settings (shadows and cascade splits included), `render.stats`
reports what the last frame drew, `render.reload_shaders` rebuilds the shaders, and `play.set`
takes `game_camera`. `scene.describe` shows the current world. Saved scenes live in
[assets/scenes/](assets/scenes/); two example scenes built by an AI are included. A saved scene
holds the entities and the environment (HDRI, IBL and skybox switches, sky blur, exposure, ambient
strength); loading it with `replace` restores both, while merging into the current scene adds the
entities only.

## Building scenes with Claude

When the editor starts, it opens a command server on `127.0.0.1` and writes how to reach it
(port and token) to `~/.pbr-renderer/server.json`. The `pbr-mcp` server in [agent/](agent/)
connects to it and exposes the scene tools over MCP.

```sh
python -m venv agent/.venv
agent/.venv/Scripts/python -m pip install -e agent      # Windows
agent/.venv/bin/python -m pip install -e agent          # macOS / Linux

claude mcp add pbr-renderer -- /path/to/PBR-Renderer/agent/.venv/Scripts/pbr-mcp.exe
```

Then start the editor and ask for something like *"put a table in the middle with a lamp on it,
light it warmly and show me"*. Claude can inspect the scene, place and edit entities, set lights,
camera and environment, take screenshots, and save scenes.

See [agent/README.md](agent/README.md) for Claude Desktop setup and for using the engine client
from your own Python code.

### In-app chat

The editor's **Chat** window talks to the same tools without leaving the app. At launch the
editor starts the agent (`python -m pbr_agent.agent` from `agent/.venv`), which connects back
through the command server. Set `ANTHROPIC_API_KEY` in the environment the editor starts from
(or run `ant auth login`); without credentials the panel says so.

Replies stream in, each tool call shows as a line you can expand, and **Stop** ends a turn early.
Each request is one undo step: **Undo turn** (or the console's Undo) reverts everything the agent
did for it. Changes made from Claude Desktop or Claude Code show up live in the app, and the chat's
changes show up for them.

`PBR_AGENT_PYTHON` points at a different Python, `PBR_AGENT_AUTOSTART=0` turns the automatic start
off (the panel has a **Start agent** button), and `PBR_AGENT_MODEL` / `PBR_AGENT_EFFORT` pick the
model and effort (default `claude-opus-5-5`, `medium`). The agent logs to
`~/.pbr-renderer/agent.log`.

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

To use a model, bring it into the project with **File > Import Asset** (or put it in the project's
`models/`); it then appears in **Add > Model** and the Inspector's model list. HDRIs work the same
way: an equirectangular `.hdr` in the project's `hdri/`. [Poly Haven](https://polyhaven.com/hdris)
is a good source.

## Project layout

```
engine/src/
  App, Platform/   window, main loop, input and input actions
  RHI/             rendering hardware interface (OpenGL backend)
  Renderer/        scene renderer, lighting, shadows, IBL baking, post-processing
  Scene/           camera, lights, materials, meshes, model loaders
  World/           entity-component world, reflection, JSON serialization, player controller,
                   scene transitions
  Assets/          shared, reference-counted asset cache
  Project/         projects, importing assets, exporting a game
  Commands/        command API, undo/redo history, JSON-RPC
  Net/             localhost WebSocket command server
  Chat/            in-app chat session (turns, undo per turn, agent connection)
  UI/              ImGui layer, entity panel, inspector, command console, chat panel, log
engine/shaders/    the engine's GLSL shaders
editor/            the editor app (ImGui panels, console, command server, chat)
game/              the game runtime (plays a project's scene; no editor UI) -- what an export ships
testbed/           builds a scene in code and draws it -- the engine used as a library
assets/            the default project: models, HDRIs, textures, scenes
agent/             pbr_agent Python package: MCP server and in-app chat agent
tests/             headless C tests, end-to-end server tests, golden images
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
ctest --test-dir build -C Release -LE gpu      # without the GPU test, e.g. on a build server
```

The C tests are headless (no window or GL context), so they run anywhere. They cover the world,
serialization, commands, undo history, placement, JSON-RPC, encoding, input and glTF materials. With Python 3 on the
path, ctest also runs an end-to-end test of the WebSocket server, and, when `agent/.venv` exists,
the `pbr_agent` test suite.

`GoldenTests` (label `gpu`) renders the reference scenes in `tests/golden/scenes/` with the game's
`--check` and compares them with the saved pictures in `tests/golden/`. A failure leaves the render
and a diff (changed pixels in red) in `build/tests/golden/`. When a rendering change is intended,
save new pictures, look at them, and commit them:

```sh
build/tests/Release/GoldenTests --update            # every case
build/tests/Release/GoldenTests --update avocado    # some
```

The saved pictures come from one GPU; on another, small rounding differences may need new ones.

## Dependencies

[GLFW](https://github.com/glfw/glfw), [glad](https://github.com/Dav1dde/glad),
[cglm](https://github.com/recp/cglm), [Dear ImGui](https://github.com/ocornut/imgui) via
[cimgui](https://github.com/cimgui/cimgui), [stb_image / stb_image_write](https://github.com/nothings/stb),
[cgltf](https://github.com/jkuhlmann/cgltf), [fast_obj](https://github.com/thisistherk/fast_obj),
[cJSON](https://github.com/DaveGamble/cJSON). Python side: the
[MCP Python SDK](https://github.com/modelcontextprotocol/python-sdk),
[websockets](https://github.com/python-websockets/websockets) and the
[Anthropic Python SDK](https://github.com/anthropics/anthropic-sdk-python).

## License

[MIT](LICENSE)
