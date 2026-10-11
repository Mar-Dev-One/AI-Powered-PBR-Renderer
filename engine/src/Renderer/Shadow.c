#include "Shadow.h"

#include "../Core/Paths.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

b8
csm_create(csm_state* csm, uint32 cascade_count, uint32 resolution)
{
    memset(csm, 0, sizeof(*csm));

    if (cascade_count > CSM_MAX_CASCADES)
    {
        LOG_WARN("csm_create: cascade_count %u > CSM_MAX_CASCADES (%d), clamping",
                 cascade_count, CSM_MAX_CASCADES);
        cascade_count = CSM_MAX_CASCADES;
    }

    char* vertex_path   = asset_path("shaders/shadow_depth.vert");
    char* fragment_path = asset_path("shaders/shadow_depth.frag");

    csm->depth_shader = rhi_shader_create_from_files(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!csm->depth_shader)
    {
        LOG_ERROR("csm_create: failed to load shadow depth shader");
        return false;
    }

    csm->cascade_count = cascade_count;
    csm->resolution    = resolution;

    rhi_framebuffer_desc desc = {
        .width                  = (uint16)resolution,
        .height                 = (uint16)resolution,
        .color_attachments      = NULL,
        .color_attachment_count = 0,
        .has_depth_attachment   = true,
        .depth_compare          = true   // pbr.frag samples these as sampler2DShadow
    };

    for (uint32 i = 0; i < cascade_count; ++i)
        csm->cascades[i].map = rhi_framebuffer_create(desc);

    return true;
}

void
csm_destroy(csm_state* csm)
{
    for (uint32 i = 0; i < csm->cascade_count; ++i)
    {
        if (csm->cascades[i].map)
            rhi_framebuffer_destroy(csm->cascades[i].map);
    }

    if (csm->depth_shader)
        rhi_shader_destroy(csm->depth_shader);

    memset(csm, 0, sizeof(*csm));
}

