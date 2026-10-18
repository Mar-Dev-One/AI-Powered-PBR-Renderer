#include "PostProcess.h"

#include "../Core/Paths.h"

#include <string.h>

// One oversized clip-space triangle instead of a two-triangle quad: it
// covers the whole screen (the parts outside [-1, 1] are clipped away) with
// no diagonal seam where pixels get shaded twice. Counter-clockwise, so the
// default back-face culling keeps it.
static const f32 TRIANGLE_VERTICES[] = {
    -1.0f, -1.0f,
     3.0f, -1.0f,
    -1.0f,  3.0f,
};

static const uint32 TRIANGLE_INDICES[] = { 0, 1, 2 };

// Texture units tonemap.frag samples the scene and the bloom from; the
// bloom passes read their source from POST_SLOT_SCENE too.
#define POST_SLOT_SCENE 0
#define POST_SLOT_BLOOM 1

// The bloom chain stops before a target would be smaller than this: below
// it the blur has nothing left to add.
#define BLOOM_MIN_SIZE 4

static rhi_framebuffer
create_scene_target(uint16 width, uint16 height)
{
    rhi_framebuffer_attachment color = { .format = RHI_FORMAT_RGBA16F };

    return rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
        .has_depth_attachment   = true
    });
}

static rhi_framebuffer
create_ldr_target(uint16 width, uint16 height)
{
    rhi_framebuffer_attachment color = { .format = RHI_FORMAT_RGBA8 };

    return rhi_framebuffer_create((rhi_framebuffer_desc){
        .width                  = width,
        .height                 = height,
        .color_attachments      = &color,
        .color_attachment_count = 1,
    });
}

static rhi_shader
load_shader(const char* vertex, const char* fragment)
{
    char* vertex_path   = engine_path(vertex);
    char* fragment_path = engine_path(fragment);

    rhi_shader shader = rhi_shader_create_from_files(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!shader)
        LOG_ERROR("post_create: failed to build '%s' + '%s'\n", vertex, fragment);
    return shader;
}

static void
destroy_bloom_chain(post_process* post)
{
    for (uint32 i = 0; i < post->bloom_mip_count; ++i)
        rhi_framebuffer_destroy(post->bloom_mips[i]);
    memset(post->bloom_mips, 0, sizeof(post->bloom_mips));
    post->bloom_mip_count = 0;
}

// Half the scene's size, then half of that, ... down to BLOOM_MIN_SIZE.
static void
create_bloom_chain(post_process* post)
{
    rhi_framebuffer_attachment color  = { .format = RHI_FORMAT_RGBA16F };
    uint16                     width  = post->width / 2;
    uint16                     height = post->height / 2;

    while (post->bloom_mip_count < POST_BLOOM_MAX_MIPS && width >= BLOOM_MIN_SIZE && height >= BLOOM_MIN_SIZE)
    {
        post->bloom_widths[post->bloom_mip_count]  = width;
        post->bloom_heights[post->bloom_mip_count] = height;
        post->bloom_mips[post->bloom_mip_count++]  = rhi_framebuffer_create((rhi_framebuffer_desc){
            .width                  = width,
            .height                 = height,
            .color_attachments      = &color,
            .color_attachment_count = 1,
        });
        width  /= 2;
        height /= 2;
    }
}

