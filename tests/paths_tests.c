// paths_tests.c
//
// Headless tests for Core/Paths.h: the engine root (an engine/ folder next to
// the executable beats the source tree), the project root (the source tree's
// assets/ unless set) and the path helpers. Leaves nothing behind.

#include "Core/FileIO.h"
#include "Core/Paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <direct.h>
    #define remove_directory _rmdir
#else
    #include <unistd.h>
    #define remove_directory rmdir
#endif

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

static b8
ends_with(const char* text, const char* suffix)
{
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

// An exported game's layout -- <exe dir>/engine/shaders/pbr.vert -- is
// preferred over the source tree. Must run before anything else asks for
// the engine root (it's looked up once).
static void
test_engine_root_next_to_executable(void)
{
    const char* exe_dir = paths_executable_dir();
    CHECK(exe_dir[0] != '\0');
    CHECK(strchr(exe_dir, '\\') == NULL);

    char engine_dir[1024], shaders_dir[1024], probe[1024];
    snprintf(engine_dir, sizeof(engine_dir), "%s/engine", exe_dir);
    snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", engine_dir);
    snprintf(probe, sizeof(probe), "%s/pbr.vert", shaders_dir);

    b8 made = file_make_directory(engine_dir) && file_make_directory(shaders_dir) && file_write_string(probe, "");
    CHECK(made);

    if (made)
    {
        CHECK(strcmp(paths_engine_root(), engine_dir) == 0);

        char* shader = engine_path("shaders/pbr.vert");
        CHECK(strcmp(shader, probe) == 0);
        free(shader);
    }

    remove(probe);
    remove_directory(shaders_dir);
    remove_directory(engine_dir);
}

// With no --project, the source tree's assets/, which has the sample scenes.
static void
test_default_project_root(void)
{
    const char* root = paths_project_root();
    CHECK(ends_with(root, "/assets"));

    char* scene = project_path("scenes/orrery_plaza.json");
    CHECK(file_exists(scene));
    free(scene);
}

static void
test_set_project_root(void)
{
    char* before = project_path("x");

    // Not a folder: refused, root unchanged.
    CHECK(!paths_set_project_root(NULL));
    CHECK(!paths_set_project_root(""));
    CHECK(!paths_set_project_root(PATHS_TEST_DIR "/does_not_exist"));
    char* after = project_path("x");
    CHECK(strcmp(before, after) == 0);
    free(after);

    // A real folder, given with Windows separators and a trailing slash,
    // comes back absolute with '/' and no trailing separator.
    CHECK(file_make_directory(PATHS_TEST_DIR));
    char messy[1024];
    snprintf(messy, sizeof(messy), "%s/", PATHS_TEST_DIR);
    for (char* c = messy; *c; ++c)
        if (*c == '/')
            *c = '\\';
#ifndef _WIN32
    snprintf(messy, sizeof(messy), "%s/", PATHS_TEST_DIR);   // '\' is a file name character elsewhere
#endif
    CHECK(paths_set_project_root(messy));

    const char* root = paths_project_root();
    CHECK(strchr(root, '\\') == NULL);
    CHECK(!ends_with(root, "/"));
    CHECK(ends_with(root, "/paths_project"));

    char* scene = project_path("scenes/a.json");
    char  expected[1100];
    snprintf(expected, sizeof(expected), "%s/scenes/a.json", root);
    CHECK(strcmp(scene, expected) == 0);
    free(scene);

    // The engine root doesn't follow the project.
    char* shader = engine_path("shaders/pbr.vert");
    CHECK(!ends_with(paths_engine_root(), "/paths_project"));
    free(shader);

    remove_directory(PATHS_TEST_DIR);
    free(before);
}

int
main(void)
{
    test_engine_root_next_to_executable();
    test_default_project_root();
    test_set_project_root();

    printf("paths_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
