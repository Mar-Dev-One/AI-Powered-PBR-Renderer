#include "SceneRenderer.h"

#include "../Core/Paths.h"
#include "Lighting.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PBR_VERTEX_PATH   "shaders/pbr.vert"
#define PBR_FRAGMENT_PATH "shaders/pbr.frag"

scene_render_settings
scene_render_settings_default(void)
{
    return (scene_render_settings){
        .ibl                  = NULL,
        .show_skybox          = true,
        .sky_blur             = 0.0f,
        .ambient_strength     = 0.5f,
        .exposure             = 1.0f,
        .shadows              = true,
        .cascade_split_lambda = 0.5f,
        .debug_show_cascades  = false,
        .clear_color          = { 0.0f, 0.0f, 0.0f }
    };
}

void
scene_render_settings_apply_environment(scene_render_settings* settings, const scene_environment* env,
                                        const ibl_environment* ibl)
{
    settings->ibl              = env->use_ibl && ibl && ibl->irradiance ? ibl : NULL;
    settings->show_skybox      = env->show_skybox;
    settings->sky_blur         = env->sky_blur;
    settings->ambient_strength = env->ambient_strength;
    settings->exposure         = env->exposure;
}

// The constants pbr.vert/pbr.frag size their arrays with, generated from
// the C definitions they have to agree with -- see the #error guards at the
// top of both shaders.
static void
scene_renderer_shader_defines(char* out, size_t size)
{
    snprintf(out, size,
             "#define CSM_MAX_CASCADES %d\n"
             "#define MAX_LIGHTS %d\n"
             "#define LIGHT_DIRECTIONAL %d\n"
             "#define LIGHT_POINT %d\n"
             "#define LIGHT_SPOT %d\n",
             CSM_MAX_CASCADES, LIGHTING_MAX_LIGHTS,
             (int)LIGHT_DIRECTIONAL, (int)LIGHT_POINT, (int)LIGHT_SPOT);
}

// A shader file's (under the engine root) modification time and size, or
// zeros if it can't be stat()ed.
static file_stamp
shader_stamp(const char* relative_path)
{
    char*       path  = engine_path(relative_path);
    struct stat info;
    file_stamp  stamp = { 0, 0 };

    if (stat(path, &info) == 0)
    {
        stamp.mtime = (int64)info.st_mtime;
        stamp.size  = (int64)info.st_size;
    }

    free(path);
    return stamp;
}

static b8
stamp_equal(file_stamp a, file_stamp b)
{
    return a.mtime == b.mtime && a.size == b.size;
}

static rhi_shader
build_pbr_shader(void)
{
    char defines[256];
    scene_renderer_shader_defines(defines, sizeof(defines));

    char* vertex_path   = engine_path(PBR_VERTEX_PATH);
    char* fragment_path = engine_path(PBR_FRAGMENT_PATH);

    rhi_shader shader = rhi_shader_create_from_files_with_defines(vertex_path, fragment_path, defines);

    free(vertex_path);
    free(fragment_path);

    return shader;
}

b8
scene_renderer_create(scene_renderer* r, uint16 width, uint16 height,
                      uint32 cascade_count, uint32 shadow_resolution)
{
    memset(r, 0, sizeof(*r));

    r->vertex_stamp   = shader_stamp(PBR_VERTEX_PATH);
    r->fragment_stamp = shader_stamp(PBR_FRAGMENT_PATH);

    r->pbr_shader = build_pbr_shader();
    if (!r->pbr_shader)
    {
        LOG_ERROR("scene_renderer_create: failed to build the PBR shader\n");
        return false;
    }

    if (!csm_create(&r->csm, cascade_count, shadow_resolution))
        return false;

    if (!post_create(&r->post, width, height))
        return false;

    return true;
}

void
scene_renderer_resize(scene_renderer* r, uint16 width, uint16 height)
{
    post_resize(&r->post, width, height);
}

// World-space direction of the first LIGHT_DIRECTIONAL in world order --
// the light lighting_bind() marks as shadowed. False if there is none.
static b8
find_shadow_light(const world* w, vec3 out_direction)
{
    light  lights[LIGHTING_MAX_LIGHTS];
    uint32 count = world_gather_lights(w, lights, NULL, LIGHTING_MAX_LIGHTS);

    for (uint32 i = 0; i < count; ++i)
    {
        if (lights[i].type == LIGHT_DIRECTIONAL)
        {
            glm_vec3_copy(lights[i].direction, out_direction);
            return true;
        }
    }

    return false;
}

