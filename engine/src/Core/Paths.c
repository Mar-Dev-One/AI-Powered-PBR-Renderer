#include "Paths.h"
#include "FileIO.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <limits.h>
#else
    #include <limits.h>
    #include <unistd.h>
#endif

#define PATHS_MAX 1024

// What engine_path() looks for to accept a candidate engine root.
#define ENGINE_PROBE "shaders/pbr.vert"

static char g_executable_dir[PATHS_MAX];
static char g_engine_root[PATHS_MAX];
static char g_project_root[PATHS_MAX];
static b8   g_executable_found;

// Backslashes to '/', trailing separators dropped (a bare "/" stays).
static void
normalize(char* path)
{
    for (char* c = path; *c; ++c)
    {
        if (*c == '\\')
            *c = '/';
    }

    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
        path[--length] = '\0';
}

static char*
join(const char* root, const char* relative_path)
{
    size_t total = strlen(root) + 1 + strlen(relative_path) + 1;
    char*  path  = malloc(total);
    if (!path)
        FATAL("Out of memory building a path for: %s", relative_path);

    snprintf(path, total, "%s/%s", root, relative_path);
    return path;
}

static b8
is_directory(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

// `path` made absolute, into `out`. False if the platform call fails.
static b8
absolute_path(const char* path, char out[PATHS_MAX])
{
#ifdef _WIN32
    return _fullpath(out, path, PATHS_MAX) != NULL;
#else
    char resolved[PATH_MAX];
    if (!realpath(path, resolved) || strlen(resolved) >= PATHS_MAX)
        return false;
    memcpy(out, resolved, strlen(resolved) + 1);
    return true;
#endif
}

const char*
paths_executable_dir(void)
{
    if (g_executable_found)
        return g_executable_dir;
    g_executable_found = true;

    char path[PATHS_MAX] = { 0 };
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, path, PATHS_MAX);
    if (length == 0 || length >= PATHS_MAX)
        return g_executable_dir;
#elif defined(__APPLE__)
    uint32_t size = PATHS_MAX;
    if (_NSGetExecutablePath(path, &size) != 0)
        return g_executable_dir;
#else
    ssize_t length = readlink("/proc/self/exe", path, PATHS_MAX - 1);
    if (length <= 0)
        return g_executable_dir;
    path[length] = '\0';
#endif

    normalize(path);
    char* slash = strrchr(path, '/');
    if (slash)
    {
        *slash = '\0';
        memcpy(g_executable_dir, path, strlen(path) + 1);
    }
    return g_executable_dir;
}

const char*
paths_engine_root(void)
{
    if (g_engine_root[0])
        return g_engine_root;

    // Next to the executable first: an exported game carries its own
    // shaders, which must win over a source tree that happens to exist.
    const char* exe_dir = paths_executable_dir();
    if (exe_dir[0])
    {
        char candidate[PATHS_MAX];
        snprintf(candidate, sizeof(candidate), "%s/engine", exe_dir);

        char* probe = join(candidate, ENGINE_PROBE);
        b8    found = file_exists(probe);
        free(probe);

        if (found)
        {
            memcpy(g_engine_root, candidate, strlen(candidate) + 1);
            return g_engine_root;
        }
    }

    snprintf(g_engine_root, sizeof(g_engine_root), "%s", DEV_ENGINE_ROOT);
    normalize(g_engine_root);
    return g_engine_root;
}

const char*
paths_project_root(void)
{
    if (!g_project_root[0])
    {
        snprintf(g_project_root, sizeof(g_project_root), "%s", DEV_PROJECT_ROOT);
        normalize(g_project_root);
    }
    return g_project_root;
}

b8
paths_set_project_root(const char* directory)
{
    char absolute[PATHS_MAX];
    if (!directory || !directory[0] || !is_directory(directory) || !absolute_path(directory, absolute))
        return false;

    normalize(absolute);
    memcpy(g_project_root, absolute, strlen(absolute) + 1);
    return true;
}

char*
engine_path(const char* relative_path)
{
    return join(paths_engine_root(), relative_path);
}

char*
project_path(const char* relative_path)
{
    return join(paths_project_root(), relative_path);
}

char*
user_data_path(const char* relative_path)
{
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    char folder[PATHS_MAX];
    snprintf(folder, sizeof(folder), "%s/.pbr-renderer", home && home[0] ? home : ".");
    normalize(folder);
    file_make_directory(folder);
    return join(folder, relative_path);
}
