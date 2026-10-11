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

### Phase 4: Python package `pbr_agent` + MCP

- `engine_client.py`: async JSON-RPC WebSocket client.
- `tools.py`: the single tool registry (name, description, schema, engine call).
- `mcp_server.py`: the official MCP Python SDK (FastMCP) over stdio; register it in Claude Desktop's
  config or with `claude mcp add`. Also expose `scene.describe` as an MCP resource.
- A shared system prompt with the scene conventions: meters, Y up, ground at y=0, naming rules,
  describe before editing, screenshot after big changes.

**Done when:** "add a red sphere left of the avocado" in Claude Desktop appears in the running app.

> The claude.ai website only reaches **remote** MCP servers over HTTPS (it needs a tunnel plus
> authentication). Claude Desktop and Claude Code run local stdio servers directly; start there.

### Phase 5: In-app chat panel

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

### Phase 6: Quality

1. **Asset catalog** `assets/catalog.json`: ID, description, tags, real-world size in meters,
   up axis, pivot. Normalize to meters on import.
2. **Placement helpers in C:** `on_ground`, `relative_to` (left/right/front/behind/on_top), placement
   without overlaps, "frame all" for the camera.
3. **Verify loop:** screenshot after multi-step turns, then check for floating objects, overlaps,
   darkness and objects out of frame. This is where LangGraph starts to pay off
   (plan → build → verify → fix); before it, a plain tool-use loop is enough.
4. Scene files: `scene.save`/`scene.load` as JSON.
5. A `fetch_asset` tool backed by the Poly Haven API (models and HDRIs).

---

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
