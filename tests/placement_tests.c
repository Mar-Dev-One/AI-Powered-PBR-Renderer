// placement_tests.c
//
// Headless tests for the high-level commands: place_model and entity.place
// (sizes, rotations, on_ground, every relative_to side, materials, errors),
// scene.save / scene.load / scene.list (round trip, names, undo, the
// environment), and
// application-registered host commands. Models are registered with
// assets_add_model() as bounds only, so nothing touches the GPU.

#include "Commands/Commands.h"
#include "Core/FileIO.h"
#include "World/LiveScene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef TEST_SCENES_DIR
#error "CMake passes TEST_SCENES_DIR (a scratch folder in the build tree)"
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

static asset_cache     g_assets;
static world           g_world;
static command_history g_history;
static command_context g_ctx;
static cJSON*          g_response;

static b8
run(const char* request)
{
    cJSON_Delete(g_response);
    char* text = cmd_execute_text(&g_ctx, request);
    g_response = cJSON_Parse(text);
    cJSON_free(text);
    return g_response && cJSON_IsTrue(cJSON_GetObjectItem(g_response, "ok"));
}

static const cJSON* result(void) { return cJSON_GetObjectItem(g_response, "result"); }
static b8 error_mentions(const char* text)
{
    const cJSON* m = cJSON_GetObjectItem(cJSON_GetObjectItem(g_response, "error"), "message");
    return m && strstr(m->valuestring, text);
}

static b8 near(f32 a, f32 b) { return fabsf(a - b) < 1e-3f; }

// World bounds of a named entity.
static void
bounds_of(const char* name, vec3 bmin, vec3 bmax)
{
    if (!world_bounds(&g_world, world_find(&g_world, name), bmin, bmax))
    {
        glm_vec3_zero(bmin);
        glm_vec3_zero(bmax);
    }
}

// A bounds-only model: `size` across, origin at the bottom centre.
static void
register_box(const char* key, f32 sx, f32 sy, f32 sz)
{
    model m = { 0 };
    m.bounds_min[0] = -sx * 0.5f; m.bounds_min[1] = 0.0f; m.bounds_min[2] = -sz * 0.5f;
    m.bounds_max[0] =  sx * 0.5f; m.bounds_max[1] = sy;   m.bounds_max[2] =  sz * 0.5f;
    assets_add_model(&g_assets, key, m);
}

static void
test_place_model(void)
{
    // Default placement: on the ground at the origin.
    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:table\",\"name\":\"table\"}}"));
    vec3 tmin, tmax;
    bounds_of("table", tmin, tmax);
    CHECK(near(tmin[1], 0.0f) && near(tmax[1], 1.0f) && near(tmin[0], -1.0f) && near(tmax[0], 1.0f));
    CHECK(cJSON_GetObjectItem(result(), "bounds") != NULL);   // reported for the next placement

    // size: longest side becomes 0.2 m; default name from the key.
    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"size\":0.2,"
              "\"relative_to\":{\"entity\":\"table\",\"side\":\"on_top\"}}}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "name")->valuestring, "cup") == 0);
    vec3 cmin, cmax;
    bounds_of("cup", cmin, cmax);
    CHECK(near(cmax[1] - cmin[1], 0.2f));
    CHECK(near(cmin[1], 1.0f));                    // resting on the table top
    CHECK(near(cmin[0] + cmax[0], 0.0f));          // centred on it

    // Every side, with and without a gap.
    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"left_cup\",\"size\":0.2,"
              "\"relative_to\":{\"entity\":\"table\",\"side\":\"left\",\"gap\":0.1}}}"));
    bounds_of("left_cup", cmin, cmax);
    CHECK(near(cmax[0], -1.1f) && near(cmin[1], 0.0f));   // same floor as the table

    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"right_cup\",\"size\":0.2,"
              "\"relative_to\":{\"entity\":\"table\",\"side\":\"right\"}}}"));
    bounds_of("right_cup", cmin, cmax);
    CHECK(near(cmin[0], 1.05f));                   // default side gap 0.05

    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"front_cup\",\"size\":0.2,"
              "\"relative_to\":{\"entity\":\"table\",\"side\":\"front\",\"gap\":0}}}"));
    bounds_of("front_cup", cmin, cmax);
    CHECK(near(cmin[2], 0.5f));

    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"back_cup\",\"size\":0.2,"
              "\"relative_to\":{\"entity\":\"table\",\"side\":\"behind\",\"gap\":0}}}"));
    bounds_of("back_cup", cmin, cmax);
    CHECK(near(cmax[2], -0.5f));

    // Yaw 90 turns the 2 x 1 table so it is long along Z.
    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:table\",\"name\":\"turned\",\"rotation_deg\":90,"
              "\"position\":[5,0,0],\"on_ground\":true}}"));
    bounds_of("turned", tmin, tmax);
    CHECK(near(tmax[0] - tmin[0], 1.0f) && near(tmax[2] - tmin[2], 2.0f) && near(tmin[0], 4.5f));

    // A material override comes along.
    CHECK(run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"red_cup\","
              "\"material\":{\"base_color\":[0.8,0.1,0.1],\"roughness\":0.3}}}"));
    c_mesh_renderer* mr = world_mesh_renderer(&g_world, world_find(&g_world, "red_cup"));
    CHECK(mr->material.fields == (MATERIAL_OVERRIDE_BASE_COLOR | MATERIAL_OVERRIDE_ROUGHNESS));
    CHECK(near(mr->material.base_color[0], 0.8f) && near(mr->material.roughness, 0.3f));
}