b8
post_create(post_process* post, uint16 width, uint16 height)
{
    memset(post, 0, sizeof(*post));

    post->tonemap_shader    = load_shader(POST_VERTEX_PATH, POST_FRAGMENT_PATH);
    post->bloom_down_shader = load_shader(POST_VERTEX_PATH, POST_BLOOM_DOWN_PATH);
    post->bloom_up_shader   = load_shader(POST_VERTEX_PATH, POST_BLOOM_UP_PATH);
    post->fxaa_shader       = load_shader(POST_VERTEX_PATH, POST_FXAA_PATH);
    if (!post->tonemap_shader || !post->bloom_down_shader || !post->bloom_up_shader || !post->fxaa_shader)
        return false;

    rhi_vertex_attribute position = { .location = 0, .component_count = 2, .offset = 0 };
    rhi_vertex_layout    layout   = { .attributes = &position, .attribute_count = 1, .stride = 2 * sizeof(f32) };

    post->triangle_vertices = rhi_vertex_buffer_create(TRIANGLE_VERTICES, sizeof(TRIANGLE_VERTICES), &layout, RHI_USAGE_STATIC);
    post->triangle_indices  = rhi_index_buffer_create(TRIANGLE_INDICES, sizeof(TRIANGLE_INDICES), RHI_USAGE_STATIC);

    // Never a zero-size framebuffer, even if created while minimized --
    // post_resize() brings it to the real size later.
    post->width        = width  ? width  : 1;
    post->height       = height ? height : 1;
    post->scene_target = create_scene_target(post->width, post->height);
    post->ldr_target   = create_ldr_target(post->width, post->height);
    create_bloom_chain(post);

    return true;
}

void
post_resize(post_process* post, uint16 width, uint16 height)
{
    if (width == 0 || height == 0)
        return;

    if (!post->scene_target || (width == post->width && height == post->height))
        return;

    rhi_framebuffer_destroy(post->scene_target);
    rhi_framebuffer_destroy(post->ldr_target);
    destroy_bloom_chain(post);

    post->width        = width;
    post->height       = height;
    post->scene_target = create_scene_target(width, height);
    post->ldr_target   = create_ldr_target(width, height);
    create_bloom_chain(post);
}

void
post_begin_scene(post_process* post, f32 r, f32 g, f32 b)
{
    if (!post->scene_target)
        return;

    post->bloom_ready = false;
    rhi_framebuffer_bind(post->scene_target);
    rhi_clear(r, g, b, 1.0f);
}

static void
draw_triangle(const post_process* post)
{
    rhi_draw_indexed(post->triangle_vertices, post->triangle_indices, 3);
}

void
post_bloom(post_process* post)
{
    if (!post->scene_target || post->bloom_mip_count == 0)
        return;

    // Each pass overwrites its whole target (down) or adds to all of it (up).
    rhi_render_state state = rhi_render_state_default();
    state.depth_write = false;
    rhi_set_render_state(state);

    // Down: the scene into mip 0, each mip into the next.
    rhi_shader down = post->bloom_down_shader;
    rhi_shader_bind(down);
    rhi_shader_set_int(down, "u_source", POST_SLOT_SCENE);

    rhi_framebuffer source        = post->scene_target;
    uint16          source_width  = post->width;
    uint16          source_height = post->height;
    for (uint32 i = 0; i < post->bloom_mip_count; ++i)
    {
        rhi_framebuffer_bind(post->bloom_mips[i]);
        rhi_shader_set_vec2(down, "u_source_texel", 1.0f / (f32)source_width, 1.0f / (f32)source_height);
        rhi_shader_set_int(down, "u_first", i == 0);
        rhi_texture_bind(rhi_framebuffer_get_color_texture(source, 0), POST_SLOT_SCENE);
        draw_triangle(post);

        source        = post->bloom_mips[i];
        source_width  = post->bloom_widths[i];
        source_height = post->bloom_heights[i];
    }

    // Up: each mip blurred into the next larger one, added to what's there.
    state.blend_enabled  = true;
    state.blend_additive = true;
    rhi_set_render_state(state);

    rhi_shader up = post->bloom_up_shader;
    rhi_shader_bind(up);
    rhi_shader_set_int(up, "u_source", POST_SLOT_SCENE);

    for (uint32 i = post->bloom_mip_count - 1; i > 0; --i)
    {
        rhi_framebuffer_bind(post->bloom_mips[i - 1]);
        rhi_shader_set_vec2(up, "u_radius", 1.0f / (f32)post->bloom_widths[i], 1.0f / (f32)post->bloom_heights[i]);
        rhi_texture_bind(rhi_framebuffer_get_color_texture(post->bloom_mips[i], 0), POST_SLOT_SCENE);
        draw_triangle(post);
    }

    rhi_set_render_state(rhi_render_state_default());
    post->bloom_ready = true;
}

