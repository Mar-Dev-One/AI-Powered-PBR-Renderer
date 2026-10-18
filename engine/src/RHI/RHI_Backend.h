#pragma once

// Internal to RHI — never included outside RHI.c and the backend
// folders (RHI/OpenGL, future RHI/Vulkan, etc). Callers of the public
// RHI.h API never see this.

#include "RHI.h"

typedef struct rhi_backend_api
{
    b8   (*init)(rhi_proc_loader loader);
    void (*shutdown)(void);

    void (*set_viewport)(uint16 x, uint16 y, uint16 width, uint16 height);
    void (*clear)(f32 r, f32 g, f32 b, f32 a);

    ////
    void (*read_pixels)(uint16 x, uint16 y, uint16 width, uint16 height, void* out_pixels);

    rhi_buffer (*vertex_buffer_create)(const void* data, uint64 size,
                                        const rhi_vertex_layout* layout,
                                        rhi_buffer_usage usage);
    rhi_buffer (*index_buffer_create)(const void* data, uint64 size, rhi_buffer_usage usage);
    rhi_buffer (*uniform_buffer_create)(uint64 size);
    void       (*uniform_buffer_bind)(rhi_buffer buffer, uint32 binding);
    rhi_buffer (*storage_buffer_create)(uint64 size);
    void       (*storage_buffer_bind)(rhi_buffer buffer, uint32 binding);
    void       (*buffer_update)(rhi_buffer buffer, const void* data, uint64 size, uint64 offset);
    void       (*buffer_destroy)(rhi_buffer buffer);

    rhi_shader (*shader_create)(rhi_shader_desc desc);
    void       (*shader_bind)(rhi_shader shader);
    void       (*shader_destroy)(rhi_shader shader);
    void       (*shader_set_mat4)(rhi_shader shader, const char* name, const f32* matrix);
    void       (*shader_set_vec2)(rhi_shader shader, const char* name, f32 x, f32 y);
    void       (*shader_set_vec3)(rhi_shader shader, const char* name, f32 x, f32 y, f32 z);
    void       (*shader_set_vec4)(rhi_shader shader, const char* name, f32 x, f32 y, f32 z, f32 w);
    void       (*shader_set_int)(rhi_shader shader, const char* name, int32 value);
    void       (*shader_set_float)(rhi_shader shader, const char* name, f32 value);

    void (*texture_bind)(rhi_texture texture, uint32 slot);
    uint64 (*texture_native_handle)(rhi_texture texture);
    rhi_texture (*texture_create)(rhi_texture_desc desc);
    void        (*texture_destroy)(rhi_texture texture);

    rhi_texture (*cubemap_create)(rhi_cubemap_desc desc);
    void        (*cubemap_generate_mipmaps)(rhi_texture cubemap);

    rhi_framebuffer (*framebuffer_create)(rhi_framebuffer_desc desc);
    void            (*framebuffer_bind)(rhi_framebuffer framebuffer);
    void            (*framebuffer_bind_default)(void);
    rhi_texture     (*framebuffer_get_color_texture)(rhi_framebuffer framebuffer, uint32 index);
    rhi_texture     (*framebuffer_get_depth_texture)(rhi_framebuffer framebuffer);
    void            (*framebuffer_destroy)(rhi_framebuffer framebuffer);
    rhi_framebuffer (*framebuffer_create_cubemap_target)(void);
    void            (*framebuffer_set_cubemap_target)(rhi_framebuffer framebuffer,
                                                       rhi_texture cubemap,
                                                       uint32 face, uint32 mip);

    void (*set_render_state)(rhi_render_state state);
    void (*set_wireframe)(b8 enabled);
    void (*set_color_write)(b8 enabled);
    void (*set_draw_region)(uint16 x, uint16 y, uint16 width, uint16 height);

    rhi_query (*timestamp_create)(void);
    void      (*timestamp_write)(rhi_query query);
    b8        (*timestamp_read)(rhi_query query, uint64* out_nanoseconds);
    void      (*timestamp_destroy)(rhi_query query);

    void (*draw_indexed)(rhi_buffer vertex_buffer, rhi_buffer index_buffer, uint32 index_count);
} rhi_backend_api;
