#pragma once

#include "Defines.h"

// Reads the entire file at `path` into a single heap-allocated,
// NUL-terminated buffer (text mode — fine for shader sources, configs,
// anything that isn't binary). Returns NULL on failure (missing file,
// read error) and logs why.
//
// Caller owns the returned buffer and must free() it.
char* file_read_to_string(const char* path);

// Called once per entry of a directory listing ("." and ".." skipped).
// `name` is just the entry name, not a full path.
typedef void (*file_visit_fn)(const char* name, b8 is_directory, void* user_data);

// Calls `visit` for every entry directly inside `directory` (not
// recursive), in no particular order. False if the directory can't be
// opened.
b8 file_list_directory(const char* directory, file_visit_fn visit, void* user_data);
