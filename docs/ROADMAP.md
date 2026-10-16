# Roadmap

Two tracks: the **AI scene builder** (describe a scene in chat, from Claude Desktop/Code or from a
chat panel inside the app, and have it built) and the **renderer/editor features** around it.
The AI track drives the order; renderer features slot in where they help it.

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
Lit Model and Grid render as before; the ground is now a regular entity.

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
reconnect), `tools.py` (24 tools, examples run against the engine as a drift test),
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

 ### ^^^^ Done ^^^^

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
2. **Placement helpers in C:** `on_ground`, `relative_to` (left/right/front/behind/on_top), placement
   without overlaps, "frame all" for the camera.
3. **Verify loop:** screenshot after multi-step turns, then check for floating objects, overlaps,
   darkness and objects out of frame. This is where LangGraph starts to pay off
   (plan → build → verify → fix); before it, a plain tool-use loop is enough.
4. ~~Scene files: `scene.save`/`scene.load` as JSON.~~ Done in Phase 2.
5. A `fetch_asset` tool backed by the Poly Haven API (models and HDRIs).

---

## Editor / runtime split

The editor (`editor/`) and the game runtime (`testbed/`) are separate apps on one engine library.
Scenes carry their environment, and `Spin` components run only in the game (`world_tick()`).

### Live link: first version, to replace

F5 in the editor starts `Testbed --live`, which follows the editor through a shared scene file
(`World/LiveScene.h`). It works, but it is a stopgap:

- Whole-world sync: every edit re-serializes and reloads every entity (cost grows with the scene)
- The game's runtime state is rebuilt on every edit; only `Spin` poses are kept, matched by name
- Entity IDs don't survive a reload; one-way only; up to ~0.2 s latency; one fixed file

Next steps:

- [ ] In-editor Play mode: Play copies the world and ticks the copy in the viewport, Stop restores
      the edited world; pause and step. Becomes the main way to test while editing
- [ ] Panels edit through commands (so every edit is an undoable change event), instead of
      writing components directly
- [ ] Separate-process live editing by incremental changes over the command server (the game
      connects as a client and applies `scene.changed` events), replacing the file link
- [ ] Split the build: a runtime library without ImGui/Chat/Net/Commands for the game, the editor
      on top (same `engine/` folder)

## Projects, viewport panel and game export

Goal: create a project, edit it in the editor, render the scene into an ImGui viewport panel with
Play / Stop / Export on its toolbar, and export the game as a folder that runs on another machine.

Blocker today: `ASSETS_ROOT` (`Core/Paths.h`) is an absolute path compiled into every binary, so
the editor can't switch projects and an exported game only runs on the machine that built it.

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

- [ ] Project format: a folder with `project.json` (name, startup scene, window size, default
      HDRI) plus `scenes/`, `models/`, `hdri/`, `textures/`
- [ ] New project: create the folder skeleton, an empty startup scene (ground, sun, camera) and
      `project.json`. Also an `editor.project.new` command, so the agent can do it
- [ ] Open project: a File menu (New / Open / Recent), recent projects in `~/.pbr-renderer/`,
      the project name in the window title; `Editor --project <dir>` on the command line
- [ ] Edit within the project: the model and HDRI pickers list the project's files instead of the
      hard-coded `MODEL_PATHS` / `HDRI_PATHS`; `scene.save/load/list` work in the project's
      `scenes/`; the current scene's name in the UI
- [ ] Save (Ctrl+S) / Save As, an unsaved-changes marker (from the revision counter), a prompt
      on close or when switching scenes/projects
- [ ] Import asset: copy a `.glb` / `.gltf` (with its files) / `.hdr` into the project
- [ ] The current demo worlds (lit model, material grid) become scenes in a sample project

### Stage 3: Viewport panel

- [ ] Render the scene to an offscreen color texture and show it in a dockable ImGui "Viewport"
      window (`igImage`); resize the render targets and the camera aspect to the panel, not the
      window
