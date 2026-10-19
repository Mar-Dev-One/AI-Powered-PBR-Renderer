# Roadmap

Two tracks: the **AI scene builder** (describe a scene in chat, from Claude Desktop/Code or from a
chat panel inside the app, and have it built) and the **renderer/editor features** around it.
The AI track drives the order; renderer features slot in where they help it.

_Updated 2026-10-18._ Where things stand: the AI scene builder (Phases 1-5) works from Claude
Desktop/Code and from the in-app chat. The editor opens projects, edits their scenes in a Viewport
panel, plays them in place, and exports a game a player can run (Stages 1-4 below). The renderer
work under "Engine roadmap" has phases 1-3 done; its follow-ups and phases 4-7 are saved for later.

## Next up

In rough priority order: the open items from the sections below, plus what came up while
building the export.

1. **Try an export on a PC without Visual Studio.** Everything says it runs (its dependencies are
   Windows' own DLLs plus the copied C++ runtime), but it has only been run on the build machine.
2. ~~**Errors a player can see.** The exported game has no console, so a `FATAL` (no OpenGL 4.6, a
   broken startup scene) just closes it, with the reason only in `log.txt`. Show a message box,
   and stop writing terminal color codes into `log.txt`.~~ Done: a Release game shows a message
   box (`Platform/Dialog.h`) on a `FATAL` (`log_set_fatal_hook()`, `Core/Log.c`) or a startup scene
   that doesn't load, then closes; never under `--check`. Logs are in color only on a terminal.
3. ~~**Play through the scene's camera.** Play keeps the editor's orbit camera; the game looks
   through the first Camera entity. A toolbar toggle to see what the player will see.~~ Done with
   the editor rewrite (below): **Game camera** on the Viewport toolbar (`play.set`'s
   `game_camera`), and looking through any Camera entity while editing (`view.look_through`).
4. **Exercise the in-app agent against the live Claude API.** Its loop is only tested with a
   scripted model (Phase 5).
5. ~~**Trim the export's runtime DLLs.** Since the library split the game has no C++ code, so it
   imports only `VCRUNTIME140.dll`; Export still copies all of `msvcp140*.dll` too.~~ Done:
   `editor/CMakeLists.txt` lets only `vcruntime140.dll` through (8 DLLs before).
6. **Editor polish:** a native folder picker for New/Open Project, Import and Export (paths are
   typed today). ~~An agent's `scene.save` should update the editor's saved state, so the title
   loses its `*`.~~ Done: `command_context.on_scene_file` reports every `scene.save` and replacing
   `scene.load`, whoever runs it; the editor follows a load through undo and redo
   (`doc_follow_history()`), and the `*` now notices every environment field.
7. **Phase 6** of the AI track: asset catalog, verify loop, `fetch_asset`.
8. **Export, later:** zip it, cache baked IBL maps so the game doesn't bake at startup, a pixel
   comparison in the export check. ~~Scene switching in the game~~: done with the `SceneTrigger`
   component (`World/SceneTransition.h`): a box that fades the player over to another saved
   scene, arriving at a named spawn entity; in the game and in the editor's Play (Scene > Add >
   Scene Trigger, an orange box with its destination in the view).

## Done

- [x] Multi-light shading: directional/point/spot in `pbr.frag` (`Renderer/Lighting.h`)
- [x] HDR scene target + fullscreen tone-mapping pass (`Renderer/PostProcess.h`)
- [x] Multi-scatter IBL, specular occlusion, filtered CSM (hardware PCF, rotated Poisson,
      normal-offset bias, cascade blending), anisotropic filtering
- [x] `SceneRenderer` owns the lit frame; cached uniform locations; C-side defines injected into
      GLSL; hot reload of `pbr.vert`/`pbr.frag`

## Open checks (before Phase 1)

- [x] Verify new shadows visually: soft and continuous, no acne or peter-panning on the avocado or
      the ground, cascade split where expected (tune `sample_cascade()` biases,
      `SHADOW_FILTER_RADIUS`, `CASCADE_BLEND_FRACTION` in `pbr.frag` if a model shows artifacts)
- [x] Verify Grid mode through `SceneRenderer`
- [x] Exercise a spot light: the testbed's "Spot light" panel, a warm cone aimed at the origin, off by default

---

## AI scene builder

### Architecture

