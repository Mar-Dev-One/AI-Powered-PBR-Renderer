#include "Image.h"

#define STB_IMAGE_IMPLEMENTATION
// stb_image logs its own errors via stbi_failure_reason(); we don't need
// the STBI_FAILURE_USERMSG variant, the default reason strings are enough.
#include "stb_image.h"

// PNG encoding to memory only (screenshots): no stdio file-writing code.
// Its unused HDR writer calls sprintf (bounded, harmless), which MSVC
// flags as deprecated; silence that for this one vendored header.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4996)
#endif
#include "stb_image_write.h"
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

#include <string.h>

image image_load(const char* path, uint8 desired_channels)
{
    image img = { 0 };

    // The RHI/GL side of this codebase treats row 0 as the bottom of the
    // image (see rhi_read_pixels' doc comment and gl_texture_create's
    // upload), matching OpenGL's own (0,0)-at-bottom-left texture
    // convention. stb_image decodes top-down by default, so flip on load
    // once here rather than flipping UVs everywhere a texture is sampled.
    stbi_set_flip_vertically_on_load(true);

    int width, height, source_channels;
    stbi_uc* pixels = stbi_load(path, &width, &height, &source_channels,
                                 (int)desired_channels);

    if (!pixels)
    {
        LOG_ERROR("Could not load image '%s': %s\n", path, stbi_failure_reason());
        return img;
    }

    img.pixels = pixels;
    img.width = (uint16)width;
    img.height = (uint16)height;
    img.channel_count = desired_channels ? desired_channels : (uint8)source_channels;

    return img;
}

image image_load_from_memory(const uint8* data, uint64 size, uint8 desired_channels)
{
    image img = { 0 };

    stbi_set_flip_vertically_on_load(true);

    int width, height, source_channels;
    stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)data, (int)size,
                                             &width, &height, &source_channels,
                                             (int)desired_channels);

    if (!pixels)
    {
        LOG_ERROR("Could not decode in-memory image (%llu bytes): %s\n",
                  (unsigned long long)size, stbi_failure_reason());
        return img;
    }

    img.pixels = pixels;
    img.width = (uint16)width;
    img.height = (uint16)height;
    img.channel_count = desired_channels ? desired_channels : (uint8)source_channels;

    return img;
}

void image_free(image* img)
{
    if (!img || !img->pixels)
        return;

    stbi_image_free(img->pixels);
    img->pixels = NULL;
}

image_hdr image_load_hdr(const char* path)
{
    image_hdr img = { 0 };

    stbi_set_flip_vertically_on_load(true);

    int width, height, source_channels;
    float* pixels = stbi_loadf(path, &width, &height, &source_channels, 4);

    if (!pixels)
    {
        LOG_ERROR("Could not load HDR image '%s': %s\n", path, stbi_failure_reason());
        return img;
    }

    if (width > 0xFFFF || height > 0xFFFF)
    {
        LOG_ERROR("HDR image '%s' is too large (%dx%d, max 65535 per side)\n", path, width, height);
        stbi_image_free(pixels);
        return img;
    }

    img.pixels = pixels;
    img.width = (uint16)width;
    img.height = (uint16)height;

    return img;
}

void image_hdr_free(image_hdr* img)
{
    if (!img || !img->pixels)
        return;

    stbi_image_free(img->pixels);
    img->pixels = NULL;
}

// stbi_write_png_to_func() hands the encoded bytes over in pieces.
typedef struct png_buffer
{
    uint8* data;
    size_t size;
    size_t capacity;
    b8     failed;
} png_buffer;

static void
append_png_bytes(void* context, void* data, int size)
{
    png_buffer* b = context;
    if (b->failed || size <= 0)
        return;

    if (b->size + (size_t)size > b->capacity)
    {
        size_t capacity = b->capacity ? b->capacity * 2 : 65536;
        while (capacity < b->size + (size_t)size)
            capacity *= 2;

        uint8* grown = realloc(b->data, capacity);
        if (!grown)
        {
            b->failed = true;
            return;
        }
        b->data     = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->size, data, (size_t)size);
    b->size += (size_t)size;
}

uint8* image_encode_png(const uint8* pixels, uint16 width, uint16 height, uint8 channel_count, size_t* out_size)
{
    png_buffer buffer = { 0 };

    // Bottom-up rows in, top-down PNG out.
    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png_to_func(append_png_bytes, &buffer, width, height, channel_count,
                                    pixels, (int)width * channel_count);
    stbi_flip_vertically_on_write(0);

    if (!ok || buffer.failed)
    {
        LOG_ERROR("Could not encode a %ux%u PNG\n", (unsigned)width, (unsigned)height);
        free(buffer.data);
        *out_size = 0;
        return NULL;
    }

    *out_size = buffer.size;
    return buffer.data;
}

void image_resize_box(const uint8* src, uint16 src_width, uint16 src_height, uint8 channel_count,
                      uint8* dst, uint16 dst_width, uint16 dst_height)
{
    for (uint32 y = 0; y < dst_height; ++y)
    {
        // Source rows [y0, y1) map onto destination row y.
        uint32 y0 = y * src_height / dst_height;
        uint32 y1 = (y + 1) * src_height / dst_height;
        if (y1 <= y0) y1 = y0 + 1;

        for (uint32 x = 0; x < dst_width; ++x)
        {
            uint32 x0 = x * src_width / dst_width;
            uint32 x1 = (x + 1) * src_width / dst_width;
            if (x1 <= x0) x1 = x0 + 1;

            uint32 count = (y1 - y0) * (x1 - x0);
            for (uint32 c = 0; c < channel_count; ++c)
            {
                uint32 sum = 0;
                for (uint32 sy = y0; sy < y1; ++sy)
                    for (uint32 sx = x0; sx < x1; ++sx)
                        sum += src[((size_t)sy * src_width + sx) * channel_count + c];

                dst[((size_t)y * dst_width + x) * channel_count + c] = (uint8)((sum + count / 2) / count);
            }
        }
    }
}
