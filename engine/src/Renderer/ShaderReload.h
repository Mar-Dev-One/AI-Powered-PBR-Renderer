#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"

// Hot reload for the engine's shaders: notice that shader files under the
// engine root changed on disk (file_watch), and rebuild a program while
// keeping the old one if the new source doesn't compile (shader_rebuild).
// SceneRenderer and the IBL cache use these for every shader they own
// (scene_renderer_poll_hot_reload(), ibl_cache_poll_hot_reload()).

#define FILE_WATCH_MAX 8

// What a watch compares to notice a file changed: the size as well as the
// mtime, because mtime only has one-second resolution and many editors save
// by truncating then rewriting within the same second.
typedef struct file_stamp
{
    int64 mtime;
    int64 size;
} file_stamp;

typedef struct file_watch
{
    const char* paths[FILE_WATCH_MAX];    // under the engine root, e.g. "shaders/pbr.frag"; string literals
    file_stamp  stamps[FILE_WATCH_MAX];   // as of the last init / change
    uint32      count;
} file_watch;

// Watches `count` (<= FILE_WATCH_MAX) files as they are now.
void file_watch_init(file_watch* watch, const char* const* paths, uint32 count);

// True once per change: when any watched file differs from what the watch
// last saw (two stat() calls per file, cheap enough for every frame).
b8 file_watch_changed(file_watch* watch);

// Builds the program from `vertex` + `fragment` (under the engine root, with
// `defines` or NULL, see rhi_shader_desc) and, if it compiles and links,
// destroys *shader and puts the new one there. On failure *shader is left as
// it was and the error is logged. True if rebuilt.
b8 shader_rebuild(rhi_shader* shader, const char* vertex, const char* fragment, const char* defines);
