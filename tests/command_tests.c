// command_tests.c
//
// Headless tests for Commands/Commands.h, driven through cmd_execute_text()
// exactly as the console and the command server will: JSON text in, JSON
// text out. Covers every command, all-or-nothing behaviour, and that error
// messages carry what a caller needs to fix the request.

#include "Commands/Commands.h"

#include <stdio.h>
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

static command_context g_ctx;
static cJSON*          g_response;   // last response, freed by the next run()

// Runs one request; returns true if the response says ok.
static b8
run(const char* request)
{
    cJSON_Delete(g_response);

    char* text = cmd_execute_text(&g_ctx, request);
    g_response = cJSON_Parse(text);
    cJSON_free(text);

    if (!g_response)
    {
        fprintf(stderr, "response is not JSON for: %s\n", request);
        ++g_failures;
        return false;
    }
    return cJSON_IsTrue(cJSON_GetObjectItem(g_response, "ok"));
}

static const cJSON* result(void)    { return cJSON_GetObjectItem(g_response, "result"); }
static const char*  error_code(void){ return cJSON_GetObjectItem(cJSON_GetObjectItem(g_response, "error"), "code")->valuestring; }
static b8 error_mentions(const char* text)
{
    const cJSON* message = cJSON_GetObjectItem(cJSON_GetObjectItem(g_response, "error"), "message");
    return message && strstr(message->valuestring, text) != NULL;
}

static void
test_request_shape(void)
{
    CHECK(!run("{not json"));
    CHECK(strcmp(error_code(), CMD_ERROR_BAD_REQUEST) == 0 && error_mentions("invalid JSON"));

    CHECK(!run("{\"args\": {}}"));
    CHECK(strcmp(error_code(), CMD_ERROR_BAD_REQUEST) == 0);

    CHECK(!run("{\"cmd\": \"entity.get\", \"args\": [1]}"));
    CHECK(strcmp(error_code(), CMD_ERROR_BAD_REQUEST) == 0);

    CHECK(!run("{\"cmd\": \"entity.make\"}"));
    CHECK(strcmp(error_code(), CMD_ERROR_UNKNOWN_COMMAND) == 0 && error_mentions("entity.create"));

    // "args" may be omitted.
    CHECK(run("{\"cmd\": \"scene.describe\"}"));

    CHECK(!run("{\"cmd\": \"scene.describe\", \"args\": {\"sumary\": true}}"));
    CHECK(error_mentions("doesn't take an argument \"sumary\"") && error_mentions("summary"));

    CHECK(run("{\"cmd\": \"commands.list\"}"));
    const cJSON* commands = cJSON_GetObjectItem(result(), "commands");
    b8 found = false;
    const cJSON* c;
    cJSON_ArrayForEach(c, commands)
    {
        if (strcmp(cJSON_GetObjectItem(c, "name")->valuestring, "entity.create") == 0)
            found = cJSON_GetArraySize(cJSON_GetObjectItem(c, "args")) == 3;
    }
    CHECK(found);
}

