#include "SceneRenderer.h"

#include "../Core/Paths.h"
#include "FrameData.h"
#include "Lighting.h"

#include <stdio.h>
#include <string.h>

#define PBR_VERTEX_PATH   "shaders/pbr.vert"
#define PBR_FRAGMENT_PATH "shaders/pbr.frag"
#define PREPASS_FRAGMENT_PATH "shaders/depth_prepass.frag"

scene_render_settings
scene_render_settings_default(void)
{
    return (scene_render_settings){
        .ibl                  = NULL,
        .show_skybox          = true,
        .sky_blur             = 0.0f,
        .ambient_strength     = 0.5f,
        .exposure             = 1.0f,
        .auto_exposure        = false,
        .delta_time           = -1.0f,
        .bloom                = 0.04f,
        .antialiasing         = SCENE_AA_FXAA,
        .tonemap              = SCENE_TONEMAP_ACES,
        .ssao                 = true,
        .ssao_radius          = 0.5f,
        .shadows              = true,
        .cascade_split_lambda = 0.5f,
        .debug_show_cascades  = false,
        .debug_view           = SCENE_VIEW_LIT,
        .clear_color          = { 0.0f, 0.0f, 0.0f }
    };
}

static const char* const DEBUG_VIEW_NAMES[SCENE_VIEW_COUNT] = {
    "lit", "albedo", "normals", "roughness", "metallic", "ao", "shadow", "lighting", "wireframe", "lights"
};

const char*
scene_debug_view_name(scene_debug_view view)
{
    return (uint32)view < SCENE_VIEW_COUNT ? DEBUG_VIEW_NAMES[view] : NULL;
}

b8
scene_debug_view_from_name(const char* name, scene_debug_view* out_view)
{
    for (uint32 i = 0; name && i < SCENE_VIEW_COUNT; ++i)
    {
        if (strcmp(name, DEBUG_VIEW_NAMES[i]) == 0)
        {
            *out_view = (scene_debug_view)i;
            return true;
        }
    }
    return false;
}

static const char* const AA_NAMES[SCENE_AA_COUNT] = { "off", "fxaa", "taa" };

const char*
scene_antialiasing_name(scene_antialiasing aa)
{
    return (uint32)aa < SCENE_AA_COUNT ? AA_NAMES[aa] : NULL;
}

b8
scene_antialiasing_from_name(const char* name, scene_antialiasing* out_aa)
{
    for (uint32 i = 0; name && i < SCENE_AA_COUNT; ++i)
    {
        if (strcmp(name, AA_NAMES[i]) == 0)
        {
            *out_aa = (scene_antialiasing)i;
            return true;
        }
    }
    return false;
}

// TAA's jitter: the Halton (2, 3) sequence's first 8 points, centred on
// the pixel -- evenly spread over its area, never repeating a position
// within the cycle.
#define TAA_JITTER_COUNT 8

static f32
halton(uint32 index, uint32 base)
{
    f32 result = 0.0f, fraction = 1.0f;
    for (uint32 i = index; i > 0; i /= base)
    {
        fraction /= (f32)base;
        result   += fraction * (f32)(i % base);
    }
    return result;
}

// The views whose pixels are data, not light: shown without exposure, tone
// mapping or skybox (see scene_debug_view).
static b8
debug_view_is_data(scene_debug_view view)
{
    return view != SCENE_VIEW_LIT && view != SCENE_VIEW_LIGHTING;
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
    settings->auto_exposure    = env->auto_exposure;
    settings->bloom            = env->bloom;
    settings->tonemap          = env->tonemap;
    settings->ssao             = env->ssao;
    settings->ssao_radius      = env->ssao_radius;
}

