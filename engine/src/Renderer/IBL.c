#include "IBL.h"

#include "../Core/Image.h"
#include "../Core/Paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Face sizes of the baked maps. The environment is the source for the other
// two, and also what the skybox displays, so it's the largest.
#define IBL_ENVIRONMENT_SIZE  512
#define IBL_IRRADIANCE_SIZE   32
#define IBL_PREFILTER_SIZE    128
#define IBL_PREFILTER_MIPS    5    // 128, 64, 32, 16, 8 -> roughness 0, .25, .5, .75, 1
#define IBL_BRDF_LUT_SIZE     256

#define CUBE_INDEX_COUNT 36
#define QUAD_INDEX_COUNT 6

// Clip-space quad for the BRDF LUT pass (vertex attribute 0 = vec2, matching
// brdf_lut.vert); the shader derives its [0, 1] uv from the position.
static const f32 QUAD_VERTICES[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
     1.0f,  1.0f,
    -1.0f,  1.0f,
};

static const uint32 QUAD_INDICES[QUAD_INDEX_COUNT] = { 0, 1, 2,  2, 3, 0 };

// [-1, 1] cube, positions only (vertex attribute 0, matching ibl_cube.vert
// and skybox.vert). Both bake and skybox draw it with culling off, so winding
// doesn't matter.
static const f32 CUBE_VERTICES[] = {
    -1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,
    -1.0f,  1.0f,  1.0f,
};

static const uint32 CUBE_INDICES[CUBE_INDEX_COUNT] = {
    0, 1, 2,  2, 3, 0,   // -Z
    4, 5, 6,  6, 7, 4,   // +Z
    0, 4, 7,  7, 3, 0,   // -X
    1, 5, 6,  6, 2, 1,   // +X
    0, 1, 5,  5, 4, 0,   // -Y
    3, 2, 6,  6, 7, 3,   // +Y
};