void
csm_update(csm_state* csm, const camera* cam, const vec3 light_dir, f32 lambda)
{
    vec3 light_dir_normalized;
    glm_vec3_normalize_to((f32*)light_dir, light_dir_normalized);

    // --- Split scheme --------------------------------------------------------
    // Practical split scheme (Zhang et al.): blend a uniform split (equal
    // depth range per cascade -- undersizes the far cascades, since
    // perspective means distant geometry covers far more screen space per
    // world-space unit) with a logarithmic split (equal *ratio* per
    // cascade -- right for perspective falloff, but crowds the near
    // cascades so tight that nearby detail barely gets more texels than
    // far detail). Splitting the difference (lambda = 0.5) gives near
    // cascades that are meaningfully tighter than the far ones without the
    // pure-log scheme's very thin first slice.
    const f32 near_plane = cam->near_plane;
    const f32 far_plane  = cam->far_plane;

    f32 splits[CSM_MAX_CASCADES + 1];
    splits[0]                  = near_plane;
    splits[csm->cascade_count] = far_plane;

    for (uint32 i = 1; i < csm->cascade_count; ++i)
    {
        f32 p       = (f32)i / (f32)csm->cascade_count;
        f32 log_v   = near_plane * powf(far_plane / near_plane, p);
        f32 unif_v  = near_plane + (far_plane - near_plane) * p;
        splits[i]   = glm_lerp(unif_v, log_v, lambda);
    }

    // --- Light basis -----------------------------------------------------
    // The right/up axes of "looking along light_dir" don't depend on where
    // the light camera sits, only on its direction -- compute them once
    // from the origin rather than per cascade.
    vec3 up_hint = { 0.0f, 1.0f, 0.0f };
    if (fabsf(glm_vec3_dot(light_dir_normalized, up_hint)) > 0.999f)
        glm_vec3_copy((vec3){ 0.0f, 0.0f, 1.0f }, up_hint);

    mat4 light_basis;
    glm_lookat((vec3){ 0.0f, 0.0f, 0.0f }, light_dir_normalized, up_hint, light_basis);

    vec3 light_right = { light_basis[0][0], light_basis[0][1], light_basis[0][2] };
    vec3 light_up    = { light_basis[1][0], light_basis[1][1], light_basis[1][2] };

    // --- Camera basis ------------------------------------------------------
    vec3 cam_forward;
    glm_vec3_sub((f32*)cam->target, (f32*)cam->position, cam_forward);
    glm_vec3_normalize(cam_forward);

    vec3 cam_right;
    glm_vec3_crossn(cam_forward, (f32*)cam->up, cam_right);

    vec3 cam_up;
    glm_vec3_cross(cam_right, cam_forward, cam_up);   // already unit length: cam_right and cam_forward are orthonormal

    f32 tan_half_fovy = tanf(glm_rad(cam->fov_degrees) * 0.5f);
    f32 tan_half_fovx = tan_half_fovy * cam->aspect;

    for (uint32 c = 0; c < csm->cascade_count; ++c)
    {
        f32 slice_near = splits[c];
        f32 slice_far  = splits[c + 1];

        // --- Frustum corners, world space -----------------------------------
        // 4 at the slice's near depth, 4 at its far depth -- found directly
        // from the camera's FOV/aspect rather than by unprojecting NDC
        // corners through an inverse projection matrix, since a
        // perspective camera's cross-section at any depth d is just
        // position + forward*d +/- right*(d*tanHalfFovX) +/- up*(d*tanHalfFovY).
        vec3 corners[8];
        f32  depths[2] = { slice_near, slice_far };

        for (int s = 0; s < 2; ++s)
        {
            f32  d = depths[s];
            vec3 center_d, half_right, half_up, corner;

            glm_vec3_scale(cam_forward, d, center_d);
            glm_vec3_add((f32*)cam->position, center_d, center_d);

            glm_vec3_scale(cam_right, d * tan_half_fovx, half_right);
            glm_vec3_scale(cam_up, d * tan_half_fovy, half_up);

            int idx = s * 4;
            for (int sx = -1; sx <= 1; sx += 2)
            {
                for (int sy = -1; sy <= 1; sy += 2)
                {
                    vec3 offset, offset_up;
                    glm_vec3_scale(half_right, (f32)sx, offset);
                    glm_vec3_scale(half_up, (f32)sy, offset_up);
                    glm_vec3_add(offset, offset_up, offset);
                    glm_vec3_add(center_d, offset, corner);
                    glm_vec3_copy(corner, corners[idx++]);
                }
            }
        }

        // --- Bounding sphere ---------------------------------------------------
        // A sphere (rather than a tight per-cascade AABB) keeps the shadow
        // box's size constant as the camera rotates -- an AABB's extents
        // change with orientation, which reintroduces the shimmering the
        // texel snap below is trying to remove.
        vec3 center = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 8; ++i)
            glm_vec3_add(center, corners[i], center);
        glm_vec3_scale(center, 1.0f / 8.0f, center);

        f32 radius = 0.0f;
        for (int i = 0; i < 8; ++i)
        {
            f32 dist = glm_vec3_distance(center, corners[i]);
            if (dist > radius)
                radius = dist;
        }
        radius = glm_max(radius, 0.05f);   // degenerate slice (near ~= far) guard

        // --- Texel snapping ------------------------------------------------
        // Without this, translating/rotating the camera by a fraction of a
        // shadow texel shifts every sampled texel's world-space footprint
        // by that same fraction, which reads as shadow edges crawling
        // ("shimmering") frame to frame. Snapping the box's centre to a
        // whole number of texels, measured along the light's own right/up
        // axes, keeps that footprint fixed between frames unless the
        // camera moves by a full texel or more.
        f32 texel_size = (radius * 2.0f) / (f32)csm->resolution;

        f32 dist_right = glm_vec3_dot(center, light_right);
        f32 dist_up    = glm_vec3_dot(center, light_up);

        f32 snapped_right = floorf(dist_right / texel_size) * texel_size;
        f32 snapped_up    = floorf(dist_up / texel_size) * texel_size;

        vec3 correction;
        glm_vec3_scale(light_right, snapped_right - dist_right, correction);
        glm_vec3_add(center, correction, center);
        glm_vec3_scale(light_up, snapped_up - dist_up, correction);
        glm_vec3_add(center, correction, center);

        // --- Light camera ----------------------------------------------------
        // Pulled back 2x the sphere radius (not just `radius`) so casters
        // sitting behind the visible slice along -light_dir -- outside the
        // sphere itself, but still able to shadow something inside it --
        // are in front of the light's near plane rather than clipped away.
        vec3 light_eye, back_off;
        glm_vec3_scale(light_dir_normalized, -(radius * 2.0f), back_off);
        glm_vec3_add(center, back_off, light_eye);

        mat4 light_view, light_proj;
        glm_lookat(light_eye, center, up_hint, light_view);
        glm_ortho(-radius, radius, -radius, radius, 0.01f, radius * 4.0f, light_proj);
        glm_mat4_mul(light_proj, light_view, csm->cascades[c].view_projection);

        csm->cascades[c].split_far        = slice_far;
        csm->cascades[c].texel_world_size = texel_size;
    }
}

void
csm_render(csm_state* csm, const scene* s)
{
    rhi_shader_bind(csm->depth_shader);

    for (uint32 i = 0; i < csm->cascade_count; ++i)
    {
        rhi_framebuffer_bind(csm->cascades[i].map);
        rhi_clear(0.0f, 0.0f, 0.0f, 1.0f);   // color ignored (no color attachment); also clears depth to 1.0

        scene_draw_models_depth(s, csm->depth_shader, csm->cascades[i].view_projection);
    }

    rhi_framebuffer_bind_default();
}

void
csm_bind(const csm_state* csm, rhi_shader pbr_shader)
{
    rhi_shader_set_int(pbr_shader, "u_cascade_count", (int32)csm->cascade_count);

    for (uint32 i = 0; i < csm->cascade_count; ++i)
    {
        char name[48];

        rhi_texture_bind(rhi_framebuffer_get_depth_texture(csm->cascades[i].map), CSM_SLOT_FIRST + i);

        snprintf(name, sizeof(name), "u_shadow_maps[%u]", i);
        rhi_shader_set_int(pbr_shader, name, (int32)(CSM_SLOT_FIRST + i));

        snprintf(name, sizeof(name), "u_light_view_projection[%u]", i);
        rhi_shader_set_mat4(pbr_shader, name, (const f32*)csm->cascades[i].view_projection);

        snprintf(name, sizeof(name), "u_cascade_splits[%u]", i);
        rhi_shader_set_float(pbr_shader, name, csm->cascades[i].split_far);

        snprintf(name, sizeof(name), "u_cascade_texel_sizes[%u]", i);
        rhi_shader_set_float(pbr_shader, name, csm->cascades[i].texel_world_size);
    }
}