// The constants pbr.vert/pbr.frag size their arrays with, generated from
// the C definitions they have to agree with -- see the #error guards at the
// top of both shaders.
static void
scene_renderer_shader_defines(char* out, size_t size)
{
    snprintf(out, size,
             "#define CSM_MAX_CASCADES %d\n"
             "#define CLUSTER_X %d\n"
             "#define CLUSTER_Y %d\n"
             "#define CLUSTER_Z %d\n"
             "#define LIGHT_DIRECTIONAL %d\n"
             "#define LIGHT_POINT %d\n"
             "#define LIGHT_SPOT %d\n"
             "#define DEBUG_VIEW_LIT %d\n"
             "#define DEBUG_VIEW_ALBEDO %d\n"
             "#define DEBUG_VIEW_NORMALS %d\n"
             "#define DEBUG_VIEW_ROUGHNESS %d\n"
             "#define DEBUG_VIEW_METALLIC %d\n"
             "#define DEBUG_VIEW_AO %d\n"
             "#define DEBUG_VIEW_SHADOW %d\n"
             "#define DEBUG_VIEW_LIGHTING %d\n"
             "#define DEBUG_VIEW_WIREFRAME %d\n"
             "#define DEBUG_VIEW_LIGHTS %d\n"
             "#define SPOT_SHADOW_MAX %d\n"
             "#define POINT_SHADOW_MAX %d\n"
             "#define PROBE_MAX %d\n"
             "#define PROBE_MIPS %d\n",
             CSM_MAX_CASCADES, CLUSTER_X, CLUSTER_Y, CLUSTER_Z,
             (int)LIGHT_DIRECTIONAL, (int)LIGHT_POINT, (int)LIGHT_SPOT,
             (int)SCENE_VIEW_LIT, (int)SCENE_VIEW_ALBEDO, (int)SCENE_VIEW_NORMALS, (int)SCENE_VIEW_ROUGHNESS,
             (int)SCENE_VIEW_METALLIC, (int)SCENE_VIEW_AO, (int)SCENE_VIEW_SHADOW, (int)SCENE_VIEW_LIGHTING,
             (int)SCENE_VIEW_WIREFRAME, (int)SCENE_VIEW_LIGHTS, SPOT_SHADOW_MAX, POINT_SHADOW_MAX, PROBE_MAX,
             PROBE_MIPS);
}

// pbr.vert with `fragment` (pbr.frag, or the depth pre-pass's).
static rhi_shader
build_pbr_shader(const char* fragment)
{
    char defines[1024];
    scene_renderer_shader_defines(defines, sizeof(defines));

    char* vertex_path   = engine_path(PBR_VERTEX_PATH);
    char* fragment_path = engine_path(fragment);

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

    static const char* const pbr_files[]    = { PBR_VERTEX_PATH, PBR_FRAGMENT_PATH };
    static const char* const prepass_files[] = { PBR_VERTEX_PATH, PREPASS_FRAGMENT_PATH };
    static const char* const ssao_files[]   = { POST_VERTEX_PATH, POST_SSAO_PATH, POST_SSAO_BLUR_PATH };
    static const char* const post_files[]   = { POST_VERTEX_PATH, POST_FRAGMENT_PATH };
    static const char* const bloom_files[]  = { POST_VERTEX_PATH, POST_BLOOM_DOWN_PATH, POST_BLOOM_UP_PATH };
    static const char* const fxaa_files[]   = { POST_VERTEX_PATH, POST_FXAA_PATH };
    static const char* const taa_files[]    = { POST_VERTEX_PATH, POST_TAA_PATH };
    static const char* const exposure_files[] = { POST_VERTEX_PATH, POST_LUMINANCE_PATH, POST_REDUCE_PATH,
                                                  POST_ADAPT_PATH };
    static const char* const shadow_files[] = { CSM_VERTEX_PATH, CSM_FRAGMENT_PATH };
    file_watch_init(&r->pbr_files, pbr_files, 2);
    file_watch_init(&r->prepass_files, prepass_files, 2);
    file_watch_init(&r->ssao_files, ssao_files, 3);
    file_watch_init(&r->post_files, post_files, 2);
    file_watch_init(&r->bloom_files, bloom_files, 3);
    file_watch_init(&r->fxaa_files, fxaa_files, 2);
    file_watch_init(&r->taa_files, taa_files, 2);
    file_watch_init(&r->exposure_files, exposure_files, 4);
    file_watch_init(&r->shadow_files, shadow_files, 2);

    r->pbr_shader     = build_pbr_shader(PBR_FRAGMENT_PATH);
    r->prepass_shader = build_pbr_shader(PREPASS_FRAGMENT_PATH);
    if (!r->pbr_shader || !r->prepass_shader)
    {
        LOG_ERROR("scene_renderer_create: failed to build the PBR or depth pre-pass shader\n");
        return false;
    }

    if (!csm_create(&r->csm, cascade_count, shadow_resolution))
        return false;

    // Each spot light's tile gets half the cascades' resolution: a spot's
    // cone is usually far smaller than a cascade.
    if (!spot_shadows_create(&r->spot, shadow_resolution / 2))
        return false;

    // A quarter of the cascades' resolution per cube face: six of them per
    // light, each covering only a 90-degree view.
    if (!point_shadows_create(&r->point, shadow_resolution / 4))
        return false;

    if (!post_create(&r->post, width, height))
        return false;

    r->shadow_buffer = rhi_uniform_buffer_create(sizeof(gpu_shadow_block));
    if (!light_clusters_create(&r->clusters))
        return false;
    if (!reflection_probes_create(&r->probes))
        return false;

    // Without timings the frame still renders: only the numbers are missing.
    if (!gpu_profiler_create(&r->profiler))
    {
        LOG_WARN("scene_renderer_create: no GPU timestamp queries, so no GPU timings\n");
        gpu_profiler_destroy(&r->profiler);
    }

    return true;
}

