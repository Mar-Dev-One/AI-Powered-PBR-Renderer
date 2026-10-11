// history_tests.c
//
// Headless tests for undo/redo, transactions, batches, revisions and change
// events (Commands/History.h through Commands/Commands.h). The key promise
// checked throughout: undo and redo restore entities in place, so IDs stay
// valid, and an undone entity's ID never gets handed to a different one.

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

static asset_cache     g_assets;
static world           g_world;
static command_history g_history;
static command_context g_ctx;
static cJSON*          g_response;

static int  g_events;
static char g_last_source[128];

static void
on_change(void* user_data, const cJSON* event)
{
    (void)user_data;
    ++g_events;
    snprintf(g_last_source, sizeof(g_last_source), "%s", cJSON_GetObjectItem(event, "source")->valuestring);
}

// Fresh world + history for each test.
static void
reset(b8 with_history)
{
    history_shutdown(&g_history, &g_world);
    world_shutdown(&g_world);

    world_init(&g_world, &g_assets);
    history_init(&g_history);

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.w              = &g_world;
    g_ctx.history        = with_history ? &g_history : NULL;
    g_ctx.on_change      = on_change;
    g_events             = 0;
}

static b8
run(const char* request)
{
    cJSON_Delete(g_response);
    char* text = cmd_execute_text(&g_ctx, request);
    g_response = cJSON_Parse(text);
    cJSON_free(text);
    return g_response && cJSON_IsTrue(cJSON_GetObjectItem(g_response, "ok"));
}

static const cJSON* result(void)     { return cJSON_GetObjectItem(g_response, "result"); }
static const char*  error_code(void) { return cJSON_GetObjectItem(cJSON_GetObjectItem(g_response, "error"), "code")->valuestring; }
static b8 error_mentions(const char* text)
{
    const cJSON* m = cJSON_GetObjectItem(cJSON_GetObjectItem(g_response, "error"), "message");
    return m && strstr(m->valuestring, text);
}

static entity_id find(const char* name) { return world_find(&g_world, name); }

static void
test_undo_redo_keeps_ids(void)
{
    reset(true);

    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"lamp\",\"components\":{\"Light\":{\"intensity\":3}}}}"));
    entity_id lamp = find("lamp");

    // The response lists what changed.
    const cJSON* change = cJSON_GetArrayItem(cJSON_GetObjectItem(g_response, "changes"), 0);
    CHECK(strcmp(cJSON_GetObjectItem(change, "op")->valuestring, "created") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(change, "name")->valuestring, "lamp") == 0);

    CHECK(run("{\"cmd\":\"component.set\",\"args\":{\"entity\":\"lamp\",\"component\":\"Light\",\"fields\":{\"intensity\":9}}}"));
    CHECK(world_light(&g_world, lamp)->intensity == 9.0f);

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "undone")->valuestring, "component.set lamp") == 0);
    CHECK(world_light(&g_world, lamp)->intensity == 3.0f);   // same ID still works

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(!world_is_alive(&g_world, lamp));
    const cJSON* undone = cJSON_GetArrayItem(cJSON_GetObjectItem(result(), "changes"), 0);
    CHECK(strcmp(cJSON_GetObjectItem(undone, "op")->valuestring, "destroyed") == 0);

    CHECK(!run("{\"cmd\":\"history.undo\"}"));
    CHECK(strcmp(error_code(), CMD_ERROR_NOT_FOUND) == 0 && error_mentions("nothing to undo"));

    // Redo brings back the very same ID.
    CHECK(run("{\"cmd\":\"history.redo\"}"));
    CHECK(world_is_alive(&g_world, lamp) && find("lamp") == lamp);
    CHECK(run("{\"cmd\":\"history.redo\"}"));
    CHECK(world_light(&g_world, lamp)->intensity == 9.0f);
    CHECK(!run("{\"cmd\":\"history.redo\"}"));
}

static void
test_undone_ids_are_never_reused(void)
{
    reset(true);

    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"ghost\"}}"));
    entity_id ghost = find("ghost");
    CHECK(run("{\"cmd\":\"history.undo\"}"));

    // The slot is free again, but the new entity must get a different ID.
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"chair\"}}"));
    entity_id chair = find("chair");
    CHECK(chair != ghost);
    CHECK(!world_is_alive(&g_world, ghost));

    // And the new command dropped the redo step.
    CHECK(!run("{\"cmd\":\"history.redo\"}"));
}

