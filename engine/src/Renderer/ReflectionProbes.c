#include "ReflectionProbes.h"

#include <stdio.h>
#include <string.h>

b8
reflection_probes_create(reflection_probes* rp)
{
    memset(rp, 0, sizeof(*rp));

    rp->maps         = rhi_cubemap_array_create(PROBE_SIZE, PROBE_MAX, PROBE_MIPS, RHI_FORMAT_RGBA16F);
    rp->capture_cube = rhi_cubemap_create((rhi_cubemap_desc){ .size = PROBE_SIZE, .format = RHI_FORMAT_RGBA16F,
                                                               .mip_count = 32 });   // the whole chain
    rhi_framebuffer_attachment color = { .format = RHI_FORMAT_RGBA16F };
    rp->capture = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width = PROBE_SIZE, .height = PROBE_SIZE, .color_attachments = &color, .color_attachment_count = 1,
        .has_depth_attachment = true });

    for (uint32 i = 0; i < PROBE_MAX; ++i)
    {
        snprintf(rp->names[i][0], sizeof(rp->names[i][0]), "u_probe_position[%u]", i);
        snprintf(rp->names[i][1], sizeof(rp->names[i][1]), "u_probe_box_min[%u]", i);
        snprintf(rp->names[i][2], sizeof(rp->names[i][2]), "u_probe_box_max[%u]", i);
    }

    return rp->maps && rp->capture_cube && rp->capture;
}

// FNV-1a over raw bytes: equal inputs, equal signature.
static uint64
hash_bytes(uint64 hash, const void* data, size_t size)
{
    const uint8* p = data;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

uint64
reflection_probes_gather(reflection_probes* rp, const world* w, uint64 environment_key)
{
    rp->count = 0;
    uint64       hash = 14695981039346656037ull;
    const uint32 mask = COMPONENT_BIT(COMPONENT_REFLECTION_PROBE);

    for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
    {
        if (rp->count == PROBE_MAX)
        {
            if (!rp->warned_count)
                LOG_WARN("reflection probes: more than %d in the scene; the rest are ignored\n", PROBE_MAX);
            rp->warned_count = true;
            break;
        }

        const c_reflection_probe* probe = world_reflection_probe(w, e);
        uint32 i = rp->count++;

        const f32* m = (const f32*)world_matrix(w, e);   // column-major: the translation is m[12..14]
        glm_vec3_copy((vec3){ m[12], m[13], m[14] }, rp->position[i]);
        glm_vec3_sub(rp->position[i], (f32*)probe->extents, rp->box_min[i]);
        glm_vec3_add(rp->position[i], (f32*)probe->extents, rp->box_max[i]);

        hash = hash_bytes(hash, rp->position[i], sizeof(vec3));
        hash = hash_bytes(hash, probe->extents, sizeof(vec3));
    }

    hash = hash_bytes(hash, &rp->count, sizeof(rp->count));
    hash = hash_bytes(hash, &environment_key, sizeof(environment_key));
    return hash ? hash : 1;   // never 0, which means "nothing baked"
}

void
reflection_probes_bind(const reflection_probes* rp, rhi_shader pbr_shader, b8 enabled)
{
    rhi_shader_set_int(pbr_shader, "u_probe_maps", PROBE_SLOT);
    if (rp->maps)
        rhi_texture_bind(rp->maps, PROBE_SLOT);

    uint32 count = enabled ? rp->count : 0;
    rhi_shader_set_int(pbr_shader, "u_probe_count", (int32)count);
    for (uint32 i = 0; i < count; ++i)
    {
        rhi_shader_set_vec3(pbr_shader, rp->names[i][0], rp->position[i][0], rp->position[i][1], rp->position[i][2]);
        rhi_shader_set_vec3(pbr_shader, rp->names[i][1], rp->box_min[i][0], rp->box_min[i][1], rp->box_min[i][2]);
        rhi_shader_set_vec3(pbr_shader, rp->names[i][2], rp->box_max[i][0], rp->box_max[i][1], rp->box_max[i][2]);
    }
}

void
reflection_probes_destroy(reflection_probes* rp)
{
    if (rp->maps)
        rhi_texture_destroy(rp->maps);
    if (rp->capture_cube)
        rhi_texture_destroy(rp->capture_cube);
    if (rp->capture)
        rhi_framebuffer_destroy(rp->capture);
    memset(rp, 0, sizeof(*rp));
}
