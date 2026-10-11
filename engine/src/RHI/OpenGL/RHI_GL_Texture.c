#include "RHI_GL_Internal.h"

static void
gl_texture_format_info(rhi_texture_format format,
                        GLenum* out_internal_format,
                        GLenum* out_upload_format,
                        GLenum* out_upload_type)
{
    switch (format)
    {
        case RHI_FORMAT_RGB8:
            *out_internal_format = GL_RGB8;
            *out_upload_format = GL_RGB;
            *out_upload_type = GL_UNSIGNED_BYTE;
            return;

        case RHI_FORMAT_RGBA8:
            *out_internal_format = GL_RGBA8;
            *out_upload_format = GL_RGBA;
            *out_upload_type = GL_UNSIGNED_BYTE;
            return;

        case RHI_FORMAT_RGBA8_SRGB:
            *out_internal_format = GL_SRGB8_ALPHA8;
            *out_upload_format = GL_RGBA;
            *out_upload_type = GL_UNSIGNED_BYTE;
            return;

        case RHI_FORMAT_RGBA16F:
            *out_internal_format = GL_RGBA16F;
            *out_upload_format = GL_RGBA;
            *out_upload_type = GL_FLOAT;
            return;

        case RHI_FORMAT_RG16F:
            *out_internal_format = GL_RG16F;
            *out_upload_format = GL_RG;
            *out_upload_type = GL_FLOAT;
            return;

        case RHI_FORMAT_DEPTH24:
            *out_internal_format = GL_DEPTH_COMPONENT24;
            *out_upload_format = GL_DEPTH_COMPONENT;
            *out_upload_type = GL_FLOAT;
            return;

        default:
            UNREACHABLE();
    }
}

static GLenum
gl_filter_to_gl(rhi_texture_filter filter, b8 has_mipmaps)
{
    if (filter == RHI_FILTER_NEAREST)
        return has_mipmaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;

    return has_mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
}

// Anisotropic filtering level for mipmapped textures: the driver's maximum,
// capped at 16x (the usual ceiling, and where returns stop being visible).
// Core since GL 4.6; queried once, on first use.
static GLfloat
gl_anisotropy_level(void)
{
    static GLfloat level = 0.0f;

    if (level == 0.0f)
    {
        GLfloat max_supported = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_supported);
        level = max_supported < 16.0f ? max_supported : 16.0f;
        if (level < 1.0f)
            level = 1.0f;
    }

    return level;
}

static GLenum
gl_wrap_to_gl(rhi_texture_wrap wrap)
{
    switch (wrap)
    {
        case RHI_WRAP_REPEAT:          return GL_REPEAT;
        case RHI_WRAP_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
        default:                       return GL_CLAMP_TO_EDGE;
    }
}

rhi_texture gl_texture_create(rhi_texture_desc desc)
{
    GLenum internal_format, upload_format, upload_type;
    gl_texture_format_info(desc.format, &internal_format, &upload_format, &upload_type);

    rhi_texture tex = malloc(sizeof(*tex));
    if (!tex)
        FATAL("Out of memory creating texture");

    tex->target = GL_TEXTURE_2D;
    tex->width = desc.width;
    tex->height = desc.height;
    tex->mip_count = 1;

    glGenTextures(1, &tex->handle);
    glBindTexture(GL_TEXTURE_2D, tex->handle);

    // GL's default GL_UNPACK_ALIGNMENT is 4, meaning it assumes each row of
    // the source data starts on a 4-byte boundary. RGB8 is 3 bytes/pixel, so
    // any width that isn't a multiple of 4 pixels (the common case for real
    // textures) breaks that assumption and GL reads each row shifted from
    // where it actually is, corrupting/skewing the image. Set it to 1
    // (tightly packed, no row padding) so uploads work for any dimensions.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // desc.pixels may be NULL — that's an empty texture (render-target
    // attachment); GL just reserves the storage and leaves it undefined
    // until something renders into it.
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_format,
                 desc.width, desc.height, 0,
                 upload_format, upload_type, desc.pixels);

    b8 mipmaps = desc.pixels && desc.generate_mipmaps;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)gl_filter_to_gl(desc.filter, mipmaps));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)gl_filter_to_gl(desc.filter, false));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)gl_wrap_to_gl(desc.wrap));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)gl_wrap_to_gl(desc.wrap));

    if (mipmaps)
    {
        glGenerateMipmap(GL_TEXTURE_2D);

        // Without this, trilinear filtering picks the mip level from the
        // longer screen-space axis of each pixel's footprint, so textures
        // on surfaces seen at a grazing angle (floors, walls) blur out.
        if (desc.filter == RHI_FILTER_LINEAR)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, gl_anisotropy_level());
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

void gl_texture_bind(rhi_texture texture, uint32 slot)
{
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(texture->target, texture->handle);
}

void gl_texture_destroy(rhi_texture texture)
{
    if (!texture)
        return;

    glDeleteTextures(1, &texture->handle);
    free(texture);
}

rhi_texture gl_cubemap_create(rhi_cubemap_desc desc)
{
    ASSERT(desc.size > 0);
    ASSERT(desc.format != RHI_FORMAT_DEPTH24);

    // Only the internal format matters here; nothing is uploaded.
    GLenum internal_format, upload_format, upload_type;
    gl_texture_format_info(desc.format, &internal_format, &upload_format, &upload_type);

    // A chain can't be longer than log2(size) + 1 levels (size -> ... -> 1).
    uint32 max_mips = 1;
    for (uint32 s = desc.size; s > 1; s >>= 1)
        ++max_mips;

    uint32 mip_count = desc.mip_count < 1 ? 1 : desc.mip_count;
    if (mip_count > max_mips)
        mip_count = max_mips;

    rhi_texture tex = malloc(sizeof(*tex));
    if (!tex)
        FATAL("Out of memory creating cubemap");

    tex->target = GL_TEXTURE_CUBE_MAP;
    tex->width = desc.size;
    tex->height = desc.size;
    tex->mip_count = mip_count;

    glGenTextures(1, &tex->handle);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex->handle);

    // Immutable storage for all six faces and every mip level, so any
    // (face, level) can be attached to a framebuffer and rendered into.
    glTexStorage2D(GL_TEXTURE_CUBE_MAP, (GLsizei)mip_count, internal_format, desc.size, desc.size);

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    mip_count > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, (GLint)(mip_count - 1));

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    return tex;
}

void gl_cubemap_generate_mipmaps(rhi_texture cubemap)
{
    ASSERT(cubemap->target == GL_TEXTURE_CUBE_MAP);

    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap->handle);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}
