#pragma once

#include "../Core/Defines.h"

#include <cglm/cglm.h>

// A light's own parameters. Position/orientation are NOT stored here --
// a Light component's placement comes from its entity's world transform
// (World/World.h), the same way a MeshRenderer's does. `direction` below is
// therefore in the entity's local space, not world space, and
// world_gather_lights() rotates it into world space when it builds the
// per-frame light list.

typedef enum light_type
{
    LIGHT_DIRECTIONAL,   // direction only; entity position is ignored
    LIGHT_POINT,         // entity position only; direction is ignored
    LIGHT_SPOT           // both: cone pointing along direction, from position
} light_type;

typedef struct light
{
    light_type type;
    vec3       color;        // linear RGB
    f32        intensity;

    vec3 direction;          // LIGHT_DIRECTIONAL / LIGHT_SPOT: local-space direction the light travels

    f32 range;                // LIGHT_POINT / LIGHT_SPOT: distance at which attenuation reaches 0; 0 = no cutoff
    f32 inner_cone_degrees;   // LIGHT_SPOT only: full intensity inside this half-angle
    f32 outer_cone_degrees;   // LIGHT_SPOT only: zero intensity outside this half-angle

    b8  cast_shadows;         // directional: the cascades (the first such light); spot: a shadow map
                              // (Renderer/SpotShadow.h). Point lights cast none yet. The light_init_*
                              // functions turn it on.
} light;

void light_init_directional(light* l, vec3 direction, vec3 color, f32 intensity);
void light_init_point(light* l, vec3 color, f32 intensity, f32 range);
void light_init_spot(light* l, vec3 direction, vec3 color, f32 intensity,
                      f32 range, f32 inner_cone_degrees, f32 outer_cone_degrees);
