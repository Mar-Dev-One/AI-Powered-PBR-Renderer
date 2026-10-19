#include "World.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Set in masks[] for live slots, above every COMPONENT_BIT().
#define WORLD_ALIVE_BIT (1u << 31)

#define WORLD_INITIAL_CAPACITY 64

static uint32    entity_generation(entity_id e)          { return e >> ENTITY_INDEX_BITS; }
static entity_id make_entity(uint32 index, uint32 gen)   { return (gen << ENTITY_INDEX_BITS) | index; }

// Slot index of `e` if it is alive, else UINT32_MAX.
static uint32
live_index(const world* w, entity_id e)
{
    if (e == ENTITY_NULL)
        return UINT32_MAX;

    uint32 index = entity_slot(e);
    if (index >= w->capacity)
        return UINT32_MAX;

    if (!(w->masks[index] & WORLD_ALIVE_BIT) || w->generations[index] != entity_generation(e))
        return UINT32_MAX;

    return index;
}

static void*
grow_array(void* array, uint32 new_capacity, size_t element_size)
{
    void* grown = realloc(array, (size_t)new_capacity * element_size);
    if (!grown)
        FATAL("world: out of memory");
    return grown;
}

static void
grow(world* w)
{
    uint32 old_capacity = w->capacity;
    uint32 new_capacity = old_capacity ? old_capacity * 2 : WORLD_INITIAL_CAPACITY;

    if (new_capacity > ENTITY_INDEX_MASK + 1)
        FATAL("world: more than %u entities", ENTITY_INDEX_MASK + 1);

    w->generations    = grow_array(w->generations,    new_capacity, sizeof(*w->generations));
    w->masks          = grow_array(w->masks,          new_capacity, sizeof(*w->masks));
    w->free_list      = grow_array(w->free_list,      new_capacity, sizeof(*w->free_list));
    w->names          = grow_array(w->names,          new_capacity, sizeof(*w->names));
    w->transforms     = grow_array(w->transforms,     new_capacity, sizeof(*w->transforms));
    w->parents        = grow_array(w->parents,        new_capacity, sizeof(*w->parents));
    w->mesh_renderers = grow_array(w->mesh_renderers, new_capacity, sizeof(*w->mesh_renderers));
    w->lights         = grow_array(w->lights,         new_capacity, sizeof(*w->lights));
    w->cameras        = grow_array(w->cameras,        new_capacity, sizeof(*w->cameras));
    w->spins          = grow_array(w->spins,          new_capacity, sizeof(*w->spins));
    w->reflection_probes = grow_array(w->reflection_probes, new_capacity, sizeof(*w->reflection_probes));
    w->player_controllers = grow_array(w->player_controllers, new_capacity, sizeof(*w->player_controllers));
    w->scene_triggers     = grow_array(w->scene_triggers,     new_capacity, sizeof(*w->scene_triggers));
    w->world_matrices = grow_array(w->world_matrices, new_capacity, sizeof(*w->world_matrices));
    w->bounds_min     = grow_array(w->bounds_min,     new_capacity, sizeof(*w->bounds_min));
    w->bounds_max     = grow_array(w->bounds_max,     new_capacity, sizeof(*w->bounds_max));
    w->update_stamps  = grow_array(w->update_stamps,  new_capacity, sizeof(*w->update_stamps));

    for (uint32 i = old_capacity; i < new_capacity; ++i)
    {
        w->generations[i]   = 1;
        w->masks[i]         = 0;
        w->update_stamps[i] = 0;
    }

    // Push the new slots so the lowest index is popped first.
    for (uint32 i = new_capacity; i > old_capacity; --i)
        w->free_list[w->free_count++] = i - 1;

    w->capacity = new_capacity;
}

void
world_init(world* w, asset_cache* assets)
{
    memset(w, 0, sizeof(*w));
    w->assets = assets;
}

void
world_shutdown(world* w)
{
    world_clear(w);

    free(w->generations);
    free(w->masks);
    free(w->free_list);
    free(w->names);
    free(w->transforms);
    free(w->parents);
    free(w->mesh_renderers);
    free(w->lights);
    free(w->cameras);
    free(w->spins);
    free(w->reflection_probes);
    free(w->player_controllers);
    free(w->scene_triggers);
    free(w->world_matrices);
    free(w->bounds_min);
    free(w->bounds_max);
    free(w->update_stamps);

    memset(w, 0, sizeof(*w));
}

