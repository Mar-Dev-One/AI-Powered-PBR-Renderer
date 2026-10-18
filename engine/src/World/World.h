#pragma once

#include "../Core/Defines.h"
#include "../Assets/AssetCache.h"
#include "../Scene/Camera.h"
#include "Components.h"

#include <cglm/cglm.h>

// A set of entities, each an ID with a name, a transform and any of the
// optional components in Components.h. Replaces the old Scene/Scene.h,
// whose entities were exactly one of model / light / camera, were never
// removed, and were addressed by raw array index.
//
// IDs: an entity_id packs a slot index with that slot's generation. Destroying
// an entity bumps the generation, so an ID kept past its entity's lifetime
// is recognised as stale -- every function below checks, and fails safely
// (NULL / false / no-op), instead of touching whatever reused the slot. That
// is what makes it safe to hand IDs to tools and to an LLM across turns.
//
// Storage: one array per component type plus a presence bitmask per slot.
// Component pointers returned below stay valid until the next
// world_create() (which may grow the arrays); don't keep them across it.
//
// Every entity always has COMPONENT_NAME and COMPONENT_TRANSFORM. Names are
// unique: world_create() makes a requested name unique by appending _2,
// _3, ... rather than failing.
//
// Rendering is not done here -- Renderer/SceneRenderer.h walks the World's
// MeshRenderer and Light entities.

typedef struct world
{
    uint32  capacity;
    uint32* generations;
    uint32* masks;           // COMPONENT_BIT()s, plus WORLD_ALIVE_BIT for live slots
    uint32* free_list;
    uint32  free_count;
    uint32  alive_count;

    c_name*          names;
    c_transform*     transforms;
    c_parent*        parents;
    c_mesh_renderer* mesh_renderers;
    c_light*         lights;
    c_camera*        cameras;
    c_spin*          spins;
    c_reflection_probe* reflection_probes;

    // Derived by world_update().
    mat4*   world_matrices;
    vec3*   bounds_min;      // world-space AABB; only meaningful for MeshRenderer entities with a loaded model
    vec3*   bounds_max;
    uint32* update_stamps;   // which world_update() last resolved each slot
    uint32  current_stamp;

    asset_cache* assets;     // borrowed; must outlive the World
} world;

void world_init(world* w, asset_cache* assets);

// Destroys every entity (releasing their assets) and frees the storage.
void world_shutdown(world* w);

// Makes `dst` (uninitialized, or shut down) an exact copy of `src`: same
// slots, generations and free list, so every entity ID names the same
// entity in both, and the next world_create() hands out the same ID in
// each. Shares src's asset cache and retains every model the copy uses.
// For the editor's Play mode, which ticks a copy and throws it away.
void world_copy(world* dst, const world* src);

// --- Entities ---------------------------------------------------------------

// New entity with a Name and an identity Transform (scale 1). `name` may be
// NULL ("entity"); a name already in use gets a _2, _3, ... suffix.
entity_id world_create(world* w, const char* name);

// Destroys `e` and, recursively, every entity parented to it. Releases
// MeshRenderer assets. No-op for a stale ID.
void world_destroy(world* w, entity_id e);

void   world_clear(world* w);   // destroys every entity
b8     world_is_alive(const world* w, entity_id e);
uint32 world_count(const world* w);

const char* world_name(const world* w, entity_id e);   // NULL if stale
entity_id   world_find(const world* w, const char* name);   // ENTITY_NULL if none

// False (and no change) if `name` is empty, too long or used by another entity.
b8 world_rename(world* w, entity_id e, const char* name);

// --- Components -------------------------------------------------------------

b8 world_has(const world* w, entity_id e, component_type type);

// Removes an optional component. Name and Transform can't be removed.
// Removing MeshRenderer releases its asset; removing Parent moves the entity
// to the root.
void world_remove(world* w, entity_id e, component_type type);

// Typed access. Each returns NULL if `e` is stale or lacks the component.
// They take a const world (like strchr) so read-only code -- renderers --
// can use them too; writing through the result from such code is a bug.
c_transform*     world_transform(const world* w, entity_id e);
c_mesh_renderer* world_mesh_renderer(const world* w, entity_id e);
c_light*         world_light(const world* w, entity_id e);
c_camera*        world_camera(const world* w, entity_id e);
c_spin*          world_spin(const world* w, entity_id e);
c_reflection_probe* world_reflection_probe(const world* w, entity_id e);
entity_id        world_parent(const world* w, entity_id e);   // ENTITY_NULL if unparented or stale

// Add (or replace) a component and return it, NULL if `e` is stale.
// world_add_mesh_renderer() retains `model` (ASSET_NONE is allowed: it
// draws nothing); visible and cast_shadows default to true.
c_mesh_renderer* world_add_mesh_renderer(world* w, entity_id e, asset_id model);
c_light*         world_add_light(world* w, entity_id e, light l);
c_camera*        world_add_camera(world* w, entity_id e, f32 fov_degrees, f32 near_plane, f32 far_plane);
c_spin*          world_add_spin(world* w, entity_id e, const vec3 degrees_per_second);
c_reflection_probe* world_add_reflection_probe(world* w, entity_id e, const vec3 extents);

