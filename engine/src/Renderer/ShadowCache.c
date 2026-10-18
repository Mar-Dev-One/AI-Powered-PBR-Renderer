#include "ShadowCache.h"

#include "../Assets/AssetCache.h"
#include "../Scene/Model.h"

uint64
shadow_signature_bytes(uint64 hash, const void* data, size_t size)
{
    const uint8* bytes = data;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Turned by a Spin every frame: its own, or one on an entity it's parented under.
static b8
caster_moving(const world* w, entity_id e)
{
    for (entity_id p = e; p; p = world_parent(w, p))
        if (world_has(w, p, COMPONENT_SPIN))
            return true;
    return false;
}

// The model `e` draws into a map, or NULL if it doesn't belong in this one.
static const model*
caster_model(const world* w, entity_id e, const vec4* planes, const f32* sphere, shadow_casters which)
{
    const c_mesh_renderer* mr = world_mesh_renderer(w, e);
    const model*           m  = assets_model(w->assets, mr->model);
    if (!m || !mr->visible || !mr->cast_shadows)
        return NULL;

    vec3 box[2];
    if (world_bounds(w, e, box[0], box[1]))
    {
        if (planes && !glm_aabb_frustum(box, (vec4*)planes))
            return NULL;
        if (sphere && !glm_aabb_sphere(box, (f32*)sphere))
            return NULL;
    }

    if (which != SHADOW_CASTERS_ALL && caster_moving(w, e) != (which == SHADOW_CASTERS_MOVING))
        return NULL;
    return m;
}

uint64
shadow_signature_casters(uint64 hash, const world* w, const vec4* planes, const f32* sphere,
                         shadow_casters which, uint32* out_count)
{
    const uint32 mask  = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    uint32       count = 0;
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        if (!caster_model(w, e, planes, sphere, which))
            continue;

        // The ID covers a caster replaced by another; the model ID (with its
        // generation) a model swapped; the matrix any move, turn or scale.
        const c_mesh_renderer* mr = world_mesh_renderer(w, e);
        hash = shadow_signature_bytes(hash, &e, sizeof(e));
        hash = shadow_signature_bytes(hash, &mr->model, sizeof(mr->model));
        hash = shadow_signature_bytes(hash, world_matrix(w, e), sizeof(mat4));
        ++count;
    }
    if (out_count)
        *out_count = count;
    return shadow_signature_bytes(hash, &count, sizeof(count));
}

void
shadow_draw_casters(const world* w, rhi_shader depth_shader, const mat4 view_projection, const vec4* planes,
                    shadow_casters which)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        const model* m = caster_model(w, e, planes, NULL, which);
        if (m)
            model_draw_depth(m, depth_shader, world_matrix(w, e), (vec4*)view_projection);
    }
}

void
shadow_layers_plan(shadow_layers* layers, const void* light_key, size_t key_size, const world* w,
                   const vec4* planes, const f32* sphere)
{
    uint32 moving = 0;
    uint64 light  = shadow_signature_bytes(SHADOW_SIGNATURE_SEED, light_key, key_size);
    uint64 fixed  = shadow_signature_casters(light, w, planes, sphere, SHADOW_CASTERS_STATIC, NULL);
    uint64 whole  = shadow_signature_casters(fixed, w, planes, sphere, SHADOW_CASTERS_MOVING, &moving);

    // 0 means "nothing drawn".
    layers->static_signature = fixed ? fixed : 1;
    layers->signature        = whole ? whole : 1;
    layers->moving           = moving > 0;
}

shadow_work
shadow_layers_work(shadow_layers* layers)
{
    if (layers->drawn == layers->signature)
        return SHADOW_WORK_NONE;
    layers->drawn = layers->signature;

    if (!layers->moving)
        return SHADOW_WORK_DRAW_ALL;   // the static atlas keeps whatever it had
    if (layers->static_drawn != layers->static_signature)
    {
        layers->static_drawn = layers->static_signature;
        return SHADOW_WORK_DRAW_STATIC;
    }
    return SHADOW_WORK_COPY_STATIC;
}
