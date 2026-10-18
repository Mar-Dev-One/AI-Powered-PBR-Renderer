#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Light.h"
#include "../World/World.h"

#include <cglm/cglm.h>

// Clustered forward lighting: the World's Light entities, and which of them
// each part of the view needs -- so a scene can hold many lights while each
// pixel only evaluates the few that reach it.
//
// The view is cut into CLUSTER_X x CLUSTER_Y screen tiles and CLUSTER_Z
// depth slices, thinner near the camera (exponential: slice k spans
// near * (far/near)^(k/Z) .. ^((k+1)/Z)). Every frame, on the CPU, each
// point or spot light's sphere of influence (its range) is mapped to the
// clusters it overlaps; lights that reach everywhere -- directional ones,
// and point/spot lights without a range -- go into a global list instead.
// pbr.frag finds its pixel's cluster and shades the global lights plus that
// cluster's own.
//
// The GPU side is three shader storage buffers (std430), bound for pbr.frag:
//   LIGHT_SSBO_BINDING    header (light count, global count) + the lights
//   CLUSTER_SSBO_BINDING  per cluster: (offset, count) into the index list
//   INDEX_SSBO_BINDING    the index list: the global lights first, then
//                         each cluster's run
// plus a few uniforms describing the grid (light_clusters_bind()).

// Lights a scene can hold; beyond it, later ones (world order) are dropped
// and logged once.
#define LIGHTING_MAX_LIGHTS 1024

#define CLUSTER_X 16
#define CLUSTER_Y 9
#define CLUSTER_Z 24
#define CLUSTER_COUNT (CLUSTER_X * CLUSTER_Y * CLUSTER_Z)

// The index list's room: an average of this many lights per cluster. A
// frame that needs more drops the rest (and logs once).
#define CLUSTER_AVERAGE_LIGHTS 32
#define CLUSTER_MAX_INDICES    (CLUSTER_COUNT * CLUSTER_AVERAGE_LIGHTS)

#define LIGHT_SSBO_BINDING   0
#define CLUSTER_SSBO_BINDING 1
#define INDEX_SSBO_BINDING   2

typedef struct light_clusters
{
    rhi_buffer light_buffer;     // header + gpu_light[LIGHTING_MAX_LIGHTS]
    rhi_buffer cluster_buffer;   // uint32 (offset, count) x CLUSTER_COUNT
    rhi_buffer index_buffer;     // uint32 x CLUSTER_MAX_INDICES

    // This frame's lights, from light_clusters_gather(): world order, which
    // is what the shadow modules' light indices refer to.
    light*  lights;
    vec3*   positions;
    uint32  count;
    int32   shadow_light_index;  // the first directional light with cast_shadows: the cascades' light, or -1

    // Scratch for light_clusters_build(), and what it produced.
    struct gpu_light* gpu_lights;   // Renderer/FrameData.h's, after a header: what's uploaded
    uint16*    ranges;           // per light: x0, x1, y0, y1, z0, z1 (x0 > x1 = in no cluster)
    uint32*    clusters;         // (offset, count) x CLUSTER_COUNT
    uint32*    indices;
    uint32     index_count;
    uint32     global_count;
    f32        tile_size[2];     // pixels per cluster tile
    f32        depth_scale;      // slice = floor(log(depth) * depth_scale + depth_bias)
    f32        depth_bias;
    b8         warned_lights;    // logged that lights were dropped
    b8         warned_indices;
} light_clusters;

// GPU buffers and CPU scratch. False (and `lc` safe to destroy) on failure.
b8 light_clusters_create(light_clusters* lc);

// This frame's lights from `w` (world_gather_lights(), World.h), and the
// cascades' light. Requires world_update() on `w`.
void light_clusters_gather(light_clusters* lc, const world* w);

// Assigns the gathered lights to clusters for this camera (`view`,
// `projection` un-jittered; `near` / `far` its planes) on a `width` x
// `height` target, and uploads everything. `unclustered` puts every light
// in the global list instead -- the reference a clustered frame must match.
void light_clusters_build(light_clusters* lc, const mat4 view, const mat4 projection, f32 near, f32 far,
                          uint16 width, uint16 height, b8 unclustered);

// Binds the storage buffers and sets pbr.frag's u_cluster_* and
// u_shadow_light_index uniforms.
void light_clusters_bind(const light_clusters* lc, rhi_shader shader);

// Safe on a zeroed / failed / already-destroyed light_clusters.
void light_clusters_destroy(light_clusters* lc);