static void*
copy_array(const void* array, uint32 capacity, size_t element_size)
{
    void* copy = grow_array(NULL, capacity, element_size);
    memcpy(copy, array, (size_t)capacity * element_size);
    return copy;
}

void
world_copy(world* dst, const world* src)
{
    world_init(dst, src->assets);
    if (src->capacity == 0)
        return;

    dst->capacity      = src->capacity;
    dst->free_count    = src->free_count;
    dst->alive_count   = src->alive_count;
    dst->current_stamp = src->current_stamp;

    #define COPY(field) dst->field = copy_array(src->field, src->capacity, sizeof(*src->field))
    COPY(generations);
    COPY(masks);
    COPY(free_list);
    COPY(names);
    COPY(transforms);
    COPY(parents);
    COPY(mesh_renderers);
    COPY(lights);
    COPY(cameras);
    COPY(spins);
    COPY(reflection_probes);
    COPY(player_controllers);
    COPY(scene_triggers);
    COPY(world_matrices);
    COPY(bounds_min);
    COPY(bounds_max);
    COPY(update_stamps);
    #undef COPY

    // The copy holds its own references, released by its world_shutdown().
    for (uint32 i = 0; i < dst->capacity; ++i)
    {
        if ((dst->masks[i] & WORLD_ALIVE_BIT) && (dst->masks[i] & COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
            assets_retain(dst->assets, dst->mesh_renderers[i].model);
    }
}

// --- Names --------------------------------------------------------------------

// Index of the live entity called `name`, other than `except_index`, or
// UINT32_MAX.
static uint32
find_name(const world* w, const char* name, uint32 except_index)
{
    for (uint32 i = 0; i < w->capacity; ++i)
    {
        if (i != except_index && (w->masks[i] & WORLD_ALIVE_BIT) && strcmp(w->names[i].value, name) == 0)
            return i;
    }
    return UINT32_MAX;
}

// Writes `base` -- or base_2, base_3, ... if taken -- into `out`, truncating
// the base so the suffix always fits.
static void
make_unique_name(const world* w, const char* base, char out[ENTITY_NAME_MAX])
{
    if (!base || !base[0])
        base = "entity";

    snprintf(out, ENTITY_NAME_MAX, "%s", base);
    if (find_name(w, out, UINT32_MAX) == UINT32_MAX)
        return;

    for (uint32 n = 2;; ++n)
    {
        char suffix[16];
        int  suffix_length = snprintf(suffix, sizeof(suffix), "_%u", n);
        int  base_length   = (int)strlen(base);
        int  keep          = ENTITY_NAME_MAX - 1 - suffix_length;

        snprintf(out, ENTITY_NAME_MAX, "%.*s%s", base_length < keep ? base_length : keep, base, suffix);
        if (find_name(w, out, UINT32_MAX) == UINT32_MAX)
            return;
    }
}

const char*
world_name(const world* w, entity_id e)
{
    uint32 index = live_index(w, e);
    return index == UINT32_MAX ? NULL : w->names[index].value;
}

entity_id
world_find(const world* w, const char* name)
{
    if (!name)
        return ENTITY_NULL;

    uint32 index = find_name(w, name, UINT32_MAX);
    return index == UINT32_MAX ? ENTITY_NULL : make_entity(index, w->generations[index]);
}

b8
world_rename(world* w, entity_id e, const char* name)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX || !name || !name[0] || strlen(name) >= ENTITY_NAME_MAX)
        return false;

    if (find_name(w, name, index) != UINT32_MAX)
        return false;

    snprintf(w->names[index].value, ENTITY_NAME_MAX, "%s", name);
    return true;
}

// --- Entities -------------------------------------------------------------------

entity_id
world_create(world* w, const char* name)
{
    char unique[ENTITY_NAME_MAX];
    make_unique_name(w, name, unique);

    if (w->free_count == 0)
        grow(w);

    uint32 index = w->free_list[--w->free_count];

    w->masks[index] = WORLD_ALIVE_BIT | COMPONENT_BIT(COMPONENT_NAME) | COMPONENT_BIT(COMPONENT_TRANSFORM);
    memcpy(w->names[index].value, unique, ENTITY_NAME_MAX);

    c_transform* t = &w->transforms[index];
    glm_vec3_zero(t->position);
    glm_vec3_zero(t->rotation_deg);
    glm_vec3_one(t->scale);

    glm_mat4_identity(w->world_matrices[index]);
    w->update_stamps[index] = 0;

    w->alive_count++;
    return make_entity(index, w->generations[index]);
}

