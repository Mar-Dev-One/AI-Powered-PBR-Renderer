#include "Lighting.h"
#include "FrameData.h"

#include <math.h>
#include <string.h>

#include <cglm/cglm.h>   // glm_rad() -- M_PI isn't standard C and isn't worth chasing a define for

void
lighting_fill(const world* w, gpu_light_block* block)
{
    light lights[LIGHTING_MAX_LIGHTS];
    vec3  world_positions[LIGHTING_MAX_LIGHTS];

    // Zeroed so entries past `count` (and the padding) upload as zeros.
    memset(block, 0, sizeof(*block));
    memset(world_positions, 0, sizeof(world_positions));

    uint32 count = world_gather_lights(w, lights, world_positions, LIGHTING_MAX_LIGHTS);

    // The cascades (Renderer/Shadow.h) are fit to the first directional
    // light in world order that casts shadows -- tell pbr.frag which
    // u_lights[] entry that is.
    block->light_count        = (int32)count;
    block->shadow_light_index = -1;

    for (uint32 i = 0; i < count; ++i)
    {
        const light* l   = &lights[i];
        gpu_light*   out = &block->lights[i];

        if (block->shadow_light_index < 0 && l->type == LIGHT_DIRECTIONAL && l->cast_shadows)
            block->shadow_light_index = (int32)i;

        out->type      = (int32)l->type;
        out->intensity = l->intensity;
        out->range     = l->range;
        glm_vec3_copy((f32*)l->color, out->color);
        glm_vec3_copy((f32*)l->direction, out->direction);
        glm_vec3_copy(world_positions[i], out->position);

        // Precomputed here (one cosf() per light per frame) rather than in
        // the shader (one per fragment per light).
        out->inner_cos = cosf(glm_rad(l->inner_cone_degrees));
        out->outer_cos = cosf(glm_rad(l->outer_cone_degrees));
    }
}
