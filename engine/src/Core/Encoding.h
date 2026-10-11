#pragma once

#include "Defines.h"

#include <stddef.h>

// Base64 (RFC 4648, standard alphabet, '=' padding): the WebSocket handshake
// and binary payloads in JSON, such as screenshots.

// Characters base64_encode() writes for `length` input bytes, not counting
// the terminating NUL.
size_t base64_encoded_length(size_t length);

// Encodes into `out`, which must hold base64_encoded_length(length) + 1 bytes.
void base64_encode(const void* data, size_t length, char* out);

// Same, into a new NUL-terminated buffer. Caller frees.
char* base64_encode_alloc(const void* data, size_t length);