void
world_destroy(world* w, entity_id e)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return;

    // Children first. Destroying one can't create new ones, so a plain scan
    // is enough; each child's own recursion handles grandchildren.
    for (uint32 i = 0; i < w->capacity; ++i)
    {
        if ((w->masks[i] & WORLD_ALIVE_BIT) && (w->masks[i] & COMPONENT_BIT(COMPONENT_PARENT))
            && w->parents[i].parent == e)
        {
            world_destroy(w, make_entity(i, w->generations[i]));
        }
    }

    if (w->masks[index] & COMPONENT_BIT(COMPONENT_MESH_RENDERER))
        assets_release(w->assets, w->mesh_renderers[index].model);

    w->masks[index] = 0;
    w->generations[index] = (w->generations[index] + 1) & ENTITY_GEN_MASK;
    if (w->generations[index] == 0)
        w->generations[index] = 1;   // 0 would let a recycled slot produce ENTITY_NULL

    w->free_list[w->free_count++] = index;
    w->alive_count--;
}

void
world_clear(world* w)
{
    for (uint32 i = 0; i < w->capacity; ++i)
    {
        if (w->masks[i] & WORLD_ALIVE_BIT)
            world_destroy(w, make_entity(i, w->generations[i]));
    }
}

b8
world_is_alive(const world* w, entity_id e)
{
    return live_index(w, e) != UINT32_MAX;
}

uint32
world_count(const world* w)
{
    return w->alive_count;
}

// --- Components ---------------------------------------------------------------

b8
world_has(const world* w, entity_id e, component_type type)
{
    uint32 index = live_index(w, e);
    return index != UINT32_MAX && (w->masks[index] & COMPONENT_BIT(type));
}

void
world_remove(world* w, entity_id e, component_type type)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX || type == COMPONENT_NAME || type == COMPONENT_TRANSFORM)
        return;

    if (!(w->masks[index] & COMPONENT_BIT(type)))
        return;

    if (type == COMPONENT_MESH_RENDERER)
        assets_release(w->assets, w->mesh_renderers[index].model);

    w->masks[index] &= ~COMPONENT_BIT(type);
}

// Pointer to slot `index` of a component array, or NULL if `e` is stale or
// lacks `type`.
#define COMPONENT_GETTER(fn, type_name, array, component)              \
    type_name* fn(const world* w, entity_id e)                          \
    {                                                                   \
        uint32 index = live_index(w, e);                                \
        if (index == UINT32_MAX || !(w->masks[index] & COMPONENT_BIT(component))) \
            return NULL;                                                \
        return &w->array[index];                                        \
    }

COMPONENT_GETTER(world_transform,     c_transform,     transforms,     COMPONENT_TRANSFORM)
COMPONENT_GETTER(world_mesh_renderer, c_mesh_renderer, mesh_renderers, COMPONENT_MESH_RENDERER)
COMPONENT_GETTER(world_light,         c_light,         lights,         COMPONENT_LIGHT)
COMPONENT_GETTER(world_camera,        c_camera,        cameras,        COMPONENT_CAMERA)
COMPONENT_GETTER(world_spin,          c_spin,          spins,          COMPONENT_SPIN)
COMPONENT_GETTER(world_reflection_probe, c_reflection_probe, reflection_probes, COMPONENT_REFLECTION_PROBE)
COMPONENT_GETTER(world_player_controller, c_player_controller, player_controllers, COMPONENT_PLAYER_CONTROLLER)
COMPONENT_GETTER(world_scene_trigger,     c_scene_trigger,     scene_triggers,     COMPONENT_SCENE_TRIGGER)

entity_id
world_parent(const world* w, entity_id e)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX || !(w->masks[index] & COMPONENT_BIT(COMPONENT_PARENT)))
        return ENTITY_NULL;
    return w->parents[index].parent;
}

c_mesh_renderer*
world_add_mesh_renderer(world* w, entity_id e, asset_id model)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    c_mesh_renderer* mr = &w->mesh_renderers[index];

    // Retain before releasing, so replacing a model with itself never
    // drops its count to zero in between.
    assets_retain(w->assets, model);
    if (w->masks[index] & COMPONENT_BIT(COMPONENT_MESH_RENDERER))
        assets_release(w->assets, mr->model);

    memset(mr, 0, sizeof(*mr));
    mr->model        = model;
    mr->visible      = true;
    mr->cast_shadows = true;

    w->masks[index] |= COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    return mr;
}