// View matrix for looking out of the origin through one cube map face.
// Targets/ups are the standard set for OpenGL's face order (+X -X +Y -Y +Z -Z),
// chosen so what's rendered into a face is what samplerCube reads back for
// that direction.
static void face_view(uint32 face, mat4 out_view)
{
    static vec3 targets[6] = {
        {  1.0f,  0.0f,  0.0f }, { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    };
    static vec3 ups[6] = {
        { 0.0f, -1.0f,  0.0f }, { 0.0f, -1.0f,  0.0f },
        { 0.0f,  0.0f,  1.0f }, { 0.0f,  0.0f, -1.0f },
        { 0.0f, -1.0f,  0.0f }, { 0.0f, -1.0f,  0.0f },
    };

    vec3 eye = { 0.0f, 0.0f, 0.0f };
    glm_lookat(eye, targets[face], ups[face], out_view);
}

static rhi_shader load_shader(const char* vertex, const char* fragment)
{
    char* vertex_path   = engine_path(vertex);
    char* fragment_path = engine_path(fragment);

    rhi_shader shader = rhi_shader_create_from_files(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!shader)
        LOG_ERROR("IBL: failed to build shader '%s' + '%s'\n", vertex, fragment);

    return shader;
}

static uint32 mip_count_for(uint32 size)
{
    uint32 count = 1;
    for (; size > 1; size >>= 1)
        ++count;
    return count;
}

// Renders the unit cube into all six faces of `target` at `mip` with the
// currently bound shader, which must read `u_view_projection`.
static void bake_faces(const ibl_environment* ibl, rhi_framebuffer framebuffer,
                       rhi_shader shader, rhi_texture target, uint32 mip)
{
    mat4 projection;
    glm_perspective(glm_rad(90.0f), 1.0f, 0.1f, 10.0f, projection);

    for (uint32 face = 0; face < 6; ++face)
    {
        mat4 view, view_projection;
        face_view(face, view);
        glm_mat4_mul(projection, view, view_projection);

        rhi_framebuffer_set_cubemap_target(framebuffer, target, face, mip);
        rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
        rhi_draw_indexed(ibl->cube_vertices, ibl->cube_indices, CUBE_INDEX_COUNT);
    }
}

b8 ibl_create(ibl_environment* ibl, const char* hdr_path)
{
    memset(ibl, 0, sizeof(*ibl));

    image_hdr hdr = image_load_hdr(hdr_path);
    if (!hdr.pixels)
        return false;   // image_load_hdr already logged why

    rhi_texture     equirect = NULL;
    rhi_framebuffer target   = NULL;
    rhi_shader      equirect_shader = NULL, irradiance_shader = NULL, prefilter_shader = NULL, brdf_shader = NULL;

    rhi_vertex_attribute position_attribute = { .location = 0, .component_count = 3, .offset = 0 };
    rhi_vertex_layout    cube_layout = {
        .attributes = &position_attribute,
        .attribute_count = 1,
        .stride = 3 * sizeof(f32)
    };

    ibl->cube_vertices = rhi_vertex_buffer_create(CUBE_VERTICES, sizeof(CUBE_VERTICES), &cube_layout, RHI_USAGE_STATIC);
    ibl->cube_indices  = rhi_index_buffer_create(CUBE_INDICES, sizeof(CUBE_INDICES), RHI_USAGE_STATIC);

    rhi_texture_desc equirect_desc = {
        .width = hdr.width,
        .height = hdr.height,
        .format = RHI_FORMAT_RGBA16F,     // float pixels are converted to half on upload
        .filter = RHI_FILTER_LINEAR,
        .wrap = RHI_WRAP_CLAMP_TO_EDGE,   // repeat would blend the two poles together
        .pixels = hdr.pixels,
        .generate_mipmaps = false         // sampled once per texel, never minified
    };
    equirect = rhi_texture_create(equirect_desc);
    image_hdr_free(&hdr);

    equirect_shader   = load_shader("shaders/ibl_cube.vert", "shaders/equirect_to_cube.frag");
    irradiance_shader = load_shader("shaders/ibl_cube.vert", "shaders/irradiance.frag");
    prefilter_shader  = load_shader("shaders/ibl_cube.vert", "shaders/prefilter.frag");
    brdf_shader       = load_shader("shaders/brdf_lut.vert", "shaders/brdf_lut.frag");
    ibl->skybox_shader = load_shader("shaders/skybox.vert", "shaders/skybox.frag");

    if (!equirect_shader || !irradiance_shader || !prefilter_shader || !brdf_shader || !ibl->skybox_shader)
        goto fail;

    ibl->environment_mip_count = mip_count_for(IBL_ENVIRONMENT_SIZE);
    ibl->prefiltered_mip_count = IBL_PREFILTER_MIPS;

    ibl->environment = rhi_cubemap_create((rhi_cubemap_desc){
        .size = IBL_ENVIRONMENT_SIZE, .format = RHI_FORMAT_RGBA16F, .mip_count = ibl->environment_mip_count });
    ibl->irradiance  = rhi_cubemap_create((rhi_cubemap_desc){
        .size = IBL_IRRADIANCE_SIZE,  .format = RHI_FORMAT_RGBA16F, .mip_count = 1 });
    ibl->prefiltered = rhi_cubemap_create((rhi_cubemap_desc){
        .size = IBL_PREFILTER_SIZE,   .format = RHI_FORMAT_RGBA16F, .mip_count = IBL_PREFILTER_MIPS });

    target = rhi_framebuffer_create_cubemap_target();

    // Every bake pass draws the inside of a cube: no culling, no depth (the
    // target has no depth buffer anyway).
    rhi_render_state bake_state = rhi_render_state_default();
    bake_state.cull_back_faces = false;
    bake_state.depth_write = false;
    rhi_set_render_state(bake_state);

    // 1. Panorama -> environment cube map, then its mip chain (both later
    //    passes read blurred levels of it).
    rhi_shader_bind(equirect_shader);
    rhi_shader_set_int(equirect_shader, "u_equirect", IBL_SLOT_ENVIRONMENT);
    rhi_texture_bind(equirect, IBL_SLOT_ENVIRONMENT);
    bake_faces(ibl, target, equirect_shader, ibl->environment, 0);
    rhi_cubemap_generate_mipmaps(ibl->environment);

    // 2. Environment -> diffuse irradiance.
    rhi_shader_bind(irradiance_shader);
    rhi_shader_set_int(irradiance_shader, "u_environment", IBL_SLOT_ENVIRONMENT);
    rhi_texture_bind(ibl->environment, IBL_SLOT_ENVIRONMENT);
    bake_faces(ibl, target, irradiance_shader, ibl->irradiance, 0);

    // 3. Environment -> GGX-prefiltered specular, one roughness per mip.
    rhi_shader_bind(prefilter_shader);
    rhi_shader_set_int(prefilter_shader, "u_environment", IBL_SLOT_ENVIRONMENT);
    rhi_texture_bind(ibl->environment, IBL_SLOT_ENVIRONMENT);
    for (uint32 mip = 0; mip < IBL_PREFILTER_MIPS; ++mip)
    {
        f32 roughness = (f32)mip / (f32)(IBL_PREFILTER_MIPS - 1);

        rhi_shader_set_float(prefilter_shader, "u_roughness", roughness);
        rhi_shader_set_float(prefilter_shader, "u_target_size", (f32)(IBL_PREFILTER_SIZE >> mip));
        bake_faces(ibl, target, prefilter_shader, ibl->prefiltered, mip);
    }

    // 4. Split-sum BRDF lookup: a fullscreen quad into a 2D RG16F target
    //    (N.V across, roughness up). Kept alive with the environment; it
    //    is what pbr.frag samples instead of an analytic fit.
    rhi_framebuffer_attachment lut_attachment = { .format = RHI_FORMAT_RG16F };
    ibl->brdf_lut_target = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width = IBL_BRDF_LUT_SIZE, .height = IBL_BRDF_LUT_SIZE,
        .color_attachments = &lut_attachment, .color_attachment_count = 1,
        .has_depth_attachment = false });
    ibl->brdf_lut = rhi_framebuffer_get_color_texture(ibl->brdf_lut_target, 0);

    rhi_vertex_attribute quad_attribute = { .location = 0, .component_count = 2, .offset = 0 };
    rhi_vertex_layout    quad_layout = { .attributes = &quad_attribute, .attribute_count = 1, .stride = 2 * sizeof(f32) };
    rhi_buffer quad_vertices = rhi_vertex_buffer_create(QUAD_VERTICES, sizeof(QUAD_VERTICES), &quad_layout, RHI_USAGE_STATIC);
    rhi_buffer quad_indices  = rhi_index_buffer_create(QUAD_INDICES, sizeof(QUAD_INDICES), RHI_USAGE_STATIC);

    rhi_framebuffer_bind(ibl->brdf_lut_target);
    rhi_shader_bind(brdf_shader);
    rhi_draw_indexed(quad_vertices, quad_indices, QUAD_INDEX_COUNT);
    rhi_buffer_destroy(quad_vertices);
    rhi_buffer_destroy(quad_indices);

    rhi_framebuffer_bind_default();
    rhi_set_render_state(rhi_render_state_default());

    rhi_framebuffer_destroy(target);
    rhi_shader_destroy(equirect_shader);
    rhi_shader_destroy(irradiance_shader);
    rhi_shader_destroy(prefilter_shader);
    rhi_shader_destroy(brdf_shader);
    rhi_texture_destroy(equirect);

    LOG_INFO("IBL: baked '%s'\n", hdr_path);
    return true;

