#include "Lighting.h"
#include "FrameData.h"

#include <math.h>
#include <string.h>

// The light buffer's header, before the lights (std430: the array starts
// 16 bytes in, a struct with vec3s aligning to 16).
typedef struct gpu_light_header
{
    uint32 light_count;
    uint32 global_count;
    uint32 pad[2];
} gpu_light_header;

b8
light_clusters_create(light_clusters* lc)
{
    memset(lc, 0, sizeof(*lc));

    lc->light_buffer   = rhi_storage_buffer_create(sizeof(gpu_light_header) + sizeof(gpu_light) * LIGHTING_MAX_LIGHTS);
    lc->cluster_buffer = rhi_storage_buffer_create(sizeof(uint32) * 2 * CLUSTER_COUNT);
    lc->index_buffer   = rhi_storage_buffer_create(sizeof(uint32) * CLUSTER_MAX_INDICES);

    lc->lights     = malloc(sizeof(light) * LIGHTING_MAX_LIGHTS);
    lc->positions  = malloc(sizeof(vec3) * LIGHTING_MAX_LIGHTS);
    lc->gpu_lights = malloc(sizeof(gpu_light_header) + sizeof(gpu_light) * LIGHTING_MAX_LIGHTS);
    lc->ranges     = malloc(sizeof(uint16) * 6 * LIGHTING_MAX_LIGHTS);
    lc->clusters   = malloc(sizeof(uint32) * 2 * CLUSTER_COUNT);
    lc->indices    = malloc(sizeof(uint32) * CLUSTER_MAX_INDICES);

    return lc->light_buffer && lc->cluster_buffer && lc->index_buffer && lc->lights && lc->positions
        && lc->gpu_lights && lc->ranges && lc->clusters && lc->indices;
}

void
light_clusters_gather(light_clusters* lc, const world* w)
{
    lc->count = world_gather_lights(w, lc->lights, lc->positions, LIGHTING_MAX_LIGHTS);
    if (lc->count == LIGHTING_MAX_LIGHTS && !lc->warned_lights)
    {
        LOG_WARN("lighting: the scene has %u lights or more; only the first %u light it\n", LIGHTING_MAX_LIGHTS,
                 LIGHTING_MAX_LIGHTS);
        lc->warned_lights = true;
    }

    lc->shadow_light_index = -1;
    for (uint32 i = 0; i < lc->count && lc->shadow_light_index < 0; ++i)
        if (lc->lights[i].type == LIGHT_DIRECTIONAL && lc->lights[i].cast_shadows)
            lc->shadow_light_index = (int32)i;
}

static b8
is_global(const light* l)
{
    return l->type == LIGHT_DIRECTIONAL || l->range <= 0.0f;
}

