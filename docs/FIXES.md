# Fixes

Bugs and design problems found in a code review (2026-10-13). Ordered by priority: 1-3 share a
root cause -- edits that bypass, or don't own, the command/history layer -- so fix them first.

## Bugs

- [ ] **1. Editor GUI writes the World directly, so undo clobbers entities or duplicates names.**
      `load_selected_model`, `set_model_spin`, the Lights panel, the grid albedo picker and the
      "Ground plane" checkbox (`editor/main.c`) change the World without commands or history.
      Reproduced:
      - destroy `model` -> GUI recreates it -> Ctrl+Z: the undo revives the old entity *on top of*
        the new one (same slot); the editor's `model_entity` ID goes stale.
      - destroy `model` -> create `box` -> GUI recreates `model` -> undo, undo: **two entities named
        "model"**. The Inspector (`UI/Inspector.c` `set_field`), the gizmo and `frame_selection`
        address entities by name, so `component.set` then edits the wrong one.

      Quick fix: pass the numeric entity id (commands accept it) instead of `world_name()`.
      Real fix: route every GUI edit through commands. (Since Stage 2 the GUI does, and the
      live link is gone.)

- [ ] **2. One anonymous transaction stack is shared by every editor.**
      - A client that sends `history.begin` and disconnects is never rolled back (`drop_client`,
        `Net/CommandServer.c`): Ctrl+Z then fails with "a transaction is open", "Undo turn" is
        disabled, and all later edits pile into that transaction.
      - A chat turn holds a transaction open (`chat_send`, `Chat/ChatSession.c`); Inspector/gizmo
        edits made meanwhile fold into it, so **"Undo turn" also reverts the user's own edits**.
      - Overlapping (not nested) begin/commit pairs record steps under the wrong label.

      Fix: give each transaction an owner (connection, chat turn, UI drag); roll it back when the
      owner goes away; keep user edits out of an agent's open turn.

- [x] **3. Network commands go to whichever world is on screen.** Gone with Projects (Stage 2):
      the editor has one world, the scene being edited; opening another scene is refused while a
      chat turn runs, and replaces the history, so no turn spans two scenes.
      Original report:
      `server_poll(state->server, active_commands(state))` in `on_frame`: switching Lit/Grid during
      an agent turn sends its commands to the grid world while the turn's transaction is on the lit
      history. Both worlds hand out the same IDs (slot 0 = `1048576` in each), so an agent's ID can
      name a different entity -- breaking the "IDs are safe to hand to an LLM" promise in
      `World/World.h`. Fix: pin the server and chat to one world.

- [x] **4. Testbed leaks models in `--live` mode.** Gone: the Testbed and `--live` were removed
      (the game loads one scene; in-editor Play replaced the live link). Original report: it never
      called `assets_collect_unused()`, so every model the editor swapped out stayed loaded.

- [ ] **5. Shortcuts follow physical key positions.** GLFW names keys by their US-layout position, so
      Ctrl+Z on AZERTY arrives as `GLFW_KEY_W` (same for W/E/R). Use ImGui's layout-aware checks
      (`igIsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)`).

## Gizmo (uncommitted work)

- [ ] Addresses the entity by name -- same as bug 1.
- [ ] Dragging scale past Transform's max (10000) fails `component.set` and prints a console line
      every frame of the drag. Clamp to the reflected range, or stop on the first failure.
- [ ] (Known, acceptable) Rotating rewrites all three Euler angles in canonical form (270 -> -90);
      under a parent with non-uniform scale the decomposition is approximate.

## Minor / robustness

- [ ] `resolve()` (`World/World.c`) recurses forever on a parent cycle, and `world_apply_states()`
      writes parents unchecked. Set the update stamp before recursing.
- [ ] Numeric entity ids are cast from `double` with no upper bound -- undefined behaviour for huge
      values (`arg_entity` in `Commands/Commands.c`, `parse_entity_ref` in `World/Serialize.c`).
- [ ] `view.frame` accepts only names although its argument is typed "entity" (`host_view_frame`).
- [ ] `batch` is all-or-nothing only for entities: `environment.set`, `view.*`, `render.set` and a
      `scene.load`'s environment inside a failed batch aren't rolled back.
- [ ] Command server accepts `Origin: null` (sent by sandboxed iframes / `file://`); the token still
      protects it. A client that stops reading makes its output buffer grow without limit.
- [ ] `state->server` is left dangling after `server_stop()` in the editor's `on_shutdown`; set it
      to NULL.
- [ ] `oriented_bounds()` (`Commands/Commands.c`) duplicates the now-public `transform_to_matrix()`.

## Architecture (when it starts to hurt)

- [ ] `editor/main.c` is ~2000 lines mixing camera, gizmo, two worlds, host commands and agent/game
      process management. Split it once bug 1 moves the GUI onto commands.
- [ ] Every mutating command snapshots and diffs the whole world (`cmd_execute`); Inspector/gizmo
      drags do it every frame. Fine at today's scene sizes, the bottleneck as they grow.
- [ ] 12-bit generations wrap after 4095 reuses of a slot; a stale ID can then match a live entity.
- [ ] `run()` / `terminate()` (`App.h`) are very generic global names for a library that now
      contains C++; consider `app_run()` / `app_terminate()`.
