#pragma once

// Shared between the OpenGL backend's own files only (RHI_GL*.c).
// Nothing outside src/RHI/OpenGL should ever include this — it's what
// keeps GLuint out of RHI.c and every layer above it.

#include "RHI_GL.h"
#include "glad/gl.h"

struct rhi_buffer
{
    GLuint handle;   // VBO or EBO
    GLuint vao;      // only set for vertex buffers, 0 for index buffers
};

// One cached glGetUniformLocation() result; name == NULL marks an empty slot.
typedef struct gl_uniform_slot
{
    char*  name;       // owned copy
    uint32 hash;
    GLint  location;   // -1 cached too, so missing names aren't re-queried
} gl_uniform_slot;

struct rhi_shader
{
    GLuint program;

    // Open-addressing hash table, power-of-two capacity (see RHI_GL_Shader.c).
    gl_uniform_slot* uniforms;
    uint32           uniform_capacity;
    uint32           uniform_count;
};

struct rhi_texture
{
    GLuint handle;
    GLenum target;      // GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP -- what gl_texture_bind() binds it as
    uint16 width;       // mip 0 size
    uint16 height;
    uint32 mip_count;   // levels allocated for cube maps; 1 for plain 2D textures
};

struct rhi_framebuffer
{
    GLuint       fbo;
    rhi_texture  color_textures[RHI_MAX_COLOR_ATTACHMENTS];
    uint32       color_texture_count;
    rhi_texture  depth_texture;   // NULL if the framebuffer has no depth attachment
    uint16       width;
    uint16       height;
};

struct rhi_query
{
    GLuint handle;    // a GL_TIMESTAMP query
    b8     written;   // glQueryCounter() has run at least once (reading one that hasn't is an error)
};

// --- RHI_GL.c (device) ---------------------------------------------------
b8   gl_init(rhi_proc_loader loader);
void gl_shutdown(void);
void gl_set_viewport(uint16 x, uint16 y, uint16 width, uint16 height);
void gl_clear(f32 r, f32 g, f32 b, f32 a);
void gl_set_render_state(rhi_render_state state);
void gl_set_wireframe(b8 enabled);
void gl_set_color_write(b8 enabled);
void gl_set_draw_region(uint16 x, uint16 y, uint16 width, uint16 height);

rhi_query gl_timestamp_create(void);
void      gl_timestamp_write(rhi_query query);
b8        gl_timestamp_read(rhi_query query, uint64* out_nanoseconds);
void      gl_timestamp_destroy(rhi_query query);
void gl_draw_indexed(rhi_buffer vertex_buffer, rhi_buffer index_buffer, uint32 index_count);

// The most recent viewport passed to gl_set_viewport(). This is what the
// window/screen viewport currently is (App resizes call rhi_set_viewport
// with the full window size), so gl_framebuffer_bind_default() can restore
// it after an offscreen pass leaves glViewport pointing at an FBO's size.
typedef struct gl_viewport { uint16 x, y, width, height; } gl_viewport;
extern gl_viewport g_gl_screen_viewport;

// --- RHI_GL_Buffer.c ---------------------------------------------------
rhi_buffer gl_vertex_buffer_create(const void* data, uint64 size,
                                    const rhi_vertex_layout* layout,
                                    rhi_buffer_usage usage);
rhi_buffer gl_index_buffer_create(const void* data, uint64 size, rhi_buffer_usage usage);
rhi_buffer gl_uniform_buffer_create(uint64 size);
void       gl_uniform_buffer_bind(rhi_buffer buffer, uint32 binding);
void       gl_storage_buffer_bind(rhi_buffer buffer, uint32 binding);
void       gl_buffer_update(rhi_buffer buffer, const void* data, uint64 size, uint64 offset);
void       gl_buffer_destroy(rhi_buffer buffer);

////
void gl_read_pixels(uint16 x, uint16 y, uint16 width, uint16 height, void* out_pixels);

// --- RHI_GL_Shader.c ---------------------------------------------------
rhi_shader gl_shader_create(rhi_shader_desc desc);
void       gl_shader_bind(rhi_shader shader);
void       gl_shader_destroy(rhi_shader shader);
void       gl_shader_set_mat4(rhi_shader shader, const char* name, const f32* matrix);
void       gl_shader_set_vec2(rhi_shader shader, const char* name, f32 x, f32 y);
void       gl_shader_set_vec3(rhi_shader shader, const char* name, f32 x, f32 y, f32 z);
void       gl_shader_set_vec4(rhi_shader shader, const char* name, f32 x, f32 y, f32 z, f32 w);
void       gl_shader_set_int(rhi_shader shader, const char* name, int32 value);
void       gl_shader_set_float(rhi_shader shader, const char* name, f32 value);

// --- RHI_GL_Texture.c ---------------------------------------------------
rhi_texture gl_texture_create(rhi_texture_desc desc);
void        gl_texture_bind(rhi_texture texture, uint32 slot);
uint64      gl_texture_native_handle(rhi_texture texture);
void        gl_texture_destroy(rhi_texture texture);
rhi_texture gl_cubemap_create(rhi_cubemap_desc desc);
void        gl_cubemap_generate_mipmaps(rhi_texture cubemap);

// --- RHI_GL_Framebuffer.c -----------------------------------------------
rhi_framebuffer gl_framebuffer_create(rhi_framebuffer_desc desc);
void            gl_framebuffer_bind(rhi_framebuffer framebuffer);
void            gl_framebuffer_bind_default(void);
rhi_texture     gl_framebuffer_get_color_texture(rhi_framebuffer framebuffer, uint32 index);
rhi_texture     gl_framebuffer_get_depth_texture(rhi_framebuffer framebuffer);
void            gl_framebuffer_destroy(rhi_framebuffer framebuffer);
rhi_framebuffer gl_framebuffer_create_cubemap_target(void);
void            gl_framebuffer_set_cubemap_target(rhi_framebuffer framebuffer, rhi_texture cubemap,
                                                   uint32 face, uint32 mip);

// Shared small helper (used by RHI_GL_Buffer.c)
GLenum gl_usage_to_gl(rhi_buffer_usage usage);