```
 Claude Desktop / Claude Code                 App (C)
          │ MCP (stdio)                ┌──────────────────────────────┐
          ▼                            │ ImGui chat panel             │
 ┌──────────────────────┐              │        │ chat.send / events  │
 │ Python: pbr_agent    │   JSON-RPC   │ ┌──────▼─────────────────┐   │
 │  tools.py (1 source) │◄────────────►│ │ Command server (WS)    │   │
 │  mcp_server.py       │  WebSocket   │ │  queue → main thread   │   │
 │  agent.py (in-app)   │  localhost   │ └──────┬─────────────────┘   │
 └──────────────────────┘              │ Commands → World (ECS)       │
          ▲ Claude API                 │        → SceneRenderer       │
                                       └──────────────────────────────┘
```

Principles:

- **The engine is the single source of truth.** Every scene change, from any client, goes through
  one command API. The debug console uses it too.
- **Tools are defined once** (`tools.py`). The generic tools' schemas come from the engine's
  component reflection, so they can't drift from the C structs.
- **The engine is the hub.** It broadcasts change events, so Claude Desktop and the in-app panel
  stay in sync when both edit the same scene.
- **MCP before the in-app panel.** Claude Desktop gives a chat UI for free and tests the tool design
  before any UI is built.

### Phase 1: World (entity-component system) replaces `Scene` — done

- Generational handles `entity_id { index, generation }`: destroyed slots bump the generation, so
  stale IDs are rejected instead of reaching a new entity.
- Components: one array per type plus a presence bitmask per entity.
  - `Name`: unique, human-readable; the LLM refers to entities by name
  - `Transform`: position, Euler rotation in degrees, scale (matrix derived from them)
  - `Parent`
  - `MeshRenderer`: asset ID plus per-entity material overrides (base color, metallic, roughness,
    emissive)
  - `Light` (existing `light` struct), `Camera`
  - `Bounds`: computed, read-only
- Asset cache: models loaded once, reference-counted, shared. The ground plane and primitives
  (cube, sphere, plane) become ordinary assets.