static void
test_undo_destroy_restores_subtree(void)
{
    reset(true);

    CHECK(run("{\"cmd\":\"batch\",\"args\":{\"label\":\"set the table\",\"commands\":["
              "{\"cmd\":\"entity.create\",\"args\":{\"name\":\"table\",\"components\":{\"Transform\":{\"position\":[1,0,0]}}}},"
              "{\"cmd\":\"entity.create\",\"args\":{\"name\":\"cup\",\"parent\":\"table\"}},"
              "{\"cmd\":\"entity.create\",\"args\":{\"name\":\"saucer\",\"parent\":\"cup\",\"components\":{\"Light\":{}}}}]}}"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "results")) == 3);
    entity_id table = find("table"), cup = find("cup"), saucer = find("saucer");

    CHECK(run("{\"cmd\":\"entity.destroy\",\"args\":{\"entity\":\"table\"}}"));
    CHECK(world_count(&g_world) == 0);

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(world_count(&g_world) == 3);
    CHECK(find("table") == table && find("cup") == cup && find("saucer") == saucer);
    CHECK(world_parent(&g_world, cup) == table && world_parent(&g_world, saucer) == cup);
    CHECK(world_has(&g_world, saucer, COMPONENT_LIGHT));
    CHECK(world_transform(&g_world, table)->position[0] == 1.0f);
    CHECK(world_matrix(&g_world, saucer)[3][0] == 1.0f);   // world_update() ran

    // The whole batch is one step.
    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "undone")->valuestring, "set the table") == 0);
    CHECK(world_count(&g_world) == 0);
}

static void
test_name_swap_undo(void)
{
    reset(true);
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"a\"}}"));
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"b\"}}"));
    entity_id a = find("a"), b = find("b");

    // Swap the two names in one step, then undo it: the raw restore must
    // not trip over the momentary duplicate.
    CHECK(run("{\"cmd\":\"batch\",\"args\":{\"commands\":["
              "{\"cmd\":\"entity.update\",\"args\":{\"entity\":\"a\",\"name\":\"tmp\"}},"
              "{\"cmd\":\"entity.update\",\"args\":{\"entity\":\"b\",\"name\":\"a\"}},"
              "{\"cmd\":\"entity.update\",\"args\":{\"entity\":\"tmp\",\"name\":\"b\"}}]}}"));
    CHECK(find("a") == b && find("b") == a);

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(find("a") == a && find("b") == b);
}

static void
test_transactions(void)
{
    reset(true);

    CHECK(run("{\"cmd\":\"history.begin\",\"args\":{\"label\":\"add a reading corner\"}}"));
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"chair\"}}"));
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"lamp\"}}"));

    CHECK(!run("{\"cmd\":\"history.undo\"}"));
    CHECK(strcmp(error_code(), CMD_ERROR_CONFLICT) == 0);

    // Nested begin/commit pairs fold into the outer one.
    CHECK(run("{\"cmd\":\"history.begin\"}"));
    CHECK(run("{\"cmd\":\"component.set\",\"args\":{\"entity\":\"lamp\",\"component\":\"Light\",\"fields\":{}}}"));
    CHECK(run("{\"cmd\":\"history.commit\"}"));
    CHECK(cJSON_GetObjectItem(result(), "depth")->valuedouble == 1.0);

    CHECK(run("{\"cmd\":\"history.commit\"}"));
    CHECK(strcmp(cJSON_GetObjectItem(result(), "recorded")->valuestring, "add a reading corner") == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "changes")) == 2);

    CHECK(run("{\"cmd\":\"history.list\"}"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "steps")) == 1);

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(world_count(&g_world) == 0);

    CHECK(!run("{\"cmd\":\"history.commit\"}"));
    CHECK(error_mentions("no transaction"));

    // Rollback: everything since begin goes, nothing is recorded.
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"keep\"}}"));
    CHECK(run("{\"cmd\":\"history.begin\"}"));
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"temp\"}}"));
    CHECK(run("{\"cmd\":\"entity.destroy\",\"args\":{\"entity\":\"keep\"}}"));
    CHECK(run("{\"cmd\":\"history.rollback\"}"));
    CHECK(find("keep") != ENTITY_NULL && find("temp") == ENTITY_NULL);
    CHECK(strstr(g_last_source, "rollback"));

    CHECK(run("{\"cmd\":\"history.list\"}"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "steps")) == 1);   // just "keep"'s creation
}

