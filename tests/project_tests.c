// project_tests.c
//
// Headless tests for Project/Project.h: names, creating a project (and that
// its starter scene loads), project.json round trips, the recent-projects
// list, and importing assets. Works in a scratch folder under the build
// tree and removes it afterwards.

#include "Core/FileIO.h"
#include "Project/Project.h"
#include "World/Serialize.h"
#include "World/World.h"

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

#define ROOT PROJECT_TEST_DIR   // set by CMake: a scratch folder in the build tree

// --- Scratch files ----------------------------------------------------------------

static void remove_tree(const char* path);

static void
remove_entry(const char* name, b8 is_directory, void* user_data)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", (const char*)user_data, name);
    if (is_directory)
        remove_tree(path);
    else
        remove(path);
}

static void
remove_tree(const char* path)
{
    file_list_directory(path, remove_entry, (void*)path);
    remove_directory(path);
}

static void
write(const char* path, const char* text)
{
    char folder[1024];
    snprintf(folder, sizeof(folder), "%s", path);
    *strrchr(folder, '/') = '\0';
    file_make_directories(folder);
    file_write_string(path, text);
}

static b8
exists(const char* format, const char* arg)
{
    char path[1024];
    snprintf(path, sizeof(path), format, arg);
    return file_exists(path);
}

// --- Tests ------------------------------------------------------------------------

static void
test_names(void)
{
    CHECK(project_name_valid("Racing Game"));
    CHECK(project_name_valid("v2-final_(copy)"));
    CHECK(!project_name_valid(""));
    CHECK(!project_name_valid(NULL));
    CHECK(!project_name_valid("a/b"));
    CHECK(!project_name_valid("a:b"));
    CHECK(!project_name_valid(".."));
    CHECK(!project_name_valid(" padded"));
    CHECK(!project_name_valid("trailing."));

    CHECK(project_scene_name_valid("main"));
    CHECK(project_scene_name_valid("level_2-b"));
    CHECK(!project_scene_name_valid("has space"));
    CHECK(!project_scene_name_valid("../escape"));
    CHECK(!project_scene_name_valid(""));
}

// A new project: its folders, settings and a starter scene that loads.
static void
test_create(asset_cache* assets)
{
    char error[512] = "";
    const char* dir = ROOT "/Racer";

    CHECK(project_create(dir, "Racer", error, sizeof(error)));
    CHECK(error[0] == '\0');
    CHECK(project_exists(dir));
    CHECK(exists("%s/scenes/main.json", dir));
    char folder[1024];
    static const char* const FOLDERS[] = { "scenes", "models", "hdri", "textures" };
    for (int i = 0; i < 4; ++i)
    {
        snprintf(folder, sizeof(folder), "%s/%s", dir, FOLDERS[i]);
        CHECK(file_is_directory(folder));
    }

    project_settings s;
    CHECK(project_read(dir, &s, error, sizeof(error)));
    CHECK(strcmp(s.name, "Racer") == 0);
    CHECK(strcmp(s.startup_scene, "main") == 0);
    CHECK(s.window_width == 1920 && s.window_height == 1080);
    CHECK(s.default_hdri[0] == '\0');

    // The starter scene, through the same loader scene.load uses. Built-in
    // shapes need a GPU, so bounds-only stand-ins are registered under
    // their keys first.
    model unit = { 0 };
    glm_vec3_fill(unit.bounds_min, -0.5f);
    glm_vec3_fill(unit.bounds_max, 0.5f);
    assets_add_model(assets, "builtin:plane", unit);
    assets_add_model(assets, "builtin:cube", unit);

    char* text = file_read_to_string(ROOT "/Racer/scenes/main.json");
    cJSON* scene = cJSON_Parse(text);
    free(text);

    world w;
    world_init(&w, assets);
    CHECK(deserialize_scene(&w, scene, NULL, NULL, error, sizeof(error)));
    CHECK(world_count(&w) == 4);
    CHECK(world_has(&w, world_find(&w, "ground"), COMPONENT_MESH_RENDERER));
    CHECK(world_has(&w, world_find(&w, "sun"), COMPONENT_LIGHT));
    CHECK(world_has(&w, world_find(&w, "cube"), COMPONENT_MESH_RENDERER));
    CHECK(world_has(&w, world_find(&w, "camera"), COMPONENT_CAMERA));
    CHECK(world_light(&w, world_find(&w, "sun"))->type == LIGHT_DIRECTIONAL);

    scene_environment env = scene_environment_default();
    CHECK(deserialize_environment(cJSON_GetObjectItem(scene, "environment"), &env, error, sizeof(error)));
    CHECK(env.hdri[0] == '\0');
    cJSON_Delete(scene);
    world_shutdown(&w);

    // Not into a folder that already has things in it, nor with a bad name.
    CHECK(!project_create(dir, "Again", error, sizeof(error)));
    CHECK(strstr(error, "isn't empty") != NULL);
    CHECK(!project_create(ROOT "/Bad", "no/slashes", error, sizeof(error)));

    // An existing empty folder is fine.
    file_make_directories(ROOT "/Empty");
    CHECK(project_create(ROOT "/Empty", "Empty", error, sizeof(error)));
}