b8
world_set_model(world* w, entity_id e, asset_id model)
{
    c_mesh_renderer* mr = world_mesh_renderer(w, e);
    if (!mr)
        return false;

    assets_retain(w->assets, model);
    assets_release(w->assets, mr->model);
    mr->model = model;
    return true;
}

c_light*
world_add_light(world* w, entity_id e, light l)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    w->lights[index] = l;
    w->masks[index] |= COMPONENT_BIT(COMPONENT_LIGHT);
    return &w->lights[index];
}

c_camera*
world_add_camera(world* w, entity_id e, f32 fov_degrees, f32 near_plane, f32 far_plane)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    w->cameras[index] = (c_camera){ fov_degrees, near_plane, far_plane };
    w->masks[index] |= COMPONENT_BIT(COMPONENT_CAMERA);
    return &w->cameras[index];
}

c_spin*
world_add_spin(world* w, entity_id e, const vec3 degrees_per_second)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    memset(&w->spins[index], 0, sizeof(w->spins[index]));   // padding too: states compare with memcmp
    glm_vec3_copy((f32*)degrees_per_second, w->spins[index].degrees_per_second);
    w->masks[index] |= COMPONENT_BIT(COMPONENT_SPIN);
    return &w->spins[index];
}

c_reflection_probe*
world_add_reflection_probe(world* w, entity_id e, const vec3 extents)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    memset(&w->reflection_probes[index], 0, sizeof(w->reflection_probes[index]));
    glm_vec3_copy((f32*)extents, w->reflection_probes[index].extents);
    w->masks[index] |= COMPONENT_BIT(COMPONENT_REFLECTION_PROBE);
    return &w->reflection_probes[index];
}

c_player_controller*
world_add_player_controller(world* w, entity_id e, player_mode mode)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    c_player_controller* pc = &w->player_controllers[index];
    memset(pc, 0, sizeof(*pc));   // padding too: states compare with memcmp
    pc->mode              = mode;
    pc->move_speed        = 4.0f;
    pc->sprint_multiplier = 2.0f;
    pc->look_sensitivity  = 1.0f;
    w->masks[index] |= COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER);
    return pc;
}

c_scene_trigger*
world_add_scene_trigger(world* w, entity_id e, const vec3 extents)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX)
        return NULL;

    c_scene_trigger* st = &w->scene_triggers[index];
    memset(st, 0, sizeof(*st));   // padding and the unused ends of the names too: states compare with memcmp
    glm_vec3_copy((f32*)extents, st->extents);
    st->fade_seconds = 0.5f;
    w->masks[index] |= COMPONENT_BIT(COMPONENT_SCENE_TRIGGER);
    return st;
}

void
world_tick(world* w, f32 dt)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_SPIN);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        uint32 index = entity_slot(e);
        for (int axis = 0; axis < 3; ++axis)
        {
            // fmodf keeps it inside Transform's +-360 range, so the pose can
            // still be read back and saved like any other.
            f32* angle = &w->transforms[index].rotation_deg[axis];
            *angle = fmodf(*angle + w->spins[index].degrees_per_second[axis] * dt, 360.0f);
        }
    }
}

b8
world_set_parent(world* w, entity_id child, entity_id parent)
{
    uint32 child_index = live_index(w, child);
    if (child_index == UINT32_MAX)
        return false;

    if (parent == ENTITY_NULL)
    {
        w->masks[child_index] &= ~COMPONENT_BIT(COMPONENT_PARENT);
        return true;
    }

    if (live_index(w, parent) == UINT32_MAX)
        return false;

    // Walk up from the new parent: reaching `child` means it would become
    // its own ancestor.
    for (entity_id a = parent; a != ENTITY_NULL; a = world_parent(w, a))
    {
        if (a == child)
            return false;
    }

    w->parents[child_index].parent = parent;
    w->masks[child_index] |= COMPONENT_BIT(COMPONENT_PARENT);
    return true;
}

// --- Derived state --------------------------------------------------------------

void
transform_to_matrix(const c_transform* t, mat4 out)
{
    glm_translate_make(out, (f32*)t->position);
    glm_rotate_y(out, glm_rad(t->rotation_deg[1]), out);
    glm_rotate_x(out, glm_rad(t->rotation_deg[0]), out);
    glm_rotate_z(out, glm_rad(t->rotation_deg[2]), out);
    glm_scale(out, (f32*)t->scale);
}

