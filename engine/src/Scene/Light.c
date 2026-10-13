#include "Light.h"

void
light_init_directional(light* l, vec3 direction, vec3 color, f32 intensity)
{
    l->type      = LIGHT_DIRECTIONAL;
    glm_vec3_normalize_to(direction, l->direction);
    glm_vec3_copy(color, l->color);
    l->intensity = intensity;

    l->range               = 0.0f;
    l->inner_cone_degrees  = 0.0f;
    l->outer_cone_degrees  = 0.0f;
}

void
light_init_point(light* l, vec3 color, f32 intensity, f32 range)
{
    l->type      = LIGHT_POINT;
    // Unused by LIGHT_POINT, but kept a valid direction (straight down) rather
    // than zero: a scene file rejects a zero direction (World/Reflect.h marks
    // it nonzero), and it's what the light aims at if it becomes a spot.
    glm_vec3_copy((vec3){ 0.0f, -1.0f, 0.0f }, l->direction);
    glm_vec3_copy(color, l->color);
    l->intensity = intensity;

    l->range               = range;
    l->inner_cone_degrees  = 0.0f;
    l->outer_cone_degrees  = 0.0f;
}

void
light_init_spot(light* l, vec3 direction, vec3 color, f32 intensity,
                f32 range, f32 inner_cone_degrees, f32 outer_cone_degrees)
{
    l->type      = LIGHT_SPOT;
    glm_vec3_normalize_to(direction, l->direction);
    glm_vec3_copy(color, l->color);
    l->intensity = intensity;

    l->range              = range;
    l->inner_cone_degrees = inner_cone_degrees;
    l->outer_cone_degrees = outer_cone_degrees;
}
