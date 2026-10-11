#include "Encoding.h"

size_t
base64_encoded_length(size_t length)
{
    return (length + 2) / 3 * 4;
}

void
base64_encode(const void* data, size_t length, char* out)
{
    static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const unsigned char* bytes = data;
    size_t               o     = 0;

    for (size_t i = 0; i < length; i += 3)
    {
        uint32 v = (uint32)bytes[i] << 16;
        if (i + 1 < length) v |= (uint32)bytes[i + 1] << 8;
        if (i + 2 < length) v |= bytes[i + 2];

        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = i + 1 < length ? ALPHABET[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < length ? ALPHABET[v & 63] : '=';
    }
    out[o] = '\0';
}

char*
base64_encode_alloc(const void* data, size_t length)
{
    char* out = malloc(base64_encoded_length(length) + 1);
    if (!out)
        FATAL("base64: out of memory");
    base64_encode(data, length, out);
    return out;
}
