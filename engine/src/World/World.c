#include "World.h"

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
    free(w->world_matrices);
    free(w->bounds_min);
    free(w->bounds_max);
    free(w->update_stamps);

    memset(w, 0, sizeof(*w));
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

static void
local_matrix(const c_transform* t, mat4 out)
{
    glm_translate_make(out, (f32*)t->position);
    glm_rotate_y(out, glm_rad(t->rotation_deg[1]), out);
    glm_rotate_x(out, glm_rad(t->rotation_deg[0]), out);
    glm_rotate_z(out, glm_rad(t->rotation_deg[2]), out);
    glm_scale(out, (f32*)t->scale);
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
    local_matrix(&w->transforms[index], local);

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
