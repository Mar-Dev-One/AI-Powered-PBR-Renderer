#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Camera.h"
#include "../World/World.h"
#include "IBL.h"
#include "PostProcess.h"
#include "Shadow.h"

// The lit frame, as one unit: cascaded shadows for the world's first
// directional light, an HDR scene target with optional skybox, the PBR
// shader with every per-frame uniform (camera, lights, IBL, cascades), every
// visible MeshRenderer, and the final tone-mapping pass. An app supplies a
// World, a camera and scene_render_settings; it never touches pbr.frag's
// uniforms itself.
//
// Two ways to drive it:
//   - scene_renderer_draw(): the whole frame.
//   - scene_renderer_begin() / scene_renderer_draw_world() /
//     scene_renderer_end(): the same frame in pieces, for an app that also
//     draws geometry that isn't in the World. begin() hands back the bound
//     PBR shader; such draws only set u_model / u_normal_matrix and a
//     material.
//
// pbr.vert/pbr.frag are hot-reloaded: scene_renderer_poll_hot_reload()
// rebuilds them when either file changes on disk, keeping the old program
// if the new source fails to compile.

typedef struct scene_render_settings
{
    const ibl_environment* ibl;   // NULL = pbr.frag's hemisphere ambient and no skybox
    b8  show_skybox;              // draw ibl's environment as the background
    f32 sky_blur;                 // 0..1, see ibl_draw_skybox()
    f32 ambient_strength;
    f32 exposure;
    b8  shadows;                  // cascaded shadows for the first directional light
    f32 cascade_split_lambda;     // see csm_update(); 0.5 is a good default
    b8  debug_show_cascades;      // tint shading by cascade, see pbr.frag
    vec3 clear_color;             // linear HDR background where nothing is drawn
} scene_render_settings;

// Sensible defaults: no IBL, skybox on, ambient 0.5, exposure 1, shadows on.
scene_render_settings scene_render_settings_default(void);

// What hot reload compares to notice a shader file changed: the size as
// well as the mtime, because mtime only has one-second resolution and many
// editors save by truncating then rewriting within the same second.
typedef struct file_stamp
{
    int64 mtime;
    int64 size;
} file_stamp;

typedef struct scene_renderer
{
    rhi_shader   pbr_shader;
    csm_state    csm;
    post_process post;

    file_stamp vertex_stamp;     // pbr.vert as of the last build
    file_stamp fragment_stamp;   // pbr.frag as of the last build
} scene_renderer;

// Builds the PBR shader, `cascade_count` x `shadow_resolution` shadow
// cascades and a width x height HDR target. Returns false (and leaves `r`
// safe to destroy) on failure.
b8 scene_renderer_create(scene_renderer* r, uint16 width, uint16 height,
                         uint32 cascade_count, uint32 shadow_resolution);

// Call on window resize.
void scene_renderer_resize(scene_renderer* r, uint16 width, uint16 height);

// Requires world_update() on `w` this frame. Renders the shadow cascades,
// binds the HDR target, draws the skybox, then binds the PBR shader with all
// per-frame state and returns it.
rhi_shader scene_renderer_begin(scene_renderer* r, const world* w, const camera* cam,
                                const scene_render_settings* settings);

// Draws every visible MeshRenderer in `w` through `shader` (from begin()):
// all opaque submeshes first, then all blended ones, so one entity's
// transparency never gets drawn before another entity's opaque surfaces.
void scene_renderer_draw_world(const world* w, rhi_shader shader);

// Tone-maps the HDR target onto the default framebuffer.
void scene_renderer_end(scene_renderer* r, const scene_render_settings* settings);

// Renders the frame -- exactly what scene_renderer_draw() shows, without
// anything drawn on top such as UI -- into an offscreen buffer and reads it
// back: tightly packed RGBA8, rows bottom-up (row 0 = bottom). The size is
// the current scene target's (the window's). Returns the pixels (caller
// frees) or NULL; leaves the default framebuffer bound.
uint8* scene_renderer_capture(scene_renderer* r, const world* w, const camera* cam,
                              const scene_render_settings* settings, uint16* out_width, uint16* out_height);

// begin() + scene_renderer_draw_world() + end().
void scene_renderer_draw(scene_renderer* r, const world* w, const camera* cam,
                         const scene_render_settings* settings);

// Rebuilds pbr.vert/pbr.frag now. On a compile/link failure the previous
// program stays in use (and the error is logged). Returns true if rebuilt.
b8 scene_renderer_reload_shaders(scene_renderer* r);

// Cheap enough to call every frame (two stat() calls): reloads the shaders
// if either source file changed since the last build.
void scene_renderer_poll_hot_reload(scene_renderer* r);

// Safe on a zeroed / failed / already-destroyed scene_renderer.
void scene_renderer_destroy(scene_renderer* r);
