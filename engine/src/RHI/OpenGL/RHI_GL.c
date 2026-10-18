#include "RHI_GL_Internal.h"

gl_viewport g_gl_screen_viewport = { 0, 0, 0, 0 };

GLenum gl_usage_to_gl(rhi_buffer_usage usage)
{
    return (usage == RHI_USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
}

b8 gl_init(rhi_proc_loader loader)
{
    if (!gladLoadGL((GLADloadfunc)loader))
    {
        LOG_ERROR("Failed to load GL function pointers\n");
        return false;
    }

    // Nothing draws correctly in 3D without this: without depth testing,
    // triangles are rasterized in submission order regardless of distance
    // from the camera.
    glEnable(GL_DEPTH_TEST);
    // LEQUAL, not LESS: after the depth pre-pass (Renderer/SceneRenderer.h)
    // the main pass draws each surface again at exactly the depth it laid
    // down, which LESS would reject.
    glDepthFunc(GL_LEQUAL);

    // Standard back-face culling. Safe to enable now (nothing currently
    // relies on seeing back faces) and needed once real meshes show up.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Filter across cube map face edges instead of clamping within a face --
    // without it, blurry reflections (high mips of a prefiltered environment)
    // show visible seams along the cube's edges.
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    return true;
}

void gl_shutdown(void)
{
    // Nothing to release at the device level yet — resources are
    // freed individually via rhi_*_destroy.
}

void gl_set_viewport(uint16 x, uint16 y, uint16 width, uint16 height)
{
    g_gl_screen_viewport = (gl_viewport){ x, y, width, height };
    glViewport(x, y, width, height);
}

void gl_clear(f32 r, f32 g, f32 b, f32 a)
{
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_set_render_state(rhi_render_state state)
{
    if (state.cull_back_faces) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);

    if (state.blend_enabled)
    {
        glEnable(GL_BLEND);
        if (state.blend_additive)
            glBlendFunc(GL_ONE, GL_ONE);
        else
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        glDisable(GL_BLEND);
    }

    glDepthMask(state.depth_write ? GL_TRUE : GL_FALSE);
    glFrontFace(state.front_face_cw ? GL_CW : GL_CCW);
}

void gl_set_wireframe(b8 enabled)
{
    glPolygonMode(GL_FRONT_AND_BACK, enabled ? GL_LINE : GL_FILL);
}

void gl_set_color_write(b8 enabled)
{
    GLboolean on = enabled ? GL_TRUE : GL_FALSE;
    glColorMask(on, on, on, on);
}

rhi_query gl_timestamp_create(void)
{
    rhi_query query = calloc(1, sizeof(*query));
    if (query)
        glGenQueries(1, &query->handle);
    return query;
}

void gl_timestamp_write(rhi_query query)
{
    glQueryCounter(query->handle, GL_TIMESTAMP);
    query->written = true;
}

b8 gl_timestamp_read(rhi_query query, uint64* out_nanoseconds)
{
    if (!query->written)
        return false;

    GLint available = 0;
    glGetQueryObjectiv(query->handle, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available)
        return false;

    GLuint64 value = 0;
    glGetQueryObjectui64v(query->handle, GL_QUERY_RESULT, &value);
    *out_nanoseconds = (uint64)value;
    return true;
}

void gl_timestamp_destroy(rhi_query query)
{
    if (!query)
        return;
    glDeleteQueries(1, &query->handle);
    free(query);
}

void gl_draw_indexed(rhi_buffer vertex_buffer, rhi_buffer index_buffer, uint32 index_count)
{
    glBindVertexArray(vertex_buffer->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer->handle);
    glDrawElements(GL_TRIANGLES, (GLsizei)index_count, GL_UNSIGNED_INT, NULL);
}

void rhi_gl_get_backend(rhi_backend_api* out_api)
{
    out_api->init = gl_init;
    out_api->shutdown = gl_shutdown;

    out_api->set_viewport = gl_set_viewport;
    out_api->clear = gl_clear;

    ////
    out_api->read_pixels = gl_read_pixels;

    out_api->vertex_buffer_create = gl_vertex_buffer_create;
    out_api->index_buffer_create = gl_index_buffer_create;
    out_api->buffer_update = gl_buffer_update;
    out_api->buffer_destroy = gl_buffer_destroy;

    out_api->shader_create = gl_shader_create;
    out_api->shader_bind = gl_shader_bind;
    out_api->shader_destroy = gl_shader_destroy;
    out_api->shader_set_mat4 = gl_shader_set_mat4;
    out_api->shader_set_vec2 = gl_shader_set_vec2;
    out_api->shader_set_vec3 = gl_shader_set_vec3;
    out_api->shader_set_vec4 = gl_shader_set_vec4;
    out_api->shader_set_int = gl_shader_set_int;
    out_api->shader_set_float = gl_shader_set_float;

    out_api->texture_bind = gl_texture_bind;
    out_api->texture_native_handle = gl_texture_native_handle;
    out_api->texture_create = gl_texture_create;
    out_api->texture_destroy = gl_texture_destroy;

    out_api->cubemap_create = gl_cubemap_create;
    out_api->cubemap_generate_mipmaps = gl_cubemap_generate_mipmaps;

    out_api->framebuffer_create = gl_framebuffer_create;
    out_api->framebuffer_bind = gl_framebuffer_bind;
    out_api->framebuffer_bind_default = gl_framebuffer_bind_default;
    out_api->framebuffer_get_color_texture = gl_framebuffer_get_color_texture;
    out_api->framebuffer_get_depth_texture = gl_framebuffer_get_depth_texture;
    out_api->framebuffer_destroy = gl_framebuffer_destroy;
    out_api->framebuffer_create_cubemap_target = gl_framebuffer_create_cubemap_target;
    out_api->framebuffer_set_cubemap_target = gl_framebuffer_set_cubemap_target;

    out_api->set_render_state = gl_set_render_state;
    out_api->set_wireframe = gl_set_wireframe;
    out_api->set_color_write = gl_set_color_write;

    out_api->timestamp_create = gl_timestamp_create;
    out_api->timestamp_write = gl_timestamp_write;
    out_api->timestamp_read = gl_timestamp_read;
    out_api->timestamp_destroy = gl_timestamp_destroy;
    out_api->draw_indexed = gl_draw_indexed;
}

////
void gl_read_pixels(uint16 x, uint16 y, uint16 width, uint16 height, void* out_pixels)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_pixels);
}