void
transform_from_matrix(const mat4 m, c_transform* out)
{
    // Translation is the last column; scale the length of each basis column.
    glm_vec3_copy((f32*)m[3], out->position);
    for (int axis = 0; axis < 3; ++axis)
        out->scale[axis] = glm_vec3_norm((f32*)m[axis]);

    // The rotation R = Ry * Rx * Rz with scale divided out. cglm is
    // column-major, so row i / column j is r[j][i]. Multiplying it out:
    //   r(1,2) = -sin(pitch)
    //   r(0,2) =  sin(yaw) cos(pitch),  r(2,2) = cos(yaw) cos(pitch)
    //   r(1,0) =  cos(pitch) sin(roll), r(1,1) = cos(pitch) cos(roll)
    vec3 r[3];
    for (int axis = 0; axis < 3; ++axis)
        glm_vec3_scale((f32*)m[axis], out->scale[axis] > 1e-12f ? 1.0f / out->scale[axis] : 0.0f, r[axis]);

    f32 sin_pitch = glm_clamp(-r[2][1], -1.0f, 1.0f);
    f32 pitch     = asinf(sin_pitch);
    f32 yaw, roll;
    if (fabsf(sin_pitch) < 0.99999f)
    {
        yaw  = atan2f(r[2][0], r[2][2]);
        roll = atan2f(r[0][1], r[1][1]);
    }
    else
    {
        // Gimbal lock (pitch at +-90): yaw and roll turn about the same axis;
        // put it all in yaw.
        roll = 0.0f;
        yaw  = atan2f(-r[0][2], r[0][0]);
    }

    out->rotation_deg[0] = glm_deg(pitch);
    out->rotation_deg[1] = glm_deg(yaw);
    out->rotation_deg[2] = glm_deg(roll);
}

// Computes slot `index`'s world matrix, resolving its parent first. Each
// slot is resolved once per world_update() (update_stamps), so the cost is
// linear however the hierarchy is laid out in storage.
static void
resolve(world* w, uint32 index)
{
    if (w->update_stamps[index] == w->current_stamp)
        return;

    mat4 local;
    transform_to_matrix(&w->transforms[index], local);

    uint32 parent_index = (w->masks[index] & COMPONENT_BIT(COMPONENT_PARENT))
        ? live_index(w, w->parents[index].parent)
        : UINT32_MAX;

    if (parent_index != UINT32_MAX)
    {
        resolve(w, parent_index);
        glm_mat4_mul(w->world_matrices[parent_index], local, w->world_matrices[index]);
    }
    else
    {
        glm_mat4_copy(local, w->world_matrices[index]);   // root, or parent destroyed
    }

    w->update_stamps[index] = w->current_stamp;
}

void
world_update(world* w)
{
    if (++w->current_stamp == 0)
        w->current_stamp = 1;   // 0 marks "never resolved"

    for (uint32 i = 0; i < w->capacity; ++i)
    {
        if (w->masks[i] & WORLD_ALIVE_BIT)
            resolve(w, i);
    }

    for (uint32 i = 0; i < w->capacity; ++i)
    {
        if (!(w->masks[i] & WORLD_ALIVE_BIT) || !(w->masks[i] & COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
            continue;

        const model* m = assets_model(w->assets, w->mesh_renderers[i].model);
        if (!m)
            continue;

        // All 8 corners of the model-space box: exact for translation and
        // scale, conservative under rotation.
        for (int corner = 0; corner < 8; ++corner)
        {
            vec3 p = {
                (corner & 1) ? m->bounds_max[0] : m->bounds_min[0],
                (corner & 2) ? m->bounds_max[1] : m->bounds_min[1],
                (corner & 4) ? m->bounds_max[2] : m->bounds_min[2],
            };

            vec3 world_p;
            glm_mat4_mulv3(w->world_matrices[i], p, 1.0f, world_p);

            if (corner == 0)
            {
                glm_vec3_copy(world_p, w->bounds_min[i]);
                glm_vec3_copy(world_p, w->bounds_max[i]);
            }
            else
            {
                glm_vec3_minv(w->bounds_min[i], world_p, w->bounds_min[i]);
                glm_vec3_maxv(w->bounds_max[i], world_p, w->bounds_max[i]);
            }
        }
    }
}

const vec4*
world_matrix(const world* w, entity_id e)
{
    uint32 index = live_index(w, e);
    return index == UINT32_MAX ? NULL : (const vec4*)w->world_matrices[index];
}

b8
world_bounds(const world* w, entity_id e, vec3 out_min, vec3 out_max)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX || !(w->masks[index] & COMPONENT_BIT(COMPONENT_MESH_RENDERER)))
        return false;

    if (!assets_model(w->assets, w->mesh_renderers[index].model))
        return false;

    glm_vec3_copy(w->bounds_min[index], out_min);
    glm_vec3_copy(w->bounds_max[index], out_max);
    return true;
}

