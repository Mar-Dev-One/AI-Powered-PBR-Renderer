#include "RHI.h"
#include "RHI_Backend.h"

#include "../Core/FileIO.h"
#include "../Core/Image.h"

#include "OpenGL/RHI_GL.h"
// Add new backend headers here as new src/RHI/<Api>/ folders appear,
// e.g. #include "Vulkan/RHI_VK.h"

static rhi_backend_api backend;

b8 rhi_init(rhi_backend_type type, rhi_proc_loader loader)
{
    switch (type)
    {
        case RHI_BACKEND_OPENGL:
            rhi_gl_get_backend(&backend);
            break;

        default:
            FATAL("Unknown or unbuilt RHI backend");
            return false;
    }

    return backend.init(loader);
}

void rhi_shutdown(void)
{
    backend.shutdown();
}

void rhi_set_viewport(uint16 x, uint16 y, uint16 width, uint16 height)
{
    backend.set_viewport(x, y, width, height);
}

void rhi_clear(f32 r, f32 g, f32 b, f32 a)
{
    backend.clear(r, g, b, a);
}

rhi_buffer rhi_vertex_buffer_create(const void* data, uint64 size,
                                     const rhi_vertex_layout* layout,
                                     rhi_buffer_usage usage)
{
    return backend.vertex_buffer_create(data, size, layout, usage);
}

rhi_buffer rhi_index_buffer_create(const void* data, uint64 size, rhi_buffer_usage usage)
{
    return backend.index_buffer_create(data, size, usage);
}

rhi_buffer rhi_uniform_buffer_create(uint64 size)
{
    return backend.uniform_buffer_create(size);
}

void rhi_uniform_buffer_bind(rhi_buffer buffer, uint32 binding)
{
    backend.uniform_buffer_bind(buffer, binding);
}

void rhi_buffer_update(rhi_buffer buffer, const void* data, uint64 size, uint64 offset)
{
    backend.buffer_update(buffer, data, size, offset);
}

void rhi_buffer_destroy(rhi_buffer buffer)
{
    backend.buffer_destroy(buffer);
}

rhi_shader rhi_shader_create(rhi_shader_desc desc)
{
    return backend.shader_create(desc);
}

rhi_shader rhi_shader_create_from_files(const char* vertex_path, const char* fragment_path)
{
    return rhi_shader_create_from_files_with_defines(vertex_path, fragment_path, NULL);
}

rhi_shader rhi_shader_create_from_files_with_defines(const char* vertex_path, const char* fragment_path,
                                                     const char* defines)
{
    char* vertex_src = file_read_to_string(vertex_path);
    char* fragment_src = file_read_to_string(fragment_path);

    rhi_shader shader = NULL;

    if (vertex_src && fragment_src)
    {
        rhi_shader_desc desc = {
            .vertex_src = vertex_src,
            .fragment_src = fragment_src,
            .defines = defines
        };

        shader = rhi_shader_create(desc);
    }

    free(vertex_src);
    free(fragment_src);

    return shader;
}

void rhi_shader_bind(rhi_shader shader)
{
    backend.shader_bind(shader);
}

void rhi_shader_destroy(rhi_shader shader)
{
    backend.shader_destroy(shader);
}

void rhi_shader_set_mat4(rhi_shader shader, const char* name, const f32* matrix)
{
    backend.shader_set_mat4(shader, name, matrix);
}

void rhi_shader_set_vec2(rhi_shader shader, const char* name, f32 x, f32 y)
{
    backend.shader_set_vec2(shader, name, x, y);
}

void rhi_shader_set_vec3(rhi_shader shader, const char* name, f32 x, f32 y, f32 z)
{
    backend.shader_set_vec3(shader, name, x, y, z);
}

void rhi_shader_set_int(rhi_shader shader, const char* name, int32 value)
{
    backend.shader_set_int(shader, name, value);
}

void rhi_shader_set_vec4(rhi_shader shader, const char* name, f32 x, f32 y, f32 z, f32 w)
{
    backend.shader_set_vec4(shader, name, x, y, z, w);
}

void rhi_shader_set_float(rhi_shader shader, const char* name, f32 value)
{
    backend.shader_set_float(shader, name, value);
}

void rhi_texture_bind(rhi_texture texture, uint32 slot)
{
    backend.texture_bind(texture, slot);
}

uint64 rhi_texture_native_handle(rhi_texture texture)
{
    return texture ? backend.texture_native_handle(texture) : 0;
}

