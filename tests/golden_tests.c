// golden_tests.c
//
// Golden-image tests: renders each reference scene in tests/golden/scenes/
// with the Game's --check (a hidden window, the saved pose, 960 x 540) and
// compares the picture with the saved one in tests/golden/. Catches a
// rendering change nobody meant to make; a change someone did mean to make
// is accepted by saving new pictures:
//
//   GoldenTests --update          every case
//   GoldenTests --update avocado  only the cases named
//
// then looking at the new PNGs before committing them. A failed case leaves
// its render and a diff (differing pixels in red over a dimmed render) in
// GOLDEN_OUT_DIR.
//
// Needs a GPU with OpenGL 4.6, so it runs on a developer's machine, not on
// a headless build server. The saved pictures come from one GPU and driver;
// another may round differently, which the tolerances below absorb up to a
// point.

#include "Core/Defines.h"
#include "Core/FileIO.h"
#include "Core/Image.h"
#include "Core/Process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #define sleep_ms(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define sleep_ms(ms) usleep((ms) * 1000)
#endif

#if !defined(GAME_EXE) || !defined(GOLDEN_DIR) || !defined(GOLDEN_OUT_DIR) || !defined(GOLDEN_PROJECT_DIR)
#error "golden_tests.c needs GAME_EXE, GOLDEN_DIR, GOLDEN_OUT_DIR and GOLDEN_PROJECT_DIR (tests/CMakeLists.txt)"
#endif

// A pixel differs when one of its channels is off by more than this (of
// 255): driver rounding stays below it, a real change usually doesn't.
#define PIXEL_TOLERANCE 8

// A case fails when more than this fraction of its pixels differ: room for
// noise along shadow and silhouette edges, not for a changed surface. On
// the GPU that saved the pictures, renders match exactly; softening the
// shadow filter (SHADOW_FILTER_RADIUS 1.75 -> 3) changes 0.07% of the lit
// avocado, so this catches that much.
#define MAX_DIFFERING_FRACTION 0.0005

#define RENDER_TIMEOUT_MS 120000

typedef struct golden_case
{
    const char* name;    // tests/golden/<name>.png
    const char* scene;   // under tests/golden/scenes/
    const char* view;    // the Game's --view, or NULL for the lit scene
    const char* aa;      // the Game's --antialiasing, or NULL for its default (FXAA)
} golden_case;

// Each covers something the others don't.
static const golden_case CASES[] = {
    { "avocado",         "avocado.json",   NULL },        // textured glTF, cascaded shadows, IBL, point light
    { "avocado_normals", "avocado.json",   "normals" },   // normal mapping
    { "avocado_shadow",  "avocado.json",   "shadow" },    // the shadow term alone
    { "materials",       "materials.json", NULL },        // metallic x roughness grid
    { "bloom",           "bloom.json",     NULL },        // a glowing emissive sphere in the dark
    { "avocado_agx",     "avocado_agx.json",     NULL },  // AgX tone mapping
    { "avocado_neutral", "avocado_neutral.json", NULL },  // Khronos PBR Neutral tone mapping
    { "avocado_auto",    "avocado_auto.json",    NULL },  // auto-exposure on a bright scene
    { "dark_auto",       "dark_auto.json",       NULL },  // auto-exposure on a dark one
    { "avocado_taa",     "avocado.json",   NULL, "taa" }, // temporal anti-aliasing over the 8-frame jitter
    { "spot_shadow",     "spot_shadow.json",     NULL },  // two spot lights' shadows, two atlas tiles
    { "point_shadow",    "point_shadow.json",    NULL },  // a point light's shadows through five cube faces
};

#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))

// Runs the Game's --check on `c` into `png`. False (said why) if it didn't
// exit with 0.
static b8
render(const golden_case* c, const char* png)
{
    char scene[1024], log[1024];
    snprintf(scene, sizeof(scene), "%s/scenes/%s", GOLDEN_DIR, c->scene);
    snprintf(log, sizeof(log), "%s/%s.log", GOLDEN_OUT_DIR, c->name);
    remove(png);
    remove(log);

    const char* argv[12];
    int         argc = 0;
    argv[argc++] = GAME_EXE;
    argv[argc++] = "--project";
    argv[argc++] = GOLDEN_PROJECT_DIR;
    argv[argc++] = "--check";
    argv[argc++] = png;
    if (c->view)
    {
        argv[argc++] = "--view";
        argv[argc++] = c->view;
    }
    if (c->aa)
    {
        argv[argc++] = "--antialiasing";
        argv[argc++] = c->aa;
    }
    argv[argc++] = scene;
    argv[argc]   = NULL;

    process* p = process_spawn(argv, log);
    if (!p)
    {
        fprintf(stderr, "  couldn't start %s\n", GAME_EXE);
        return false;
    }

    int exit_code = -1;
    int waited    = 0;
    while (process_running(p, &exit_code) && waited < RENDER_TIMEOUT_MS)
    {
        sleep_ms(50);
        waited += 50;
    }
    process_destroy(p);   // ends it, if it timed out

    if (waited >= RENDER_TIMEOUT_MS)
    {
        fprintf(stderr, "  the game took over %d s; see %s\n", RENDER_TIMEOUT_MS / 1000, log);
        return false;
    }
    if (exit_code != 0)
    {
        fprintf(stderr, "  the game's check failed (exit code %d); see %s\n", exit_code, log);
        return false;
    }
    return true;
}