fail:
    rhi_framebuffer_destroy(target);   // NULL-safe, like the other destroys
    rhi_shader_destroy(equirect_shader);
    rhi_shader_destroy(irradiance_shader);
    rhi_shader_destroy(prefilter_shader);
    rhi_shader_destroy(brdf_shader);
    rhi_texture_destroy(equirect);
    ibl_destroy(ibl);
    return false;
}

void ibl_destroy(ibl_environment* ibl)
{
    if (!ibl)
        return;

    rhi_texture_destroy(ibl->environment);
    rhi_texture_destroy(ibl->irradiance);
    rhi_texture_destroy(ibl->prefiltered);
    rhi_framebuffer_destroy(ibl->brdf_lut_target);   // also frees brdf_lut
    rhi_shader_destroy(ibl->skybox_shader);
    rhi_buffer_destroy(ibl->cube_vertices);
    rhi_buffer_destroy(ibl->cube_indices);

    memset(ibl, 0, sizeof(*ibl));
}

void ibl_bind(const ibl_environment* ibl, rhi_shader pbr_shader)
{
    // Always point the cube samplers at their own units, even with no
    // environment: a samplerCube and a sampler2D that resolve to the same unit
    // (an unset sampler uniform is 0, where the base color texture lives)
    // make every draw fail with GL_INVALID_OPERATION.
    rhi_shader_set_int(pbr_shader, "u_irradiance_map", IBL_SLOT_IRRADIANCE);
    rhi_shader_set_int(pbr_shader, "u_prefiltered_map", IBL_SLOT_PREFILTERED);
    rhi_shader_set_int(pbr_shader, "u_brdf_lut", IBL_SLOT_BRDF_LUT);

    if (!ibl || !ibl->irradiance || !ibl->prefiltered || !ibl->brdf_lut)
    {
        rhi_shader_set_int(pbr_shader, "u_use_ibl", 0);
        return;
    }

    rhi_texture_bind(ibl->irradiance, IBL_SLOT_IRRADIANCE);
    rhi_texture_bind(ibl->prefiltered, IBL_SLOT_PREFILTERED);
    rhi_texture_bind(ibl->brdf_lut, IBL_SLOT_BRDF_LUT);

    rhi_shader_set_float(pbr_shader, "u_prefiltered_max_lod", (f32)(ibl->prefiltered_mip_count - 1));
    rhi_shader_set_int(pbr_shader, "u_use_ibl", 1);
}

