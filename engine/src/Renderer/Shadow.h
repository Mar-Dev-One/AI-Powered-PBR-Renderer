#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Camera.h"
#include "../Scene/Scene.h"

#include <cglm/cglm.h>

// Cascaded shadow mapping for the scene's key directional light.
//
// The old approach (a single ortho box, fixed at [-2, 2] on every axis and
// pushed back 6 units along -light_dir) only worked because the demo always
// re-centred whatever model it loaded into a 2-unit cube first
// (model_get_fit_transform() in Scene/Model.h) -- the shadow box was sized
// for that cube, not for a real scene. Anything wider than the cube (the
// ground plane, a second model, a scene that isn't recentred to the origin)
// either fell outside the box (no shadow) or, if the box were just made
// bigger to compensate, every model shrank to a handful of shadow-map
// texels regardless of camera distance (blocky shadows up close).
//
// CSM fixes both by tying the shadow box to the *camera* instead of the
// scene: the camera's view frustum is sliced into CSM_MAX_CASCADES depth
// ranges (near cascades small and close -- high texel density where detail
// is visible -- far cascades big and coarse), and each slice gets its own
// tightly-fit ortho box computed fresh every frame from camera.fov/aspect/
// near/far. Scale of the scene no longer matters; only distance from the
// camera does.
#define CSM_MAX_CASCADES 4

// Fixed texture units the cascades live on when bound for drawing.
// Material.h owns 0..4 (MATERIAL_SLOT_COUNT); IBL.h's IBL_SLOT_* start
// right after CSM_SLOT_FIRST..CSM_SLOT_FIRST+CSM_MAX_CASCADES-1 (see that
// header's own comment, updated to match).
#define CSM_SLOT_FIRST 5

typedef struct shadow_cascade
{
    rhi_framebuffer map;              // depth-only, resolution x resolution
    mat4            view_projection;  // light's view * projection for this slice; recomputed every csm_update()
    f32             split_far;        // view-space depth (distance along the camera's forward axis) this cascade's far edge sits at
} shadow_cascade;

typedef struct csm_state
{
    shadow_cascade cascades[CSM_MAX_CASCADES];
    uint32         cascade_count;
    uint32         resolution;
    rhi_shader     depth_shader;   // shadow_depth.vert/frag -- owned here so csm_render() is self-contained
} csm_state;

// Loads shadow_depth.vert/frag and allocates `cascade_count` (clamped to
// CSM_MAX_CASCADES) depth-only framebuffers at `resolution` x `resolution`.
// Returns false (and leaves *csm zeroed, safe to pass to csm_destroy) if the
// shader fails to build.
b8   csm_create(csm_state* csm, uint32 cascade_count, uint32 resolution);
void csm_destroy(csm_state* csm);

// Recomputes every cascade's split distance and light view-projection from
// this frame's camera and light direction. `light_dir` is the direction the
// light travels (toward the surface), same convention as pbr.frag's
// u_lights[].direction -- not required to be pre-normalized. `lambda` blends the
// near/far split scheme between fully uniform (0.0) and fully logarithmic
// (1.0); 0.5 is a good default (see csm_update()'s comment for why).
// Call once per frame before csm_render().
void csm_update(csm_state* csm, const camera* cam, const vec3 light_dir, f32 lambda);

// Renders every cascade's depth-only pass: binds each cascade's framebuffer
// in turn and draws `s` through csm's own depth shader with that cascade's
// view_projection. Leaves the default framebuffer bound afterwards.
// Requires scene_update_world_transforms() to have been called on `s` this
// frame (same requirement as scene_draw_models_depth() itself).
void csm_render(csm_state* csm, const scene* s);

// Per-frame setup for the PBR shader (assets/shaders/pbr.frag): binds each
// cascade's depth texture to CSM_SLOT_FIRST + i and sets u_cascade_count,
// u_cascade_splits[], and u_light_view_projection[]. Call after csm_update()
// (and, for the frame's shadows to be current, after csm_render()) and
// before drawing lit geometry.
void csm_bind(const csm_state* csm, rhi_shader pbr_shader);
