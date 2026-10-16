# AI Powered PBR-Renderer

A physically based renderer in C11 and OpenGL 4.6, with an AI scene builder: describe a scene in
Claude Desktop or Claude Code and watch it get built in the running app.

- **Rendering**: Cook-Torrance GGX metallic-roughness shading, image-based lighting from HDRIs
  (multi-scatter, specular occlusion), cascaded shadow maps with filtered soft shadows, HDR
  rendering with ACES tone mapping and exposure, directional / point / spot lights.
- **Scene**: an entity-component world with generational handles, reflected components, a
  reference-counted asset cache, and JSON scene files.
- **Commands**: every scene change goes through one JSON command API, with validation, clear
  errors, and undo/redo grouped into transactions. The in-app console, the GUI and AI clients all
  use it.
- **Remote control**: a localhost WebSocket server speaking JSON-RPC 2.0, with change
  notifications and offscreen screenshots.
- **AI**: a Python package (`pbr_agent`) with an async engine client, 24 tool definitions, an
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

This builds two apps on the same engine library. Both find their files at runtime (the engine's
shaders, and a project folder), so they can be started from any directory.

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
  behaviours: a `Spin` component turns its entity at `degrees_per_second`, as in the editor's Play
  mode. Esc quits. A Debug build is a console program, so its log shows in the terminal; a Release
  build opens no console and writes `log.txt` next to itself.

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

## Using the editor

**Projects.** The editor works on a project: a folder with a `project.json` (name, startup scene,
window size, default HDRI) and `scenes/`, `models/`, `hdri/` and `textures/`. Every model and HDRI
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
- **Project Settings** sets the name, the startup scene the game plays, its window size, and the
  HDRI new scenes start with.

The **Scene** window's **Add** section puts a model from the project, a cube, sphere or plane
(at the size given), a light, a camera matching the current view, or an empty entity at the
centre of the view, on the ground. Its **Environment** section sets ambient strength, exposure,
the HDRI (from the project's `hdri/`), IBL, the skybox and its blur.

**Viewport:** the scene is drawn in the **Viewport** panel, docked in the middle the first time;
like any panel it can be resized, re-docked or pulled out into its own window, and the image
always matches its size. Orbit, zoom, picking and the gizmo work inside it. Its toolbar holds
Play / Pause / Step, the gizmo's mode, space and snapping, and Export (coming with game export).

**Play:** **Play** (Ctrl+P) runs a copy of the scene in the Viewport — Spin components turn — with
a green border (amber while paused; Ctrl+Shift+P pauses, F10 steps one frame). While playing
you can still select, inspect and move things, and the agent works on the copy too; **Stop**
throws the copy away, environment changes included, and the edited scene is exactly as it was.
There's no undo while playing, and saving waits until you stop.

**Selecting:** click an object in the view (a short click; dragging still orbits) or its name in
the **Entities** panel. The selection is outlined in orange with its name; lights and cameras are
picked by clicking where they are. **F** frames the selection, **Esc** clears it (and quits when
nothing is selected), and clicking empty space deselects.

**Editing:** the **Inspector** shows the selected entity's name, parent and every component as
editing fields — drags for numbers and vectors (within each field's range), color pickers,
checkboxes, dropdowns for enums and models, and a checkbox per material override. **Add
component...** adds a MeshRenderer, Light, Camera or Spin; the **X** on a component's header
removes it; **Delete** removes the entity. Every edit runs as a command, exactly like the console
or the agent: it is validated (a rejected value shows in red and changes nothing), reaches
connected clients, and is undoable — a whole drag is one step. **Ctrl+Z** / **Ctrl+Y** undo and redo.

**Gizmo:** the selection gets move / rotate / scale handles in the view —
**W** / **E** / **R** switch between them. The Viewport toolbar has the **Local / World** toggle (scale
always uses the object's own axes) and **Snap** with its step (meters, degrees or scale factor);
holding **Ctrl** while dragging flips snapping. A drag is one undoable step, like an Inspector edit,
and works on parented entities (the result is stored relative to the parent).

Lights are edited like any entity: select one and change it in the Inspector.

| Input | Action |
|---|---|
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
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
camera and environment. `scene.describe` shows the current world. Saved scenes live in
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

## Models

`model_load(path)` ([engine/src/Scene/Model.h](engine/src/Scene/Model.h)) picks a loader by file
extension:

| Format | Loader | Notes |
|---|---|---|
| `.gltf` / `.glb` | [cgltf](https://github.com/jkuhlmann/cgltf) | Scene hierarchy, multiple meshes and materials, embedded or external textures, PBR metallic-roughness (base color, metal/rough, normal, occlusion, emissive), alpha mask/blend, double-sided. |
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
  App, Platform/   window, main loop, input
  RHI/             rendering hardware interface (OpenGL backend)
  Renderer/        scene renderer, lighting, shadows, IBL baking, post-processing
  Scene/           camera, lights, materials, meshes, model loaders
  World/           entity-component world, reflection, JSON serialization
  Assets/          shared, reference-counted asset cache
  Commands/        command API, undo/redo history, JSON-RPC
  Net/             localhost WebSocket command server
  Chat/            in-app chat session (turns, undo per turn, agent connection)
  UI/              ImGui layer, entity panel, command console, chat panel
editor/            the editor app (ImGui panels, console, command server, chat)
engine/shaders/    the engine's GLSL shaders
game/              the game runtime (plays a project's scene; no editor UI) -- what an export ships
assets/            the default project: models, HDRIs, textures, scenes
agent/             pbr_agent Python package: MCP server and in-app chat agent
tests/             headless C tests and end-to-end server tests
docs/              roadmap
```

## Tests

The C tests are headless (no window or GL context), so they run anywhere:

```sh
ctest --test-dir build -C Release --output-on-failure
```

They cover the world, serialization, commands, undo history, placement, JSON-RPC and encoding.
With Python 3 on the path, ctest also runs an end-to-end test of the WebSocket server, and, when
`agent/.venv` exists, the `pbr_agent` test suite.

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
