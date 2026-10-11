#include "Lighting.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cglm/cglm.h>   // glm_rad() -- M_PI isn't standard C and isn't worth chasing a define for

void
lighting_bind(rhi_shader shader, const world* w)
{
    light lights[LIGHTING_MAX_LIGHTS];
    vec3  world_positions[LIGHTING_MAX_LIGHTS];

    // Zeroed so entries past `count` never reach a debugger as garbage.
    memset(world_positions, 0, sizeof(world_positions));

    uint32 count = world_gather_lights(w, lights, world_positions, LIGHTING_MAX_LIGHTS);

    rhi_shader_set_int(shader, "u_light_count", (int32)count);

    // The cascades (Renderer/Shadow.h) are fit to the first directional
    // light in world order -- tell pbr.frag which u_lights[] entry that is.
    int32 shadow_light_index = -1;

    for (uint32 i = 0; i < count; ++i)
    {
        const light* l = &lights[i];
        char         name[48];

        if (shadow_light_index < 0 && l->type == LIGHT_DIRECTIONAL)
            shadow_light_index = (int32)i;

        snprintf(name, sizeof(name), "u_lights[%u].type", i);
        rhi_shader_set_int(shader, name, (int32)l->type);

        snprintf(name, sizeof(name), "u_lights[%u].color", i);
        rhi_shader_set_vec3(shader, name, l->color[0], l->color[1], l->color[2]);

        snprintf(name, sizeof(name), "u_lights[%u].intensity", i);
        rhi_shader_set_float(shader, name, l->intensity);

        snprintf(name, sizeof(name), "u_lights[%u].direction", i);
        rhi_shader_set_vec3(shader, name, l->direction[0], l->direction[1], l->direction[2]);

        snprintf(name, sizeof(name), "u_lights[%u].position", i);
        rhi_shader_set_vec3(shader, name, world_positions[i][0], world_positions[i][1], world_positions[i][2]);

        snprintf(name, sizeof(name), "u_lights[%u].range", i);
        rhi_shader_set_float(shader, name, l->range);

        // Precomputed here (one cosf() per light per frame) rather than in
        // the shader (one per fragment per light).
        snprintf(name, sizeof(name), "u_lights[%u].inner_cos", i);
        rhi_shader_set_float(shader, name, cosf(glm_rad(l->inner_cone_degrees)));

        snprintf(name, sizeof(name), "u_lights[%u].outer_cos", i);
        rhi_shader_set_float(shader, name, cosf(glm_rad(l->outer_cone_degrees)));
    }

    rhi_shader_set_int(shader, "u_shadow_light_index", shadow_light_index);
}