static void
test_place_model_errors(void)
{
    uint32 before = world_count(&g_world);

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"builtin:pyramid\"}}"));
    CHECK(error_mentions("builtin:cube"));

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"size\":0.2,\"scale\":2}}"));
    CHECK(error_mentions("either \"size\" or \"scale\""));

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"size\":-1}}"));
    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"scale\":[1,0,1]}}"));

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"position\":[0,0,0],"
               "\"relative_to\":{\"entity\":\"table\",\"side\":\"on_top\"}}}"));
    CHECK(error_mentions("not both"));

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"relative_to\":{\"entity\":\"table\",\"side\":\"under\"}}}"));
    CHECK(error_mentions("\"on_top\"") && error_mentions("behind"));

    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"relative_to\":{\"entity\":\"sofa\",\"side\":\"left\"}}}"));
    CHECK(error_mentions("no entity named \"sofa\""));

    // A bad material is caught after the entity was created -- and the
    // dispatcher removes it again.
    CHECK(!run("{\"cmd\":\"place_model\",\"args\":{\"model\":\"test:cup\",\"name\":\"bad_cup\",\"material\":{\"roughness\":7}}}"));
    CHECK(error_mentions("material.roughness"));
    CHECK(world_find(&g_world, "bad_cup") == ENTITY_NULL);

    CHECK(world_count(&g_world) == before);
}

static void
test_entity_place(void)
{
    // Move an existing entity on top of another.
    CHECK(run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"red_cup\",\"relative_to\":{\"entity\":\"turned\",\"side\":\"on_top\"}}}"));
    vec3 cmin, cmax;
    bounds_of("red_cup", cmin, cmax);
    CHECK(near(cmin[1], 1.0f) && near((cmin[0] + cmax[0]) * 0.5f, 5.0f));

    // on_ground keeps x/z.
    CHECK(run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"red_cup\",\"on_ground\":true}}"));
    bounds_of("red_cup", cmin, cmax);
    CHECK(near(cmin[1], 0.0f) && near((cmin[0] + cmax[0]) * 0.5f, 5.0f));

    CHECK(!run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"red_cup\"}}"));
    CHECK(error_mentions("say where"));

    CHECK(!run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"red_cup\",\"relative_to\":{\"entity\":\"red_cup\",\"side\":\"left\"}}}"));
    CHECK(error_mentions("relative to itself"));

    CHECK(run("{\"cmd\":\"entity.update\",\"args\":{\"entity\":\"red_cup\",\"parent\":\"turned\"}}"));
    CHECK(!run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"red_cup\",\"on_ground\":true}}"));
    CHECK(error_mentions("parented to \"turned\""));

    // Undo works on placement like on anything else.
    entity_id table = world_find(&g_world, "table");
    f32 before_x = world_transform(&g_world, table)->position[0];
    CHECK(run("{\"cmd\":\"entity.place\",\"args\":{\"entity\":\"table\",\"position\":[3,0,3],\"on_ground\":true}}"));
    CHECK(near(world_transform(&g_world, table)->position[0], 3.0f));
    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(near(world_transform(&g_world, table)->position[0], before_x));
}