static void
test_settings_round_trip(void)
{
    char error[512];
    const char* dir = ROOT "/Settings";
    CHECK(project_create(dir, "Settings", error, sizeof(error)));

    project_settings s = project_settings_default("Renamed");
    snprintf(s.startup_scene, sizeof(s.startup_scene), "level_1");
    snprintf(s.default_hdri, sizeof(s.default_hdri), "hdri/sky.hdr");
    s.window_width  = 1280;
    s.window_height = 720;
    CHECK(project_write(dir, &s, error, sizeof(error)));

    project_settings back;
    CHECK(project_read(dir, &back, error, sizeof(error)));
    CHECK(strcmp(back.name, "Renamed") == 0);
    CHECK(strcmp(back.startup_scene, "level_1") == 0);
    CHECK(strcmp(back.default_hdri, "hdri/sky.hdr") == 0);
    CHECK(back.window_width == 1280 && back.window_height == 720);

    // Default input isn't written; changed input is, and comes back.
    CHECK(!back.input.custom && back.input.count == s.input.count);
    char* text = file_read_to_string(ROOT "/Settings/project.json");
    CHECK(text && !strstr(text, "\"input\""));
    free(text);

    s.input.custom = true;
    input_action_map_remove(&s.input, input_action_map_find(&s.input, "jump"));
    int fire = input_action_map_add(&s.input, "fire", INPUT_ACTION_BUTTON);
    CHECK(input_action_map_bind(&s.input, fire, input_binding_mouse_button(INPUT_MOUSE_LEFT), NULL, 0));
    CHECK(project_write(dir, &s, error, sizeof(error)));
    CHECK(project_read(dir, &back, error, sizeof(error)));
    CHECK(back.input.custom && input_action_map_find(&back.input, "jump") < 0);
    int fire_back = input_action_map_find(&back.input, "fire");
    CHECK(fire_back >= 0 && back.input.actions[fire_back].binding_count == 1);

    // A broken "input" section makes the project unreadable, saying where.
    write(ROOT "/BadInput/project.json",
          "{\"format\": \"pbr-project\", \"input\": {\"actions\": {\"fire\": {\"type\": \"button\", \"bindings\": [\"key:nope\"]}}}}");
    CHECK(!project_read(ROOT "/BadInput", &back, error, sizeof(error)));
    CHECK(strstr(error, "input") && strstr(error, "fire"));

    // Bad values are refused, and the file keeps what it had.
    project_settings bad = s;
    snprintf(bad.default_hdri, sizeof(bad.default_hdri), "../outside.hdr");
    CHECK(!project_write(dir, &bad, error, sizeof(error)));
    bad = s;
    snprintf(bad.startup_scene, sizeof(bad.startup_scene), "no spaces");
    CHECK(!project_write(dir, &bad, error, sizeof(error)));
    CHECK(project_read(dir, &back, error, sizeof(error)) && strcmp(back.startup_scene, "level_1") == 0);

    // A folder without project.json, and a project.json that isn't one.
    CHECK(!project_read(ROOT, &back, error, sizeof(error)));
    CHECK(strstr(error, "isn't a project") != NULL);
    write(ROOT "/NotAProject/project.json", "{\"name\": \"x\"}");
    CHECK(!project_read(ROOT "/NotAProject", &back, error, sizeof(error)));
}

