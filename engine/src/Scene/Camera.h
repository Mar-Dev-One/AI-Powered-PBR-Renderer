#pragma once

#include "../Core/Defines.h"

#include <cglm/cglm.h>

// A simple look-at perspective camera. Kept deliberately dumb (no
// controller/input logic in here) — Camera only knows how to turn its own
// state into view/projection matrices; something above it (App/demo code,
// eventually a proper Scene) is responsible for moving position/target
// around in response to input or animation.
typedef struct camera
{
    vec3 position;
    vec3 target;
    vec3 up;

    f32 fov_degrees;
    f32 aspect;        // width / height
    f32 near_plane;
    f32 far_plane;
} camera;

void camera_init(camera* cam,
                  vec3 position,
                  vec3 target,
                  f32 fov_degrees,
                  f32 aspect,
                  f32 near_plane,
                  f32 far_plane);

// Call on window resize so the projection doesn't stretch/squash.
void camera_set_aspect(camera* cam, f32 aspect);

void camera_get_view(const camera* cam, mat4 out_view);
void camera_get_projection(const camera* cam, mat4 out_projection);

// Convenience: out_view_projection = projection * view.
void camera_get_view_projection(const camera* cam, mat4 out_view_projection);

// Screen <-> world, for picking and drawing over the scene. Screen points are
// in pixels from the top-left of a `width` x `height` view, the way mouse
// coordinates come.

// The ray through screen point (x, y): it starts on the near plane and
// out_direction is normalized.
void camera_screen_ray(const camera* cam, f32 x, f32 y, f32 width, f32 height, vec3 out_origin, vec3 out_direction);

// Where a world point lands on screen. False (outputs untouched) if it is
// behind the camera.
b8 camera_world_to_screen(const camera* cam, const vec3 point, f32 width, f32 height, f32* out_x, f32* out_y);
