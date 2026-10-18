#include "RHI_GL_Internal.h"

rhi_buffer gl_vertex_buffer_create(const void* data, uint64 size,
                                    const rhi_vertex_layout* layout,
                                    rhi_buffer_usage usage)
{
    rhi_buffer buf = malloc(sizeof(*buf));
    if (!buf)
        FATAL("Out of memory creating vertex buffer");

    glGenVertexArrays(1, &buf->vao);
    glGenBuffers(1, &buf->handle);

    glBindVertexArray(buf->vao);
    glBindBuffer(GL_ARRAY_BUFFER, buf->handle);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, data, gl_usage_to_gl(usage));

    for (uint32 i = 0; i < layout->attribute_count; ++i)
    {
        const rhi_vertex_attribute* attr = &layout->attributes[i];

        glEnableVertexAttribArray(attr->location);
        glVertexAttribPointer(attr->location,
                               (GLint)attr->component_count,
                               GL_FLOAT,
                               GL_FALSE,
                               (GLsizei)layout->stride,
                               (const void*)(uintptr_t)attr->offset);
    }

    glBindVertexArray(0);

    return buf;
}

rhi_buffer gl_index_buffer_create(const void* data, uint64 size, rhi_buffer_usage usage)
{
    rhi_buffer buf = malloc(sizeof(*buf));
    if (!buf)
        FATAL("Out of memory creating index buffer");

    buf->vao = 0;

    glGenBuffers(1, &buf->handle);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf->handle);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)size, data, gl_usage_to_gl(usage));

    return buf;
}

rhi_buffer gl_uniform_buffer_create(uint64 size)
{
    rhi_buffer buf = malloc(sizeof(*buf));
    if (!buf)
        FATAL("Out of memory creating uniform buffer");

    buf->vao = 0;
    glCreateBuffers(1, &buf->handle);
    glNamedBufferData(buf->handle, (GLsizeiptr)size, NULL, GL_DYNAMIC_DRAW);
    return buf;
}

void gl_uniform_buffer_bind(rhi_buffer buffer, uint32 binding)
{
    glBindBufferBase(GL_UNIFORM_BUFFER, binding, buffer->handle);
}

void gl_storage_buffer_bind(rhi_buffer buffer, uint32 binding)
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer->handle);
}

void gl_buffer_update(rhi_buffer buffer, const void* data, uint64 size, uint64 offset)
{
    // Direct state access: binding the buffer to update it would, for an
    // index or uniform buffer, replace the bound vertex array's index buffer.
    glNamedBufferSubData(buffer->handle, (GLintptr)offset, (GLsizeiptr)size, data);
}

void gl_buffer_destroy(rhi_buffer buffer)
{
    if (!buffer)
        return;

    if (buffer->vao)
        glDeleteVertexArrays(1, &buffer->vao);

    glDeleteBuffers(1, &buffer->handle);
    free(buffer);
}
