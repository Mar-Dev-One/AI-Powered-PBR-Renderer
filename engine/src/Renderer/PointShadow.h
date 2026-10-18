#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Light.h"
#include "../World/World.h"

#include <cglm/cglm.h>

// Shadows for point lights: each of up to POINT_SHADOW_MAX shadow-casting
// point lights renders its six 90-degree cube faces as tiles of one depth
// atlas (6 faces across, one light per row) -- a single texture unit, the
// last of the 16 a fragment shader is promised (Renderer/SpotShadow.h took
// the one before). pbr.frag picks the face from the light-to-surface
// direction and works out the face's coordinates and depth itself: the six
// face rotations (u_point_shadow_faces) are the same for every light, so it
// needs only each light's position and near / far planes.
//
// Which lights: the first POINT_SHADOW_MAX point lights, in world order,
// with cast_shadows set and some intensity.

#define POINT_SHADOW_MAX   4     // rows of the atlas
#define POINT_SHADOW_SLOT  15    // texture unit: after SpotShadow.h's SPOT_SHADOW_SLOT
#define POINT_SHADOW_FACES 6     // +X, -X, +Y, -Y, +Z, -Z

typedef struct point_shadows
{
    rhi_framebuffer atlas;       // depth only, comparison sampling: face_size * 6 x face_size * POINT_SHADOW_MAX
    uint32          face_size;
    mat4            face_rotation[POINT_SHADOW_FACES];   // world direction -> each face's view space

    // This frame's shadowed lights, from point_shadows_update().
    uint32 count;
    int32  light_index[POINT_SHADOW_MAX];   // which u_lights[] entry each row is for
    vec3   position[POINT_SHADOW_MAX];
    f32    near[POINT_SHADOW_MAX];
    f32    far[POINT_SHADOW_MAX];
} point_shadows;

// An atlas of 6 x POINT_SHADOW_MAX faces of `face_size` texels. False (and
// `ps` safe to destroy) on failure.
b8 point_shadows_create(point_shadows* ps, uint32 face_size);

// Picks this frame's shadowed point lights from `lights` / `positions` (as
// world_gather_lights() returns them).
void point_shadows_update(point_shadows* ps, const light* lights, const vec3* positions, uint32 count);

// Renders each picked light's six faces with the depth-only shader
// `depth_shader` (Renderer/Shadow.h's) -- per face, only casters inside
// that face's view, out to the light's range. Needs world_update(). Leaves the
// default framebuffer bound.
void point_shadows_render(point_shadows* ps, const world* w, rhi_shader depth_shader);

// Binds the atlas to POINT_SHADOW_SLOT and sets pbr.frag's u_point_shadow_*
// uniforms. Always call it -- with count 0 nothing is shadowed.
void point_shadows_bind(const point_shadows* ps, rhi_shader pbr_shader);

// Safe on a zeroed / failed / already-destroyed point_shadows.
void point_shadows_destroy(point_shadows* ps);
