// serialize_tests.c
//
// Headless tests for World/Serialize.h: JSON output shape and rounding,
// partial updates, all-or-nothing validation, error messages, enums,
// material overrides, names, parents, and asset keys that never reach the
// GPU (null and unknown built-ins).

#include "World/Serialize.h"
#include "World/World.h"

#include <math.h>
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

static char g_error[512];

// Parses `json` and applies it to `e`'s `type` component.
static b8
set(world* w, entity_id e, component_type type, const char* json)
{
    cJSON* fields = cJSON_Parse(json);
    if (!fields)
    {
        fprintf(stderr, "bad test JSON: %s\n", json);
        ++g_failures;
        return false;
    }

    g_error[0] = '\0';
    b8 ok = deserialize_component(w, e, type, fields, g_error, sizeof(g_error));
    cJSON_Delete(fields);
    return ok;
}

static b8
error_mentions(const char* text)
{
    return strstr(g_error, text) != NULL;
}

static void
test_entity_output(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id table = world_create(&w, "table");
    entity_id lamp  = world_create(&w, "lamp");
    world_set_parent(&w, lamp, table);
    world_transform(&w, lamp)->position[1] = 0.3f;

    light l;
    light_init_spot(&l, (vec3){ 0.0f, -1.0f, 0.0f }, (vec3){ 1.0f, 0.9f, 0.8f }, 5.0f, 4.0f, 15.0f, 25.0f);
    world_add_light(&w, lamp, l);

    cJSON* json = serialize_entity(&w, lamp);
    CHECK(json != NULL);

    CHECK(strcmp(cJSON_GetObjectItem(json, "name")->valuestring, "lamp") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(json, "parent")->valuestring, "table") == 0);
    CHECK((entity_id)cJSON_GetObjectItem(json, "id")->valuedouble == lamp);

    cJSON* components = cJSON_GetObjectItem(json, "components");
    CHECK(cJSON_GetObjectItem(components, "Name") == NULL);     // top-level "name" instead
    CHECK(cJSON_GetObjectItem(components, "Parent") == NULL);   // top-level "parent" instead

    cJSON* transform = cJSON_GetObjectItem(components, "Transform");
    double y = cJSON_GetArrayItem(cJSON_GetObjectItem(transform, "position"), 1)->valuedouble;
    CHECK(y == 0.3);   // rounded: exactly the double 0.3, not 0.30000001192...

    // Printed text carries the short form too.
    char* text = cJSON_PrintUnformatted(transform);
    CHECK(strstr(text, "0.3") && !strstr(text, "0.300000"));
    cJSON_free(text);

    cJSON* light_json = cJSON_GetObjectItem(components, "Light");
    CHECK(strcmp(cJSON_GetObjectItem(light_json, "type")->valuestring, "spot") == 0);
    CHECK(cJSON_GetObjectItem(light_json, "intensity")->valuedouble == 5.0);

    cJSON_Delete(json);

    cJSON* root = serialize_entity(&w, table);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(root, "parent")));
    cJSON_Delete(root);

    CHECK(serialize_entity(&w, ENTITY_NULL) == NULL);
    CHECK(serialize_component(&w, table, COMPONENT_LIGHT) == NULL);

    world_shutdown(&w);
}

static void
test_partial_update_and_atomicity(asset_cache* assets)
{
    world w;
    world_init(&w, assets);
    entity_id e = world_create(&w, "box");

    CHECK(set(&w, e, COMPONENT_TRANSFORM, "{\"position\": [1, 2, 3]}"));
    c_transform* t = world_transform(&w, e);
    CHECK(t->position[0] == 1.0f && t->position[2] == 3.0f);
    CHECK(t->scale[0] == 1.0f);   // untouched

    // One bad field rejects the whole request: position must not change.
    CHECK(!set(&w, e, COMPONENT_TRANSFORM, "{\"position\": [9, 9, 9], \"scale\": [0, 1, 1]}"));
    CHECK(error_mentions("Transform.scale[0]") && error_mentions("out of range"));
    CHECK(t->position[0] == 1.0f);

    CHECK(!set(&w, e, COMPONENT_TRANSFORM, "{\"position\": [1, 2]}"));
    CHECK(error_mentions("array of 3 numbers"));

    CHECK(!set(&w, e, COMPONENT_TRANSFORM, "{\"positon\": [1, 2, 3]}"));
    CHECK(error_mentions("no field \"positon\"") && error_mentions("position, rotation_deg, scale"));

    CHECK(!set(&w, e, COMPONENT_TRANSFORM, "[1, 2, 3]"));
    CHECK(error_mentions("expected an object"));

    CHECK(set(&w, e, COMPONENT_TRANSFORM, "{}"));   // nothing to do is fine

    // A NULL error buffer is allowed.
    cJSON* bad = cJSON_Parse("{\"nope\": 1}");
    CHECK(!deserialize_component(&w, e, COMPONENT_TRANSFORM, bad, NULL, 0));
    cJSON_Delete(bad);

    world_shutdown(&w);
}