// Swaps a MeshRenderer's model, keeping reference counts right. False if
// `e` is stale or has no MeshRenderer.
b8 world_set_model(world* w, entity_id e, asset_id model);

// Parents `child` to `parent` (ENTITY_NULL = move to the root). False, and
// no change, if either is stale or `parent` is `child` or one of its
// descendants. The child keeps its local transform, so it moves with the
// new parent from the next world_update().
b8 world_set_parent(world* w, entity_id child, entity_id parent);

// --- Derived state ---------------------------------------------------------

// Recomputes every entity's world matrix (parent chains included) and every
// MeshRenderer's world-space bounds. Call once per frame after changing
// transforms and before rendering or gathering lights.
void world_update(world* w);

// Advances the behaviour components by `dt` seconds -- today, Spin turns
// rotation_deg (kept within +-360). For a running game: the game runtime
// calls it once per frame before world_update(); the editor doesn't (it
// previews Spin only while drawing), so its scenes stay as authored.
void world_tick(world* w, f32 dt);

// World matrix as of the last world_update(); NULL if stale.
const vec4* world_matrix(const world* w, entity_id e);

// World-space AABB as of the last world_update(). False if `e` has no
// MeshRenderer with a loaded model.
b8 world_bounds(const world* w, entity_id e, vec3 out_min, vec3 out_max);

// What a camera should frame to show the scene, as of the last
// world_update(): the bounds of every visible MeshRenderer -- leaving out
// flat ones (ground planes, floors: under WORLD_FLAT_RATIO as tall as they
// are wide), which would shrink everything else to dots, unless they're all
// there is. False if nothing visible has bounds.
#define WORLD_FLAT_RATIO 0.02f
b8 world_framing_bounds(const world* w, vec3 out_min, vec3 out_max);

// A Transform as a matrix and back: local = T * Ry * Rx * Rz * S (Euler
// degrees applied yaw, pitch, roll -- see Components.h). transform_from_matrix()
// is the exact inverse for any matrix a Transform can produce (no shear,
// positive scale): rotation comes back with pitch in [-90, 90] and yaw/roll
// in (-180, 180]. For gizmos, which hand back a whole matrix.
void transform_to_matrix(const c_transform* t, mat4 out);
void transform_from_matrix(const mat4 m, c_transform* out);

// The entity a ray (origin + t * direction, t >= 0; direction normalized)
// hits first, as of the last world_update(): a visible MeshRenderer by its
// world bounds, or -- since they have no surface to click -- a Light or
// Camera without bounds by a sphere of `marker_radius` around its position.
// A box the ray starts inside (a room around the camera) doesn't count.
// ENTITY_NULL if nothing is hit; *out_distance (may be NULL) gets t.
entity_id world_pick(const world* w, const vec3 origin, const vec3 direction, f32 marker_radius, f32* out_distance);

// Every Light entity's light, with `direction` rotated into world space
// (normalized) and, if out_positions is non-NULL, its world position.
// Storage order, up to `max`. Returns how many were written.
uint32 world_gather_lights(const world* w, light* out_lights, vec3* out_positions, uint32 max);

// Fills `out` from a Camera entity's world transform and settings. False
// if `e` is stale or has no Camera.
b8 world_camera_view(const world* w, entity_id e, f32 aspect, camera* out);

// --- Raw state (undo/redo) ---------------------------------------------------
// Everything about one storage slot, live or not. The command layer's history
// (Commands/History.h) records these before and after each change, and
// restores them exactly -- same slot, same generation -- so IDs handed out
// before an undo still name the same entity after it.

typedef struct entity_state
{
    entity_id       id;      // slot + generation (for a dead slot: the generation it will hand out next)
    b8              alive;
    uint32          mask;    // COMPONENT_BIT()s
    c_name          name;
    c_transform     transform;
    c_parent        parent;
    c_mesh_renderer mesh_renderer;
    c_light         light;
    c_camera        camera;
    c_spin          spin;
    c_reflection_probe reflection_probe;
} entity_state;

// Number of slots world_capture_states() writes.
uint32 world_slot_count(const world* w);

// Copies every slot's state into `out` (world_slot_count() entries).
void world_capture_states(const world* w, entity_state* out);

// The state a slot past the end of the storage is in: dead, generation 1.
void world_empty_state(uint32 slot, entity_state* out);

// True if the two states differ in anything observable; *out_components
// (may be NULL) gets the COMPONENT_BIT()s whose presence or data differ.
b8 entity_state_differs(const entity_state* a, const entity_state* b, uint32* out_components);

// Puts each listed slot into its recorded state: revives it (with its
// recorded ID) or kills it, writes every component, keeps MeshRenderer
// reference counts right. A killed slot's generation still moves forward,
// so its old ID stays stale rather than being handed out again.
// The states must describe a consistent world together (as states captured
// from one moment do) -- names and parents are written as recorded, without
// the uniqueness/cycle checks the normal API applies. Call world_update()
// afterwards.
void world_apply_states(world* w, const entity_state* states, uint32 count);

// --- Iteration --------------------------------------------------------------

// Next live entity after `previous` (ENTITY_NULL = start) having every
// component in `required_mask` (0 = any). ENTITY_NULL when done:
//   for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
entity_id world_next(const world* w, entity_id previous, uint32 required_mask);
