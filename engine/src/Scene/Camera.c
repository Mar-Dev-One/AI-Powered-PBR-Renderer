#include "Camera.h"

void camera_init(camera* cam,
                  vec3 position,
                  vec3 target,
                  f32 fov_degrees,
                  f32 aspect,
                  f32 near_plane,
                  f32 far_plane)
{
    glm_vec3_copy(position, cam->position);
    glm_vec3_copy(target, cam->target);
    glm_vec3_copy((vec3){ 0.0f, 1.0f, 0.0f }, cam->up);

    cam->fov_degrees = fov_degrees;
    cam->aspect      = aspect;
    cam->near_plane  = near_plane;
    cam->far_plane   = far_plane;
}

void camera_set_aspect(camera* cam, f32 aspect)
{
    cam->aspect = aspect;
}

void camera_get_view(const camera* cam, mat4 out_view)
{
    glm_lookat((f32*)cam->position, (f32*)cam->target, (f32*)cam->up, out_view);
}

void camera_get_projection(const camera* cam, mat4 out_projection)
{
    glm_perspective(glm_rad(cam->fov_degrees), cam->aspect,
                     cam->near_plane, cam->far_plane, out_projection);
}

void camera_get_view_projection(const camera* cam, mat4 out_view_projection)
{
    mat4 view, projection;
    camera_get_view(cam, view);
    camera_get_projection(cam, projection);

    glm_mat4_mul(projection, view, out_view_projection);
}

void camera_screen_ray(const camera* cam, f32 x, f32 y, f32 width, f32 height, vec3 out_origin, vec3 out_direction)
{
    mat4 view_projection, inverse;
    camera_get_view_projection(cam, view_projection);
    glm_mat4_inv(view_projection, inverse);

    // Pixels (y down) -> normalized device coordinates (y up), then back
    // through the inverse view-projection at the near (-1) and far (+1)
    // depths of OpenGL's clip space.
    f32 ndc_x = 2.0f * x / width - 1.0f;
    f32 ndc_y = 1.0f - 2.0f * y / height;

    vec4 near_point, far_point;
    glm_mat4_mulv(inverse, (vec4){ ndc_x, ndc_y, -1.0f, 1.0f }, near_point);
    glm_mat4_mulv(inverse, (vec4){ ndc_x, ndc_y,  1.0f, 1.0f }, far_point);
    glm_vec3_scale(near_point, 1.0f / near_point[3], out_origin);

    vec3 far3;
    glm_vec3_scale(far_point, 1.0f / far_point[3], far3);
    glm_vec3_sub(far3, out_origin, out_direction);
    glm_vec3_normalize(out_direction);
}

b8 camera_world_to_screen(const camera* cam, const vec3 point, f32 width, f32 height, f32* out_x, f32* out_y)
{
    mat4 view_projection;
    camera_get_view_projection(cam, view_projection);

    vec4 clip;
    glm_mat4_mulv(view_projection, (vec4){ point[0], point[1], point[2], 1.0f }, clip);
    if (clip[3] <= 1e-5f)
        return false;

    *out_x = ( clip[0] / clip[3] * 0.5f + 0.5f) * width;
    *out_y = (-clip[1] / clip[3] * 0.5f + 0.5f) * height;
    return true;
}