rhi_shader
scene_renderer_begin(scene_renderer* r, const world* w, const camera* cam,
                     const scene_render_settings* settings)
{
    // --- Shadow pass ---------------------------------------------------------
    // Before the HDR target is bound: csm_render() binds its own
    // framebuffers and leaves the default one bound when it's done.
    vec3 shadow_dir;
    b8   shadowed = settings->shadows && find_shadow_light(w, shadow_dir);

    if (shadowed)
    {
        csm_update(&r->csm, cam, shadow_dir, settings->cascade_split_lambda);
        csm_render(&r->csm, w);
    }

    // --- Scene target + background ----------------------------------------------
    post_begin_scene(&r->post, settings->clear_color[0], settings->clear_color[1], settings->clear_color[2]);

    mat4 view, projection, view_projection;
    camera_get_view(cam, view);
    camera_get_projection(cam, projection);
    glm_mat4_mul(projection, view, view_projection);

    if (settings->ibl && settings->show_skybox)
        ibl_draw_skybox(settings->ibl, view, projection, settings->sky_blur);

    // --- PBR shader -------------------------------------------------------------
    rhi_shader shader = r->pbr_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
    rhi_shader_set_mat4(shader, "u_view", (const f32*)view);   // pbr.vert: v_view_depth, for cascade selection
    rhi_shader_set_vec3(shader, "u_view_pos", cam->position[0], cam->position[1], cam->position[2]);
    rhi_shader_set_float(shader, "u_ambient_strength", settings->ambient_strength);
    rhi_shader_set_int(shader, "u_debug_show_cascades", settings->debug_show_cascades);

    lighting_bind(shader, w);
    ibl_bind(settings->ibl, shader);

    // Always bound, even unshadowed: it points u_shadow_maps[] at their own
    // units. Left at the default unit 0, those sampler2DShadow uniforms would
    // share a unit with the material's sampler2D base color, which GL
    // rejects at draw time. u_cascade_count = 0 then turns shadowing off.
    csm_bind(&r->csm, shader);
    if (!shadowed)
        rhi_shader_set_int(shader, "u_cascade_count", 0);

    return shader;
}

void
scene_renderer_end(scene_renderer* r, const scene_render_settings* settings)
{
    post_end_scene(&r->post, settings->exposure);
}

void
scene_renderer_draw_world(const world* w, rhi_shader shader)
{
    const uint32          mask      = COMPONENT_BIT(COMPONENT_MESH_RENDERER);
    const model_draw_pass passes[2] = { MODEL_PASS_OPAQUE, MODEL_PASS_BLEND };

    for (int p = 0; p < 2; ++p)
    {
        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        {
            const c_mesh_renderer* mr = world_mesh_renderer(w, e);
            const model*           m  = assets_model(w->assets, mr->model);
            if (!m || !mr->visible)
                continue;

            model_draw_ex(m, shader, world_matrix(w, e), &mr->material, passes[p]);
        }
    }
}

void
scene_renderer_draw(scene_renderer* r, const world* w, const camera* cam,
                    const scene_render_settings* settings)
{
    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    scene_renderer_end(r, settings);
}

void
scene_renderer_draw_to(scene_renderer* r, const world* w, const camera* cam,
                       const scene_render_settings* settings, rhi_framebuffer target)
{
    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    post_end_scene_to(&r->post, settings->exposure, target);
    rhi_framebuffer_bind_default();
}

uint8*
scene_renderer_capture(scene_renderer* r, const world* w, const camera* cam,
                       const scene_render_settings* settings, uint16* out_width, uint16* out_height)
{
    uint16 width  = r->post.width;
    uint16 height = r->post.height;
    if (!width || !height)
        return NULL;

    // Screenshots are rare: a target per capture keeps nothing resident.
    rhi_framebuffer_attachment color  = { .format = RHI_FORMAT_RGBA8 };
    rhi_framebuffer            target = rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
    });

    rhi_shader shader = scene_renderer_begin(r, w, cam, settings);
    scene_renderer_draw_world(w, shader);
    post_end_scene_to(&r->post, settings->exposure, target);   // leaves `target` bound, to read

    uint8* pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        FATAL("scene_renderer_capture: out of memory");
    rhi_read_pixels(0, 0, width, height, pixels);

    rhi_framebuffer_bind_default();
    rhi_framebuffer_destroy(target);

    *out_width  = width;
    *out_height = height;
    return pixels;
}

b8
scene_renderer_reload_shaders(scene_renderer* r)
{
    rhi_shader shader = build_pbr_shader();
    if (!shader)
    {
        LOG_ERROR("Shader hot reload: pbr.vert/pbr.frag failed to build, keeping the previous version\n");
        return false;
    }

    rhi_shader_destroy(r->pbr_shader);
    r->pbr_shader = shader;

    LOG_INFO("Shader hot reload: rebuilt pbr.vert/pbr.frag\n");
    return true;
}

void
scene_renderer_poll_hot_reload(scene_renderer* r)
{
    file_stamp vertex_stamp   = shader_stamp(PBR_VERTEX_PATH);
    file_stamp fragment_stamp = shader_stamp(PBR_FRAGMENT_PATH);

    if (stamp_equal(vertex_stamp, r->vertex_stamp) && stamp_equal(fragment_stamp, r->fragment_stamp))
        return;

    // Recorded before the rebuild, so a broken edit is reported once rather
    // than every frame until the file changes again.
    r->vertex_stamp   = vertex_stamp;
    r->fragment_stamp = fragment_stamp;

    scene_renderer_reload_shaders(r);
}

void
scene_renderer_destroy(scene_renderer* r)
{
    post_destroy(&r->post);
    csm_destroy(&r->csm);

    if (r->pbr_shader)
        rhi_shader_destroy(r->pbr_shader);

    memset(r, 0, sizeof(*r));
}
