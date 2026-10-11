#pragma once

#include "../Core/Defines.h"
#include "Camera.h"
#include "Light.h"
#include "Model.h"

#include <cglm/cglm.h>

// A flat list of placed things -- models, lights, cameras -- each with its
// own local transform and an optional parent. Lets an app build a scene out
// of model_load()'d models and light_init_*()'d lights instead of wiring
// loose fields together by hand every frame (which is what testbed/main.c
// did before this: one scene_model, one light_dir/light_color pair, one
// camera, all separate struct members with no shared representation).
//
// Deliberately a flat array + parent index, not a tree of heap nodes:
// cache-friendly to walk in scene_update_world_transforms() / the
// scene_draw_* helpers below, and entities are never moved once added
// (index-stable for the scene's lifetime), so a parent index stays valid
// for as long as the scene exists. There is no entity removal yet --
// see the note on scene_destroy() below.
//
// Ownership: a scene entity BORROWS the model/camera pointer passed to
// scene_add_model() / scene_add_camera(), same as model::default_material
// borrows nothing and model::textures owns everything -- there's no single
// rule, so each type's doc comment says which. Lights are stored by value
// (light is a small POD, nothing to own). Callers are still responsible for
// model_load()/model_destroy() and camera_init() themselves; the scene only
// records where those things sit.

typedef enum entity_kind
{
    ENTITY_MODEL,
    ENTITY_LIGHT,
    ENTITY_CAMERA
} entity_kind;

#define ENTITY_NO_PARENT (-1)
#define ENTITY_NAME_MAX  64

typedef struct entity
{
    char        name[ENTITY_NAME_MAX];   // for editor/debug UI; not looked up by name elsewhere yet
    entity_kind kind;

    mat4  local_transform;   // relative to `parent` (or to world, if parent == ENTITY_NO_PARENT)
    mat4  world_transform;   // cached by scene_update_world_transforms(); stale/zeroed until first called
    int32 parent;            // index into scene::entities, or ENTITY_NO_PARENT

    union
    {
        model*  as_model;    // ENTITY_MODEL  -- borrowed; scene never calls model_destroy()
        light   as_light;    // ENTITY_LIGHT  -- owned by value
        camera* as_camera;   // ENTITY_CAMERA -- borrowed; scene never frees it
    };
} entity;

typedef struct scene
{
    entity* entities;
    uint32  entity_count;
    uint32  entity_capacity;   // entities[] allocation size; grows geometrically, see Scene.c

    int32 active_camera;   // index into entities, or ENTITY_NO_PARENT if none set
} scene;

void scene_init(scene* s);

// Every scene_add_* appends one entity and returns its index (stable for
// the scene's lifetime -- usable as a future `parent` argument, or to look
// the entity up again via &s->entities[index]). `local_transform` may be
// NULL for identity. Returns ENTITY_NO_PARENT if `parent` is out of range
// (a bad index is a caller bug -- logged, not fatal, so a demo app doesn't
// crash over it).
int32 scene_add_model(scene* s, model* m, const mat4 local_transform, int32 parent);
int32 scene_add_light(scene* s, light l, const mat4 local_transform, int32 parent);
int32 scene_add_camera(scene* s, camera* cam, const mat4 local_transform, int32 parent);

// Marks `index` as the camera scene-level code should render from. Logs a
// warning and leaves active_camera unchanged if entities[index] is not an
// ENTITY_CAMERA (or index is out of range).
void scene_set_active_camera(scene* s, int32 index);

// Recomputes every entity's world_transform from its local_transform and
// its parent chain: world = parent.world * local, or world = local when
// parent == ENTITY_NO_PARENT. Call once per frame before reading any
// world_transform (directly, or via the scene_draw_* helpers below).
//
// Entities are resolved in storage order, so a parent's world_transform is
// only guaranteed up to date for children added AFTER it -- add parents
// before their children. (Not enforced here: a forward reference just
// reads that parent's previous frame's world_transform instead of this
// frame's, which self-corrects one frame later -- not worth a topological
// sort for what is, today, never more than two or three levels deep.)
void scene_update_world_transforms(scene* s);

// Draws every ENTITY_MODEL in the scene through `shader`, which must
// already be bound with every uniform model_draw() itself doesn't set
// (view-projection, lighting, IBL, shadow map -- see Model.h's model_draw()
// contract). Skips entities added via scene_add_model(s, NULL, ...).
// Requires scene_update_world_transforms() to have been called this frame.
void scene_draw_models(const scene* s, rhi_shader shader);

// Same, but through model_draw_depth() for a shadow pass -- see Model.h.
void scene_draw_models_depth(const scene* s, rhi_shader shader, const mat4 light_view_projection);

// Number of ENTITY_LIGHT entities currently in the scene -- size
// out_lights/out_world_positions for scene_gather_lights() with this.
uint32 scene_count_lights(const scene* s);

// Copies every ENTITY_LIGHT's light data into out_lights (up to
// max_lights), with `direction` rotated from the entity's local space into
// world space for LIGHT_DIRECTIONAL/LIGHT_SPOT, and the matching entry of
// out_world_positions set to that entity's world-space position (the
// translation column of world_transform) for LIGHT_POINT/LIGHT_SPOT --
// meaningless and left untouched for LIGHT_DIRECTIONAL. out_world_positions
// may be NULL if the caller only wants directions/colors.
//
// This does not itself feed a shader -- Renderer/Lighting.h's
// lighting_bind() builds on it to upload pbr.frag's u_lights[].
// Requires scene_update_world_transforms() to have been called this frame.
uint32 scene_gather_lights(const scene* s, light* out_lights, vec3* out_world_positions, uint32 max_lights);

// Frees the entities array. Does NOT free models/cameras pointed to by
// ENTITY_MODEL/ENTITY_CAMERA entries -- the scene borrowed them (see the
// ownership note above); destroy those separately, same as callers already
// do without a scene. Safe to call on a zero-initialized scene.
void scene_destroy(scene* s);