static void
test_scene_files(void)
{
    uint32 count = world_count(&g_world);

    CHECK(run("{\"cmd\":\"scene.save\",\"args\":{\"name\":\"kitchen\"}}"));
    CHECK(cJSON_GetObjectItem(result(), "entities")->valuedouble == count);

    CHECK(run("{\"cmd\":\"scene.list\"}"));
    b8 listed = false;
    const cJSON* n;
    cJSON_ArrayForEach(n, cJSON_GetObjectItem(result(), "scenes"))
        listed |= strcmp(n->valuestring, "kitchen") == 0;
    CHECK(listed);

    // Path tricks are refused.
    CHECK(!run("{\"cmd\":\"scene.save\",\"args\":{\"name\":\"../evil\"}}"));
    CHECK(error_mentions("only contain letters"));

    // A context that must not save (the editor's Play copy) says why, and
    // writes nothing.
    g_ctx.save_refused = "the scene is playing";
    CHECK(!run("{\"cmd\":\"scene.save\",\"args\":{\"name\":\"refused\"}}"));
    CHECK(error_mentions("the scene is playing"));
    CHECK(!file_exists(TEST_SCENES_DIR "/refused.json"));
    g_ctx.save_refused = NULL;
    CHECK(!run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"nope\"}}"));
    CHECK(error_mentions("no saved scene"));

    // Load replaces the scene and restores names, parents, models, materials.
    CHECK(run("{\"cmd\":\"scene.clear\"}"));
    CHECK(run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"kitchen\"}}"));
    CHECK(world_count(&g_world) == count);

    entity_id cup    = world_find(&g_world, "red_cup");
    entity_id turned = world_find(&g_world, "turned");
    CHECK(world_parent(&g_world, cup) == turned);
    CHECK(near(world_mesh_renderer(&g_world, cup)->material.roughness, 0.3f));
    CHECK(strcmp(assets_key(&g_assets, world_mesh_renderer(&g_world, turned)->model), "test:table") == 0);
    CHECK(near(world_transform(&g_world, turned)->rotation_deg[1], 90.0f));

    // Merging adds a second copy, renamed where names clash, parents intact.
    CHECK(run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"kitchen\",\"replace\":false}}"));
    CHECK(world_count(&g_world) == count * 2);
    entity_id cup2 = world_find(&g_world, "red_cup_2");
    CHECK(cup2 != ENTITY_NULL && world_parent(&g_world, cup2) == world_find(&g_world, "turned_2"));

    // A whole load is a single undo step.
    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(world_count(&g_world) == count);
}

// The environment travels with the scene: saved by scene.save, restored by a
// replacing scene.load, left alone by a merging one, and a bad one fails the
// load before the world is touched.
static void
test_scene_environment(void)
{
    scene_environment env = scene_environment_default();
    snprintf(env.hdri, sizeof(env.hdri), "hdri/test.hdr");
    env.exposure    = 2.5f;
    env.show_skybox = false;
    g_ctx.environment = &env;

    CHECK(run("{\"cmd\":\"scene.save\",\"args\":{\"name\":\"lit_kitchen\"}}"));

    env = scene_environment_default();
    CHECK(run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"lit_kitchen\",\"replace\":false}}"));
    CHECK(env.hdri[0] == '\0' && near(env.exposure, 1.0f));   // merging keeps the current one

    CHECK(run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"lit_kitchen\"}}"));
    CHECK(strcmp(env.hdri, "hdri/test.hdr") == 0 && near(env.exposure, 2.5f) && !env.show_skybox);

    // A file from before environments existed loads and keeps the current one.
    char path[1024];
    snprintf(path, sizeof(path), "%s/old_style.json", TEST_SCENES_DIR);
    CHECK(file_write_string(path, "{\"format\":\"pbr-scene\",\"version\":1,\"entities\":[{\"name\":\"lone\"}]}"));
    CHECK(run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"old_style\"}}"));
    CHECK(world_find(&g_world, "lone") != ENTITY_NULL && near(env.exposure, 2.5f));

    // A bad environment fails the whole load and changes nothing.
    snprintf(path, sizeof(path), "%s/bad_env.json", TEST_SCENES_DIR);
    CHECK(file_write_string(path, "{\"format\":\"pbr-scene\",\"version\":1,\"environment\":{\"exposure\":-1},"
                                  "\"entities\":[{\"name\":\"never\"}]}"));
    CHECK(!run("{\"cmd\":\"scene.load\",\"args\":{\"name\":\"bad_env\"}}"));
    CHECK(error_mentions("exposure"));
    CHECK(world_find(&g_world, "lone") != ENTITY_NULL && world_find(&g_world, "never") == ENTITY_NULL);
    CHECK(near(env.exposure, 2.5f));

    g_ctx.environment = NULL;
}