void
scene_renderer_resize(scene_renderer* r, uint16 width, uint16 height)
{
    post_resize(&r->post, width, height);
}

// Every visible MeshRenderer's submeshes of the given passes, in order:
// all of the first pass, then all of the next, so one entity's
// transparency never gets drawn before another entity's opaque surfaces.
static void
draw_entities(const world* w, rhi_shader shader, const model_draw_pass* passes, int pass_count)
{
    const uint32 mask = COMPONENT_BIT(COMPONENT_MESH_RENDERER);

    for (int p = 0; p < pass_count; ++p)
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

// World-space direction of the cascades' light (Renderer/Lighting.h: the
// first directional light with cast_shadows). False if there is none.
static b8
find_shadow_light(const light_clusters* lc, vec3 out_direction)
{
    if (lc->shadow_light_index < 0)
        return false;
    glm_vec3_copy(lc->lights[lc->shadow_light_index].direction, out_direction);
    return true;
}

// Binds the PBR shader with everything a lit draw needs: the camera
// (`view_projection`, `eye`; `depth_view` is the main camera's view, which
// picks the shadow cascade), the IBL, AO (`ao`, or NULL), the clustered
// lights as last built, every shadow, and the reflection probes when
// `probes`. Returns the shader.
static rhi_shader
bind_lit_shader(scene_renderer* r, const scene_render_settings* settings, const mat4 depth_view,
                const mat4 view_projection, const vec3 eye, scene_debug_view debug_view, rhi_texture ao, b8 probes)
{
    rhi_shader shader = r->pbr_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_mat4(shader, "u_view_projection", (const f32*)view_projection);
    rhi_shader_set_mat4(shader, "u_view", (const f32*)depth_view);   // pbr.vert: v_view_depth, for cascade selection
    rhi_shader_set_vec3(shader, "u_view_pos", eye[0], eye[1], eye[2]);
    rhi_shader_set_float(shader, "u_ambient_strength", settings->ambient_strength);
    rhi_shader_set_int(shader, "u_debug_show_cascades", settings->debug_show_cascades);
    rhi_shader_set_int(shader, "u_debug_view", (int)debug_view);

    ibl_bind(settings->ibl, shader);

    // Like the shadow maps below, always pointed at its own unit, so it
    // never shares one with a sampler of another type.
    rhi_shader_set_int(shader, "u_ssao", POST_SSAO_SLOT);
    rhi_shader_set_int(shader, "u_use_ssao", ao != NULL);
    if (ao)
        rhi_texture_bind(ao, POST_SSAO_SLOT);

    // The lights, by cluster.
    light_clusters_bind(&r->clusters, shader);

    // Always bound, even unshadowed: they point the shadow samplers at their
    // own units. Left at the default unit 0, those sampler2DShadow uniforms
    // would share a unit with the material's sampler2D base color, which GL
    // rejects at draw time. A cascade count of 0 then turns shadowing off.
    gpu_shadow_block shadows;
    memset(&shadows, 0, sizeof(shadows));
    csm_bind(&r->csm, shader, &shadows);
    if (!r->shadowed)
        shadows.cascade_count = 0;
    spot_shadows_bind(&r->spot, shader, &shadows);
    point_shadows_bind(&r->point, shader, &shadows);
    rhi_buffer_update(r->shadow_buffer, &shadows, sizeof(shadows), 0);
    rhi_uniform_buffer_bind(r->shadow_buffer, FRAME_SHADOW_BINDING);

    reflection_probes_bind(&r->probes, shader, probes && r->probes.baked_signature != 0);
    return shader;
}

// How many times the probes are captured: the first sees shiny surfaces
// reflecting the environment (a chrome ball in the room shows the sky);
// each later capture sees them reflecting the previous one -- the room.
#define PROBE_BOUNCES 2

// Captures every reflection probe: the lit scene in its six directions
// (lights evaluated everywhere -- the clusters are laid out for the main
// camera's screen), each prefiltered into its layer of the array, then
// again with the probes on (PROBE_BOUNCES). `main_view` picks the shadow
// cascades, which are fit to the main camera.
static void
bake_probes(scene_renderer* r, const world* w, const scene_render_settings* settings, const mat4 main_view,
            const mat4 main_projection, f32 near, f32 far)
{
    reflection_probes* rp = &r->probes;
    light_clusters_build(&r->clusters, main_view, main_projection, near, far, PROBE_SIZE, PROBE_SIZE, true);

    mat4 projection;
    glm_perspective(glm_rad(90.0f), 1.0f, 0.05f, 500.0f, projection);
    const model_draw_pass passes[2] = { MODEL_PASS_OPAQUE, MODEL_PASS_BLEND };

    // Nothing to reflect yet on the first bounce: bind_lit_shader() leaves
    // the probes off while baked_signature is 0.
    uint64 signature     = rp->baked_signature;
    rp->baked_signature  = 0;

    for (uint32 bounce = 0; bounce < PROBE_BOUNCES; ++bounce)
    for (uint32 p = 0; p < rp->count; ++p)
    {
        for (uint32 face = 0; face < 6; ++face)
        {
            mat4 rotation, translation, view, view_projection;
            ibl_face_view(face, rotation);
            glm_translate_make(translation, (vec3){ -rp->position[p][0], -rp->position[p][1], -rp->position[p][2] });
            glm_mat4_mul(rotation, translation, view);
            glm_mat4_mul(projection, view, view_projection);

            rhi_framebuffer_bind(rp->capture);
            rhi_clear(settings->clear_color[0], settings->clear_color[1], settings->clear_color[2], 1.0f);
            if (settings->ibl && settings->show_skybox)
                ibl_draw_skybox(settings->ibl, view, projection, settings->sky_blur);

            rhi_shader shader = bind_lit_shader(r, settings, main_view, view_projection, rp->position[p],
                                                SCENE_VIEW_LIT, NULL, bounce > 0);
            draw_entities(w, shader, passes, 2);

            rhi_texture_copy_to_cubemap_face(rhi_framebuffer_get_color_texture(rp->capture, 0), rp->capture_cube, face);
        }

        rhi_cubemap_generate_mipmaps(rp->capture_cube);
        ibl_prefilter_into_array(rp->capture_cube, rp->maps, p, PROBE_MIPS, PROBE_SIZE);

        // After the first bounce every layer holds a capture to reflect.
        if (p + 1 == rp->count)
            rp->baked_signature = 1;
    }

    rp->baked_signature = signature;
    rhi_framebuffer_bind_default();
    LOG_INFO("Reflection probes: baked %u\n", rp->count);
}

// What the probes' captures depend on besides the probes themselves: the
// environment that lights and backs the scene.
static uint64
environment_key(const scene_render_settings* settings)
{
    uint64 key = (uint64)(uintptr_t)settings->ibl;
    if (settings->ibl)
        key = key * 31u + (uint64)(uintptr_t)settings->ibl->irradiance;
    key = key * 31u + (uint64)settings->show_skybox;
    uint32 bits;
    memcpy(&bits, &settings->ambient_strength, sizeof(bits));
    return key * 31u + bits;
}

void
scene_renderer_bake_probes(scene_renderer* r)
{
    r->probes.force_bake = true;
}

rhi_shader
scene_renderer_begin(scene_renderer* r, const world* w, const camera* cam,
                     const scene_render_settings* settings)
{
    // --- Shadow pass ---------------------------------------------------------
    // Before the HDR target is bound: csm_render() binds its own
    // framebuffers and leaves the default one bound when it's done.
    gpu_profiler_begin_frame(&r->profiler);

    // The frame's lights, once: the shadows and the clusters both use them,
    // and their order is what the shadows' light indices refer to.
    light_clusters_gather(&r->clusters, w);

    vec3 shadow_dir;
    b8   shadowed = settings->shadows && find_shadow_light(&r->clusters, shadow_dir);
    r->shadowed   = shadowed;

    if (shadowed)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Shadows");
        csm_update(&r->csm, cam, shadow_dir, settings->cascade_split_lambda);
        csm_render(&r->csm, w);
        gpu_profiler_end(&r->profiler, scope);
    }

    // Spot and point lights, each into its tiles of an atlas.
    r->spot.count  = 0;
    r->point.count = 0;
    if (settings->shadows)
    {
        const light* lights    = r->clusters.lights;
        const vec3*  positions = (const vec3*)r->clusters.positions;
        uint32       count     = r->clusters.count;
        spot_shadows_update(&r->spot, lights, positions, count);
        if (r->spot.count)
        {
            uint32 scope = gpu_profiler_begin(&r->profiler, "Spot shadows");
            spot_shadows_render(&r->spot, w, r->csm.depth_shader);
            gpu_profiler_end(&r->profiler, scope);
        }

        point_shadows_update(&r->point, lights, positions, count);
        if (r->point.count)
        {
            uint32 scope = gpu_profiler_begin(&r->profiler, "Point shadows");
            point_shadows_render(&r->point, w, r->csm.depth_shader);
            gpu_profiler_end(&r->profiler, scope);
        }
    }

    mat4 view, projection, view_projection;
    camera_get_view(cam, view);
    camera_get_projection(cam, projection);
    glm_mat4_mul(projection, view, view_projection);

    // --- Reflection probes: baked again when they or the environment changed -----------
    uint64 probe_signature = reflection_probes_gather(&r->probes, w, environment_key(settings));
    if (probe_signature != r->probes.baked_signature || r->probes.force_bake)
    {
        if (r->probes.count)
        {
            uint32 scope = gpu_profiler_begin(&r->profiler, "Probe bake");
            bake_probes(r, w, settings, view, projection, cam->near_plane, cam->far_plane);
            gpu_profiler_end(&r->profiler, scope);
        }
        r->probes.baked_signature = probe_signature;
        r->probes.force_bake      = false;
    }

    // --- Scene target + background ----------------------------------------------
    post_begin_scene(&r->post, settings->clear_color[0], settings->clear_color[1], settings->clear_color[2]);

    // --- Lights by cluster ---------------------------------------------------------------
    // With the un-jittered camera (TAA's shift is a fraction of a pixel; the
    // clusters allow for it).
    light_clusters_build(&r->clusters, view, projection, cam->near_plane, cam->far_plane, r->post.width,
                         r->post.height, settings->debug_unclustered);

    // --- TAA: the reprojection, and this frame's sub-pixel shift ---------------------
    // The reprojection uses the un-jittered matrices: where the surface
    // under a pixel sat on screen last frame, as the camera moved.
    r->taa_active = settings->antialiasing == SCENE_AA_TAA && !debug_view_is_data(settings->debug_view);
    if (r->taa_active)
    {
        mat4 inverse_view_projection;
        glm_mat4_inv(view_projection, inverse_view_projection);
        glm_mat4_mul(r->has_previous ? r->previous_view_projection : view_projection, inverse_view_projection,
                     r->taa_reprojection);

        // Shift the whole image by a fraction of a pixel: offsetting the
        // projection's third column moves clip x/y by a constant in NDC.
        uint32 step = r->taa_frame++ % TAA_JITTER_COUNT;
        f32    jx   = (halton(step + 1, 2) - 0.5f) * 2.0f / (f32)r->post.width;
        f32    jy   = (halton(step + 1, 3) - 0.5f) * 2.0f / (f32)r->post.height;
        glm_mat4_copy(view_projection, r->previous_view_projection);
        r->has_previous = true;

        projection[2][0] -= jx;
        projection[2][1] -= jy;
        glm_mat4_mul(projection, view, view_projection);
    }
    else
    {
        glm_mat4_copy(view_projection, r->previous_view_projection);
        r->has_previous = true;
    }

    // --- Depth pre-pass + ambient occlusion --------------------------------------
    // Not in the wireframe view: the pre-pass's solid depth would hide the
    // edges behind the front faces.
    b8 ssao = settings->ssao && settings->debug_view != SCENE_VIEW_WIREFRAME;
    if (ssao)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Depth");
        rhi_set_color_write(false);
        rhi_shader_bind(r->prepass_shader);
        rhi_shader_set_mat4(r->prepass_shader, "u_view_projection", (const f32*)view_projection);
        rhi_shader_set_mat4(r->prepass_shader, "u_view", (const f32*)view);
        const model_draw_pass opaque = MODEL_PASS_OPAQUE;
        draw_entities(w, r->prepass_shader, &opaque, 1);
        rhi_set_color_write(true);
        gpu_profiler_end(&r->profiler, scope);

        mat4 inverse_projection;
        glm_mat4_inv(projection, inverse_projection);
        scope = gpu_profiler_begin(&r->profiler, "SSAO");
        post_ssao(&r->post, (const f32*)projection, (const f32*)inverse_projection, settings->ssao_radius);
        gpu_profiler_end(&r->profiler, scope);

        post_resume_scene(&r->post);
    }

    if (settings->ibl && settings->show_skybox && !debug_view_is_data(settings->debug_view))
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Sky");
        ibl_draw_skybox(settings->ibl, view, projection, settings->sky_blur);
        gpu_profiler_end(&r->profiler, scope);
    }

    // Until finish_frame(): the world's geometry only, not the passes around it.
    if (settings->debug_view == SCENE_VIEW_WIREFRAME)
        rhi_set_wireframe(true);

    // --- PBR shader -------------------------------------------------------------
    rhi_texture ao     = ssao ? post_ssao_texture(&r->post) : NULL;
    rhi_shader  shader = bind_lit_shader(r, settings, view, view_projection, cam->position, settings->debug_view,
                                         ao, true);

    r->scene_scope = gpu_profiler_begin(&r->profiler, "Scene");
    return shader;
}

