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

This builds two apps on the same engine library. The assets path is compiled into both, so they
can be started from any directory.

- **Editor** (`build/editor/Release/Editor.exe` with Visual Studio): the scene panels, command
  console, command server and in-app chat described below.
- **Testbed** (`build/testbed/Release/Testbed.exe`): the game runtime. It loads a scene saved by
  the editor and plays it, with no UI, console or command server:
  `Testbed [--width N] [--height N] [--fullscreen] [--live] [scene] [hdri]`, where `scene` is a name under `assets/scenes/` (default
  `orrery_plaza`) or a path to a `.json` file, and `hdri` is a `.hdr` under `assets/` that
  overrides the scene's own. The scene file carries its environment (HDRI, exposure, ambient
  strength, skybox), so it looks as it did in the editor. It views the scene through its first
  Camera entity; without one, the camera frames the scene's objects (not its ground plane),
  left-drag orbits and the wheel zooms. The window is 1280 x 720 by default. The game runs the
  scene's behaviours: a `Spin` component turns its entity at `degrees_per_second`. The editor
  previews the turning but keeps the authored pose in the scene, so what you save (and what the
  live link sends) is what you authored. The model viewer's **Spin** checkbox is such a component. Esc quits.

**Live editing.** Press **F5** (or **Play in Testbed** in the Scene window) in the editor to start
the game with `--live`: it plays the world the editor has on screen and picks up every change —
from the panels, the console or the agent — within a fraction of a second, while it keeps running
(spinning entities keep turning). The editor publishes its world and environment to
`~/.pbr-renderer/live_scene.json` whenever it changes, and the game reloads that file; the editor
stays the source of truth. The game started by F5 closes with the editor; its output goes to
`~/.pbr-renderer/game.log`.

## Using the editor

The **Scene** window switches between two modes:

- **Lit Model**: a model viewer on a ground plane, with a model picker, spin, and a cascade debug
  view.
- **Grid**: a metallic × roughness sphere grid, for checking IBL and the BRDF.

**Selecting:** click an object in the view (a short click; dragging still orbits) or its name in
the **Entities** panel. The selection is outlined in orange with its name; lights and cameras are
picked by clicking where they are. **F** frames the selection, **Esc** clears it (and quits when
nothing is selected), and clicking empty space deselects.

**Editing:** the **Inspector** shows the selected entity's name, parent and every component as
editing fields — drags for numbers and vectors (within each field's range), color pickers,
checkboxes, dropdowns for enums and models, and a checkbox per material override. **Add
component...** adds a MeshRenderer, Light, Camera or Spin; the **X** on a component's header
removes it; **Delete** removes the entity. Every edit runs as a command, exactly like the console
or the agent: it is validated (a rejected value shows in red and changes nothing), reaches the
live game, and is undoable — a whole drag is one step. **Ctrl+Z** / **Ctrl+Y** undo and redo.

**Gizmo:** the selection gets move / rotate / scale handles in the view —
**W** / **E** / **R** switch between them. The Scene panel has the **Local / World** toggle (scale
always uses the object's own axes) and **Snap** with its step (meters, degrees or scale factor);
holding **Ctrl** while dragging flips snapping. A drag is one undoable step, like an Inspector edit,
and works on parented entities (the result is stored relative to the parent).

The lighting panel edits every light in the scene, plus ambient strength, exposure, the HDRI, IBL
and the skybox.

| Input | Action |
|---|---|
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
| F1 | Command console |
| Chat window | Ask the AI agent to build or change the scene |
| Esc | Quit |

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
`assets/shaders/pbr.*`. Not supported yet: skinning and animation, morph targets, Draco/meshopt
compression, `KHR_texture_transform`, and second UV sets.

To add a model to the editor's picker, put it in `assets/models/` and add it to `MODEL_PATHS` in
[editor/main.c](editor/main.c). HDRIs work the same way: an equirectangular `.hdr` in
`assets/hdri/`, listed in `HDRI_PATHS`. [Poly Haven](https://polyhaven.com/hdris) is a good
source.

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
testbed/           the game runtime (loads and plays a scene; no editor UI)
assets/            shaders, models, HDRIs, textures, scenes
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