static void
test_create_and_get(void)
{
    CHECK(run("{\"cmd\": \"entity.create\", \"args\": {\"name\": \"table\", "
              "\"components\": {\"Transform\": {\"position\": [0, 0.75, 0]}, "
              "\"MeshRenderer\": {\"model\": null, \"material.roughness\": 0.6}}}}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "name")->valuestring, "table") == 0);

    entity_id table = world_find(g_ctx.w, "table");
    CHECK(world_has(g_ctx.w, table, COMPONENT_MESH_RENDERER));
    CHECK(world_mesh_renderer(g_ctx.w, table)->material.roughness == 0.6f);

    // A child, parented by name: its world matrix is up to date right away.
    CHECK(run("{\"cmd\": \"entity.create\", \"args\": {\"name\": \"cup\", \"parent\": \"table\", "
              "\"components\": {\"Transform\": {\"position\": [0.2, 0.1, 0]}}}}"));
    entity_id cup = world_find(g_ctx.w, "cup");
    CHECK(world_parent(g_ctx.w, cup) == table);
    CHECK(world_matrix(g_ctx.w, cup)[3][1] > 0.84f && world_matrix(g_ctx.w, cup)[3][1] < 0.86f);

    // A taken name gets a suffix, and the result reports it.
    CHECK(run("{\"cmd\": \"entity.create\", \"args\": {\"name\": \"cup\"}}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "name")->valuestring, "cup_2") == 0);

    // Get by name and by id.
    CHECK(run("{\"cmd\": \"entity.get\", \"args\": {\"entity\": \"cup\"}}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "parent")->valuestring, "table") == 0);

    char by_id[128];
    snprintf(by_id, sizeof(by_id), "{\"cmd\": \"entity.get\", \"args\": {\"entity\": %u}}", (unsigned)cup);
    CHECK(run(by_id));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "name")->valuestring, "cup") == 0);

    CHECK(!run("{\"cmd\": \"entity.get\", \"args\": {\"entity\": \"chair\"}}"));
    CHECK(strcmp(error_code(), CMD_ERROR_NOT_FOUND) == 0);
    CHECK(error_mentions("no entity named \"chair\"") && error_mentions("table") && error_mentions("cup_2"));

    CHECK(!run("{\"cmd\": \"entity.get\", \"args\": {}}"));
    CHECK(error_mentions("missing required argument \"entity\""));
}

static void
test_create_is_all_or_nothing(void)
{
    uint32 before = world_count(g_ctx.w);

    // The second component fails: the entity must not exist afterwards.
    CHECK(!run("{\"cmd\": \"entity.create\", \"args\": {\"name\": \"bad\", "
               "\"components\": {\"Transform\": {\"position\": [1, 1, 1]}, \"Light\": {\"type\": \"laser\"}}}}"));
    CHECK(strcmp(error_code(), CMD_ERROR_INVALID_VALUE) == 0 && error_mentions("\"spot\""));
    CHECK(world_count(g_ctx.w) == before && world_find(g_ctx.w, "bad") == ENTITY_NULL);

    CHECK(!run("{\"cmd\": \"entity.create\", \"args\": {\"components\": {\"Lamp\": {}}}}"));
    CHECK(error_mentions("unknown component \"Lamp\"") && error_mentions("Light"));
    CHECK(world_count(g_ctx.w) == before);

    CHECK(!run("{\"cmd\": \"entity.create\", \"args\": {\"components\": {\"Name\": {\"value\": \"x\"}}}}"));
    CHECK(error_mentions("\"name\""));

    CHECK(!run("{\"cmd\": \"entity.create\", \"args\": {\"parent\": \"nowhere\"}}"));
    CHECK(world_count(g_ctx.w) == before);

    // A cross-field rule (spot cone) also rolls the whole entity back.
    CHECK(!run("{\"cmd\": \"entity.create\", \"args\": {\"name\": \"spot\", \"components\": "
               "{\"Light\": {\"type\": \"spot\", \"inner_cone_degrees\": 40, \"outer_cone_degrees\": 10}}}}"));
    CHECK(error_mentions("outer_cone_degrees"));
    CHECK(world_find(g_ctx.w, "spot") == ENTITY_NULL);
}

static void
test_components(void)
{
    CHECK(run("{\"cmd\": \"component.set\", \"args\": {\"entity\": \"table\", \"component\": \"Light\", "
              "\"fields\": {\"type\": \"spot\", \"intensity\": 8, \"inner_cone_degrees\": 10, \"outer_cone_degrees\": 20}}}"));
    entity_id table = world_find(g_ctx.w, "table");
    c_light*  l     = world_light(g_ctx.w, table);
    CHECK(l && l->intensity == 8.0f);

    // Breaking the cone rule rolls back the whole request, including the
    // valid intensity change sent with it.
    CHECK(!run("{\"cmd\": \"component.set\", \"args\": {\"entity\": \"table\", \"component\": \"Light\", "
               "\"fields\": {\"intensity\": 99, \"inner_cone_degrees\": 30}}}"));
    l = world_light(g_ctx.w, table);
    CHECK(l->intensity == 8.0f && l->inner_cone_degrees == 10.0f);

    CHECK(!run("{\"cmd\": \"component.set\", \"args\": {\"entity\": \"table\", \"component\": \"Light\", \"fields\": 5}}"));
    CHECK(error_mentions("\"fields\" must be an object"));

    CHECK(run("{\"cmd\": \"component.remove\", \"args\": {\"entity\": \"table\", \"component\": \"Light\"}}"));
    CHECK(!world_has(g_ctx.w, table, COMPONENT_LIGHT));

    CHECK(!run("{\"cmd\": \"component.remove\", \"args\": {\"entity\": \"table\", \"component\": \"Light\"}}"));
    CHECK(strcmp(error_code(), CMD_ERROR_NOT_FOUND) == 0);

    CHECK(!run("{\"cmd\": \"component.remove\", \"args\": {\"entity\": \"table\", \"component\": \"Transform\"}}"));
    CHECK(error_mentions("can't be removed"));
}

static void
test_update(void)
{
    entity_id cup2 = world_find(g_ctx.w, "cup_2");

    CHECK(run("{\"cmd\": \"entity.update\", \"args\": {\"entity\": \"cup_2\", \"name\": \"saucer\", \"parent\": \"table\"}}"));
    CHECK(world_find(g_ctx.w, "saucer") == cup2);
    CHECK(world_parent(g_ctx.w, cup2) == world_find(g_ctx.w, "table"));

    // Valid parent + clashing name: neither change may stick.
    CHECK(!run("{\"cmd\": \"entity.update\", \"args\": {\"entity\": \"saucer\", \"name\": \"cup\", \"parent\": null}}"));
    CHECK(error_mentions("already used"));
    CHECK(world_parent(g_ctx.w, cup2) == world_find(g_ctx.w, "table"));
    CHECK(world_find(g_ctx.w, "saucer") == cup2);

    CHECK(!run("{\"cmd\": \"entity.update\", \"args\": {\"entity\": \"table\", \"parent\": \"saucer\"}}"));
    CHECK(error_mentions("descendants"));

    CHECK(!run("{\"cmd\": \"entity.update\", \"args\": {\"entity\": \"table\"}}"));
    CHECK(error_mentions("nothing to update"));

    CHECK(run("{\"cmd\": \"entity.update\", \"args\": {\"entity\": \"saucer\", \"parent\": null}}"));
    CHECK(world_parent(g_ctx.w, cup2) == ENTITY_NULL);
}

static void
test_describe_destroy_clear(void)
{
    CHECK(run("{\"cmd\": \"scene.describe\", \"args\": {\"summary\": true}}"));
    const cJSON* entities = cJSON_GetObjectItem(result(), "entities");
    CHECK(cJSON_GetArraySize(entities) == (int)world_count(g_ctx.w));
    const cJSON* first = cJSON_GetArrayItem(entities, 0);
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(first, "position")));
    CHECK(cJSON_IsArray(cJSON_GetObjectItem(first, "components")));

    // Destroying the table takes the cup (its child) with it, and says so.
    CHECK(run("{\"cmd\": \"entity.destroy\", \"args\": {\"entity\": \"table\"}}"));
    const cJSON* destroyed = cJSON_GetObjectItem(result(), "destroyed");
    CHECK(cJSON_GetArraySize(destroyed) == 2);
    CHECK(world_find(g_ctx.w, "cup") == ENTITY_NULL);
    CHECK(world_find(g_ctx.w, "saucer") != ENTITY_NULL);

    CHECK(run("{\"cmd\": \"scene.clear\"}"));
    CHECK(cJSON_GetObjectItem(result(), "destroyed")->valuedouble == 1.0);
    CHECK(world_count(g_ctx.w) == 0);

    CHECK(!run("{\"cmd\": \"entity.get\", \"args\": {\"entity\": \"saucer\"}}"));
    CHECK(error_mentions("the scene is empty"));
}

static void
test_components_describe(void)
{
    CHECK(run("{\"cmd\": \"components.describe\"}"));
    const cJSON* components = cJSON_GetObjectItem(result(), "components");
    CHECK(cJSON_GetArraySize(components) == 6);   // Transform, MeshRenderer, Light, Camera, Spin, ReflectionProbe

    const cJSON* light = NULL;
    const cJSON* c;
    cJSON_ArrayForEach(c, components)
    {
        CHECK(strcmp(cJSON_GetObjectItem(c, "name")->valuestring, "Name") != 0);
        if (strcmp(cJSON_GetObjectItem(c, "name")->valuestring, "Light") == 0)
            light = c;
    }
    CHECK(light != NULL);

    b8 has_enum = false, has_range = false;
    const cJSON* f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(light, "fields"))
    {
        const char* name = cJSON_GetObjectItem(f, "name")->valuestring;
        if (strcmp(name, "type") == 0)
            has_enum = cJSON_GetArraySize(cJSON_GetObjectItem(f, "values")) == 3;
        if (strcmp(name, "intensity") == 0)
            has_range = cJSON_GetObjectItem(f, "max") != NULL;
        CHECK(cJSON_GetObjectItem(f, "description") != NULL);
    }
    CHECK(has_enum && has_range);
}

static void
test_assets_list(void)
{
    CHECK(run("{\"cmd\": \"assets.list\"}"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "builtins")) == 3);

    // Reads the real assets/models folder.
    b8 found = false;
    const cJSON* m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(result(), "models"))
        found |= strcmp(m->valuestring, "models/Avocado.glb") == 0;
    CHECK(found);
}

int
main(void)
{
    asset_cache assets;
    world       w;
    assets_init(&assets);
    world_init(&w, &assets);
    g_ctx.w = &w;

    test_request_shape();
    test_create_and_get();
    test_create_is_all_or_nothing();
    test_components();
    test_update();
    test_describe_destroy_clear();
    test_assets_list();
    test_components_describe();

    cJSON_Delete(g_response);
    world_shutdown(&w);
    assets_shutdown(&assets);

    printf("command_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