- Component reflection table (field name, type, range) drives JSON, validation and tool schemas.
  JSON via [cJSON](https://github.com/DaveGamble/cJSON).
- Port `SceneRenderer`, `Lighting`, `Shadow` and the testbed to the world.

**Done when:** the testbed builds its demo through the world API, and removing an entity
mid-frame is safe.

Status: done. `World/` (World, Components, Reflect), `Assets/AssetCache`, built-in cube/sphere/plane,
per-entity material overrides, read-only `UI/EntityPanel`, headless `tests/world_tests.c` (ctest).
Lit Model and Grid render as before; the ground is now a regular entity. (Since Stage 2 both are
scene files in the sample project: `model_viewer`, `material_grid`.)

### Phase 2: Command layer (in-engine, no networking) — done

- `cmd_execute(world, json) → json`.
- Generic commands: `entity.create`, `entity.destroy`, `component.set`, `component.remove`,
  `scene.describe`, `scene.clear`, `scene.save`, `scene.load`, `assets.list`.
- High-level commands: `place_model(asset, position | on_ground | relative_to {entity, side, gap})`,
  `set_environment(hdri)`, `set_camera(look_at, from)`.
- Validation: clamp values, reject unknown names, return clear error messages.
- Undo/redo: each command stores the previous values it changed; commands group into
  **transactions** so one chat request undoes as one step.
- A revision counter plus change events `{rev, entity, component, op}`.
- An ImGui command console for typing JSON commands by hand.

**Done when:** a scene can be built, changed and undone entirely from the console.

Status: done. `World/Serialize` (cJSON 1.7.18), `Commands/Commands` (generic + `place_model`,
`entity.place`, `scene.save/load/list`, `batch`, `history.*`, app-registered host commands),
`Commands/History` (in-place undo/redo, transactions, change events), `UI/CommandConsole` (F1).
The testbed edits World components directly, so the console and the GUI never fight.

### Phase 3: Command server — done

- [mongoose](https://github.com/cesanta/mongoose) WebSocket on `127.0.0.1` only, plus a token
  from a config file.
- JSON-RPC 2.0: `cmd.*` requests; the server pushes `event.scene_changed` to every client.
- The network thread queues commands; the main thread runs them in `on_frame` (GL and the world
  are only touched on the main thread).
- `view.screenshot`: `rhi_read_pixels` → PNG (`stb_image_write`) → base64.

**Done when:** a short Python script adds a light and gets a screenshot back.

Status: done. Our own WebSocket server (MIT; mongoose is GPL) in `Net/CommandServer`, JSON-RPC 2.0
in `Commands/JsonRpc`, token + origin checks, discovery file `~/.pbr-renderer/server.json`,
non-blocking polling on the main thread (no threads), `scene.changed` notifications (tagged by
world; one per batch), `view.screenshot` (offscreen render without UI, box-downscaled PNG, base64).
Tested end to end with a stdlib-only Python client (`tests/server_test.py`).

### Phase 4: Python package `pbr_agent` + MCP — done

- `engine_client.py`: async JSON-RPC WebSocket client.
- `tools.py`: the single tool registry (name, description, schema, engine call).
- `mcp_server.py`: the official MCP Python SDK (FastMCP) over stdio; register it in Claude Desktop's
  config or with `claude mcp add`. Also expose `scene.describe` as an MCP resource.
- A shared system prompt with the scene conventions: meters, Y up, ground at y=0, naming rules,
  describe before editing, screenshot after big changes.

**Done when:** "add a red sphere left of the avocado" in Claude Desktop appears in the running app.

Status: done (in `agent/`). `EngineClient` (async, discovery with stale-pid check, safe
reconnect), `tools.py` (24 tools then; 34 now, 15 of them editor-only: camera and
look-through, environment, render settings and stats, reflection bake, log, screenshot, project,
play, export -- examples run against the engine as a drift test),
`prompt.py` (shared instructions), `mcp_server.py` / `pbr-mcp` on MCP SDK 2.x (low-level Server,
stdio, screenshots as images, `scene://current`). Verified end to end through a real MCP client
against the running Testbed. Registration: `agent/README.md`.

> The claude.ai website only reaches **remote** MCP servers over HTTPS (it needs a tunnel plus
> authentication). Claude Desktop and Claude Code run local stdio servers directly; start there.

### Phase 5: In-app chat panel — done

- `agent.py`: a Claude API tool-use loop over the same `tools.py`; keeps history and streams
  replies; each user turn is wrapped in a transaction.
- Flow:
  1. The panel sends `chat.send` to the engine.
  2. The agent, connected as a client, runs.
  3. It calls tools over the same connection.
  4. It streams its reply back as `chat.delta` events.
- Panel: message history and streaming text, a collapsible line per tool call, **Undo turn**,
  **Stop**, a busy indicator.
- The app spawns the Python sidecar (path from config) and shows "agent offline" if it isn't
  running. API keys stay in the sidecar's environment.

**Done when:** the same request works from both front-ends, and each one sees the other's changes
live.

Status: done. `Chat/ChatSession` (the conversation, `chat.*` commands, one transaction per turn
that the engine opens and closes itself, so Stop or a lost agent can't leave undo blocked),
`UI/ChatPanel` (streaming text, collapsible tool calls, Stop, Undo turn, New chat, busy and
offline states), `server.hello` roles in `Net/CommandServer` (the agent is the client holding
`chat_agent`), `Core/Process` (the app starts the sidecar, logs to `~/.pbr-renderer/agent.log`).
`agent/pbr_agent/agent.py`: streaming Claude tool-use loop over `tools.py`, append-only history,
progress notes between tool calls. Tested end to end against `ServerHost` with a scripted model
(`agent/tests/test_agent.py`). Not yet exercised against the live Claude API.

### Phase 6: Quality

1. **Asset catalog** `assets/catalog.json`: ID, description, tags, real-world size in meters,
   up axis, pivot. Normalize to meters on import.
2. ~~**Placement helpers in C:** `on_ground`, `relative_to` (left/right/front/behind/on_top), placement
   without overlaps, "frame all" for the camera.~~ Done: `place_model` / `entity.place`
   (`on_ground`, `relative_to` with a gap and no overlap) and `view.frame`.
3. **Verify loop:** screenshot after multi-step turns, then check for floating objects, overlaps,
   darkness and objects out of frame. This is where LangGraph starts to pay off
   (plan → build → verify → fix); before it, a plain tool-use loop is enough.
4. ~~Scene files: `scene.save`/`scene.load` as JSON.~~ Done in Phase 2.
5. A `fetch_asset` tool backed by the Poly Haven API (models and HDRIs).

---

## Editor rewrite (2026-10-18)

The editor was one 3,400-line `editor/main.c`; it is now split by job (`editor/Editor.h` lists
the files: HostCommands, Actions, Viewport, Panels, Menus, Export, Agent) and covers every engine
feature that had no UI:

- [x] **Edit** menu (undo / redo naming the step, duplicate with children, delete, unparent,
      select parent) and a **History** panel that jumps to any step
- [x] Shortcuts read through ImGui every frame, so they work whichever panel has focus (the GLFW
      key callback only got keys when the Viewport was focused); Ctrl+D and Delete are new
- [x] Outliner: name filter, each entity's kind, drag-and-drop parenting that keeps the world
      pose, right-click menu (`UI/EntityPanel.h` returns the request; the editor runs it as
      commands, one undo step)
- [x] Viewport overlays: light markers with the selected light's range / cone / direction,
      camera frustums, reflection probe boxes, an optional ground grid -- lines clipped to the
      near plane
- [x] Look through any Camera entity, and Play through the game camera (`view.look_through`,
      `play.set` `game_camera`); picking, the gizmo and screenshots use the camera on screen
- [x] **Renderer** panel: anti-aliasing, debug view, shadows on/off and the cascade split (new in
      `render.set`), cascade tint, unclustered lighting, Reload shaders (`render.reload_shaders`),
      Bake reflections, and the last frame's numbers (`render.stats`)
- [x] **Assets** panel: the project's models and HDRIs, Import, the asset cache with reference
      counts and Unload unused
- [x] Add > Reflection probe; Add, Play and Help menus; a status bar; new panels dock beside
      the old ones in an existing imgui.ini
- [x] Agent tools to match: `look_through`, `render_stats`, `bake_reflections`, `get_log`;
      `shadows` in `set_render`, `game_camera` in `set_play`; ReflectionProbe in `set_component` /
      `remove_component`
- [ ] Not yet: thumbnails in Assets, multi-selection, copy / paste between scenes

## Editor / runtime split

The editor (`editor/`) and the game runtime (`game/`, the `Game` target: a console program in
Debug, none in Release) are separate apps on one engine library. Scenes carry their environment,
and `Spin` components run in the game (`world_tick()`) and in the editor's Play mode.

The first live link -- F5 started `Testbed --live`, which followed the editor through a shared
scene file (`World/LiveScene.h`) -- was removed with the Testbed target (2026-10-16): in-editor
Play replaced it, and it re-serialized the whole scene several times a second while the editor
ran. To see the real game in its own window, export, or run `Game --project <folder>`.

Next steps:

- [x] In-editor Play mode: Play copies the world and ticks the copy in the viewport, Stop restores
      the edited world; pause and step. Becomes the main way to test while editing
      (done with the Viewport toolbar, Stage 3 below)
- [x] Panels edit through commands (so every edit is an undoable change event), instead of
      writing components directly (the Inspector, gizmo and Add do; the Scene panel's direct
      light and model controls went away in Stage 2. The environment -- HDRI, exposure,
      ambient -- isn't World data and still has no undo.)
- [ ] Optional, if a separate game window that follows edits is wanted again: the game connects
      to the command server as a client and applies `scene.changed` events incrementally
- [x] Split the build: `PBR_Runtime` (no ImGui/Chat/Net/Commands) for the game, `PBR_Tools` on
      top of it for the editor, both from `engine/`. App reaches ImGui only through the
      `App.ui` hook table. The Release game went from 1.75 MB to 552 KB

## Projects, viewport panel and game export

Goal: create a project, edit it in the editor, render the scene into an ImGui viewport panel with
Play / Stop / Export on its toolbar, and export the game as a folder that runs on another machine.

All four stages are done; what's left of them is in **Next up** at the top. (The original
blocker -- `ASSETS_ROOT`, an absolute path compiled into every binary -- went in Stage 1.)

### Stage 1: Runtime paths (prerequisite for everything below)

- [x] Set the asset root at startup (`paths_set_project_root()`) instead of at compile time; keep
      `ASSETS_ROOT` only as the development fallback
- [x] Split engine data (shaders, built-in textures) from project data (scenes, models, HDRIs):
      two roots, `engine_path()` and `project_path()`. The engine root is found next to the
      executable, so a copied folder still finds its shaders

Status: done. `Core/Paths.h`: shaders moved to `engine/shaders/`; the engine root is
`<exe dir>/engine` when it has `shaders/pbr.vert`, else the source tree (`DEV_ENGINE_ROOT`); the
project root is `--project <dir>` (Editor and Testbed; F5 passes it on), else `assets/`
(`DEV_PROJECT_ROOT`). `tests/paths_tests.c`. Checked by hand: a copied `Testbed.exe` with
`engine/shaders/` beside it uses those shaders and the `--project` folder's files.
Note for Stage 2: the asset cache is keyed by project-relative path, so switching projects in a
running editor must clear it.

### Stage 2: Projects

- [x] Project format: a folder with `project.json` (name, startup scene, window size, default
      HDRI) plus `scenes/`, `models/`, `hdri/`, `textures/`
- [x] New project: create the folder skeleton, an empty startup scene (ground, sun, camera) and
      `project.json`. Also an `editor.project.new` command, so the agent can do it
- [x] Open project: a File menu (New / Open / Recent), recent projects in `~/.pbr-renderer/`,
      the project name in the window title; `Editor --project <dir>` on the command line
- [x] Edit within the project: the model and HDRI pickers list the project's files instead of the
      hard-coded `MODEL_PATHS` / `HDRI_PATHS`; `scene.save/load/list` work in the project's
      `scenes/`; the current scene's name in the UI
- [x] Save (Ctrl+S) / Save As, an unsaved-changes marker (from the revision counter), a prompt
      on close or when switching scenes/projects
- [x] Import asset: copy a `.glb` / `.gltf` (with its files) / `.hdr` into the project
- [x] The current demo worlds (lit model, material grid) become scenes in a sample project

Status: done. `Project/Project.h` (project.json, `project_create()` with a starter scene of
ground, sun, cube and camera, recent projects, `project_import_asset()` for .glb / .gltf / .obj /
.hdr), `tests/project_tests.c`. The editor edits one scene of the open project: File and View
menus, Save / Save As / unsaved-changes prompt (also on the window's close button:
`App.on_close_request`), Project Settings, an Add section (project models, shapes, lights, a
camera from the view), the HDRI list from the project. Agent: `project.info` / `project.create`
commands and tools (`create_project` doesn't open the project: switching mid-turn would pull the
scene from under the agent). `assets/` is the sample project (`model_viewer`, `material_grid`).
The Testbed plays a project's startup scene when given no scene.
Not yet: a native folder picker (paths are typed or picked from Recent). (An agent's `scene.save`
and `scene.load` now update the editor's saved state and scene name: "Next up" item 6.)

### Stage 3: Viewport panel

- [x] Render the scene to an offscreen color texture and show it in a dockable ImGui "Viewport"
      window (`igImage`); resize the render targets and the camera aspect to the panel, not the
      window
- [x] Input from the panel: orbit, zoom, click-to-pick and the gizmo use panel-local coordinates
      and only act while the panel is hovered (they use the main viewport today); the selection
      outline is drawn in the panel

Status (items 1-2): done. `scene_renderer_draw_to()` renders into the editor's `view_target`
(RGBA8, the panel's exact size; `rhi_texture_native_handle()` hands it to ImGui), shown in the
"Viewport" window, docked into the main area on first run. Mouse input reads ImGui's state over
an invisible button covering the view (`view_input()`): drag orbits, a short click picks, the
wheel zooms; the outline and gizmo draw into the panel's draw list. The window has
`NoNavInputs`, so shortcuts keep working with the view focused. `view.screenshot` now renders at
the panel's size.
- [x] Toolbar along the panel's top: Play / Pause / Step / Stop, Export, and the gizmo
      move/rotate/scale and local/world toggles (moved from the Scene panel)
- [x] Play from the toolbar runs the in-editor Play mode (see "Editor / runtime split" above:
      copy the world, tick the copy, restore on Stop), with a coloured border on the panel while
      playing. Options: view through the scene's Camera entity; play in a separate window
      (the F5 Testbed then; removed since, see Editor / runtime split)

Status (items 3-4): done. `world_copy()` (exact copy, same IDs; `tests/world_tests.c`). Toolbar:
Play/Stop (Ctrl+P), Pause/Resume (Ctrl+Shift+P), Step (F10), the play clock, gizmo
operation/space/snap, Export (enabled in Stage 4). While playing everything on screen --
view, selection, gizmo, Inspector, Entities, console, server, agent, Add -- works on the copy
(no undo); Stop drops it and the environment changes made meanwhile. Saving is refused
(`command_context.save_refused`, also for the agent and scripts); opening a scene or project
stops play first; Play waits while a chat turn runs. `play.set` / `play.status` commands and a
`set_play` tool. First run without an imgui.ini gets a default layout (`default_layout()`).
Viewing through the scene's Camera entity while playing came with the editor rewrite (see
"Editor rewrite" below). Separate-window play was F5, removed since (Editor / runtime split).

### Stage 4: Game export

- [x] The runtime plays a project: `Testbed --project <dir>`, or with no arguments the
      `project.json` next to the executable; opens the startup scene, window title and size from
      the project
- [x] A shippable runtime: a Release build of the game, no console window on Windows (WIN32
      subsystem), named after the project. The editor finds a prebuilt one beside itself
- [ ] ...linked without the editor parts (needs the runtime library split above)
- [x] Export (toolbar button and `editor.export` command): choose an output folder, then copy
      the runtime executable and the DLLs it needs, the engine shaders, `project.json`, the
      scenes, and only the assets those scenes reference (walk every MeshRenderer model and the
      environment HDRI). Report the size and any missing files; refuse when a scene points
      outside the project
- [x] Check the export: run the exported game headless on the startup scene and screenshot it
      (reuses golden-image tests), so a broken export fails in the editor and not on a player's
      machine

Status: done. `project_export()` (`Project/Project.h`, tested in `tests/project_tests.c`) writes
`<Name>.exe`, the C++ runtime DLLs, `engine/shaders/`, `project.json`, every saved scene and only
the models (with their .gltf/.obj files) and HDRIs they use; everything is checked first, and it
refuses missing files, paths outside the project, an unsaved startup scene, and folders that
aren't empty or an earlier export. `Game` target: `game/main.c` (formerly `testbed/main.c`; `PBR_GAME_NO_CONSOLE` outside Debug) -- no
console window, log in `log.txt` beside it; it finds `project.json` next to itself and takes the
title and window size from it. The editor ships the Release `Game` when it's built (else the
Debug one, with a warning), with the DLLs CMake's `InstallRequiredSystemLibraries` finds.
`--check PNG`: the exported game renders its startup scene in a hidden window and exits 0/1; the
Export dialog runs it in the background and shows the picture. Toolbar Export, File > Export
Game, `project.export` / `project.export_status` commands and tools.
Not done: the runtime library split (the game still links ImGui, the command layer and the
server code, unused: the Release game is 1.7 MB); a pixel comparison against saved golden
images (the check only proves the scene loads and renders). Scene switching came later: the
`SceneTrigger` component (below).
- [ ] Later: zip the export; cache baked IBL maps in the export so the game doesn't bake at
      startup; ~~scene switching in the game~~ (done: `SceneTrigger`)

## Engine roadmap

The renderer and runtime work, numbered as it was planned on 2026-10-18. Phases 1-3 are done
(details under "Feature roadmap" below); the rest is saved for later. **Resume with "Follow-ups"
or phase 4.**

### Done

- **1. Seeing what the renderer does:** 1.1 debug views, 1.2 GPU timers per pass, 1.3 golden-image
  tests (`tests/golden_tests.c`), 1.4 in-app log window, 1.5 hot reload for every shader.
- **2. Image quality:** 2.1 bloom, 2.2 `KHR_materials_emissive_strength` (was already in), 2.3 FXAA,
  2.4 AgX and Khronos PBR Neutral tone mapping, 2.5 auto-exposure, 2.6 SSAO (with a depth
  pre-pass), 2.7 TAA.
- **3. Lighting:** 3.1 spot light shadows, 3.2 point light shadows, 3.3 per-frame data in uniform
  buffers, 3.4 clustered forward lighting (up to 1024 lights), 3.5 reflection probes.

### Follow-ups found along the way

Smallest and most useful first.

- ~~**Shadow map cache:** re-render a spot or point light's maps only when something in its range
  moves.~~ Done (`Renderer/ShadowCache.h`): each spot tile and point row keeps a signature (the
  light's view and its casters' entity, model and world matrix) and is redrawn only when it
  changes; casters turned by a Spin are drawn each frame over a kept copy of the static layer
  (`rhi_framebuffer_copy_depth()`). Sample scene, on a GPU running about 4x slower than the
  3.5 ms figure was measured on: point shadows 14-18 ms -> 1-2 ms; layered and direct maps give
  identical pictures. `render.stats` / the Renderer panel say how many maps were redrawn.
- **Sun shadow cascades:** now the biggest pass -- about half the GPU frame in the sample scene
  (31 of 62 ms on that slow GPU): 4 cascades redraw the whole scene every frame. They follow the
  camera, so a cache helps only while it's still; the same static/moving split would keep a
  Spin from forcing a full redraw. Or fewer / smaller cascades for distant ones.
- **Tighter light-to-cluster test:** test each light's sphere against each cluster's box rather than
  its screen rectangle; dense scenes still put 32+ lights in many clusters.
- **Half-resolution SSAO:** about 2.4 ms of the 4.4 ms SSAO costs at full resolution.
- **Per-object motion vectors for TAA:** moving entities (`Spin`) aren't tracked, only the camera.
- **2D texture arrays (or bindless textures) in the RHI:** all 16 guaranteed texture units are
  used; cube map arrays exist already.
- **Reflection probes:** probe-based diffuse lighting (irradiance), rotated boxes, more than 8.

### 4. Larger scenes

- 4.1 Frustum culling from the entities' `Bounds`.
- 4.2 Instancing for repeated models.
- 4.3 Transparency sorting across all entities, not just within one model.
- 4.4 Async HDRI loading and baking, so changing the environment no longer freezes the editor and
  chat.
- 4.5 Baked IBL maps cached in exports, so the game doesn't bake at startup.

### 5. Animation

- 5.1 glTF skinning and animation playback: an `Animator` component.
- 5.2 Play / loop / speed commands, so the agent can use animations.
- 5.3 Morph targets.

### 6. Game runtime (only if this should become a game engine)

- ~~6.1 Input actions: named actions mapped to keys and gamepad buttons.~~ Done:
  `Platform/Input.h` (keys, mouse, gamepads, cursor lock) and `Platform/InputActions.h` (named
  buttons and axes, with walk/fly/look defaults), saved in project.json's `"input"`; edited in
  Project Settings (press to bind) and by `input.get` / `input.set`.
- ~~6.2 A first-person / fly camera controller, so a player can walk through the scene.~~ Done:
  the `PlayerController` component (`World/PlayerController.h`), walk or fly, on the starter
  scene's camera. The game locks the mouse to it; in the editor, Play through the game camera
  and click the view. Walking keeps its height until there's physics (6.4), so no jumping yet.
- 6.3 Audio with miniaudio (a single-file C library): an `AudioSource` component.
- 6.4 Physics with Jolt through its C wrapper (JoltC): rigid bodies, colliders, raycasts.
- 6.5 Scripting, most likely Lua, for behaviours beyond `Spin`.

Decide first whether this is an AI scene builder with a polished renderer or a game engine: for a
scene builder, phases 4-5 are the engine work and phase 6 can wait.

### 7. Materials (whenever needed)

- 7.1 `KHR_texture_transform` and a second UV set.
- 7.2 Clearcoat.
- 7.3 Transmission / IOR.
- 7.4 Sheen and anisotropy.

## Feature roadmap

### Now: small, high value

- [x] FXAA (`fxaa.frag`, a pass after `tonemap.frag`, which writes luma to alpha): on by
      default, View menu and `render.set`; off in the data views
- [x] Bloom (between the scene pass and `post_end_scene()`): `post_bloom()`, the scene
      environment's `bloom` (default 0.04; the Scene panel, `render.set`)
- [x] `KHR_materials_emissive_strength` (read by `Scene/ModelGltf.c` since glTF loading landed;
      checked with a test model at strength 1 and 12)
- [x] Debug views: albedo, normals, roughness/metal, AO, shadow only, lighting only, wireframe
      (`scene_render_settings.debug_view`; the Viewport toolbar, View > Debug view, `render.set`)
- [x] GPU timers per pass in ImGui (`Renderer/GpuProfiler.h`; View > GPU timings, `render.timings`)
- [ ] Async HDRI loading and baking (the current bake blocks a frame, which would freeze the chat UI)
- [x] Golden-image tests: headless render of reference scenes compared against saved PNGs
      (`tests/golden_tests.c`, label `gpu`). Not in CI: there is none yet, and it needs a GPU.

### With the ECS / AI work

- [x] Mouse picking (enables "make *this* red"): click in the Viewport (`world_pick()`)
- [x] Gizmos ([ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)): move, rotate and scale what
      the AI placed (W/E/R, local/world, snapping, one undo step per drag)
- [x] Outliner and inspector panels (driven by the reflection table): Entities and Inspector
- [ ] Asset browser with thumbnails, from the catalog
- [ ] Fly camera alongside the orbit camera
- [ ] Frustum culling (from `Bounds`)
- [ ] Instancing for repeated assets
- [ ] Transparency sorting across all entities, not just within one model
- [ ] AI: selection and camera context sent automatically to the in-app agent (picking now
      exists, so "make *this* red" just needs the selection passed along)
- [ ] AI: lighting presets `set_lighting_mood("sunset")` (sun, color, HDRI, exposure)
- [ ] AI: scene templates (studio, outdoor daylight, product shot)
- [ ] AI: camera shots (close-up, wide, rule-of-thirds framing)
- [ ] AI: per-turn diff summary, shown in the chat panel and fed back to the model
- [x] Hot reload for every shader, not just `pbr.*` (`Renderer/ShaderReload.h`): tone mapping and
      shadow depth rebuild in place; an IBL bake or skybox shader re-bakes the environment
- [x] In-app log window (shader errors visible in the UI): View > Log, opens by itself on an
      error (`UI/LogPanel.h`, `log_set_sink()`); `log.get` for scripts and the agent

### Later

- [x] AgX / Khronos PBR Neutral tone mapping: the scene environment's `tonemap` (ACES stays the
      default; the Scene panel, `render.set`)
- [x] Auto-exposure: the scene environment's `auto_exposure` (off by default; `exposure` is then
      compensation). Measured on the GPU, adapts over about a second, Krawczyk's key, +-4 EV
- [x] SSAO: a depth pre-pass, then hemisphere SSAO from depth (`ssao.frag`, 4 x 4 tiled
      rotations cancelled by an edge-aware blur), applied to ambient light only. The scene
      environment's `ssao` (on by default) and `ssao_radius`. About 4.4 ms at full resolution
      on the sample scene (2.0 pre-pass, 2.4 SSAO): half-resolution SSAO would cut most of it
- [x] TAA: Halton (2, 3) sub-pixel jitter, reprojection through depth (camera motion; moving
      objects aren't tracked), variance clipping in YCoCg, brightness-weighted blend
      (`taa.frag`). Anti-aliasing is now one setting, off / fxaa (default) / taa: View menu,
      `render.set`'s `antialiasing`, the game's `--antialiasing`
- [ ] Per-object motion vectors for TAA (`Spin` and other moving entities)
- [ ] Rendering: GTAO, height or volumetric fog
- [x] Shadows for spot lights (`Renderer/SpotShadow.h`): up to 4, perspective maps in one
      2 x 2 depth atlas (one texture unit), casters culled to each light's range. Lights have a
      `cast_shadows` field (on by default), which the sun's cascades respect too
- [x] Shadows for point lights (`Renderer/PointShadow.h`): up to 4, six 90-degree faces each in
      one depth atlas (the last of the 16 guaranteed texture units); pbr.frag picks the face
      and projects into it itself. Spot and point maps cull casters to each view's frustum
- [x] Cache local light shadow maps while nothing in a light's range moves
      (`Renderer/ShadowCache.h`; moving casters drawn over a kept static layer)
- [ ] Texture arrays (or bindless textures) in the RHI: every guaranteed texture unit is used now.
      Cube map arrays exist (`rhi_cubemap_array_create()`, for the probes); 2D arrays don't yet
- [x] Local reflection probes with parallax correction (`Renderer/ReflectionProbes.h`): a
      `ReflectionProbe` component (a box), up to 8 baked into one cube map array (two bounces),
      box-projected in pbr.frag, full strength inside the box and fading over 0.5 m outside.
      Re-baked when a probe or the environment changes, or on `render.bake_reflections`
- [ ] Screen-space reflections
- [ ] Materials: `KHR_texture_transform` and a second UV set, clearcoat,
      transmission/IOR/volume, sheen, anisotropy, iridescence
- [ ] glTF skinning and animation, morph targets
- [x] Clustered forward lighting (`Renderer/Lighting.h`): 16 x 9 x 24 clusters (exponential depth),
      lights assigned on the CPU each frame, three storage buffers; up to 1024 lights (was 8).
      `debug_unclustered` / the game's `--unclustered` is the reference (200 lights: identical);
      the Lights debug view is a heatmap of lights per pixel
- [ ] Tighter light-to-cluster test (sphere against each cluster's box, not the sphere's screen
      rectangle): dense scenes still give many clusters 32+ lights
- [x] Per-frame data in UBOs (removes `snprintf` uniform names in `lighting_bind`): pbr.frag's
      LightBlock and ShadowBlock, std140 copies in `Renderer/FrameData.h` checked by static
      asserts; `rhi_uniform_buffer_create()` / `rhi_uniform_buffer_bind()`
- [ ] RenderDoc capture hotkey
- [ ] AI: `generate_asset(prompt)` behind the catalog interface
