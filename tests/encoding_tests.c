// encoding_tests.c
//
// Headless tests for Core/Encoding.h (base64) and the screenshot helpers in
// Core/Image.h: PNG encoding (checked by decoding it again) and the box
// filter used to shrink captures.

#include "Core/Encoding.h"
#include "Core/Image.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++g_checks;                                                             \
        if (!(condition))                                                       \
        {                                                                       \
            ++g_failures;                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

static void
test_base64(void)
{
    // RFC 4648 test vectors.
    static const char* const INPUT[]  = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char* const OUTPUT[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };

    for (int i = 0; i < 7; ++i)
    {
        char* encoded = base64_encode_alloc(INPUT[i], strlen(INPUT[i]));
        CHECK(strcmp(encoded, OUTPUT[i]) == 0);
        CHECK(strlen(encoded) == base64_encoded_length(strlen(INPUT[i])));
        free(encoded);
    }

    // Every byte value survives (the high ones included).
    unsigned char bytes[256];
    for (int i = 0; i < 256; ++i)
        bytes[i] = (unsigned char)i;
    char* encoded = base64_encode_alloc(bytes, sizeof(bytes));
    CHECK(strncmp(encoded, "AAECAwQF", 8) == 0);
    CHECK(strcmp(encoded + strlen(encoded) - 8, "/P3+/w==") == 0);   // 256 = 85 * 3 + 1: one padded byte
    free(encoded);
}

static void
test_png_round_trip(void)
{
    // A 5 x 3 RGB image where every pixel is distinct, rows bottom-up.
    enum { W = 5, H = 3 };
    uint8 pixels[W * H * 3];
    for (int i = 0; i < W * H * 3; ++i)
        pixels[i] = (uint8)(i * 17 + 3);

    size_t size;
    uint8* png = image_encode_png(pixels, W, H, 3, &size);
    CHECK(png != NULL && size > 8);
    CHECK(png && memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0);

    // image_load_from_memory() also uses bottom-up rows, so the pixels
    // must come back exactly as they went in.
    image decoded = image_load_from_memory(png, size, 3);
    CHECK(decoded.pixels && decoded.width == W && decoded.height == H && decoded.channel_count == 3);
    CHECK(decoded.pixels && memcmp(decoded.pixels, pixels, sizeof(pixels)) == 0);

    image_free(&decoded);
    free(png);
}

static void
test_resize_box(void)
{
    // 4 x 2 single-channel -> 2 x 1: each output pixel averages a 2 x 2 block.
    const uint8 src[8] = { 0, 10, 100, 200,
                           20, 30, 100, 0 };
    uint8 dst[2];
    image_resize_box(src, 4, 2, 1, dst, 2, 1);
    CHECK(dst[0] == 15);    // (0 + 10 + 20 + 30) / 4
    CHECK(dst[1] == 100);   // (100 + 200 + 100 + 0) / 4

    // Same size is a copy.
    uint8 copy[8];
    image_resize_box(src, 4, 2, 1, copy, 4, 2);
    CHECK(memcmp(copy, src, sizeof(src)) == 0);

    // Channels stay separate, and rounding is to nearest.
    const uint8 rgb[6] = { 255, 0, 1, 0, 255, 2 };   // two pixels
    uint8 one[3];
    image_resize_box(rgb, 2, 1, 3, one, 1, 1);
    CHECK(one[0] == 128 && one[1] == 128 && one[2] == 2);   // 127.5 -> 128, 1.5 -> 2

    // Uneven shrink (5 -> 2) still covers every source pixel.
    const uint8 row[5] = { 10, 10, 10, 50, 50 };
    uint8 two[2];
    image_resize_box(row, 5, 1, 1, two, 2, 1);
    CHECK(two[0] == 10 && two[1] == 37);   // [0,2) and [2,5): (10 + 50 + 50) / 3
}

int
main(void)
{
    test_base64();
    test_png_round_trip();
    test_resize_box();

    printf("encoding_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
