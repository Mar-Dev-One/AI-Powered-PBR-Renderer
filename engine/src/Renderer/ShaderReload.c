#include "ShaderReload.h"

#include "../Core/Paths.h"

#include <string.h>
#include <sys/stat.h>

// A file's (under the engine root) modification time and size, or zeros if
// it can't be stat()ed.
static file_stamp
stamp_of(const char* relative_path)
{
    char*       path  = engine_path(relative_path);
    struct stat info;
    file_stamp  stamp = { 0, 0 };

    if (stat(path, &info) == 0)
    {
        stamp.mtime = (int64)info.st_mtime;
        stamp.size  = (int64)info.st_size;
    }

    free(path);
    return stamp;
}

void
file_watch_init(file_watch* watch, const char* const* paths, uint32 count)
{
    memset(watch, 0, sizeof(*watch));
    watch->count = count < FILE_WATCH_MAX ? count : FILE_WATCH_MAX;
    for (uint32 i = 0; i < watch->count; ++i)
    {
        watch->paths[i]  = paths[i];
        watch->stamps[i] = stamp_of(paths[i]);
    }
}

b8
file_watch_changed(file_watch* watch)
{
    b8 changed = false;
    for (uint32 i = 0; i < watch->count; ++i)
    {
        file_stamp now = stamp_of(watch->paths[i]);
        if (now.mtime != watch->stamps[i].mtime || now.size != watch->stamps[i].size)
        {
            // Recorded before any rebuild, so a broken edit is reported once
            // rather than every frame until the file changes again.
            watch->stamps[i] = now;
            changed          = true;
        }
    }
    return changed;
}

b8
shader_rebuild(rhi_shader* shader, const char* vertex, const char* fragment, const char* defines)
{
    char* vertex_path   = engine_path(vertex);
    char* fragment_path = engine_path(fragment);

    rhi_shader rebuilt = defines ? rhi_shader_create_from_files_with_defines(vertex_path, fragment_path, defines)
                                 : rhi_shader_create_from_files(vertex_path, fragment_path);
    free(vertex_path);
    free(fragment_path);

    if (!rebuilt)
    {
        LOG_ERROR("Shader hot reload: %s + %s failed to build, keeping the previous version\n", vertex, fragment);
        return false;
    }

    if (*shader)
        rhi_shader_destroy(*shader);
    *shader = rebuilt;

    LOG_INFO("Shader hot reload: rebuilt %s + %s\n", vertex, fragment);
    return true;
}