// Resolves the HDR target into `target` (NULL = the screen), the way
// settings->debug_view wants it.
static void
finish_frame(scene_renderer* r, const scene_render_settings* settings, rhi_framebuffer target)
{
    gpu_profiler_end(&r->profiler, r->scene_scope);
    rhi_set_wireframe(false);

    if (r->taa_active)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "TAA");
        post_taa(&r->post, (const f32*)r->taa_reprojection);
        gpu_profiler_end(&r->profiler, scope);
    }

    post_settings post = {
        .exposure = settings->exposure,
        .raw      = debug_view_is_data(settings->debug_view),
        .bloom    = settings->bloom,
        .fxaa     = settings->antialiasing == SCENE_AA_FXAA,
        .tonemap  = (int)settings->tonemap,
        .auto_exposure = settings->auto_exposure && !debug_view_is_data(settings->debug_view),
    };

    if (post.auto_exposure)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Exposure");
        post_meter_exposure(&r->post, settings->delta_time);
        gpu_profiler_end(&r->profiler, scope);
    }

    if (post.bloom > 0.0f && !post.raw)
    {
        uint32 scope = gpu_profiler_begin(&r->profiler, "Bloom");
        post_bloom(&r->post);
        gpu_profiler_end(&r->profiler, scope);
    }

    uint32 scope = gpu_profiler_begin(&r->profiler, "Post");
    post_end_scene_to(&r->post, &post, target);
    gpu_profiler_end(&r->profiler, scope);
}

