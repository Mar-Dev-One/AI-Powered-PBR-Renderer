#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"

// HDR scene target + final display pass. Lit passes (pbr.frag, skybox.frag)
// write linear, unclamped radiance into an RGBA16F framebuffer between
// post_begin_scene() and post_end_scene(); post_end_scene() then draws one
// fullscreen triangle to the default framebuffer that applies bloom,
// exposure, ACES tone mapping and gamma (engine/shaders/tonemap.frag).
// Keeping tone mapping out of the per-object shaders means blended materials
// blend in linear HDR space, and leaves a single place for later post
// effects (auto-exposure) to hook into.
//
// Anti-aliasing (post_settings.fxaa): tone mapping writes into an RGBA8
// target with each pixel's luma in alpha instead, and fxaa.frag smooths its
// edges into the final picture.
//
// Bloom (post_bloom(), between the two): the scene is halved down a chain
// of up to POST_BLOOM_MAX_MIPS targets (bloom_downsample.frag) and blurred
// back up it (bloom_upsample.frag), then mixed into the picture by its
// strength. No threshold -- the physically based kind: everything spreads a
// little, and only what's far brighter than its surroundings (the sun, a
// lamp, emissive surfaces) visibly glows.

// The shaders' files, under the engine root.
#define POST_VERTEX_PATH          "shaders/fullscreen.vert"
#define POST_FRAGMENT_PATH        "shaders/tonemap.frag"
#define POST_BLOOM_DOWN_PATH      "shaders/bloom_downsample.frag"
#define POST_BLOOM_UP_PATH        "shaders/bloom_upsample.frag"
#define POST_FXAA_PATH            "shaders/fxaa.frag"

#define POST_BLOOM_MAX_MIPS 6

typedef struct post_process
{
    rhi_framebuffer scene_target;   // RGBA16F color + depth, window-sized
    uint16          width;
    uint16          height;

    rhi_shader tonemap_shader;
    rhi_buffer triangle_vertices;   // one clip-space triangle covering the screen
    rhi_buffer triangle_indices;

    rhi_shader      bloom_down_shader;
    rhi_shader      bloom_up_shader;
    rhi_framebuffer bloom_mips[POST_BLOOM_MAX_MIPS];   // RGBA16F, half the scene's size, then half again...
    uint16          bloom_widths[POST_BLOOM_MAX_MIPS];
    uint16          bloom_heights[POST_BLOOM_MAX_MIPS];
    uint32          bloom_mip_count;
    b8              bloom_ready;    // post_bloom() ran since post_begin_scene()

    rhi_shader      fxaa_shader;
    rhi_framebuffer ldr_target;     // RGBA8, window-sized: the tone-mapped picture FXAA reads
} post_process;

// How post_end_scene() turns the scene into the picture.
typedef struct post_settings
{
    f32 exposure;
    b8  raw;     // a debug view's data (0..1): gamma only -- no bloom, exposure or ACES
    f32 bloom;   // 0 (none) .. 1: how much of the blurred scene is mixed in; needs post_bloom() first
    b8  fxaa;    // smooth jagged edges (not with raw: a data view's values stay exact)
} post_settings;

// Loads the shaders and allocates a width x height scene target (and its
// bloom chain). Returns false (and leaves `post` safe to destroy) on failure.
b8 post_create(post_process* post, uint16 width, uint16 height);

// Re-allocates the scene target and bloom chain at the new size. A no-op
// for 0 x 0 (minimized window) or an unchanged size.
void post_resize(post_process* post, uint16 width, uint16 height);

// Binds the HDR scene target (and its viewport) and clears it to the given
// linear color and depth 1. Call after any pass that binds its own
// framebuffer -- csm_render() and the IBL bake both rebind the default one
// when they finish.
void post_begin_scene(post_process* post, f32 r, f32 g, f32 b);

// Blurs the finished scene down and back up the bloom chain, for
// post_end_scene() to mix in. Only worth calling when settings.bloom > 0.
// Leaves a bloom target bound.
void post_bloom(post_process* post);

// Binds the default framebuffer and resolves the scene target onto it:
// mix(color, bloom, settings.bloom) * exposure -> ACES -> gamma 2.2, then
// FXAA if settings.fxaa. Leaves
// the default render state active, so UI drawn afterwards lands on top of
// the tone-mapped image.
void post_end_scene(post_process* post, const post_settings* settings);

// Same, but into `target` (an RGBA8 framebuffer the size of the scene
// target) instead of the screen -- for screenshots. Leaves `target` bound.
void post_end_scene_to(post_process* post, const post_settings* settings, rhi_framebuffer target);

// Safe on a zeroed / failed / already-destroyed post_process.
void post_destroy(post_process* post);
