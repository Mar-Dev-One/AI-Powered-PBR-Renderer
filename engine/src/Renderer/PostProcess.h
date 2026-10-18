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
// Auto-exposure (post_meter_exposure(), post_settings.auto_exposure): the
// scene's center-weighted average log luminance, measured on the GPU
// (exposure_luminance.frag, then halved down to 1 x 1), drifts into an
// adapted value (exposure_adapt.frag) that tone mapping exposes to middle
// grey. Nothing is read back to the CPU, so it never stalls.
//
// Ambient occlusion (post_ssao(), between a depth pre-pass and the main
// pass): ssao.frag estimates from the scene's depth how much sky each
// pixel's surface sees, ssao_blur.frag cleans it up, and pbr.frag reads it
// (POST_SSAO_SLOT) to darken its ambient light in creases and contact areas.
//
// Temporal anti-aliasing (post_taa(), first after the scene pass): the
// frame, rendered with a sub-pixel camera shift that changes every frame
// (SceneRenderer.c), is blended into a history reprojected from the last
// frame (taa.frag). Everything after it -- bloom, auto-exposure, tone
// mapping -- reads the result instead of the scene target.
//
// FXAA (post_settings.fxaa): tone mapping writes into an RGBA8
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
#define POST_LUMINANCE_PATH       "shaders/exposure_luminance.frag"
#define POST_REDUCE_PATH          "shaders/exposure_reduce.frag"
#define POST_ADAPT_PATH           "shaders/exposure_adapt.frag"
#define POST_SSAO_PATH            "shaders/ssao.frag"
#define POST_SSAO_BLUR_PATH       "shaders/ssao_blur.frag"
#define POST_TAA_PATH             "shaders/taa.frag"

// The texture unit pbr.frag reads the AO from: after IBL.h's units (9..12).
#define POST_SSAO_SLOT 13

// Auto-exposure measures the scene at this size (a power of two), then
// halves it down to 1 x 1.
#define POST_METER_SIZE   128
#define POST_METER_LEVELS 8   // 128, 64, ..., 1

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

    rhi_shader      luminance_shader;
    rhi_shader      reduce_shader;
    rhi_shader      adapt_shader;
    rhi_framebuffer meter[POST_METER_LEVELS];   // RGBA16F, POST_METER_SIZE down to 1 x 1
    rhi_framebuffer adapted[2];     // 1 x 1 RGBA16F, ping-pong: r = adapted log2 luminance
    uint32          adapted_current;
    b8              adapted_valid;  // adapted[adapted_current] holds a value

    rhi_shader      ssao_shader;
    rhi_shader      ssao_blur_shader;
    rhi_framebuffer ssao_raw;       // RGBA8, window-sized: ssao.frag's noisy output
    rhi_framebuffer ssao_target;    // RGBA8, window-sized: blurred, what pbr.frag reads
    b8              ssao_ready;     // post_ssao() ran since post_begin_scene()

    rhi_shader      taa_shader;
    rhi_framebuffer taa_history[2]; // RGBA16F, window-sized, ping-pong: the last result and the next
    uint32          taa_current;    // taa_history[taa_current] holds the last result
    b8              taa_valid;      // ...and it's from the frame just before this one
    b8              taa_ready;      // post_taa() ran since post_begin_scene()
} post_process;

// How post_end_scene() turns the scene into the picture.
typedef struct post_settings
{
    f32 exposure;
    b8  raw;     // a debug view's data (0..1): gamma only -- no bloom, exposure or ACES
    f32 bloom;   // 0 (none) .. 1: how much of the blurred scene is mixed in; needs post_bloom() first
    b8  fxaa;    // smooth jagged edges (not with raw: a data view's values stay exact)
    int tonemap; // World/Environment.h's scene_tonemap
    b8  auto_exposure;   // expose to the adapted brightness (post_meter_exposure()), `exposure` on top
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

// Binds the scene target again (and its viewport) without clearing it, to
// carry on drawing after a pass that bound its own framebuffer.
void post_resume_scene(post_process* post);

// Ambient occlusion from the scene target's depth -- which must hold the
// opaque scene by now (a depth pre-pass) -- for pbr.frag to read through
// post_ssao_texture(). `projection` and `inverse_projection` are the
// camera's (column-major mat4s); `radius` is how far, in meters, nearby
// geometry darkens a point. Leaves an SSAO target bound.
void post_ssao(post_process* post, const f32* projection, const f32* inverse_projection, f32 radius);

// This frame's AO (one texel per pixel), or NULL if post_ssao() hasn't run.
rhi_texture post_ssao_texture(const post_process* post);

// Temporal anti-aliasing: blends the finished scene (rendered with this
// frame's jitter) into the history. `reprojection` (a column-major mat4)
// takes this frame's NDC to last frame's clip space. The first frame, and
// any frame after one without post_taa(), starts the history over. Call
// before post_bloom() / post_meter_exposure(), which then read the result.
// Leaves a history target bound.
void post_taa(post_process* post, const f32* reprojection);

// Blurs the finished scene down and back up the bloom chain, for
// post_end_scene() to mix in. Only worth calling when settings.bloom > 0.
// Leaves a bloom target bound.
void post_bloom(post_process* post);

// Measures the finished scene's brightness and adapts towards it, for
// post_settings.auto_exposure. `delta_time` is the seconds since the last
// frame: < 0 jumps straight to this frame's brightness (the first frame, a
// one-off render that must not depend on frame times), 0 keeps the adapted
// value as it is (a screenshot of what's on screen). Leaves a meter target
// bound.
void post_meter_exposure(post_process* post, f32 delta_time);

// Binds the default framebuffer and resolves the scene target onto it:
// mix(color, bloom, settings.bloom) * exposure -> tone mapping -> gamma 2.2, then
// FXAA if settings.fxaa. Leaves
// the default render state active, so UI drawn afterwards lands on top of
// the tone-mapped image.
void post_end_scene(post_process* post, const post_settings* settings);

// Same, but into `target` (an RGBA8 framebuffer the size of the scene
// target) instead of the screen -- for screenshots. Leaves `target` bound.
void post_end_scene_to(post_process* post, const post_settings* settings, rhi_framebuffer target);

// Safe on a zeroed / failed / already-destroyed post_process.
void post_destroy(post_process* post);