void
scene_renderer_end(scene_renderer* r, const scene_render_settings* settings)
{
    finish_frame(r, settings, NULL);
}

void
scene_renderer_draw_world(const world* w, rhi_shader shader)
{
    const model_draw_pass passes[2] = { MODEL_PASS_OPAQUE, MODEL_PASS_BLEND };
    draw_entities(w, shader, passes, 2);
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
    finish_frame(r, settings, target);
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
    finish_frame(r, settings, target);   // leaves `target` bound, to read

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

static b8
reload_bloom(scene_renderer* r)
{
    b8 down = shader_rebuild(&r->post.bloom_down_shader, POST_VERTEX_PATH, POST_BLOOM_DOWN_PATH, NULL);
    b8 up   = shader_rebuild(&r->post.bloom_up_shader, POST_VERTEX_PATH, POST_BLOOM_UP_PATH, NULL);
    return down && up;
}

static b8
reload_exposure(scene_renderer* r)
{
    b8 luminance = shader_rebuild(&r->post.luminance_shader, POST_VERTEX_PATH, POST_LUMINANCE_PATH, NULL);
    b8 reduce    = shader_rebuild(&r->post.reduce_shader, POST_VERTEX_PATH, POST_REDUCE_PATH, NULL);
    b8 adapt     = shader_rebuild(&r->post.adapt_shader, POST_VERTEX_PATH, POST_ADAPT_PATH, NULL);
    return luminance && reduce && adapt;
}

static b8
reload_pbr(scene_renderer* r)
{
    char defines[1024];
    scene_renderer_shader_defines(defines, sizeof(defines));
    return shader_rebuild(&r->pbr_shader, PBR_VERTEX_PATH, PBR_FRAGMENT_PATH, defines);
}

static b8
reload_prepass(scene_renderer* r)
{
    char defines[1024];
    scene_renderer_shader_defines(defines, sizeof(defines));
    return shader_rebuild(&r->prepass_shader, PBR_VERTEX_PATH, PREPASS_FRAGMENT_PATH, defines);
}

static b8
reload_ssao(scene_renderer* r)
{
    b8 ssao = shader_rebuild(&r->post.ssao_shader, POST_VERTEX_PATH, POST_SSAO_PATH, NULL);
    b8 blur = shader_rebuild(&r->post.ssao_blur_shader, POST_VERTEX_PATH, POST_SSAO_BLUR_PATH, NULL);
    return ssao && blur;
}

// Every shader keeps its uniforms' values per program, and each pass sets
// all of its own every time it runs, so a swapped-in program needs nothing
// carried over.
b8
scene_renderer_reload_shaders(scene_renderer* r)
{
    b8 pbr    = reload_pbr(r);
    b8 post   = shader_rebuild(&r->post.tonemap_shader, POST_VERTEX_PATH, POST_FRAGMENT_PATH, NULL);
    b8 shadow = shader_rebuild(&r->csm.depth_shader, CSM_VERTEX_PATH, CSM_FRAGMENT_PATH, NULL);
    b8 bloom  = reload_bloom(r);
    b8 fxaa   = shader_rebuild(&r->post.fxaa_shader, POST_VERTEX_PATH, POST_FXAA_PATH, NULL)
             && shader_rebuild(&r->post.taa_shader, POST_VERTEX_PATH, POST_TAA_PATH, NULL);
    b8 exposure = reload_exposure(r);
    b8 prepass  = reload_prepass(r);
    b8 ssao     = reload_ssao(r);
    return pbr && post && shadow && bloom && fxaa && exposure && prepass && ssao;
}

void
scene_renderer_poll_hot_reload(scene_renderer* r)
{
    if (file_watch_changed(&r->prepass_files))
        reload_prepass(r);
    if (file_watch_changed(&r->ssao_files))
        reload_ssao(r);
    if (file_watch_changed(&r->pbr_files))
        reload_pbr(r);
    if (file_watch_changed(&r->post_files))
        shader_rebuild(&r->post.tonemap_shader, POST_VERTEX_PATH, POST_FRAGMENT_PATH, NULL);
    if (file_watch_changed(&r->bloom_files))
        reload_bloom(r);
    if (file_watch_changed(&r->exposure_files))
        reload_exposure(r);
    if (file_watch_changed(&r->taa_files))
        shader_rebuild(&r->post.taa_shader, POST_VERTEX_PATH, POST_TAA_PATH, NULL);
    if (file_watch_changed(&r->fxaa_files))
        shader_rebuild(&r->post.fxaa_shader, POST_VERTEX_PATH, POST_FXAA_PATH, NULL);
    if (file_watch_changed(&r->shadow_files))
        shader_rebuild(&r->csm.depth_shader, CSM_VERTEX_PATH, CSM_FRAGMENT_PATH, NULL);
}

void
scene_renderer_destroy(scene_renderer* r)
{
    gpu_profiler_destroy(&r->profiler);
    post_destroy(&r->post);
    light_clusters_destroy(&r->clusters);
    reflection_probes_destroy(&r->probes);
    if (r->shadow_buffer)
        rhi_buffer_destroy(r->shadow_buffer);
    spot_shadows_destroy(&r->spot);
    point_shadows_destroy(&r->point);
    csm_destroy(&r->csm);

    if (r->pbr_shader)
        rhi_shader_destroy(r->pbr_shader);
    if (r->prepass_shader)
        rhi_shader_destroy(r->prepass_shader);

    memset(r, 0, sizeof(*r));
}
