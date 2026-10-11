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

// Texture unit the scene color is sampled from in tonemap.frag.
#define POST_SLOT_SCENE 0

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

b8
post_create(post_process* post, uint16 width, uint16 height)
{
    memset(post, 0, sizeof(*post));

    char* vertex_path   = asset_path("shaders/fullscreen.vert");
    char* fragment_path = asset_path("shaders/tonemap.frag");

    post->tonemap_shader = rhi_shader_create_from_files(vertex_path, fragment_path);

    free(vertex_path);
    free(fragment_path);

    if (!post->tonemap_shader)
    {
        LOG_ERROR("post_create: failed to build the tone-mapping shader\n");
        return false;
    }

    rhi_vertex_attribute position = { .location = 0, .component_count = 2, .offset = 0 };
    rhi_vertex_layout    layout   = { .attributes = &position, .attribute_count = 1, .stride = 2 * sizeof(f32) };

    post->triangle_vertices = rhi_vertex_buffer_create(TRIANGLE_VERTICES, sizeof(TRIANGLE_VERTICES), &layout, RHI_USAGE_STATIC);
    post->triangle_indices  = rhi_index_buffer_create(TRIANGLE_INDICES, sizeof(TRIANGLE_INDICES), RHI_USAGE_STATIC);

    // Never a zero-size framebuffer, even if created while minimized --
    // post_resize() brings it to the real size later.
    post->width        = width  ? width  : 1;
    post->height       = height ? height : 1;
    post->scene_target = create_scene_target(post->width, post->height);

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

    post->width        = width;
    post->height       = height;
    post->scene_target = create_scene_target(width, height);
}

void
post_begin_scene(post_process* post, f32 r, f32 g, f32 b)
{
    if (!post->scene_target)
        return;

    rhi_framebuffer_bind(post->scene_target);
    rhi_clear(r, g, b, 1.0f);
}

void
post_end_scene(post_process* post, f32 exposure)
{
    post_end_scene_to(post, exposure, NULL);
}

void
post_end_scene_to(post_process* post, f32 exposure, rhi_framebuffer target)
{
    if (!post->scene_target)
        return;

    if (target)
        rhi_framebuffer_bind(target);
    else
        rhi_framebuffer_bind_default();

    // Every pixel is overwritten, so don't let depth or blending from the
    // scene pass get in the way, and don't leave depth behind for the UI.
    rhi_render_state state = rhi_render_state_default();
    state.depth_write = false;
    rhi_set_render_state(state);

    rhi_shader shader = post->tonemap_shader;

    rhi_shader_bind(shader);
    rhi_shader_set_int(shader, "u_scene", POST_SLOT_SCENE);
    rhi_shader_set_float(shader, "u_exposure", exposure);
    rhi_texture_bind(rhi_framebuffer_get_color_texture(post->scene_target, 0), POST_SLOT_SCENE);

    rhi_draw_indexed(post->triangle_vertices, post->triangle_indices, 3);

    rhi_set_render_state(rhi_render_state_default());
}

void
post_destroy(post_process* post)
{
    if (post->scene_target)
        rhi_framebuffer_destroy(post->scene_target);
    if (post->triangle_vertices)
        rhi_buffer_destroy(post->triangle_vertices);
    if (post->triangle_indices)
        rhi_buffer_destroy(post->triangle_indices);
    if (post->tonemap_shader)
        rhi_shader_destroy(post->tonemap_shader);

    memset(post, 0, sizeof(*post));
}
