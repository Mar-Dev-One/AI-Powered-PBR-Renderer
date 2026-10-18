#include "PointShadow.h"
#include "FrameData.h"

#include "../Assets/AssetCache.h"
#include "../Scene/Model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// As SpotShadow.c's: close enough for a lamp's bulb housing, and how far
// the faces reach without a range.
#define POINT_NEAR         0.05f
#define POINT_DEFAULT_FAR 50.0f

// The cube faces' directions and up vectors (any up not along the
// direction works; pbr.frag only uses the resulting rotations).
static const vec3 FACE_DIRECTIONS[POINT_SHADOW_FACES] = {
    {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
};
static const vec3 FACE_UPS[POINT_SHADOW_FACES] = {
    { 0.0f, -1.0f,  0.0f }, { 0.0f, -1.0f,  0.0f },
    { 0.0f,  0.0f,  1.0f }, { 0.0f,  0.0f, -1.0f },
    { 0.0f, -1.0f,  0.0f }, { 0.0f, -1.0f,  0.0f },
};

b8
point_shadows_create(point_shadows* ps, uint32 face_size)
{
    memset(ps, 0, sizeof(*ps));
    ps->face_size = face_size;
    ps->atlas     = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                = (uint16)(face_size * POINT_SHADOW_FACES),
        .height               = (uint16)(face_size * POINT_SHADOW_MAX),
        .has_depth_attachment = true,
        .depth_compare        = true,
    });

    vec3 origin = { 0.0f, 0.0f, 0.0f };
    for (uint32 f = 0; f < POINT_SHADOW_FACES; ++f)
        glm_lookat(origin, (f32*)FACE_DIRECTIONS[f], (f32*)FACE_UPS[f], ps->face_rotation[f]);

    return ps->atlas != NULL;
}

void
point_shadows_update(point_shadows* ps, const light* lights, const vec3* positions, uint32 count)
{
    ps->count = 0;

    for (uint32 i = 0; i < count && ps->count < POINT_SHADOW_MAX; ++i)
    {
        const light* l = &lights[i];
        if (l->type != LIGHT_POINT || !l->cast_shadows || l->intensity <= 0.0f)
            continue;

        uint32 row = ps->count++;
        ps->light_index[row] = (int32)i;
        glm_vec3_copy((f32*)positions[i], ps->position[row]);
        ps->near[row] = POINT_NEAR;
        ps->far[row]  = l->range > 0.0f ? l->range : POINT_DEFAULT_FAR;
    }
}

void
point_shadows_render(point_shadows* ps, const world* w, rhi_shader depth_shader)
{
    if (!ps->atlas || ps->count == 0)
        return;

    rhi_framebuffer_bind(ps->atlas);
    rhi_clear(0.0f, 0.0f, 0.0f, 1.0f);
    rhi_shader_bind(depth_shader);

    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    for (uint32 r = 0; r < ps->count; ++r)
    {
        mat4 projection;
        glm_perspective(glm_rad(90.0f), 1.0f, ps->near[r], ps->far[r], projection);

        for (uint32 f = 0; f < POINT_SHADOW_FACES; ++f)
        {
            vec3 target;
            glm_vec3_add(ps->position[r], (f32*)FACE_DIRECTIONS[f], target);
            mat4 view, view_projection;
            glm_lookat(ps->position[r], target, (f32*)FACE_UPS[f], view);
            glm_mat4_mul(projection, view, view_projection);

            // Only what this face's 90-degree view can see (its far plane
            // being the light's range).
            vec4 planes[6];
            glm_frustum_planes(view_projection, planes);

            rhi_set_draw_region((uint16)(f * ps->face_size), (uint16)(r * ps->face_size),
                                (uint16)ps->face_size, (uint16)ps->face_size);

            for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
            {
                const c_mesh_renderer* mr = world_mesh_renderer(w, e);
                const model*           m  = assets_model(w->assets, mr->model);
                if (!m || !mr->visible || !mr->cast_shadows)
                    continue;

                vec3 box[2];
                if (world_bounds(w, e, box[0], box[1]) && !glm_aabb_frustum(box, planes))
                    continue;

                model_draw_depth(m, depth_shader, world_matrix(w, e), view_projection);
            }
        }
    }

    rhi_framebuffer_bind_default();
}

void
point_shadows_bind(const point_shadows* ps, rhi_shader pbr_shader, struct gpu_shadow_block* block)
{
    rhi_shader_set_int(pbr_shader, "u_point_shadow_atlas", POINT_SHADOW_SLOT);
    if (ps->atlas)
        rhi_texture_bind(rhi_framebuffer_get_depth_texture(ps->atlas), POINT_SHADOW_SLOT);

    block->point_count       = (int32)ps->count;
    block->point_texel_scale = ps->face_size ? 2.0f / (f32)ps->face_size : 0.0f;
    for (uint32 f = 0; f < POINT_SHADOW_FACES; ++f)
        glm_mat4_copy((vec4*)ps->face_rotation[f], block->point_faces[f]);

    for (uint32 r = 0; r < ps->count; ++r)
    {
        block->point_light[r][0]  = ps->light_index[r];
        block->point_planes[r][0] = ps->near[r];
        block->point_planes[r][1] = ps->far[r];
    }
}

void
point_shadows_destroy(point_shadows* ps)
{
    if (ps->atlas)
        rhi_framebuffer_destroy(ps->atlas);
    memset(ps, 0, sizeof(*ps));
}