static void
test_batch_failure_changes_nothing(void)
{
    reset(true);
    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"floor\"}}"));
    uint64 revision = g_ctx.revision;

    CHECK(!run("{\"cmd\":\"batch\",\"args\":{\"commands\":["
               "{\"cmd\":\"entity.create\",\"args\":{\"name\":\"rug\"}},"
               "{\"cmd\":\"entity.destroy\",\"args\":{\"entity\":\"floor\"}},"
               "{\"cmd\":\"component.set\",\"args\":{\"entity\":\"nope\",\"component\":\"Light\",\"fields\":{}}}]}}"));
    CHECK(strcmp(error_code(), CMD_ERROR_BATCH_FAILED) == 0);
    CHECK(error_mentions("commands[2]") && error_mentions("no entity named \"nope\""));

    CHECK(find("floor") != ENTITY_NULL && find("rug") == ENTITY_NULL);
    CHECK(strcmp(g_last_source, "batch rolled back") == 0);
    CHECK(g_ctx.revision > revision);   // listeners saw the changes and their undo

    CHECK(run("{\"cmd\":\"history.list\"}"));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "steps")) == 1);

    CHECK(!run("{\"cmd\":\"batch\",\"args\":{\"commands\":[{\"cmd\":\"history.undo\"}]}}"));
    CHECK(error_mentions("can't be used inside a batch"));

    CHECK(!run("{\"cmd\":\"batch\",\"args\":{\"commands\":[]}}"));
    CHECK(error_mentions("non-empty array"));
}

static void
test_revisions_and_events(void)
{
    reset(true);

    CHECK(run("{\"cmd\":\"scene.describe\"}"));   // read-only: no revision, no event
    CHECK(g_ctx.revision == 0 && g_events == 0);

    CHECK(run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"x\"}}"));
    CHECK(g_ctx.revision == 1 && g_events == 1);
    CHECK(strcmp(g_last_source, "entity.create x") == 0);

    CHECK(!run("{\"cmd\":\"entity.create\",\"args\":{\"components\":{\"Light\":{\"type\":\"laser\"}}}}"));
    CHECK(g_ctx.revision == 1 && g_events == 1);   // failed: nothing changed, nothing announced

    // A command that changes nothing doesn't count either.
    CHECK(run("{\"cmd\":\"component.set\",\"args\":{\"entity\":\"x\",\"component\":\"Transform\",\"fields\":{}}}"));
    CHECK(g_ctx.revision == 1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(g_response, "changes")) == 0);

    CHECK(run("{\"cmd\":\"history.undo\"}"));
    CHECK(g_ctx.revision == 2 && strcmp(g_last_source, "undo entity.create x") == 0);
}

static void
test_without_history(void)
{
    reset(false);

    CHECK(!run("{\"cmd\":\"history.undo\"}"));
    CHECK(strcmp(error_code(), CMD_ERROR_UNAVAILABLE) == 0);

    // Batches stay all-or-nothing without a history.
    CHECK(!run("{\"cmd\":\"batch\",\"args\":{\"commands\":["
               "{\"cmd\":\"entity.create\",\"args\":{\"name\":\"a\"}},"
               "{\"cmd\":\"entity.get\",\"args\":{\"entity\":\"zzz\"}}]}}"));
    CHECK(world_count(&g_world) == 0);
}

static void
test_step_limit(void)
{
    reset(true);
    for (int i = 0; i < HISTORY_MAX_STEPS + 10; ++i)
        run("{\"cmd\":\"entity.create\",\"args\":{\"name\":\"e\"}}");

    CHECK(g_history.step_count == HISTORY_MAX_STEPS);

    int undone = 0;
    while (run("{\"cmd\":\"history.undo\"}"))
        ++undone;
    CHECK(undone == HISTORY_MAX_STEPS);
    CHECK(world_count(&g_world) == 10);   // the oldest steps fell off the end
}

int
main(void)
{
    assets_init(&g_assets);
    world_init(&g_world, &g_assets);
    history_init(&g_history);

    test_undo_redo_keeps_ids();
    test_undone_ids_are_never_reused();
    test_undo_destroy_restores_subtree();
    test_name_swap_undo();
    test_transactions();
    test_batch_failure_changes_nothing();
    test_revisions_and_events();
    test_without_history();
    test_step_limit();

    cJSON_Delete(g_response);
    history_shutdown(&g_history, &g_world);
    world_shutdown(&g_world);
    assets_shutdown(&g_assets);

    printf("history_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