static int
clamp_int(int value, int lo, int hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

static int
depth_slice(const light_clusters* lc, f32 depth)
{
    return clamp_int((int)floorf(logf(depth) * lc->depth_scale + lc->depth_bias), 0, CLUSTER_Z - 1);
}

// The clusters light `i`'s sphere overlaps, into lc->ranges[i]: false if
// none (it's out of view).
static b8
light_cluster_range(light_clusters* lc, uint32 i, const mat4 view, const mat4 projection, f32 near, f32 far,
                    uint16 width, uint16 height)
{
    f32 radius = lc->lights[i].range;

    vec3 center;
    glm_mat4_mulv3((vec4*)view, lc->positions[i], 1.0f, center);
    f32 depth = -center[2];   // the camera looks down -z

    f32 depth_near = depth - radius, depth_far = depth + radius;
    if (depth_far < near || depth_near > far)
        return false;

    int z0 = depth_slice(lc, fmaxf(depth_near, near));
    int z1 = depth_slice(lc, fminf(depth_far, far));

    // On screen: project the sphere's view-space box. If the sphere reaches
    // behind the near plane the projection breaks down -- then every tile.
    int x0 = 0, x1 = CLUSTER_X - 1, y0 = 0, y1 = CLUSTER_Y - 1;
    if (depth_near > near)
    {
        f32 lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
        for (int corner = 0; corner < 8; ++corner)
        {
            vec4 p = { center[0] + ((corner & 1) ? radius : -radius), center[1] + ((corner & 2) ? radius : -radius),
                       center[2] + ((corner & 4) ? radius : -radius), 1.0f };
            vec4 clip;
            glm_mat4_mulv((vec4*)projection, p, clip);
            for (int a = 0; a < 2; ++a)
            {
                f32 ndc = clip[a] / clip[3];
                lo[a] = fminf(lo[a], ndc);
                hi[a] = fmaxf(hi[a], ndc);
            }
        }
        if (hi[0] < -1.0f || lo[0] > 1.0f || hi[1] < -1.0f || lo[1] > 1.0f)
            return false;   // off screen

        // In pixels, a pixel wider each way: TAA's jitter moves pixels by
        // up to half of one.
        f32 px0 = (lo[0] * 0.5f + 0.5f) * width - 1.0f, px1 = (hi[0] * 0.5f + 0.5f) * width + 1.0f;
        f32 py0 = (lo[1] * 0.5f + 0.5f) * height - 1.0f, py1 = (hi[1] * 0.5f + 0.5f) * height + 1.0f;
        x0 = clamp_int((int)floorf(px0 / lc->tile_size[0]), 0, CLUSTER_X - 1);
        x1 = clamp_int((int)floorf(px1 / lc->tile_size[0]), 0, CLUSTER_X - 1);
        y0 = clamp_int((int)floorf(py0 / lc->tile_size[1]), 0, CLUSTER_Y - 1);
        y1 = clamp_int((int)floorf(py1 / lc->tile_size[1]), 0, CLUSTER_Y - 1);
    }

    uint16* r = &lc->ranges[i * 6];
    r[0] = (uint16)x0, r[1] = (uint16)x1, r[2] = (uint16)y0, r[3] = (uint16)y1, r[4] = (uint16)z0, r[5] = (uint16)z1;
    return true;
}

void
light_clusters_build(light_clusters* lc, const mat4 view, const mat4 projection, f32 near, f32 far,
                     uint16 width, uint16 height, b8 unclustered)
{
    lc->tile_size[0] = (f32)width / (f32)CLUSTER_X;
    lc->tile_size[1] = (f32)height / (f32)CLUSTER_Y;
    lc->depth_scale  = (f32)CLUSTER_Z / logf(far / near);
    lc->depth_bias   = -(f32)CLUSTER_Z * logf(near) / logf(far / near);

    // --- The lights, as pbr.frag reads them ---------------------------------------
    gpu_light_header* header = (gpu_light_header*)lc->gpu_lights;
    gpu_light*        out    = (gpu_light*)(header + 1);
    memset(out, 0, sizeof(gpu_light) * lc->count);
    for (uint32 i = 0; i < lc->count; ++i)
    {
        const light* l = &lc->lights[i];
        out[i].type      = (int32)l->type;
        out[i].intensity = l->intensity;
        out[i].range     = l->range;
        glm_vec3_copy((f32*)l->color, out[i].color);
        glm_vec3_copy((f32*)l->direction, out[i].direction);
        glm_vec3_copy(lc->positions[i], out[i].position);
        // Precomputed here (one cosf() per light per frame) rather than in
        // the shader (one per fragment per light).
        out[i].inner_cos = cosf(glm_rad(l->inner_cone_degrees));
        out[i].outer_cos = cosf(glm_rad(l->outer_cone_degrees));
    }

    // --- Global lights first in the index list --------------------------------------
    uint32 n = 0;
    for (uint32 i = 0; i < lc->count; ++i)
        if (unclustered || is_global(&lc->lights[i]))
            lc->indices[n++] = i;
    lc->global_count = n;

    // --- Clustered ones: count per cluster, lay out the runs, fill them ------------
    uint32* counts = lc->clusters;   // (offset, count) pairs; offsets filled below
    memset(counts, 0, sizeof(uint32) * 2 * CLUSTER_COUNT);

    for (uint32 i = 0; i < lc->count; ++i)
    {
        uint16* r = &lc->ranges[i * 6];
        if (unclustered || is_global(&lc->lights[i]) || !light_cluster_range(lc, i, view, projection, near, far, width, height))
        {
            r[0] = 1, r[1] = 0;   // in no cluster
            continue;
        }
        for (int z = r[4]; z <= r[5]; ++z)
            for (int y = r[2]; y <= r[3]; ++y)
                for (int x = r[0]; x <= r[1]; ++x)
                    ++counts[2 * (x + CLUSTER_X * (y + CLUSTER_Y * z)) + 1];
    }

    for (uint32 c = 0; c < CLUSTER_COUNT; ++c)
    {
        uint32 wanted = counts[2 * c + 1];
        uint32 room   = n < CLUSTER_MAX_INDICES ? CLUSTER_MAX_INDICES - n : 0;
        if (wanted > room && !lc->warned_indices)
        {
            LOG_WARN("lighting: too many lights overlap in the view; some clusters drop lights\n");
            lc->warned_indices = true;
        }
        counts[2 * c]     = n;
        counts[2 * c + 1] = 0;
        n += wanted < room ? wanted : room;
        // The run's capacity is what's left before the next offset; the
        // count is filled as lights are added.
    }
    lc->index_count = n;

    for (uint32 i = 0; i < lc->count; ++i)
    {
        const uint16* r = &lc->ranges[i * 6];
        if (r[0] > r[1])
            continue;
        for (int z = r[4]; z <= r[5]; ++z)
            for (int y = r[2]; y <= r[3]; ++y)
                for (int x = r[0]; x <= r[1]; ++x)
                {
                    uint32 c     = (uint32)(x + CLUSTER_X * (y + CLUSTER_Y * z));
                    uint32 end   = c + 1 < CLUSTER_COUNT ? counts[2 * (c + 1)] : lc->index_count;
                    uint32 at    = counts[2 * c] + counts[2 * c + 1];
                    if (at < end)
                    {
                        lc->indices[at] = i;
                        ++counts[2 * c + 1];
                    }
                }
    }

    // --- Upload --------------------------------------------------------------------
    header->light_count  = lc->count;
    header->global_count = lc->global_count;
    rhi_buffer_update(lc->light_buffer, lc->gpu_lights, sizeof(gpu_light_header) + sizeof(gpu_light) * lc->count, 0);
    rhi_buffer_update(lc->cluster_buffer, lc->clusters, sizeof(uint32) * 2 * CLUSTER_COUNT, 0);
    if (lc->index_count)
        rhi_buffer_update(lc->index_buffer, lc->indices, sizeof(uint32) * lc->index_count, 0);
}

void
light_clusters_bind(const light_clusters* lc, rhi_shader shader)
{
    rhi_storage_buffer_bind(lc->light_buffer, LIGHT_SSBO_BINDING);
    rhi_storage_buffer_bind(lc->cluster_buffer, CLUSTER_SSBO_BINDING);
    rhi_storage_buffer_bind(lc->index_buffer, INDEX_SSBO_BINDING);

    rhi_shader_set_vec2(shader, "u_cluster_tile", lc->tile_size[0], lc->tile_size[1]);
    rhi_shader_set_vec2(shader, "u_cluster_depth", lc->depth_scale, lc->depth_bias);
    rhi_shader_set_int(shader, "u_shadow_light_index", lc->shadow_light_index);
}

void
light_clusters_destroy(light_clusters* lc)
{
    if (lc->light_buffer)
        rhi_buffer_destroy(lc->light_buffer);
    if (lc->cluster_buffer)
        rhi_buffer_destroy(lc->cluster_buffer);
    if (lc->index_buffer)
        rhi_buffer_destroy(lc->index_buffer);
    free(lc->lights);
    free(lc->positions);
    free(lc->gpu_lights);
    free(lc->ranges);
    free(lc->clusters);
    free(lc->indices);
    memset(lc, 0, sizeof(*lc));
}
