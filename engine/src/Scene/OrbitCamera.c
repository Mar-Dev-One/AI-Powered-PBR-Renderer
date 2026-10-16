#include "OrbitCamera.h"

#include <math.h>

void
orbit_apply(const orbit_camera* orbit, camera* cam)
{
    f32 yaw_rad   = glm_rad(orbit->yaw);
    f32 pitch_rad = glm_rad(orbit->pitch);

    cam->position[0] = cam->target[0] + orbit->distance * cosf(pitch_rad) * sinf(yaw_rad);
    cam->position[1] = cam->target[1] + orbit->distance * sinf(pitch_rad);
    cam->position[2] = cam->target[2] + orbit->distance * cosf(pitch_rad) * cosf(yaw_rad);
}

void
orbit_from_camera(orbit_camera* orbit, const camera* cam)
{
    vec3 offset;
    glm_vec3_sub((f32*)cam->position, (f32*)cam->target, offset);

    orbit->distance = glm_max(glm_vec3_norm(offset), 0.01f);
    orbit->yaw      = glm_deg(atan2f(offset[0], offset[2]));
    orbit->pitch    = glm_deg(asinf(glm_clamp(offset[1] / orbit->distance, -1.0f, 1.0f)));
}

void
orbit_drag(orbit_camera* orbit, camera* cam, f32 dx, f32 dy)
{
    orbit->yaw  += dx * ORBIT_DEG_PER_PIXEL;
    orbit->pitch = glm_clamp(orbit->pitch - dy * ORBIT_DEG_PER_PIXEL, -ORBIT_PITCH_LIMIT, ORBIT_PITCH_LIMIT);
    orbit_apply(orbit, cam);
}

void
orbit_zoom(orbit_camera* orbit, camera* cam, f32 wheel)
{
    const f32 zoom_speed = 0.1f;

    orbit->distance -= wheel * zoom_speed * orbit->distance;
    orbit->distance  = glm_clamp(orbit->distance, ORBIT_MIN_DISTANCE, ORBIT_MAX_DISTANCE);
    orbit_apply(orbit, cam);
}

void
orbit_fit(orbit_camera* orbit, camera* cam, const vec3 lo, const vec3 hi, f32 margin)
{
    glm_vec3_center((f32*)lo, (f32*)hi, cam->target);
    f32 radius = glm_max(glm_vec3_distance((f32*)lo, (f32*)hi) * 0.5f, 0.05f);

    orbit->distance = radius / sinf(glm_rad(cam->fov_degrees) * 0.5f) * margin;
    orbit_apply(orbit, cam);
}
