#pragma once

#include "../Core/Defines.h"
#include "Camera.h"

#include <cglm/cglm.h>

// Mouse-orbit control for a Camera (Camera.h stays dumb: position + target).
// The camera circles its target at `distance`, from `yaw` degrees around +Y
// (0 = looking down -Z from +Z) and `pitch` degrees above the horizon. Every
// function that changes the orbit recomputes cam->position from it; none
// moves cam->target except orbit_fit().

#define ORBIT_DEG_PER_PIXEL 0.25f
#define ORBIT_PITCH_LIMIT   89.0f   // degrees; straight up or down would flip the view
#define ORBIT_MIN_DISTANCE  0.5f
#define ORBIT_MAX_DISTANCE  500.0f

typedef struct orbit_camera
{
    f32 yaw;
    f32 pitch;
    f32 distance;
} orbit_camera;

// cam->position from the orbit, around cam->target.
void orbit_apply(const orbit_camera* orbit, camera* cam);

// The orbit that puts the camera where it already is -- so orbiting carries
// on from wherever something else (a command) moved it.
void orbit_from_camera(orbit_camera* orbit, const camera* cam);

// A mouse drag of (dx, dy) pixels, y down: right turns the view right, up
// tilts it up.
void orbit_drag(orbit_camera* orbit, camera* cam, f32 dx, f32 dy);

// Wheel zoom by a share of the distance, so it feels the same close up and
// across a big scene; clamped to ORBIT_MIN/MAX_DISTANCE.
void orbit_zoom(orbit_camera* orbit, camera* cam, f32 wheel);

// Aims at the middle of [lo, hi] and backs off until a sphere around it fits
// the vertical field of view, times `margin` (1 = touching the edges).
// Keeps the viewing angle. Not clamped, so a huge scene still fits.
void orbit_fit(orbit_camera* orbit, camera* cam, const vec3 lo, const vec3 hi, f32 margin);