rhi_texture rhi_texture_create(rhi_texture_desc desc)
{
    return backend.texture_create(desc);
}

rhi_texture rhi_texture_create_from_file(const char* path,
                                          rhi_texture_filter filter,
                                          rhi_texture_wrap wrap,
                                          b8 generate_mipmaps)
{
    image img = image_load(path, 4);
    if (!img.pixels)
        return NULL;

    rhi_texture_desc desc = {
        .width = img.width,
        .height = img.height,
        .format = RHI_FORMAT_RGBA8,
        .filter = filter,
        .wrap = wrap,
        .pixels = img.pixels,
        .generate_mipmaps = generate_mipmaps
    };

    rhi_texture texture = rhi_texture_create(desc);

    image_free(&img);

    return texture;
}

void rhi_texture_destroy(rhi_texture texture)
{
    backend.texture_destroy(texture);
}

rhi_texture rhi_cubemap_create(rhi_cubemap_desc desc)
{
    return backend.cubemap_create(desc);
}

void rhi_cubemap_generate_mipmaps(rhi_texture cubemap)
{
    backend.cubemap_generate_mipmaps(cubemap);
}

rhi_framebuffer rhi_framebuffer_create(rhi_framebuffer_desc desc)
{
    // ASSERT compiles to nothing in release (NDEBUG) builds, but the
    // backend still writes color_attachment_count entries into a
    // fixed-size RHI_MAX_COLOR_ATTACHMENTS stack array — so this bound
    // has to be enforced unconditionally, not just asserted in debug.
    if (desc.color_attachment_count > RHI_MAX_COLOR_ATTACHMENTS)
        FATAL("rhi_framebuffer_create: color_attachment_count (%u) exceeds RHI_MAX_COLOR_ATTACHMENTS (%u)",
              desc.color_attachment_count, RHI_MAX_COLOR_ATTACHMENTS);

    return backend.framebuffer_create(desc);
}

void rhi_framebuffer_bind(rhi_framebuffer framebuffer)
{
    backend.framebuffer_bind(framebuffer);
}

void rhi_framebuffer_bind_default(void)
{
    backend.framebuffer_bind_default();
}

rhi_texture rhi_framebuffer_get_color_texture(rhi_framebuffer framebuffer, uint32 index)
{
    return backend.framebuffer_get_color_texture(framebuffer, index);
}

rhi_texture rhi_framebuffer_get_depth_texture(rhi_framebuffer framebuffer)
{
    return backend.framebuffer_get_depth_texture(framebuffer);
}

void rhi_framebuffer_destroy(rhi_framebuffer framebuffer)
{
    backend.framebuffer_destroy(framebuffer);
}

rhi_framebuffer rhi_framebuffer_create_cubemap_target(void)
{
    return backend.framebuffer_create_cubemap_target();
}

void rhi_framebuffer_set_cubemap_target(rhi_framebuffer framebuffer, rhi_texture cubemap,
                                        uint32 face, uint32 mip)
{
    backend.framebuffer_set_cubemap_target(framebuffer, cubemap, face, mip);
}

rhi_render_state rhi_render_state_default(void)
{
    return (rhi_render_state){ .cull_back_faces = true, .blend_enabled = false, .blend_additive = false,
                               .depth_write = true, .front_face_cw = false };
}

void rhi_set_render_state(rhi_render_state state)
{
    backend.set_render_state(state);
}

void rhi_set_wireframe(b8 enabled)
{
    backend.set_wireframe(enabled);
}

void rhi_set_color_write(b8 enabled)
{
    backend.set_color_write(enabled);
}

void rhi_set_draw_region(uint16 x, uint16 y, uint16 width, uint16 height)
{
    backend.set_draw_region(x, y, width, height);
}

rhi_query rhi_timestamp_create(void)
{
    return backend.timestamp_create();
}

void rhi_timestamp_write(rhi_query query)
{
    backend.timestamp_write(query);
}

b8 rhi_timestamp_read(rhi_query query, uint64* out_nanoseconds)
{
    return backend.timestamp_read(query, out_nanoseconds);
}

void rhi_timestamp_destroy(rhi_query query)
{
    backend.timestamp_destroy(query);
}

void rhi_draw_indexed(rhi_buffer vertex_buffer, rhi_buffer index_buffer, uint32 index_count)
{
    backend.draw_indexed(vertex_buffer, index_buffer, index_count);
}


////
void rhi_read_pixels(uint16 x, uint16 y, uint16 width, uint16 height, void* out_pixels)
{
    backend.read_pixels(x, y, width, height, out_pixels);
}