// Grows [lo, hi] by every visible MeshRenderer's bounds; with `skip_flat`,
// leaves out flat ones. False if none counted.
static b8
grow_framing_bounds(const world* w, b8 skip_flat, vec3 lo, vec3 hi)
{
    b8 any = false;

    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        vec3 e_lo, e_hi;
        if (!w->mesh_renderers[entity_slot(e)].visible || !world_bounds(w, e, e_lo, e_hi))
            continue;

        f32 height = e_hi[1] - e_lo[1];
        f32 width  = fmaxf(e_hi[0] - e_lo[0], e_hi[2] - e_lo[2]);
        if (skip_flat && height < width * WORLD_FLAT_RATIO)
            continue;

        glm_vec3_minv(lo, e_lo, lo);
        glm_vec3_maxv(hi, e_hi, hi);
        any = true;
    }
    return any;
}

b8
world_framing_bounds(const world* w, vec3 out_min, vec3 out_max)
{
    vec3 lo = { FLT_MAX, FLT_MAX, FLT_MAX };
    vec3 hi = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    if (!grow_framing_bounds(w, true, lo, hi) && !grow_framing_bounds(w, false, lo, hi))
        return false;

    glm_vec3_copy(lo, out_min);
    glm_vec3_copy(hi, out_max);
    return true;
}

