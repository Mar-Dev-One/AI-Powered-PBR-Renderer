#include "LiveScene.h"

#include "../Core/FileIO.h"
#include "Serialize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

void
live_scene_default_path(char* out, size_t size)
{
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    snprintf(out, size, "%s/.pbr-renderer/live_scene.json", home ? home : ".");
}

static void
set_path(char* out, const char* path)
{
    if (path)
        snprintf(out, LIVE_SCENE_PATH_MAX, "%s", path);
    else
        live_scene_default_path(out, LIVE_SCENE_PATH_MAX);
}

// Replaces `path` with `text` in one step: readers see the old file or the
// new one, never a partial write.
static b8
write_atomically(const char* path, const char* text)
{
    char temp[LIVE_SCENE_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (!file_write_string(temp, text))
        return false;

#ifdef _WIN32
    // rename() won't replace an existing file on Windows.
    return MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(temp, path) == 0;
#endif
}

static char*
duplicate(const char* text)
{
    size_t length = strlen(text) + 1;
    char*  copy   = malloc(length);
    if (!copy)
        FATAL("live scene: out of memory");
    memcpy(copy, text, length);
    return copy;
}

// --- Publisher -------------------------------------------------------------------

void
live_publisher_init(live_publisher* p, const char* path)
{
    memset(p, 0, sizeof(*p));
    set_path(p->path, path);
    p->last_check = -1.0e9;

    // The folder (one level is enough), as the command server does for server.json.
    char folder[LIVE_SCENE_PATH_MAX];
    snprintf(folder, sizeof(folder), "%s", p->path);
    char* slash = strrchr(folder, '/');
    char* back  = strrchr(folder, '\\');
    if (back > slash)
        slash = back;
    if (slash)
    {
        *slash = '\0';
        file_make_directory(folder);
    }
}

b8
live_publisher_publish(live_publisher* p, const world* w, const scene_environment* env, f64 now, f64 interval)
{
    if (interval > 0.0 && now - p->last_check < interval)
        return false;
    p->last_check = now;

    cJSON* scene = serialize_world(w);
    cJSON_AddStringToObject(scene, "format", SCENE_FORMAT);
    cJSON_AddNumberToObject(scene, "version", SCENE_VERSION);
    if (env)
        cJSON_AddItemToObject(scene, "environment", serialize_environment(env));

    char* text = cJSON_PrintUnformatted(scene);
    cJSON_Delete(scene);

    b8 written = false;
    if (!p->last_text || strcmp(p->last_text, text) != 0)
    {
        written = write_atomically(p->path, text);
        if (written)
        {
            free(p->last_text);
            p->last_text = duplicate(text);
        }
        else
        {
            LOG_WARN("live scene: could not write %s\n", p->path);
        }
    }

    cJSON_free(text);
    return written;
}

void
live_publisher_shutdown(live_publisher* p)
{
    free(p->last_text);
    p->last_text = NULL;
}

// --- Watcher ----------------------------------------------------------------------

void
live_watcher_init(live_watcher* w, const char* path)
{
    memset(w, 0, sizeof(*w));
    set_path(w->path, path);
    w->last_check = -1.0e9;
}

cJSON*
live_watcher_poll(live_watcher* w, f64 now, f64 interval)
{
    if (interval > 0.0 && now - w->last_check < interval)
        return NULL;
    w->last_check = now;

    // Compared by content, not mtime: mtime has one-second resolution, and a
    // dragged slider rewrites the file many times a second.
    char* text = file_exists(w->path) ? file_read_to_string(w->path) : NULL;
    if (!text)
        return NULL;

    if (w->last_text && strcmp(w->last_text, text) == 0)
    {
        free(text);
        return NULL;
    }

    cJSON* scene = cJSON_Parse(text);
    if (!scene)
    {
        free(text);   // not valid JSON (shouldn't happen with atomic writes): try again next time
        return NULL;
    }

    free(w->last_text);
    w->last_text = text;
    return scene;
}

void
live_watcher_shutdown(live_watcher* w)
{
    free(w->last_text);
    w->last_text = NULL;
}
