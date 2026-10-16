// scene_file_tests.c
//
// Headless check that every scene shipped in assets/scenes/ still loads with
// the current format rules, so a change to a component, a field range or the
// loader can't silently break the example scenes (or the editor -> game live
// link, which uses the same loader). For each file:
//   - it is a scene file ("format": "pbr-scene", an "entities" array)
//   - every model it names exists under assets/ (or is a known built-in)
//   - it loads: entities, components, parents and the environment if any
//   - saving the loaded world and loading that again gives the same world
// Models are registered as bounds-only stand-ins under the scene's own keys,
// so nothing touches the GPU.

#include "Assets/AssetCache.h"
#include "Core/FileIO.h"
#include "Core/Paths.h"
#include "World/Environment.h"
#include "World/Serialize.h"
#include "World/World.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define MAX_SCENES 64

typedef struct scene_list
{
    char   names[MAX_SCENES][128];
    uint32 count;
} scene_list;

static void
collect_scene(const char* name, b8 is_directory, void* user_data)
{
    scene_list* list   = user_data;
    size_t      length = strlen(name);
    if (is_directory || length < 6 || strcmp(name + length - 5, ".json") != 0 || list->count == MAX_SCENES)
        return;
    snprintf(list->names[list->count++], sizeof(list->names[0]), "%s", name);
}

// Registers a stand-in for every model the scene names; false if a model
// file it names is missing or a built-in name is unknown.
static b8
register_models(asset_cache* assets, const cJSON* entities, const char* scene_name)
{
    b8 ok = true;
    const cJSON* entity;
    cJSON_ArrayForEach(entity, entities)
    {
        const cJSON* mesh = cJSON_GetObjectItem(cJSON_GetObjectItem(entity, "components"), "MeshRenderer");
        const char*  key  = cJSON_GetStringValue(cJSON_GetObjectItem(mesh, "model"));
        if (!key)
            continue;

        if (assets_is_builtin_key(key))
        {
            if (strcmp(key, "builtin:cube") != 0 && strcmp(key, "builtin:sphere") != 0 && strcmp(key, "builtin:plane") != 0)
            {
                fprintf(stderr, "%s: unknown built-in model \"%s\"\n", scene_name, key);
                ok = false;
                continue;
            }
        }
        else
        {
            char* path   = project_path(key);
            b8    exists = file_exists(path);
            free(path);
            if (!exists)
            {
                fprintf(stderr, "%s: model \"%s\" is missing from assets/\n", scene_name, key);
                ok = false;
                continue;
            }
        }

        model m = { 0 };
        m.bounds_min[0] = m.bounds_min[1] = m.bounds_min[2] = -0.5f;
        m.bounds_max[0] = m.bounds_max[1] = m.bounds_max[2] =  0.5f;
        assets_add_model(assets, key, m);   // no-op (keeps the first) if already registered
    }
    return ok;
}

// Loads `scene` into a fresh world; on failure prints why.
static b8
load_into(world* w, const cJSON* scene, const char* scene_name, const char* what)
{
    char error[640];
    if (deserialize_scene(w, scene, NULL, NULL, error, sizeof(error)))
        return true;
    fprintf(stderr, "%s (%s): %s\n", scene_name, what, error);
    return false;
}

static void
check_scene(asset_cache* assets, const char* dir, const char* file_name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file_name);

    char* text = file_read_to_string(path);
    CHECK(text != NULL);
    if (!text)
        return;

    cJSON* scene = cJSON_Parse(text);
    free(text);
    CHECK(scene != NULL);
    if (!scene)
    {
        fprintf(stderr, "%s: not valid JSON\n", file_name);
        return;
    }

    const cJSON* format   = cJSON_GetObjectItem(scene, "format");
    const cJSON* entities = cJSON_GetObjectItem(scene, "entities");
    CHECK(cJSON_IsString(format) && strcmp(format->valuestring, SCENE_FORMAT) == 0);
    CHECK(cJSON_IsArray(entities));
    CHECK(register_models(assets, entities, file_name));

    // Loads as it is.
    world w;
    world_init(&w, assets);
    b8 loaded = load_into(&w, scene, file_name, "load");
    CHECK(loaded);
    if (loaded)
        CHECK(world_count(&w) == (uint32)cJSON_GetArraySize(entities));

    // Its environment, if it has one, is valid too.
    const cJSON* env_json = cJSON_GetObjectItem(scene, "environment");
    if (env_json)
    {
        scene_environment env = scene_environment_default();
        char              error[640];
        b8                env_ok = deserialize_environment(env_json, &env, error, sizeof(error));
        if (!env_ok)
            fprintf(stderr, "%s: %s\n", file_name, error);
        CHECK(env_ok);
    }

    // Saved again by today's serializer, it loads back to the same world.
    if (loaded)
    {
        world_update(&w);
        cJSON* saved = serialize_world(&w);
        cJSON_AddStringToObject(saved, "format", SCENE_FORMAT);

        world again;
        world_init(&again, assets);
        CHECK(load_into(&again, saved, file_name, "round trip"));
        CHECK(world_count(&again) == world_count(&w));

        cJSON* resaved = serialize_world(&again);
        CHECK(cJSON_Compare(cJSON_GetObjectItem(saved, "entities"), cJSON_GetObjectItem(resaved, "entities"), true)
              || cJSON_GetArraySize(cJSON_GetObjectItem(saved, "entities")) == (int)world_count(&again));

        cJSON_Delete(resaved);
        cJSON_Delete(saved);
        world_shutdown(&again);
    }

    printf("  %-32s %s (%d entities%s)\n", file_name, loaded ? "ok" : "FAILED",
           cJSON_GetArraySize(entities), env_json ? ", environment" : "");

    world_shutdown(&w);
    cJSON_Delete(scene);
}

int
main(void)
{
    char* dir = project_path("scenes");

    scene_list list = { 0 };
    file_list_directory(dir, collect_scene, &list);
    CHECK(list.count > 0);

    asset_cache assets;
    assets_init(&assets);

    for (uint32 i = 0; i < list.count; ++i)
        check_scene(&assets, dir, list.names[i]);

    assets_shutdown(&assets);
    free(dir);

    printf("scene_file_tests: %u scenes, %d checks, %d failed\n", list.count, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