static void
test_recent(void)
{
    const char* file = ROOT "/recent.json";
    char        error[512];
    char        dirs[12][256];
    for (int i = 0; i < 12; ++i)
    {
        char name[16];
        snprintf(name, sizeof(name), "R%d", i);
        snprintf(dirs[i], sizeof(dirs[i]), ROOT "/recent/%s", name);
        CHECK(project_create(dirs[i], name, error, sizeof(error)));
    }

    project_recent list;
    project_recent_load(&list, file);
    CHECK(list.count == 0);   // no file yet

    project_recent_add(&list, dirs[0], file);
    project_recent_add(&list, dirs[1], file);
    project_recent_add(&list, dirs[0], file);   // again: back to the front, not twice
    CHECK(list.count == 2);
    CHECK(strcmp(list.paths[0], dirs[0]) == 0 && strcmp(list.paths[1], dirs[1]) == 0);

    // Saved, and read back in the same order.
    project_recent loaded;
    project_recent_load(&loaded, file);
    CHECK(loaded.count == 2 && strcmp(loaded.paths[0], dirs[0]) == 0);

    // At most PROJECT_RECENT_MAX; the oldest go.
    for (int i = 0; i < 12; ++i)
        project_recent_add(&list, dirs[i], file);
    CHECK(list.count == PROJECT_RECENT_MAX);
    CHECK(strcmp(list.paths[0], dirs[11]) == 0);
    CHECK(strcmp(list.paths[PROJECT_RECENT_MAX - 1], dirs[2]) == 0);

    // A project that's gone is dropped on load.
    remove_tree(dirs[11]);
    project_recent_load(&loaded, file);
    CHECK(loaded.count == PROJECT_RECENT_MAX - 1);
    CHECK(strcmp(loaded.paths[0], dirs[10]) == 0);
}

static void
test_import(void)
{
    char error[512], key[256];
    const char* project = ROOT "/Importer";
    CHECK(project_create(project, "Importer", error, sizeof(error)));

    // .glb and .hdr: one file each.
    write(ROOT "/src/chair.glb", "glTF-binary");
    write(ROOT "/src/sky.hdr", "#?RADIANCE");
    CHECK(project_import_asset(project, ROOT "/src/chair.glb", key, sizeof(key), error, sizeof(error)));
    CHECK(strcmp(key, "models/chair.glb") == 0);
    CHECK(exists("%s/models/chair.glb", project));
    CHECK(project_import_asset(project, ROOT "/src/sky.hdr", key, sizeof(key), error, sizeof(error)));
    CHECK(strcmp(key, "hdri/sky.hdr") == 0);

    // Twice: refused, the first copy stays.
    CHECK(!project_import_asset(project, ROOT "/src/chair.glb", key, sizeof(key), error, sizeof(error)));
    CHECK(strstr(error, "already has") != NULL);

    // .gltf: the buffers and images it names come along, sub-folders and
    // percent-encoded names included; embedded data isn't a file.
    write(ROOT "/src/lamp/lamp.gltf",
          "{\"buffers\": [{\"uri\": \"lamp.bin\"}, {\"uri\": \"data:application/octet-stream;base64,AAAA\"}],"
          " \"images\": [{\"uri\": \"textures/base%20color.png\"}, {\"bufferView\": 0}]}");
    write(ROOT "/src/lamp/lamp.bin", "bin");
    write(ROOT "/src/lamp/textures/base color.png", "png");
    CHECK(project_import_asset(project, ROOT "\\src\\lamp\\lamp.gltf", key, sizeof(key), error, sizeof(error)));
    CHECK(strcmp(key, "models/lamp/lamp.gltf") == 0);
    CHECK(exists("%s/models/lamp/lamp.bin", project));
    CHECK(exists("%s/models/lamp/textures/base color.png", project));

    // A missing dependency: refused, and nothing was copied.
    write(ROOT "/src/broken/broken.gltf", "{\"buffers\": [{\"uri\": \"missing.bin\"}]}");
    CHECK(!project_import_asset(project, ROOT "/src/broken/broken.gltf", key, sizeof(key), error, sizeof(error)));
    CHECK(strstr(error, "missing.bin") != NULL);
    CHECK(!exists("%s/models/broken/broken.gltf", project));

    // A dependency outside the model's folder: refused.
    write(ROOT "/src/escape/escape.gltf", "{\"buffers\": [{\"uri\": \"../lamp/lamp.bin\"}]}");
    CHECK(!project_import_asset(project, ROOT "/src/escape/escape.gltf", key, sizeof(key), error, sizeof(error)));
    CHECK(strstr(error, "outside") != NULL);

    // .obj: its .mtl and the textures the .mtl names (options before the file).
    write(ROOT "/src/crate/crate.obj", "mtllib crate.mtl\nv 0 0 0\n");
    write(ROOT "/src/crate/crate.mtl", "newmtl wood\nmap_Kd wood.png\n  map_Bump -bm 0.5 wood_n.png\r\n");
    write(ROOT "/src/crate/wood.png", "png");
    write(ROOT "/src/crate/wood_n.png", "png");
    CHECK(project_import_asset(project, ROOT "/src/crate/crate.obj", key, sizeof(key), error, sizeof(error)));
    CHECK(strcmp(key, "models/crate/crate.obj") == 0);
    CHECK(exists("%s/models/crate/crate.mtl", project));
    CHECK(exists("%s/models/crate/wood.png", project));
    CHECK(exists("%s/models/crate/wood_n.png", project));

    // Unsupported, and missing.
    write(ROOT "/src/thing.fbx", "fbx");
    CHECK(!project_import_asset(project, ROOT "/src/thing.fbx", key, sizeof(key), error, sizeof(error)));
    CHECK(!project_import_asset(project, ROOT "/src/nope.glb", key, sizeof(key), error, sizeof(error)));
}

