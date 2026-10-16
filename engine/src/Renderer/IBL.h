#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../World/Environment.h"

#include <cglm/cglm.h>

// Image-based lighting: turns an equirectangular HDR panorama into the things
// a PBR shader needs to light a surface from "everywhere at once", plus a
// skybox to look at it.
//
//   environment  the panorama as a cube map with a mip chain (the sky itself)
//   irradiance   32^2 cube map: diffuse light arriving from each direction
//   prefiltered  128^2 cube map: mip N = the environment blurred by the GGX
//                specular lobe of roughness N / (mip_count - 1)
//   brdf_lut     256^2 RG16F texture: the BRDF half of the split-sum
//                approximation. u = N.V, v = perceptual roughness; R = scale
//                and G = bias applied to F0 (F0 * R + G). It doesn't depend on
//                the panorama, but is baked with the other maps in ibl_create().
//
// Baking happens once, on the GPU, inside ibl_create().

// Texture units the maps live on when bound for drawing. Material.h owns
// 0..4 (MATERIAL_SLOT_COUNT) and Shadow.h's cascades own
// CSM_SLOT_FIRST..CSM_SLOT_FIRST+CSM_MAX_CASCADES-1 (5..8), so IBL starts
// right after those.
#define IBL_SLOT_IRRADIANCE   9
#define IBL_SLOT_PREFILTERED  10
#define IBL_SLOT_ENVIRONMENT  11   // only bound while the skybox is drawn
#define IBL_SLOT_BRDF_LUT     12

typedef struct ibl_environment
{
    rhi_texture environment;
    rhi_texture irradiance;
    rhi_texture prefiltered;
    rhi_texture brdf_lut;          // borrowed from brdf_lut_target; don't destroy it directly
    uint32      environment_mip_count;
    uint32      prefiltered_mip_count;

    // Owns brdf_lut's storage (the RHI only creates 2D textures as framebuffer
    // attachments); ibl_destroy() frees the texture by destroying this.
    rhi_framebuffer brdf_lut_target;

    // Skybox drawing resources, kept for the environment's lifetime.
    rhi_buffer  cube_vertices;
    rhi_buffer  cube_indices;
    rhi_shader  skybox_shader;
} ibl_environment;

// Bakes `hdr_path` (an absolute path to a Radiance .hdr, ideally a 2:1
// equirectangular panorama; see Core/Paths.h project_path()). Needs a live GL
// context, and leaves the viewport / render state as it found them.
//
// Returns false and leaves *ibl zeroed (safe to pass to ibl_destroy and
// ibl_bind) if the file can't be read or a shader fails to build. Call
// ibl_destroy() first if *ibl already holds an environment.
b8   ibl_create(ibl_environment* ibl, const char* hdr_path);

// Safe on a zeroed / failed / already-destroyed environment.
void ibl_destroy(ibl_environment* ibl);

// The environment a scene's HDRI setting (World/Environment.h's `hdri`)
// bakes to, rebaked only when that setting changes -- baking takes a while.
// A zeroed ibl_cache is empty and valid.
typedef struct ibl_cache
{
    ibl_environment maps;                         // all zeroes when nothing is baked, or the bake failed
    char            baked[ENVIRONMENT_HDRI_MAX];  // the setting `maps` was baked from
    b8              valid;                        // false until the first sync, and after ibl_cache_invalidate()
} ibl_cache;

// Bakes `hdri` (a path under the project, "" = none) unless it's what the
// cache already holds. A file that can't be loaded is logged and leaves
// `maps` zeroed: the hemisphere ambient light, not a dead app.
void ibl_cache_sync(ibl_cache* cache, const char* hdri);

// Bakes again on the next sync, even for the same setting -- after switching
// projects, where "hdri/sky.hdr" may name another file.
void ibl_cache_invalidate(ibl_cache* cache);

// The baked maps, or NULL if there are none.
const ibl_environment* ibl_cache_maps(const ibl_cache* cache);

void ibl_cache_destroy(ibl_cache* cache);

// Per-frame setup for the PBR shader (engine/shaders/pbr.frag): binds the
// irradiance, prefiltered and BRDF LUT maps to IBL_SLOT_* and sets its u_use_ibl,
// u_irradiance_map, u_prefiltered_map, u_prefiltered_max_lod and u_brdf_lut uniforms.
// Pass ibl = NULL (or a failed environment) to switch the shader back to its
// built-in hemisphere ambient; the sampler uniforms are still pointed at
// their own units so the program stays valid to draw with.
void ibl_bind(const ibl_environment* ibl, rhi_shader pbr_shader);

// Draws the environment as a background at the far plane. Call before the
// scene geometry (blended materials need the sky already behind them).
// `blur` is 0..1: 0 = sharp panorama, 1 = the blurriest mip. Writes linear
// HDR, so draw it into Renderer/PostProcess.h's scene target. Leaves the
// default render state active afterwards.
void ibl_draw_skybox(const ibl_environment* ibl, const mat4 view, const mat4 projection, f32 blur);
