#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Light.h"
#include "../World/World.h"

#include <cglm/cglm.h>

// Shadows for spot lights: each of up to SPOT_SHADOW_MAX shadow-casting
// spot lights gets a perspective depth map looking down its cone, all
// tiles of one depth atlas (2 x 2) -- one texture unit for all of them,
// where separate maps would run past the 16 a fragment shader is promised.
// pbr.frag finds a light's tile (u_spot_shadow_light), and filters it as it
// does the sun's cascades (Renderer/Shadow.h).
//
// Which lights: the first SPOT_SHADOW_MAX spot lights, in world order,
// with cast_shadows set and some intensity.

#define SPOT_SHADOW_MAX  4    // tiles in the atlas, 2 x 2
#define SPOT_SHADOW_SLOT 14   // texture unit: after Renderer/PostProcess.h's POST_SSAO_SLOT

typedef struct spot_shadows
{
    rhi_framebuffer atlas;       // depth only, comparison sampling: tile_size * 2 square
    uint32          tile_size;

    // This frame's shadowed spots, from spot_shadows_update().
    uint32 count;
    int32  light_index[SPOT_SHADOW_MAX];      // which u_lights[] entry (world_gather_lights() order) each tile is for
    mat4   view_projection[SPOT_SHADOW_MAX];
    f32    texel_scale[SPOT_SHADOW_MAX];      // a texel's world size per meter from the light, for the bias
} spot_shadows;

// An atlas of 2 x 2 tiles of `tile_size` texels. False (and `ss` safe to
// destroy) on failure.
b8 spot_shadows_create(spot_shadows* ss, uint32 tile_size);

// Picks this frame's shadowed spot lights from `lights` / `positions` (as
// world_gather_lights() returns them) and aims a tile at each.
void spot_shadows_update(spot_shadows* ss, const light* lights, const vec3* positions, uint32 count);

// Renders the shadow casters' depth into each picked light's tile, with the
// depth-only shader `depth_shader` (Renderer/Shadow.h's) -- only casters
// inside the light's view, out to its range: a lamp's map doesn't redraw
// the whole town. Needs world_update(). Leaves the default
// framebuffer bound.
void spot_shadows_render(spot_shadows* ss, const world* w, rhi_shader depth_shader);

struct gpu_shadow_block;   // Renderer/FrameData.h

// Binds the atlas to SPOT_SHADOW_SLOT (pointing u_spot_shadow_atlas there)
// and fills the spot lights' part of `block`, pbr.frag's u_spot_shadow_*.
// Always call it -- with count 0 nothing is shadowed -- so the sampler
// always has its own unit.
void spot_shadows_bind(const spot_shadows* ss, rhi_shader pbr_shader, struct gpu_shadow_block* block);

// Safe on a zeroed / failed / already-destroyed spot_shadows.
void spot_shadows_destroy(spot_shadows* ss);