// A scene file whose one entity uses `model` (NULL = none) and whose
// environment uses `hdri` (NULL = none).
static void
write_scene(const char* path, const char* model, const char* hdri)
{
    char text[1024];
    snprintf(text, sizeof(text),
             "{\"format\": \"pbr-scene\", \"version\": 1, \"entities\": [{\"name\": \"thing\", \"parent\": null, "
             "\"components\": {\"MeshRenderer\": {\"model\": %s%s%s}}}], \"environment\": {\"hdri\": %s%s%s}}",
             model ? "\"" : "", model ? model : "null", model ? "\"" : "",
             hdri ? "\"" : "", hdri ? hdri : "null", hdri ? "\"" : "");
    write(path, text);
}

static void
test_export(void)
{
    char error[512];
    const char* project = ROOT "/Exporter";
    const char* out     = ROOT "/exported";
    CHECK(project_create(project, "Exporter", error, sizeof(error)));

    // What the editor would hand over: a game executable, the engine's
    // shaders and a runtime DLL.
    write(ROOT "/bin/Game.exe", "MZ game");
    write(ROOT "/bin/vcruntime140.dll", "MZ dll");
    write(ROOT "/engine/shaders/pbr.vert", "glsl");
    write(ROOT "/engine/shaders/pbr.frag", "glsl");
    const char* dlls[] = { ROOT "/bin/vcruntime140.dll" };
    project_export_options options = { ROOT "/bin/Game.exe", ROOT "/engine", dlls, 1 };

    // Assets: two used (one a .gltf with a buffer and a texture), one not.
    write(ROOT "/Exporter/models/chair.glb", "glb");
    write(ROOT "/Exporter/models/unused.glb", "glb");
    write(ROOT "/Exporter/models/lamp/lamp.gltf",
          "{\"buffers\": [{\"uri\": \"lamp.bin\"}], \"images\": [{\"uri\": \"tex/shade.png\"}]}");
    write(ROOT "/Exporter/models/lamp/lamp.bin", "bin");
    write(ROOT "/Exporter/models/lamp/tex/shade.png", "png");
    write(ROOT "/Exporter/hdri/sky.hdr", "hdr");
    write_scene(ROOT "/Exporter/scenes/level2.json", "models/lamp/lamp.gltf", "hdri/sky.hdr");
    write_scene(ROOT "/Exporter/scenes/level3.json", "models/chair.glb", NULL);

    project_export_report report;
    CHECK(project_export(project, out, &options, &report, error, sizeof(error)));
    CHECK(strcmp(report.executable, ROOT "/exported/Exporter.exe") == 0);
    CHECK(report.scenes == 3);   // main (the starter scene) + level2 + level3
    CHECK(report.bytes > 0);
    CHECK(exists("%s/Exporter.exe", out));
    CHECK(exists("%s/vcruntime140.dll", out));
    CHECK(exists("%s/engine/shaders/pbr.vert", out) && exists("%s/engine/shaders/pbr.frag", out));
    CHECK(exists("%s/project.json", out));
    CHECK(exists("%s/scenes/main.json", out) && exists("%s/scenes/level2.json", out));
    CHECK(exists("%s/models/chair.glb", out));
    CHECK(exists("%s/models/lamp/lamp.gltf", out) && exists("%s/models/lamp/lamp.bin", out));
    CHECK(exists("%s/models/lamp/tex/shade.png", out));
    CHECK(exists("%s/hdri/sky.hdr", out));
    CHECK(!exists("%s/models/unused.glb", out));   // no scene uses it
    CHECK(exists("%s/.pbr-export", out));

    // The export is a project the game can open by itself.
    project_settings settings;
    CHECK(project_read(out, &settings, error, sizeof(error)) && strcmp(settings.name, "Exporter") == 0);

    // Again into the same folder: an earlier export is replaced.
    CHECK(project_export(project, out, &options, &report, error, sizeof(error)));

    // Refusals, each leaving the target untouched.
    const char* fresh = ROOT "/fresh";
    write_scene(ROOT "/Exporter/scenes/broken.json", "models/missing.glb", NULL);
    CHECK(!project_export(project, fresh, &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "missing.glb") != NULL && strstr(error, "broken") != NULL);
    CHECK(!file_is_directory(fresh));

    write_scene(ROOT "/Exporter/scenes/broken.json", "../outside.glb", NULL);
    CHECK(!project_export(project, fresh, &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "outside the project") != NULL);
    remove(ROOT "/Exporter/scenes/broken.json");

    project_settings s;
    CHECK(project_read(project, &s, error, sizeof(error)));
    snprintf(s.startup_scene, sizeof(s.startup_scene), "never_saved");
    CHECK(project_write(project, &s, error, sizeof(error)));
    CHECK(!project_export(project, fresh, &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "never_saved") != NULL);
    snprintf(s.startup_scene, sizeof(s.startup_scene), "main");
    CHECK(project_write(project, &s, error, sizeof(error)));

    write(ROOT "/occupied/notes.txt", "mine");
    CHECK(!project_export(project, ROOT "/occupied", &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "aren't an earlier export") != NULL);

    CHECK(!project_export(project, ROOT "/Exporter/build", &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "outside the project folder") != NULL);
    char sneaky[1024];   // the same folder, spelled with '\' and a trailing separator
    snprintf(sneaky, sizeof(sneaky), "%s/Exporter\\build\\", ROOT);
    CHECK(!project_export(project, sneaky, &options, &report, error, sizeof(error)));
    CHECK(strstr(error, "outside the project folder") != NULL);
#ifdef _WIN32
    snprintf(sneaky, sizeof(sneaky), "%s/EXPORTER/build", ROOT);   // Windows paths ignore case
    CHECK(!project_export(project, sneaky, &options, &report, error, sizeof(error)));
#endif

    // A model path written with '\' (Windows-style) still brings its .gltf's files.
    const char* slashes = ROOT "/slashes";
    write_scene(ROOT "/Exporter/scenes/level4.json", "models\\\\lamp\\\\lamp.gltf", NULL);
    CHECK(project_export(project, slashes, &options, &report, error, sizeof(error)));
    CHECK(exists("%s/models/lamp/lamp.bin", slashes) && exists("%s/models/lamp/tex/shade.png", slashes));
    remove(ROOT "/Exporter/scenes/level4.json");

    project_export_options no_game = { ROOT "/bin/Missing.exe", ROOT "/engine", NULL, 0 };
    CHECK(!project_export(project, fresh, &no_game, &report, error, sizeof(error)));
    CHECK(strstr(error, "Game target") != NULL);
    CHECK(!file_is_directory(fresh));
}

int
main(void)
{
    remove_tree(ROOT);   // left over from a crashed run
    file_make_directories(ROOT);

    asset_cache assets;
    assets_init(&assets);

    test_names();
    test_create(&assets);
    test_settings_round_trip();
    test_recent();
    test_import();
    test_export();

    assets_shutdown(&assets);
    remove_tree(ROOT);

    printf("project_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