// The editor -> game live link: the watcher hands out a scene only when its
// content changed, and the throttle holds back checks between intervals.
static void
test_live_scene(void)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/live_test.json", TEST_SCENES_DIR);
    remove(path);

    live_publisher publisher;
    live_watcher   watcher;
    live_publisher_init(&publisher, path);
    live_watcher_init(&watcher, path);

    CHECK(live_watcher_poll(&watcher, 0.0, 0.0) == NULL);   // nothing published yet

    scene_environment env = scene_environment_default();
    CHECK(live_publisher_publish(&publisher, &g_world, &env, 0.0, 0.0));
    CHECK(!live_publisher_publish(&publisher, &g_world, &env, 1.0, 0.0));   // unchanged: not rewritten

    cJSON* scene = live_watcher_poll(&watcher, 0.0, 0.0);
    CHECK(scene && cJSON_GetArraySize(cJSON_GetObjectItem(scene, "entities")) == (int)world_count(&g_world));
    cJSON_Delete(scene);
    CHECK(live_watcher_poll(&watcher, 1.0, 0.0) == NULL);   // same content again

    // A change goes out, and comes back in -- once the throttle allows a check.
    env.exposure = 3.0f;
    CHECK(!live_publisher_publish(&publisher, &g_world, &env, 1.05, 0.1));   // too soon after 1.0
    CHECK(live_publisher_publish(&publisher, &g_world, &env, 1.2, 0.1));
    scene = live_watcher_poll(&watcher, 2.0, 0.1);
    CHECK(scene && cJSON_GetObjectItem(cJSON_GetObjectItem(scene, "environment"), "exposure")->valuedouble == 3.0);
    cJSON_Delete(scene);

    live_watcher_shutdown(&watcher);
    live_publisher_shutdown(&publisher);
    remove(path);
}

// --- Host commands -----------------------------------------------------------

static int g_exposure_calls;

static b8
host_set_exposure(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)user_data; (void)ctx;
    const cJSON* value = cJSON_GetObjectItem(args, "value");
    if (!cJSON_IsNumber(value) || value->valuedouble <= 0.0)
    {
        snprintf(error, error_size, "value must be a positive number");
        return false;
    }
    ++g_exposure_calls;
    *out_result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*out_result, "exposure", value->valuedouble);
    return true;
}

static const cmd_arg HOST_ARGS[] = { { "value", "number", true, "Exposure multiplier" } };
static const cmd_host_command HOST_COMMANDS[] = {
    { "view.set_exposure", "Sets the exposure", HOST_ARGS, 1, false, host_set_exposure },
};

static void
test_host_commands(void)
{
    g_ctx.host_commands      = HOST_COMMANDS;
    g_ctx.host_command_count = 1;

    CHECK(run("{\"cmd\":\"view.set_exposure\",\"args\":{\"value\":1.5}}"));
    CHECK(g_exposure_calls == 1 && cJSON_GetObjectItem(result(), "exposure")->valuedouble == 1.5);

    CHECK(!run("{\"cmd\":\"view.set_exposure\",\"args\":{\"value\":-1}}"));
    CHECK(error_mentions("positive number"));

    CHECK(!run("{\"cmd\":\"view.set_exposure\",\"args\":{\"valu\":1}}"));
    CHECK(error_mentions("doesn't take an argument \"valu\""));

    CHECK(run("{\"cmd\":\"commands.list\"}"));
    b8 listed = false;
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(result(), "commands"))
        listed |= strcmp(cJSON_GetObjectItem(c, "name")->valuestring, "view.set_exposure") == 0;
    CHECK(listed);

    CHECK(!run("{\"cmd\":\"view.zoom\"}"));
    CHECK(error_mentions("view.set_exposure"));   // listed among the known commands

    // Host commands work inside batches too.
    CHECK(run("{\"cmd\":\"batch\",\"args\":{\"commands\":[{\"cmd\":\"view.set_exposure\",\"args\":{\"value\":2}}]}}"));
    CHECK(g_exposure_calls == 2);
}

int
main(void)
{
    assets_init(&g_assets);
    world_init(&g_world, &g_assets);
    history_init(&g_history);

    g_ctx.w          = &g_world;
    g_ctx.history    = &g_history;
    g_ctx.scenes_dir = TEST_SCENES_DIR;

    register_box("test:table", 2.0f, 1.0f, 1.0f);
    register_box("test:cup", 0.5f, 1.0f, 0.5f);

    test_place_model();
    test_place_model_errors();
    test_entity_place();
    test_scene_files();
    test_live_scene();
    test_host_commands();
    test_scene_environment();   // last: it replaces the world

    cJSON_Delete(g_response);
    history_shutdown(&g_history, &g_world);
    world_shutdown(&g_world);
    assets_shutdown(&g_assets);

    printf("placement_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