- [ ] Input from the panel: orbit, zoom, click-to-pick and the gizmo use panel-local coordinates
      and only act while the panel is hovered (they use the main viewport today); the selection
      outline is drawn in the panel
- [ ] Toolbar along the panel's top: Play / Pause / Step / Stop, Export, and the gizmo
      move/rotate/scale and local/world toggles (moved from the Scene panel)
- [ ] Play from the toolbar runs the in-editor Play mode (see "Editor / runtime split" above:
      copy the world, tick the copy, restore on Stop), with a coloured border on the panel while
      playing. Options: view through the scene's Camera entity; play in a separate window
      (today's F5 Testbed)

### Stage 4: Game export

- [ ] The runtime plays a project: `Testbed --project <dir>`, or with no arguments the
      `project.json` next to the executable; opens the startup scene, window title and size from
      the project
- [ ] A shippable runtime: a Release build of the game, linked without the editor parts (needs
      the runtime library split above), no console window on Windows (WIN32 subsystem), named
      after the project. The editor finds a prebuilt one beside itself
- [ ] Export (toolbar button and `editor.export` command): choose an output folder, then copy
      the runtime executable and the DLLs it needs, the engine shaders, `project.json`, the
      scenes, and only the assets those scenes reference (walk every MeshRenderer model and the
      environment HDRI). Report the size and any missing files; refuse when a scene points
      outside the project
- [ ] Check the export: run the exported game headless on the startup scene and screenshot it
      (reuses golden-image tests), so a broken export fails in the editor and not on a player's
      machine
- [ ] Later: zip the export; cache baked IBL maps in the export so the game doesn't bake at
      startup; scene switching in the game (a component or a startup list)

## Feature roadmap

### Now: small, high value

- [ ] FXAA in `tonemap.frag`
- [ ] Bloom (between the scene pass and `post_end_scene()`)
- [ ] `KHR_materials_emissive_strength`
- [ ] Debug views: albedo, normals, roughness/metal, AO, shadow only, lighting only, wireframe
- [ ] GPU timers per pass in ImGui
- [ ] Async HDRI loading and baking (the current bake blocks a frame, which would freeze the chat UI)
- [ ] Golden-image tests: headless render of reference scenes compared against saved PNGs, in CI

### With the ECS / AI work

- [ ] Mouse picking (enables "make *this* red")
- [ ] Gizmos ([ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)): move, rotate and scale what
      the AI placed
- [ ] Outliner and inspector panels (driven by the reflection table)
- [ ] Asset browser with thumbnails, from the catalog
- [ ] Fly camera alongside the orbit camera
- [ ] Frustum culling (from `Bounds`)
- [ ] Instancing for repeated assets
- [ ] Transparency sorting across all entities, not just within one model
- [ ] AI: selection and camera context sent automatically to the in-app agent
- [ ] AI: lighting presets `set_lighting_mood("sunset")` (sun, color, HDRI, exposure)
- [ ] AI: scene templates (studio, outdoor daylight, product shot)
- [ ] AI: camera shots (close-up, wide, rule-of-thirds framing)
- [ ] AI: per-turn diff summary, shown in the chat panel and fed back to the model
- [ ] Hot reload for every shader, not just `pbr.*`
- [ ] In-app log window (shader errors visible in the UI)

### Later

- [ ] Rendering: TAA (also cleans up shadow/SSAO noise), SSAO/GTAO, auto-exposure,
      AgX / Khronos PBR Neutral tone mapping, height or volumetric fog
- [ ] Shadows for point lights (cube maps) and spot lights (perspective maps)
- [ ] Screen-space reflections, local reflection probes with parallax correction
- [ ] Materials: `KHR_texture_transform` and a second UV set, clearcoat,
      transmission/IOR/volume, sheen, anisotropy, iridescence
- [ ] glTF skinning and animation, morph targets
- [ ] Clustered forward lighting (once scenes have dozens of lights)
- [ ] Per-frame data in UBOs (removes `snprintf` uniform names in `lighting_bind`)
- [ ] RenderDoc capture hotkey
- [ ] AI: `generate_asset(prompt)` behind the catalog interface