void ibl_draw_skybox(const ibl_environment* ibl, const mat4 view, const mat4 projection, f32 blur)
{
    if (!ibl || !ibl->environment || !ibl->skybox_shader)
        return;

    if (blur < 0.0f) blur = 0.0f;
    if (blur > 1.0f) blur = 1.0f;

    // Behind everything, and it must not write depth: the scene draws after it.
    rhi_render_state state = rhi_render_state_default();
    state.cull_back_faces = false;
    state.depth_write = false;
    rhi_set_render_state(state);

    rhi_shader shader = ibl->skybox_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_mat4(shader, "u_view", (const f32*)view);
    rhi_shader_set_mat4(shader, "u_projection", (const f32*)projection);
    rhi_shader_set_float(shader, "u_lod", blur * (f32)(ibl->environment_mip_count - 1));
    rhi_shader_set_int(shader, "u_environment", IBL_SLOT_ENVIRONMENT);
    rhi_texture_bind(ibl->environment, IBL_SLOT_ENVIRONMENT);

    rhi_draw_indexed(ibl->cube_vertices, ibl->cube_indices, CUBE_INDEX_COUNT);

    rhi_set_render_state(rhi_render_state_default());
}

// --- ibl_cache -----------------------------------------------------------------

void
ibl_cache_sync(ibl_cache* cache, const char* hdri)
{
    if (cache->valid && strcmp(cache->baked, hdri) == 0)
        return;

    ibl_destroy(&cache->maps);
    snprintf(cache->baked, sizeof(cache->baked), "%s", hdri);
    cache->valid = true;

    if (!hdri[0])
        return;   // no HDRI: the hemisphere ambient light

    char* path = project_path(hdri);
    if (!ibl_create(&cache->maps, path))
        LOG_WARN("No environment loaded from '%s' -- using the hemisphere ambient light\n", path);
    free(path);
}

void
ibl_cache_invalidate(ibl_cache* cache)
{
    cache->valid = false;
}

void
ibl_cache_poll_hot_reload(ibl_cache* cache)
{
    // Every file ibl_create() builds a shader from.
    static const char* const files[] = {
        "shaders/ibl_cube.vert", "shaders/equirect_to_cube.frag", "shaders/irradiance.frag", "shaders/prefilter.frag",
        "shaders/brdf_lut.vert", "shaders/brdf_lut.frag",         "shaders/skybox.vert",     "shaders/skybox.frag",
    };

    // A zeroed cache starts watching on its first poll.
    if (cache->shaders.count == 0)
    {
        file_watch_init(&cache->shaders, files, sizeof(files) / sizeof(files[0]));
        return;
    }

    if (file_watch_changed(&cache->shaders))
    {
        LOG_INFO("Shader hot reload: an IBL shader changed, baking the environment again\n");
        ibl_cache_invalidate(cache);
    }
}

const ibl_environment*
ibl_cache_maps(const ibl_cache* cache)
{
    return cache->maps.irradiance ? &cache->maps : NULL;
}

void
ibl_cache_destroy(ibl_cache* cache)
{
    ibl_destroy(&cache->maps);
    cache->valid = false;
}
