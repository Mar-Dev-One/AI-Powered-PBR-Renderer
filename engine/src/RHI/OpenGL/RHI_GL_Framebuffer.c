#include "RHI_GL_Internal.h"

static rhi_texture
create_attachment(rhi_texture_format format, uint16 width, uint16 height)
{
    rhi_texture_desc desc = {
        .width = width,
        .height = height,
        .format = format,
        .filter = RHI_FILTER_LINEAR,
        .wrap = RHI_WRAP_CLAMP_TO_EDGE,
        .pixels = NULL,           // render targets start empty — the GPU fills them in
        .generate_mipmaps = false
    };

    return gl_texture_create(desc);
}

rhi_framebuffer gl_framebuffer_create(rhi_framebuffer_desc desc)
{
    // Bound is already enforced unconditionally in rhi_framebuffer_create()
    // (see RHI.c) before this backend function is ever called.
    ASSERT(desc.color_attachment_count <= RHI_MAX_COLOR_ATTACHMENTS);

    rhi_framebuffer fb = malloc(sizeof(*fb));
    if (!fb)
        FATAL("Out of memory creating framebuffer");

    fb->width = desc.width;
    fb->height = desc.height;
    fb->color_texture_count = desc.color_attachment_count;
    fb->depth_texture = NULL;

    glGenFramebuffers(1, &fb->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);

    GLenum draw_buffers[RHI_MAX_COLOR_ATTACHMENTS];

    for (uint32 i = 0; i < desc.color_attachment_count; ++i)
    {
        rhi_texture tex = create_attachment(desc.color_attachments[i].format, desc.width, desc.height);
        fb->color_textures[i] = tex;

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                GL_TEXTURE_2D, tex->handle, 0);
        draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
    }

    if (desc.color_attachment_count > 0)
        glDrawBuffers((GLsizei)desc.color_attachment_count, draw_buffers);
    else
        glDrawBuffer(GL_NONE);   // depth-only target, e.g. a shadow map

    if (desc.has_depth_attachment)
    {
        fb->depth_texture = create_attachment(RHI_FORMAT_DEPTH24, desc.width, desc.height);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_TEXTURE_2D, fb->depth_texture->handle, 0);

        // texture() on a sampler2DShadow then returns the fraction of the
        // 2x2 texels around the lookup that pass ref <= stored depth,
        // bilinearly weighted -- PCF in hardware. The attachment is
        // linear-filtered already (create_attachment()).
        if (desc.depth_compare)
        {
            glTextureParameteri(fb->depth_texture->handle, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTextureParameteri(fb->depth_texture->handle, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        FATAL("Framebuffer incomplete");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return fb;
}

void gl_framebuffer_bind(rhi_framebuffer framebuffer)
{
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->fbo);
    glViewport(0, 0, framebuffer->width, framebuffer->height);
}

void gl_framebuffer_bind_default(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // gl_framebuffer_bind() leaves glViewport set to the FBO's size. Restore
    // it to the actual window/screen viewport, otherwise anything drawn
    // after an offscreen pass (of a different size than the window) renders
    // into the wrong region until the next resize event.
    gl_viewport vp = g_gl_screen_viewport;
    glViewport(vp.x, vp.y, vp.width, vp.height);
}

rhi_texture gl_framebuffer_get_color_texture(rhi_framebuffer framebuffer, uint32 index)
{
    ASSERT(index < framebuffer->color_texture_count);
    return framebuffer->color_textures[index];
}

rhi_texture gl_framebuffer_get_depth_texture(rhi_framebuffer framebuffer)
{
    return framebuffer->depth_texture;
}

void gl_framebuffer_destroy(rhi_framebuffer framebuffer)
{
    if (!framebuffer)
        return;

    for (uint32 i = 0; i < framebuffer->color_texture_count; ++i)
        gl_texture_destroy(framebuffer->color_textures[i]);

    gl_texture_destroy(framebuffer->depth_texture);

    glDeleteFramebuffers(1, &framebuffer->fbo);
    free(framebuffer);
}

void gl_framebuffer_set_cubemap_array_target(rhi_framebuffer framebuffer, rhi_texture array,
                                             uint32 layer, uint32 face, uint32 mip)
{
    ASSERT(array->target == GL_TEXTURE_CUBE_MAP_ARRAY);
    ASSERT(face < 6);
    ASSERT(mip < array->mip_count);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->fbo);
    // An array's layer-faces run layer 0's +X..-Z, then layer 1's, ...
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, array->handle, (GLint)mip, (GLint)(layer * 6 + face));

    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);

    uint16 size = (uint16)(array->width >> mip);
    framebuffer->width  = size ? size : 1;
    framebuffer->height = framebuffer->width;

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        FATAL("Cubemap array framebuffer incomplete");

    glViewport(0, 0, framebuffer->width, framebuffer->height);
}

rhi_framebuffer gl_framebuffer_create_cubemap_target(void)
{
    rhi_framebuffer fb = malloc(sizeof(*fb));
    if (!fb)
        FATAL("Out of memory creating framebuffer");

    // No attachments of its own: the color target (a face of somebody else's
    // cube map) is swapped in by gl_framebuffer_set_cubemap_target(), so
    // color_texture_count stays 0 and destroy never frees the cube map.
    fb->color_texture_count = 0;
    fb->depth_texture = NULL;
    fb->width = 0;
    fb->height = 0;

    glGenFramebuffers(1, &fb->fbo);

    return fb;
}

void gl_framebuffer_set_cubemap_target(rhi_framebuffer framebuffer, rhi_texture cubemap,
                                       uint32 face, uint32 mip)
{
    ASSERT(cubemap->target == GL_TEXTURE_CUBE_MAP);
    ASSERT(face < 6);
    ASSERT(mip < cubemap->mip_count);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, cubemap->handle, (GLint)mip);

    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &draw_buffer);

    uint16 size = (uint16)(cubemap->width >> mip);
    framebuffer->width = size ? size : 1;
    framebuffer->height = framebuffer->width;

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        FATAL("Cubemap framebuffer incomplete");

    glViewport(0, 0, framebuffer->width, framebuffer->height);
}
