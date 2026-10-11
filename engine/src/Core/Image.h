#pragma once

#include "Defines.h"

// Decoded image data, always top-left origin as loaded from disk (see
// image_load's flip note). channel_count is whatever the source file
// actually has (1 = grey, 3 = RGB, 4 = RGBA) unless a specific channel
// count is requested — see image_load.
typedef struct image
{
    uint8* pixels;   // channel_count bytes per pixel, row-major, no padding
    uint16 width;
    uint16 height;
    uint8  channel_count;
} image;

// Loads and decodes an image file (PNG, JPG, BMP, TGA, ... — anything
// stb_image supports) into 8-bit-per-channel pixel data.
//
// `desired_channels` forces the decoded output to that many channels
// (e.g. pass 4 to always get RGBA regardless of the source file), or
// pass 0 to keep whatever the file itself has.
//
// Returns an image with pixels == NULL on failure (missing file, unknown
// format, decode error) and logs why. Caller owns image.pixels on success
// and must free it with image_free().
image image_load(const char* path, uint8 desired_channels);

// Same as image_load(), but decodes an image that's already in memory
// (an embedded glTF/GLB texture, a network download, ...). `data` is the
// still-encoded file contents (PNG/JPG bytes), not raw pixels. Same flip,
// channel and failure behaviour as image_load().
image image_load_from_memory(const uint8* data, uint64 size, uint8 desired_channels);

void  image_free(image* img);

// Encodes 8-bit pixels as a PNG file in memory. Rows are in this codebase's
// bottom-up order (row 0 = bottom, as image_load() and rhi_read_pixels()
// produce them); the PNG comes out the right way up. Returns the encoded
// bytes (caller frees) and their count in *out_size, or NULL on failure.
uint8* image_encode_png(const uint8* pixels, uint16 width, uint16 height, uint8 channel_count, size_t* out_size);

// Shrinks (or copies, if the sizes match) `src` into `dst` by averaging the
// block of source pixels each destination pixel covers -- a box filter,
// which keeps thin details from flickering away the way skipping pixels
// would. Works for any channel count; row order is preserved.
void image_resize_box(const uint8* src, uint16 src_width, uint16 src_height, uint8 channel_count,
                      uint8* dst, uint16 dst_width, uint16 dst_height);

// Decoded floating-point (HDR) image: always 4 channels (RGBA, linear
// radiance, alpha = 1), 32-bit float per channel. Used for environment maps,
// where values well above 1.0 (the sun, bright windows) matter.
typedef struct image_hdr
{
    f32*   pixels;   // 4 floats per pixel, row-major, no padding
    uint16 width;
    uint16 height;
} image_hdr;

// Loads a Radiance .hdr file (or any other format stb_image supports, which
// is converted to linear float). Same bottom-left-origin flip as image_load(),
// so row 0 is the bottom of the picture.
//
// Returns an image with pixels == NULL on failure and logs why. Caller owns
// image.pixels on success and must free it with image_hdr_free().
image_hdr image_load_hdr(const char* path);

void image_hdr_free(image_hdr* img);