static b8
write_png(const char* path, const uint8* rgba, uint16 width, uint16 height)
{
    size_t size;
    uint8* png  = image_encode_png(rgba, width, height, 4, &size);
    FILE*  file = png ? fopen(path, "wb") : NULL;
    b8     ok   = file && fwrite(png, 1, size, file) == size;
    if (file)
        ok = fclose(file) == 0 && ok;
    free(png);
    return ok;
}

// Compares `actual` with `expected`; on a mismatch writes the diff picture
// to `diff_path`. True if they match within the tolerances.
static b8
compare(const image* actual, const image* expected, const char* diff_path)
{
    if (actual->width != expected->width || actual->height != expected->height)
    {
        fprintf(stderr, "  size %ux%u, saved picture %ux%u\n", actual->width, actual->height, expected->width,
                expected->height);
        return false;
    }

    size_t pixels    = (size_t)actual->width * actual->height;
    size_t differing = 0;
    int    worst     = 0;
    uint8* diff      = malloc(pixels * 4);
    if (!diff)
        FATAL("golden_tests: out of memory");

    for (size_t i = 0; i < pixels; ++i)
    {
        const uint8* a = actual->pixels + i * 4;
        const uint8* e = expected->pixels + i * 4;

        int largest = 0;
        for (int ch = 0; ch < 3; ++ch)
        {
            int d = abs((int)a[ch] - (int)e[ch]);
            largest = d > largest ? d : largest;
        }
        worst = largest > worst ? largest : worst;

        uint8* out = diff + i * 4;
        if (largest > PIXEL_TOLERANCE)
        {
            ++differing;
            out[0] = 255, out[1] = 0, out[2] = 0;
        }
        else
        {
            uint8 grey = (uint8)((a[0] + a[1] + a[2]) / 12);   // a dim render, for where things are
            out[0] = grey, out[1] = grey, out[2] = grey;
        }
        out[3] = 255;
    }

    double fraction = (double)differing / (double)pixels;
    b8     ok       = fraction <= MAX_DIFFERING_FRACTION;
    printf("  %zu of %zu pixels differ (%.3f%%, limit %.3f%%), largest difference %d\n", differing, pixels,
           fraction * 100.0, MAX_DIFFERING_FRACTION * 100.0, worst);

    if (!ok && write_png(diff_path, diff, actual->width, actual->height))
        fprintf(stderr, "  diff: %s\n", diff_path);

    free(diff);
    return ok;
}

static b8
named(const golden_case* c, int argc, char** argv, int first)
{
    if (first >= argc)
        return true;   // no names: every case
    for (int i = first; i < argc; ++i)
        if (strcmp(argv[i], c->name) == 0)
            return true;
    return false;
}

int main(int argc, char** argv)
{
    b8  update = argc > 1 && strcmp(argv[1], "--update") == 0;
    int first  = update ? 2 : 1;

    file_make_directories(GOLDEN_OUT_DIR);

    int failures = 0, ran = 0;
    for (size_t i = 0; i < CASE_COUNT; ++i)
    {
        const golden_case* c = &CASES[i];
        if (!named(c, argc, argv, first))
            continue;
        ++ran;
        printf("%s%s\n", c->name, update ? " (update)" : "");

        char actual_path[1024], expected_path[1024], diff_path[1024];
        snprintf(actual_path, sizeof(actual_path), "%s/%s.png", GOLDEN_OUT_DIR, c->name);
        snprintf(expected_path, sizeof(expected_path), "%s/%s.png", GOLDEN_DIR, c->name);
        snprintf(diff_path, sizeof(diff_path), "%s/%s.diff.png", GOLDEN_OUT_DIR, c->name);
        remove(diff_path);

        if (!render(c, actual_path))
        {
            ++failures;
            continue;
        }

        image actual = image_load(actual_path, 4);
        if (!actual.pixels)
        {
            fprintf(stderr, "  can't read the render %s\n", actual_path);
            ++failures;
            continue;
        }

        if (update)
        {
            if (write_png(expected_path, actual.pixels, actual.width, actual.height))
                printf("  saved %s\n", expected_path);
            else
            {
                fprintf(stderr, "  couldn't write %s\n", expected_path);
                ++failures;
            }
            image_free(&actual);
            continue;
        }

        if (!file_exists(expected_path))
        {
            fprintf(stderr, "  no saved picture %s: run GoldenTests --update %s, then look at it\n", expected_path,
                    c->name);
            ++failures;
            image_free(&actual);
            continue;
        }

        image expected = image_load(expected_path, 4);
        b8    ok       = expected.pixels && compare(&actual, &expected, diff_path);
        if (!expected.pixels)
            fprintf(stderr, "  can't read the saved picture %s\n", expected_path);
        if (!ok)
        {
            fprintf(stderr, "  FAILED -- the render is %s\n", actual_path);
            ++failures;
        }

        image_free(&actual);
        image_free(&expected);
    }

    if (ran == 0)
    {
        fprintf(stderr, "no case by those names\n");
        return 1;
    }
    printf("%d of %d case(s) %s\n", ran - failures, ran, update ? "saved" : "match");
    return failures ? 1 : 0;
}