void
post_end_scene(post_process* post, const post_settings* settings)
{
    post_end_scene_to(post, settings, NULL);
}

void
post_end_scene_to(post_process* post, const post_settings* settings, rhi_framebuffer target)
{
    if (!post->scene_target)
        return;

    // With FXAA, tone mapping goes to the LDR target first, and FXAA from
    // there to `target`.
    b8 fxaa = settings->fxaa && !settings->raw && post->ldr_target;
    if (fxaa)
        rhi_framebuffer_bind(post->ldr_target);
    else if (target)
        rhi_framebuffer_bind(target);
    else
        rhi_framebuffer_bind_default();

    // Every pixel is overwritten, so don't let depth or blending from the
    // scene pass get in the way, and don't leave depth behind for the UI.
    rhi_render_state state = rhi_render_state_default();
    state.depth_write = false;
    rhi_set_render_state(state);

    b8         bloom  = settings->bloom > 0.0f && !settings->raw && post->bloom_ready;
    rhi_shader shader = post->tonemap_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_int(shader, "u_scene", POST_SLOT_SCENE);
    rhi_shader_set_int(shader, "u_bloom", POST_SLOT_BLOOM);
    rhi_shader_set_float(shader, "u_exposure", settings->exposure);
    rhi_shader_set_int(shader, "u_tonemap", settings->tonemap);
    rhi_shader_set_int(shader, "u_raw", settings->raw);
    rhi_shader_set_int(shader, "u_luma_alpha", fxaa);
    rhi_shader_set_float(shader, "u_bloom_strength", bloom ? settings->bloom : 0.0f);
    rhi_shader_set_float(shader, "u_bloom_scale", post->bloom_mip_count ? 1.0f / (f32)post->bloom_mip_count : 0.0f);
    rhi_texture_bind(rhi_framebuffer_get_color_texture(post->scene_target, 0), POST_SLOT_SCENE);
    if (bloom)
        rhi_texture_bind(rhi_framebuffer_get_color_texture(post->bloom_mips[0], 0), POST_SLOT_BLOOM);

    draw_triangle(post);

    if (fxaa)
    {
        if (target)
            rhi_framebuffer_bind(target);
        else
            rhi_framebuffer_bind_default();

        rhi_shader_bind(post->fxaa_shader);
        rhi_shader_set_int(post->fxaa_shader, "u_source", POST_SLOT_SCENE);
        rhi_shader_set_vec2(post->fxaa_shader, "u_texel", 1.0f / (f32)post->width, 1.0f / (f32)post->height);
        rhi_texture_bind(rhi_framebuffer_get_color_texture(post->ldr_target, 0), POST_SLOT_SCENE);
        draw_triangle(post);
    }

    rhi_set_render_state(rhi_render_state_default());
}

void
post_destroy(post_process* post)
{
    if (post->scene_target)
        rhi_framebuffer_destroy(post->scene_target);
    if (post->ldr_target)
        rhi_framebuffer_destroy(post->ldr_target);
    destroy_bloom_chain(post);
    if (post->triangle_vertices)
        rhi_buffer_destroy(post->triangle_vertices);
    if (post->triangle_indices)
        rhi_buffer_destroy(post->triangle_indices);
    if (post->tonemap_shader)
        rhi_shader_destroy(post->tonemap_shader);
    if (post->bloom_down_shader)
        rhi_shader_destroy(post->bloom_down_shader);
    if (post->bloom_up_shader)
        rhi_shader_destroy(post->bloom_up_shader);
    if (post->fxaa_shader)
        rhi_shader_destroy(post->fxaa_shader);

    memset(post, 0, sizeof(*post));
}
