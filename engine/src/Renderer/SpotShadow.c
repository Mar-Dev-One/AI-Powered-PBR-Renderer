#include "SpotShadow.h"

#include "../Assets/AssetCache.h"
#include "../Scene/Model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// The light's camera sees a little past the cone's edge, so the filter and
// the normal offset never read outside the map at the rim of the light.
#define CONE_MARGIN_DEGREES 4.0f

// Near plane: close enough for a lamp shade around the light, far enough to
// keep depth precision. Without a range, the map reaches this far.
#define SPOT_NEAR         0.05f
#define SPOT_DEFAULT_FAR 50.0f

b8
spot_shadows_create(spot_shadows* ss, uint32 tile_size)
{
    memset(ss, 0, sizeof(*ss));
    ss->tile_size = tile_size;
    ss->atlas     = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                = (uint16)(tile_size * 2),
        .height               = (uint16)(tile_size * 2),
        .has_depth_attachment = true,
        .depth_compare        = true,   // sampler2DShadow: hardware 2x2 PCF, as the cascades
    });
    return ss->atlas != NULL;
}

void
spot_shadows_update(spot_shadows* ss, const light* lights, const vec3* positions, uint32 count)
{
    ss->count = 0;

    for (uint32 i = 0; i < count && ss->count < SPOT_SHADOW_MAX; ++i)
    {
        const light* l = &lights[i];
        if (l->type != LIGHT_SPOT || !l->cast_shadows || l->intensity <= 0.0f)
            continue;

        uint32 tile = ss->count++;
        ss->light_index[tile] = (int32)i;

        // Looking down the cone from the light. Any up vector not along
        // the direction will do: the cone is round.
        vec3 target, up = { 0.0f, 1.0f, 0.0f };
        if (fabsf(l->direction[1]) > 0.99f)
            glm_vec3_copy((vec3){ 0.0f, 0.0f, 1.0f }, up);
        glm_vec3_add((f32*)positions[i], (f32*)l->direction, target);

        f32 fov = glm_rad(fminf(2.0f * l->outer_cone_degrees + CONE_MARGIN_DEGREES, 170.0f));
        f32 far = l->range > 0.0f ? l->range : SPOT_DEFAULT_FAR;

        mat4 view, projection;
        glm_lookat((f32*)positions[i], target, up, view);
        glm_perspective(fov, 1.0f, SPOT_NEAR, far, projection);
        glm_mat4_mul(projection, view, ss->view_projection[tile]);

        // At a meter from the light the tile spans 2 tan(fov/2) meters.
        ss->texel_scale[tile] = 2.0f * tanf(fov * 0.5f) / (f32)ss->tile_size;

        glm_vec3_copy((f32*)positions[i], ss->position[tile]);
        glm_vec3_normalize_to((f32*)l->direction, ss->direction[tile]);
        ss->far[tile] = far;
    }
}

// Can tile `t`'s light reach anything inside the box? A sphere around the
// box against the light's range, and against the plane behind the light:
// conservative -- it never skips a caster that could cast into the map.
static b8
in_reach(const spot_shadows* ss, uint32 t, const vec3 box_min, const vec3 box_max)
{
    vec3 center, half, to_center;
    glm_vec3_add((f32*)box_min, (f32*)box_max, center);
    glm_vec3_scale(center, 0.5f, center);
    glm_vec3_sub((f32*)box_max, center, half);
    f32 radius = glm_vec3_norm(half);

    glm_vec3_sub(center, (f32*)ss->position[t], to_center);
    if (glm_vec3_norm(to_center) - radius > ss->far[t])
        return false;   // beyond the light's range
    if (glm_vec3_dot(to_center, (f32*)ss->direction[t]) < -radius)
        return false;   // wholly behind the light
    return true;
}

void
spot_shadows_render(spot_shadows* ss, const world* w, rhi_shader depth_shader)
{
    if (!ss->atlas || ss->count == 0)
        return;

    rhi_framebuffer_bind(ss->atlas);
    rhi_clear(0.0f, 0.0f, 0.0f, 1.0f);   // depth to 1.0 everywhere (there's no color)
    rhi_shader_bind(depth_shader);

    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    for (uint32 t = 0; t < ss->count; ++t)
    {
        // Drawn into its tile only: x and y pick one of the 2 x 2.
        rhi_set_draw_region((uint16)((t & 1) * ss->tile_size), (uint16)((t >> 1) * ss->tile_size),
                            (uint16)ss->tile_size, (uint16)ss->tile_size);

        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        {
            const c_mesh_renderer* mr = world_mesh_renderer(w, e);
            const model*           m  = assets_model(w->assets, mr->model);
            if (!m || !mr->visible || !mr->cast_shadows)
                continue;

            vec3 box_min, box_max;
            if (world_bounds(w, e, box_min, box_max) && !in_reach(ss, t, box_min, box_max))
                continue;

            model_draw_depth(m, depth_shader, world_matrix(w, e), ss->view_projection[t]);
        }
    }

    rhi_framebuffer_bind_default();
}

void
spot_shadows_bind(const spot_shadows* ss, rhi_shader pbr_shader)
{
    rhi_shader_set_int(pbr_shader, "u_spot_shadow_atlas", SPOT_SHADOW_SLOT);
    rhi_shader_set_int(pbr_shader, "u_spot_shadow_count", (int32)ss->count);
    if (ss->atlas)
        rhi_texture_bind(rhi_framebuffer_get_depth_texture(ss->atlas), SPOT_SHADOW_SLOT);

    for (uint32 t = 0; t < ss->count; ++t)
    {
        char name[48];

        snprintf(name, sizeof(name), "u_spot_shadow_light[%u]", t);
        rhi_shader_set_int(pbr_shader, name, ss->light_index[t]);

        snprintf(name, sizeof(name), "u_spot_shadow_matrix[%u]", t);
        rhi_shader_set_mat4(pbr_shader, name, (const f32*)ss->view_projection[t]);

        snprintf(name, sizeof(name), "u_spot_shadow_texel_scale[%u]", t);
        rhi_shader_set_float(pbr_shader, name, ss->texel_scale[t]);
    }
}

void
spot_shadows_destroy(spot_shadows* ss)
{
    if (ss->atlas)
        rhi_framebuffer_destroy(ss->atlas);
    memset(ss, 0, sizeof(*ss));
}
