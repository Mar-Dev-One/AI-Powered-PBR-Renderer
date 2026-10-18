#pragma once

#include "../Core/Defines.h"
#include "Lighting.h"
#include "PointShadow.h"
#include "Shadow.h"
#include "SpotShadow.h"

#include <stddef.h>

// The CPU copies of pbr.frag's two uniform blocks, laid out to GLSL's
// std140 rules byte for byte: SceneRenderer fills them each frame
// (lighting_fill(), csm_bind(), spot_shadows_bind(), point_shadows_bind()),
// uploads them as uniform buffers and binds those to the blocks' binding
// points -- one upload per block instead of a uniform call per light and
// cascade, each with a name built by snprintf().
//
// std140 in short: vec3 and vec4 align to 16 bytes, and every element of an
// array (a float or an int too) takes a multiple of 16 -- hence the padding
// and the [4]s below. The static asserts check the offsets against the
// declarations in pbr.frag.

#define FRAME_LIGHT_BINDING  0   // layout(binding = ...) of pbr.frag's LightBlock
#define FRAME_SHADOW_BINDING 1   // ...and of its ShadowBlock

// pbr.frag's struct scene_light.
typedef struct gpu_light
{
    int32 type;              // Scene/Light.h's light_type
    f32   pad0[3];
    f32   color[3];          // linear RGB
    f32   intensity;
    f32   direction[3];      // world space, normalized: the way the light travels
    f32   pad1;
    f32   position[3];       // world space
    f32   range;
    f32   inner_cos;
    f32   outer_cos;
    f32   pad2[2];
} gpu_light;

typedef struct gpu_light_block
{
    gpu_light lights[LIGHTING_MAX_LIGHTS];
    int32     light_count;
    int32     shadow_light_index;   // the cascades' light in lights[], or -1
    int32     pad[2];
} gpu_light_block;

typedef struct gpu_shadow_block
{
    // Cascades (Renderer/Shadow.h).
    mat4  light_view_projection[CSM_MAX_CASCADES];
    f32   cascade_splits[CSM_MAX_CASCADES][4];        // [i][0]
    f32   cascade_texel_sizes[CSM_MAX_CASCADES][4];   // [i][0]
    int32 cascade_count;
    int32 spot_count;
    int32 point_count;
    f32   point_texel_scale;

    // Spot lights (Renderer/SpotShadow.h).
    int32 spot_light[SPOT_SHADOW_MAX][4];             // [t][0]
    mat4  spot_matrix[SPOT_SHADOW_MAX];
    f32   spot_texel_scale[SPOT_SHADOW_MAX][4];       // [t][0]

    // Point lights (Renderer/PointShadow.h).
    int32 point_light[POINT_SHADOW_MAX][4];           // [r][0]
    f32   point_planes[POINT_SHADOW_MAX][4];          // [r][0..1]: near, far
    mat4  point_faces[POINT_SHADOW_FACES];
} gpu_shadow_block;

// The offsets pbr.frag's std140 blocks give these members.
STATIC_ASSERT(sizeof(gpu_light) == 80, "gpu_light must match std140's struct scene_light");
STATIC_ASSERT(offsetof(gpu_light, color) == 16 && offsetof(gpu_light, direction) == 32
              && offsetof(gpu_light, position) == 48 && offsetof(gpu_light, range) == 60
              && offsetof(gpu_light, inner_cos) == 64, "gpu_light's members must sit at std140's offsets");
STATIC_ASSERT(offsetof(gpu_light_block, light_count) == 80 * LIGHTING_MAX_LIGHTS,
              "u_light_count must follow u_lights[]");
STATIC_ASSERT(offsetof(gpu_shadow_block, cascade_splits) == 64 * CSM_MAX_CASCADES
              && offsetof(gpu_shadow_block, cascade_count) == 96 * CSM_MAX_CASCADES
              && offsetof(gpu_shadow_block, spot_light) == 96 * CSM_MAX_CASCADES + 16
              && offsetof(gpu_shadow_block, spot_matrix) == 96 * CSM_MAX_CASCADES + 16 + 16 * SPOT_SHADOW_MAX
              && offsetof(gpu_shadow_block, point_faces) == 96 * CSM_MAX_CASCADES + 16 + 96 * SPOT_SHADOW_MAX
                                                            + 32 * POINT_SHADOW_MAX,
              "gpu_shadow_block's members must sit at std140's offsets");