static void
test_lights_and_enums(asset_cache* assets)
{
    world w;
    world_init(&w, assets);
    entity_id e = world_create(&w, "lamp");

    // Setting a component the entity lacks adds it with defaults.
    CHECK(!world_has(&w, e, COMPONENT_LIGHT));
    CHECK(set(&w, e, COMPONENT_LIGHT, "{\"type\": \"spot\", \"intensity\": 12}"));
    c_light* l = world_light(&w, e);
    CHECK(l && l->type == LIGHT_SPOT && l->intensity == 12.0f);
    CHECK(l->color[0] == 1.0f && l->direction[1] == -1.0f);   // defaults

    CHECK(!set(&w, e, COMPONENT_LIGHT, "{\"type\": \"laser\"}"));
    CHECK(error_mentions("\"directional\", \"point\", \"spot\""));

    CHECK(!set(&w, e, COMPONENT_LIGHT, "{\"direction\": [0, 0, 0]}"));
    CHECK(error_mentions("must not be [0, 0, 0]"));

    CHECK(!set(&w, e, COMPONENT_LIGHT, "{\"intensity\": \"bright\"}"));
    CHECK(error_mentions("expected a number"));

    CHECK(!set(&w, e, COMPONENT_LIGHT, "{\"intensity\": null}"));
    CHECK(error_mentions("null is not allowed"));

    // A failed request on a component the entity lacks must not add it.
    CHECK(!set(&w, e, COMPONENT_CAMERA, "{\"fov_degrees\": 500}"));
    CHECK(!world_has(&w, e, COMPONENT_CAMERA));

    world_shutdown(&w);
}

static void
test_material_overrides(asset_cache* assets)
{
    world w;
    world_init(&w, assets);
    entity_id e = world_create(&w, "ball");

    CHECK(set(&w, e, COMPONENT_MESH_RENDERER, "{\"model\": null, \"material.roughness\": 0.2}"));
    c_mesh_renderer* mr = world_mesh_renderer(&w, e);
    CHECK(mr->material.fields == MATERIAL_OVERRIDE_ROUGHNESS);
    CHECK(mr->material.roughness == 0.2f);

    cJSON* json = serialize_component(&w, e, COMPONENT_MESH_RENDERER);
    CHECK(cJSON_GetObjectItem(json, "material.roughness") != NULL);
    CHECK(cJSON_GetObjectItem(json, "material.metallic") == NULL);   // not overridden: omitted
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(json, "model")));
    cJSON_Delete(json);

    CHECK(set(&w, e, COMPONENT_MESH_RENDERER, "{\"material.roughness\": null}"));
    CHECK(mr->material.fields == 0);

    CHECK(!set(&w, e, COMPONENT_MESH_RENDERER, "{\"material.metallic\": 1.5}"));
    CHECK(error_mentions("out of range [0, 1]"));

    // Unknown built-ins are rejected without touching the GPU.
    CHECK(!set(&w, e, COMPONENT_MESH_RENDERER, "{\"model\": \"builtin:pyramid\"}"));
    CHECK(error_mentions("builtin:cube"));

    CHECK(!set(&w, e, COMPONENT_MESH_RENDERER, "{\"model\": 42}"));
    CHECK(error_mentions("asset key"));

    world_shutdown(&w);
}

static void
test_names_and_parents(asset_cache* assets)
{
    world w;
    world_init(&w, assets);
    entity_id table = world_create(&w, "table");
    entity_id cup   = world_create(&w, "cup");

    CHECK(set(&w, cup, COMPONENT_NAME, "{\"value\": \"mug\"}"));
    CHECK(world_find(&w, "mug") == cup);

    CHECK(!set(&w, cup, COMPONENT_NAME, "{\"value\": \"table\"}"));
    CHECK(error_mentions("already used"));

    CHECK(set(&w, cup, COMPONENT_NAME, "{\"value\": \"mug\"}"));   // its own name: fine

    CHECK(set(&w, cup, COMPONENT_PARENT, "{\"parent\": \"table\"}"));
    CHECK(world_parent(&w, cup) == table);

    // By numeric ID works too.
    char by_id[64];
    snprintf(by_id, sizeof(by_id), "{\"parent\": %u}", (unsigned)cup);
    CHECK(!set(&w, table, COMPONENT_PARENT, by_id));   // table -> mug -> table: a cycle
    CHECK(error_mentions("descendants"));

    CHECK(!set(&w, cup, COMPONENT_PARENT, "{\"parent\": \"ghost\"}"));
    CHECK(error_mentions("no entity matches"));

    CHECK(set(&w, cup, COMPONENT_PARENT, "{\"parent\": null}"));
    CHECK(world_parent(&w, cup) == ENTITY_NULL);

    world_shutdown(&w);
}

static void
test_world_order(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    // The child is created first, so storage order is child-then-parent;
    // the output must still list the parent first.
    entity_id child  = world_create(&w, "child");
    entity_id parent = world_create(&w, "parent");
    world_create(&w, "loner");
    world_set_parent(&w, child, parent);

    cJSON* json     = serialize_world(&w);
    cJSON* entities = cJSON_GetObjectItem(json, "entities");
    CHECK(cJSON_GetArraySize(entities) == 3);

    int parent_index = -1, child_index = -1;
    for (int i = 0; i < cJSON_GetArraySize(entities); ++i)
    {
        const char* name = cJSON_GetObjectItem(cJSON_GetArrayItem(entities, i), "name")->valuestring;
        if (strcmp(name, "parent") == 0) parent_index = i;
        if (strcmp(name, "child") == 0)  child_index  = i;
    }
    CHECK(parent_index >= 0 && child_index > parent_index);

    cJSON_Delete(json);
    world_shutdown(&w);
}

int
main(void)
{
    asset_cache assets;
    assets_init(&assets);

    test_entity_output(&assets);
    test_partial_update_and_atomicity(&assets);
    test_lights_and_enums(&assets);
    test_material_overrides(&assets);
    test_names_and_parents(&assets);
    test_world_order(&assets);

    assets_shutdown(&assets);

    printf("serialize_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