// Slab test: the entry distance of the ray into [lo, hi], or -1 if it misses
// or starts inside.
static f32
ray_box(const vec3 origin, const vec3 direction, const vec3 lo, const vec3 hi)
{
    f32 t_enter = -FLT_MAX;
    f32 t_exit  = FLT_MAX;

    for (int axis = 0; axis < 3; ++axis)
    {
        if (fabsf(direction[axis]) < 1e-8f)
        {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
                return -1.0f;   // parallel to this slab and outside it
            continue;
        }

        f32 t0 = (lo[axis] - origin[axis]) / direction[axis];
        f32 t1 = (hi[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) { f32 swap = t0; t0 = t1; t1 = swap; }

        t_enter = fmaxf(t_enter, t0);
        t_exit  = fminf(t_exit, t1);
        if (t_enter > t_exit)
            return -1.0f;
    }

    return t_enter >= 0.0f ? t_enter : -1.0f;
}

// Entry distance of the ray into a sphere, or -1 if it misses or starts inside.
static f32
ray_sphere(const vec3 origin, const vec3 direction, const vec3 center, f32 radius)
{
    vec3 to_center;
    glm_vec3_sub((f32*)center, (f32*)origin, to_center);

    f32 along = glm_vec3_dot(to_center, (f32*)direction);
    f32 miss2 = glm_vec3_norm2(to_center) - along * along;
    if (miss2 > radius * radius)
        return -1.0f;

    f32 t = along - sqrtf(radius * radius - miss2);
    return t >= 0.0f ? t : -1.0f;
}

entity_id
world_pick(const world* w, const vec3 origin, const vec3 direction, f32 marker_radius, f32* out_distance)
{
    entity_id best      = ENTITY_NULL;
    f32       best_t    = FLT_MAX;
    const uint32 marker = COMPONENT_BIT(COMPONENT_LIGHT) | COMPONENT_BIT(COMPONENT_CAMERA);

    for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
    {
        uint32 index = entity_slot(e);
        f32    t     = -1.0f;

        vec3 lo, hi;
        if (world_bounds(w, e, lo, hi))
        {
            if (!w->mesh_renderers[index].visible)
                continue;
            t = ray_box(origin, direction, lo, hi);
        }
        else if (w->masks[index] & marker)
        {
            const vec4* m = w->world_matrices[index];
            t = ray_sphere(origin, direction, (vec3){ m[3][0], m[3][1], m[3][2] }, marker_radius);
        }

        if (t >= 0.0f && t < best_t)
        {
            best_t = t;
            best   = e;
        }
    }

    if (out_distance && best)
        *out_distance = best_t;
    return best;
}

uint32
world_gather_lights(const world* w, light* out_lights, vec3* out_positions, uint32 max)
{
    uint32 written = 0;

    for (uint32 i = 0; i < w->capacity && written < max; ++i)
    {
        if (!(w->masks[i] & WORLD_ALIVE_BIT) || !(w->masks[i] & COMPONENT_BIT(COMPONENT_LIGHT)))
            continue;

        light l = w->lights[i];

        // Rotate (and scale -- undone by the normalize) the local direction;
        // the upper 3x3 drops translation.
        mat3 rotation;
        glm_mat4_pick3((vec4*)w->world_matrices[i], rotation);
        glm_mat3_mulv(rotation, l.direction, l.direction);
        if (glm_vec3_norm2(l.direction) > 1e-12f)
            glm_vec3_normalize(l.direction);

        out_lights[written] = l;

        if (out_positions)
        {
            out_positions[written][0] = w->world_matrices[i][3][0];
            out_positions[written][1] = w->world_matrices[i][3][1];
            out_positions[written][2] = w->world_matrices[i][3][2];
        }

        ++written;
    }

    return written;
}

b8
world_camera_view(const world* w, entity_id e, f32 aspect, camera* out)
{
    uint32 index = live_index(w, e);
    if (index == UINT32_MAX || !(w->masks[index] & COMPONENT_BIT(COMPONENT_CAMERA)))
        return false;

    const vec4* m   = (const vec4*)w->world_matrices[index];
    const c_camera* c = &w->cameras[index];

    vec3 position = { m[3][0], m[3][1], m[3][2] };
    vec3 forward  = { -m[2][0], -m[2][1], -m[2][2] };
    vec3 up       = { m[1][0], m[1][1], m[1][2] };
    glm_vec3_normalize(forward);
    glm_vec3_normalize(up);

    vec3 target;
    glm_vec3_add(position, forward, target);

    camera_init(out, position, target, c->fov_degrees, aspect, c->near_plane, c->far_plane);
    glm_vec3_copy(up, out->up);
    return true;
}

// --- Raw state (undo/redo) -------------------------------------------------------

uint32
world_slot_count(const world* w)
{
    return w->capacity;
}

void
world_empty_state(uint32 slot, entity_state* out)
{
    memset(out, 0, sizeof(*out));
    out->id = make_entity(slot, 1);
}

void
world_capture_states(const world* w, entity_state* out)
{
    for (uint32 i = 0; i < w->capacity; ++i)
    {
        entity_state* s = &out[i];
        memset(s, 0, sizeof(*s));

        s->id    = make_entity(i, w->generations[i]);
        s->alive = (w->masks[i] & WORLD_ALIVE_BIT) != 0;
        if (!s->alive)
            continue;

        s->mask = w->masks[i] & ~WORLD_ALIVE_BIT;

        // Only the meaningful bytes of the name, so states compare by value.
        snprintf(s->name.value, ENTITY_NAME_MAX, "%s", w->names[i].value);
        s->transform = w->transforms[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_PARENT))        s->parent        = w->parents[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_MESH_RENDERER)) s->mesh_renderer = w->mesh_renderers[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_LIGHT))         s->light         = w->lights[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_CAMERA))        s->camera        = w->cameras[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_SPIN))          s->spin          = w->spins[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_REFLECTION_PROBE)) s->reflection_probe = w->reflection_probes[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER)) s->player_controller = w->player_controllers[i];
        if (s->mask & COMPONENT_BIT(COMPONENT_SCENE_TRIGGER))     s->scene_trigger     = w->scene_triggers[i];
    }
}

