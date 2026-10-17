#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Camera.h"
#include "../World/Environment.h"
#include "../World/World.h"
#include "GpuProfiler.h"
#include "IBL.h"
#include "PostProcess.h"
#include "ShaderReload.h"
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
// Each frame's passes are timed on the GPU ("Shadows", "Sky", "Scene",
// "Bloom", "Post"; scene_renderer.profiler.timings). "Scene" runs from the end of
// begin() to end(), so it includes whatever the app draws in between.
//
// Its shaders are hot-reloaded -- PBR, tone mapping, bloom and shadow depth:
// scene_renderer_poll_hot_reload() rebuilds one when its files change on
// disk, keeping the old program if the new source fails to compile. (The
// IBL's own shaders reload through ibl_cache_poll_hot_reload().)

// What the frame shows: the lit scene, or one input to its shading. The
// data views (albedo through shadow, and wireframe) show values in 0..1
// as they are -- no exposure or tone mapping, no skybox -- so a black
// pixel in "Metallic" really is 0. Lighting is the lit scene with every
// surface white and no emission: the light alone.
typedef enum scene_debug_view
{
    SCENE_VIEW_LIT,
    SCENE_VIEW_ALBEDO,      // base color, textures included (linear, shown as sRGB)
    SCENE_VIEW_NORMALS,     // world-space shading normal (normal maps applied), xyz -> rgb
    SCENE_VIEW_ROUGHNESS,
    SCENE_VIEW_METALLIC,
    SCENE_VIEW_AO,          // the occlusion texture (white without one)
    SCENE_VIEW_SHADOW,      // the shadowed light's visibility: black = in shadow
    SCENE_VIEW_LIGHTING,
    SCENE_VIEW_WIREFRAME,
    SCENE_VIEW_COUNT
} scene_debug_view;

// "lit", "albedo", ... (the render.set command's names); NULL out of range.
const char* scene_debug_view_name(scene_debug_view view);

// The inverse; false if `name` isn't one.
b8 scene_debug_view_from_name(const char* name, scene_debug_view* out_view);

typedef struct scene_render_settings
{
    const ibl_environment* ibl;   // NULL = pbr.frag's hemisphere ambient and no skybox
    b8  show_skybox;              // draw ibl's environment as the background
    f32 sky_blur;                 // 0..1, see ibl_draw_skybox()
    f32 ambient_strength;
    f32 exposure;
    f32 bloom;                    // 0 = off; see Renderer/PostProcess.h
    b8  shadows;                  // cascaded shadows for the first directional light
    f32 cascade_split_lambda;     // see csm_update(); 0.5 is a good default
    b8  debug_show_cascades;      // tint shading by cascade, see pbr.frag
    scene_debug_view debug_view;  // SCENE_VIEW_LIT for the real thing
    vec3 clear_color;             // linear HDR background where nothing is drawn
} scene_render_settings;

// Sensible defaults: no IBL, skybox on, ambient 0.5, exposure 1, bloom 0.04,
// shadows on.
scene_render_settings scene_render_settings_default(void);

// Copies a scene's environment into `settings`: the lighting values, and
// `ibl` as the IBL when env->use_ibl is set and `ibl` holds a baked HDRI
// (may be NULL). Baking env->hdri into `ibl` is the caller's job.
void scene_render_settings_apply_environment(scene_render_settings* settings, const scene_environment* env,
                                             const ibl_environment* ibl);

typedef struct scene_renderer
{
    rhi_shader   pbr_shader;
    csm_state    csm;
    post_process post;
    gpu_profiler profiler;
    uint32       scene_scope;    // "Scene": from begin() to end()

    // Hot reload: each shader's files.
    file_watch pbr_files;
    file_watch post_files;
    file_watch bloom_files;
    file_watch shadow_files;
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

// The whole frame, like scene_renderer_draw(), tone-mapped into `target`
// (an RGBA8 framebuffer the size of the scene target, see
// scene_renderer_resize()) instead of the screen -- for a view shown inside
// a UI panel. Binds the default framebuffer again afterwards.
void scene_renderer_draw_to(scene_renderer* r, const world* w, const camera* cam,
                            const scene_render_settings* settings, rhi_framebuffer target);

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

// Rebuilds every shader it owns now. One that fails to compile or link
// keeps its previous program (and the error is logged). True if all rebuilt.
b8 scene_renderer_reload_shaders(scene_renderer* r);

// Cheap enough to call every frame (a stat() per shader file): rebuilds
// each shader whose files changed since it was last built.
void scene_renderer_poll_hot_reload(scene_renderer* r);

// Safe on a zeroed / failed / already-destroyed scene_renderer.
void scene_renderer_destroy(scene_renderer* r);
