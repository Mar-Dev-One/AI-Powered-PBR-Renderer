#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"

// HDR scene target + final display pass. Lit passes (pbr.frag, skybox.frag)
// write linear, unclamped radiance into an RGBA16F framebuffer between
// post_begin_scene() and post_end_scene(); post_end_scene() then draws one
// fullscreen triangle to the default framebuffer that applies exposure, ACES
// tone mapping and gamma (engine/shaders/tonemap.frag). Keeping tone mapping
// out of the per-object shaders means blended materials blend in linear HDR
// space, and leaves a single place for later post effects (bloom, AA,
// auto-exposure) to hook into.

// The tone-mapping shader's files, under the engine root.
#define POST_VERTEX_PATH   "shaders/fullscreen.vert"
#define POST_FRAGMENT_PATH "shaders/tonemap.frag"

typedef struct post_process
{
    rhi_framebuffer scene_target;   // RGBA16F color + depth, window-sized
    uint16          width;
    uint16          height;

    rhi_shader tonemap_shader;
    rhi_buffer triangle_vertices;   // one clip-space triangle covering the screen
    rhi_buffer triangle_indices;
} post_process;

// Loads the tone-mapping shader and allocates a width x height scene target.
// Returns false (and leaves `post` safe to destroy) on failure.
b8 post_create(post_process* post, uint16 width, uint16 height);

// Re-allocates the scene target at the new size. A no-op for 0 x 0
// (minimized window) or an unchanged size.
void post_resize(post_process* post, uint16 width, uint16 height);

// Binds the HDR scene target (and its viewport) and clears it to the given
// linear color and depth 1. Call after any pass that binds its own
// framebuffer -- csm_render() and the IBL bake both rebind the default one
// when they finish.
void post_begin_scene(post_process* post, f32 r, f32 g, f32 b);

// Binds the default framebuffer and resolves the scene target onto it:
// color * exposure -> ACES -> gamma 2.2. `raw` skips exposure and ACES (gamma
// only), for a debug view whose pixels are data in 0..1 rather than light.
// Leaves the default render state active, so UI drawn afterwards lands on
// top of the tone-mapped image.
void post_end_scene(post_process* post, f32 exposure, b8 raw);

// Same, but into `target` (an RGBA8 framebuffer the size of the scene
// target) instead of the screen -- for screenshots. Leaves `target` bound.
void post_end_scene_to(post_process* post, f32 exposure, b8 raw, rhi_framebuffer target);

// Safe on a zeroed / failed / already-destroyed post_process.
void post_destroy(post_process* post);