b8
entity_state_differs(const entity_state* a, const entity_state* b, uint32* out_components)
{
    uint32 changed = 0;

    if (a->alive && b->alive)
    {
        changed = a->mask ^ b->mask;   // components added or removed
        uint32 both = a->mask & b->mask;

        if (strcmp(a->name.value, b->name.value) != 0)
            changed |= COMPONENT_BIT(COMPONENT_NAME);

        // memcmp is fine here: component structs are always copied whole
        // (padding included), so equal values compare equal byte for byte.
        #define DIFF(bit, field) \
            if ((both & COMPONENT_BIT(bit)) && memcmp(&a->field, &b->field, sizeof(a->field)) != 0) changed |= COMPONENT_BIT(bit)
        DIFF(COMPONENT_TRANSFORM,     transform);
        DIFF(COMPONENT_PARENT,        parent);
        DIFF(COMPONENT_MESH_RENDERER, mesh_renderer);
        DIFF(COMPONENT_LIGHT,         light);
        DIFF(COMPONENT_CAMERA,        camera);
        DIFF(COMPONENT_SPIN,          spin);
        DIFF(COMPONENT_REFLECTION_PROBE, reflection_probe);
        DIFF(COMPONENT_PLAYER_CONTROLLER, player_controller);
        DIFF(COMPONENT_SCENE_TRIGGER,     scene_trigger);
        #undef DIFF
    }
    else if (a->alive != b->alive)
    {
        changed = a->alive ? a->mask : b->mask;
    }

    if (out_components)
        *out_components = changed;

    return changed != 0 || a->alive != b->alive || (a->alive && a->id != b->id);
}

// Removes `index` from the free list (it is about to be revived in place).
static void
take_from_free_list(world* w, uint32 index)
{
    for (uint32 i = 0; i < w->free_count; ++i)
    {
        if (w->free_list[i] == index)
        {
            w->free_list[i] = w->free_list[--w->free_count];
            return;
        }
    }
}

void
world_apply_states(world* w, const entity_state* states, uint32 count)
{
    // Room for every slot mentioned.
    for (uint32 i = 0; i < count; ++i)
    {
        while (entity_slot(states[i].id) >= w->capacity)
            grow(w);
    }

    // 1. Everything that should be alive: revive in place and write all
    //    components raw. Parents are written as recorded, so by the end of
    //    this pass every surviving child already points where it should.
    for (uint32 i = 0; i < count; ++i)
    {
        const entity_state* s     = &states[i];
        uint32              index = entity_slot(s->id);
        if (!s->alive)
            continue;

        b8 was_alive = (w->masks[index] & WORLD_ALIVE_BIT) != 0;
        b8 had_model = was_alive && (w->masks[index] & COMPONENT_BIT(COMPONENT_MESH_RENDERER));

        if (!was_alive)
        {
            take_from_free_list(w, index);
            w->alive_count++;
        }

        // Retain the new model before releasing the old (they may be the same).
        if (s->mask & COMPONENT_BIT(COMPONENT_MESH_RENDERER))
            assets_retain(w->assets, s->mesh_renderer.model);
        if (had_model)
            assets_release(w->assets, w->mesh_renderers[index].model);

        w->generations[index]    = s->id >> ENTITY_INDEX_BITS;
        w->masks[index]          = s->mask | WORLD_ALIVE_BIT;
        w->names[index]          = s->name;
        w->transforms[index]     = s->transform;
        w->parents[index]        = s->parent;
        w->mesh_renderers[index] = s->mesh_renderer;
        w->lights[index]         = s->light;
        w->cameras[index]        = s->camera;
        w->spins[index]          = s->spin;
        w->reflection_probes[index] = s->reflection_probe;
        w->player_controllers[index] = s->player_controller;
        w->scene_triggers[index]     = s->scene_trigger;
        w->update_stamps[index]  = 0;
    }

    // 2. Everything that should be dead. Any child that should outlive its
    //    parent was re-pointed in pass 1, so world_destroy()'s recursion only
    //    reaches entities that are meant to go too.
    //
    //    The generation is deliberately NOT wound back to the recorded one:
    //    world_destroy() bumps it, so after undoing a creation the next
    //    world_create() hands out a new ID rather than the undone entity's --
    //    anyone still holding that ID sees it as stale, not as some new
    //    entity. (Redo can still revive the exact ID: recording any new step
    //    discards the redo tail first.)
    for (uint32 i = 0; i < count; ++i)
    {
        const entity_state* s     = &states[i];
        uint32              index = entity_slot(s->id);
        if (!s->alive && (w->masks[index] & WORLD_ALIVE_BIT))
            world_destroy(w, make_entity(index, w->generations[index]));
    }
}

// --- Iteration ------------------------------------------------------------------

entity_id
world_next(const world* w, entity_id previous, uint32 required_mask)
{
    uint32 start = previous == ENTITY_NULL ? 0 : entity_slot(previous) + 1;
    uint32 need  = WORLD_ALIVE_BIT | required_mask;

    for (uint32 i = start; i < w->capacity; ++i)
    {
        if ((w->masks[i] & need) == need)
            return make_entity(i, w->generations[i]);
    }

    return ENTITY_NULL;
}
