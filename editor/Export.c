// editor/Export.c
//
// The Export dialog's work: Project/Project.h project_export() with the Game
// executable (GAME_RELEASE_EXE when it's been built, else GAME_EXE) and,
// for a Release game, the C++ runtime DLLs CMake found (GAME_RUNTIME_DLLS);
// then the exported game's --check run, watched by watch_export_check().

#include "Editor.h"

#include "Core/FileIO.h"
#include "Core/Paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EXPORT_MAX_DLLS 16

static int64
modified_time(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (int64)info.st_mtime : 0;
}

// *out_release: it's a non-Debug build (the one for players). *out_stale
// (may be NULL): it's the separately built Release game, older than this
// build's own Game -- probably missing the latest engine changes.
const char*
game_executable(b8* out_release, b8* out_stale)
{
    if (out_stale)
        *out_stale = false;
    if (!GAME_EXE_IS_DEBUG)
    {
        *out_release = true;   // this build's own Game is the player one
        return GAME_EXE;
    }

    *out_release = file_exists(GAME_RELEASE_EXE);
    if (!*out_release)
        return GAME_EXE;
    if (out_stale)
        *out_stale = modified_time(GAME_RELEASE_EXE) < modified_time(GAME_EXE);
    return GAME_RELEASE_EXE;
}

// Splits GAME_RUNTIME_DLLS ('|'-separated) into `paths` (pointers into
// `buffer`); returns how many.
static uint32
runtime_dlls(char* buffer, size_t size, const char* paths[EXPORT_MAX_DLLS])
{
    snprintf(buffer, size, "%s", GAME_RUNTIME_DLLS);
    uint32 count = 0;
    for (char* item = buffer; item && *item && count < EXPORT_MAX_DLLS;)
    {
        char* bar = strchr(item, '|');
        if (bar)
            *bar++ = '\0';
        paths[count++] = item;
        item = bar;
    }
    return count;
}

void
default_export_location(const editor_state* state, char* out, size_t size)
{
    char projects[PROJECT_PATH_MAX];
    default_projects_location(projects, sizeof(projects));   // .../Documents/PBR Projects
    char* last = strrchr(projects, '/');
    if (last)
        *last = '\0';
    snprintf(out, size, "%s/PBR Exports/%s", projects, state->project.name);
}

// Forgets the last export -- its folder, status, check and picture, ending a
// check still running -- when the project it belonged to closes (and at
// shutdown), so the next export doesn't go to that project's folder.
void
reset_export(editor_state* state)
{
    process_destroy(state->check_process);
    state->check_process = NULL;
    if (state->check_picture)
        rhi_texture_destroy(state->check_picture);
    state->check_picture    = NULL;
    state->check            = CHECK_NONE;
    state->export_ok        = false;
    state->export_dir[0]    = '\0';
    state->export_status[0] = '\0';
}

b8
run_export(editor_state* state, const char* folder)
{
    if (state->check_process)
    {
        snprintf(state->export_status, sizeof(state->export_status), "The last export's check is still running.");
        return false;
    }
    if (state->check_picture)
    {
        rhi_texture_destroy(state->check_picture);
        state->check_picture = NULL;
    }
    state->check = CHECK_NONE;

    b8          release;
    const char* game = game_executable(&release, NULL);
    char        dll_buffer[4096];
    const char* dlls[EXPORT_MAX_DLLS];
    uint32      dll_count = release ? runtime_dlls(dll_buffer, sizeof(dll_buffer), dlls) : 0;

    project_export_options options = { game, paths_engine_root(), dlls, dll_count };
    char error[640];
    state->export_ok = project_export(paths_project_root(), folder, &options, &state->export_report, error, sizeof(error));
    if (!state->export_ok)
    {
        snprintf(state->export_status, sizeof(state->export_status), "Not exported: %s", error);
        return false;
    }

    snprintf(state->export_status, sizeof(state->export_status), "Exported %u files (%.1f MB), %u scene(s), to %s.",
             state->export_report.files, (double)state->export_report.bytes / (1024.0 * 1024.0),
             state->export_report.scenes, folder);
    console_info(&state->console, state->export_status);

    // The game checks itself: its startup scene, in a hidden window.
    char* png = user_data_path("export_check.png");
    snprintf(state->check_png, sizeof(state->check_png), "%s", png);
    free(png);
    remove(state->check_png);

    // A Release game writes log.txt beside itself; a Debug one logs to its
    // console, caught here instead.
    char* log = user_data_path("export_check.log");
    remove(log);
    const char* argv[] = { state->export_report.executable, "--check", state->check_png, NULL };
    state->check_process = process_spawn(argv, log);
    free(log);
    state->check           = state->check_process ? CHECK_RUNNING : CHECK_FAILED;
    state->check_exit_code = -1;
    return true;
}

void
watch_export_check(editor_state* state)
{
    int exit_code;
    if (!state->check_process || process_running(state->check_process, &exit_code))
        return;

    process_destroy(state->check_process);
    state->check_process   = NULL;
    state->check           = exit_code == 0 ? CHECK_PASSED : CHECK_FAILED;
    state->check_exit_code = exit_code;
    if (exit_code == 0 && file_exists(state->check_png))
        state->check_picture = rhi_texture_create_from_file(state->check_png, RHI_FILTER_LINEAR, RHI_WRAP_CLAMP_TO_EDGE, false);

    console_info(&state->console, exit_code == 0 ? "Export check passed: the game loads and renders its startup scene."
                                                 : "Export check FAILED: see log.txt in the export folder "
                                                   "(~/.pbr-renderer/export_check.log for a Debug game).");
}

void
open_folder(const char* folder)
{
#ifdef _WIN32
    char path[PROJECT_PATH_MAX];
    snprintf(path, sizeof(path), "%s", folder);
    for (char* c = path; *c; ++c)
        if (*c == '/')
            *c = '\\';
    const char* argv[] = { "C:\\Windows\\explorer.exe", path, NULL };
#elif defined(__APPLE__)
    const char* argv[] = { "/usr/bin/open", folder, NULL };
#else
    const char* argv[] = { "/usr/bin/xdg-open", folder, NULL };
#endif
    // Left to run on its own: process_destroy() would end it. Its small
    // handle is never freed (one per click).
    (void)process_spawn(argv, NULL);
}
