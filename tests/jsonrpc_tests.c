// jsonrpc_tests.c
//
// Headless tests for Commands/JsonRpc.h: request validation, success and
// error mapping (standard JSON-RPC codes plus the command layer's own code
// in error.data), "changes" placement, and notifications.

#include "Commands/JsonRpc.h"

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
static cJSON*          g_response;

// Sends `message`; false if no response came back (a notification).
static b8
call(const char* message)
{
    cJSON_Delete(g_response);
    g_response = NULL;

    char* text = jsonrpc_handle(&g_ctx, message);
    if (!text)
        return false;

    g_response = cJSON_Parse(text);
    cJSON_free(text);
    return true;
}

static const cJSON* result(void)     { return cJSON_GetObjectItem(g_response, "result"); }
static const cJSON* error(void)      { return cJSON_GetObjectItem(g_response, "error"); }
static int          error_code(void) { return (int)cJSON_GetObjectItem(error(), "code")->valuedouble; }
static const char*  data_code(void)  { return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(error(), "data"), "code")); }

int
main(void)
{
    asset_cache assets;
    world       w;
    assets_init(&assets);
    world_init(&w, &assets);
    g_ctx.w = &w;

    // Success: the command's result, with its changes alongside.
    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"entity.create\",\"params\":{\"name\":\"lamp\"}}"));
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g_response, "jsonrpc")), "2.0") == 0);
    CHECK(cJSON_GetObjectItem(g_response, "id")->valuedouble == 1.0);
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(result(), "name")), "lamp") == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(result(), "changes")) == 1);
    CHECK(error() == NULL);

    // String ids round-trip; missing params is fine.
    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"scene.describe\"}"));
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(g_response, "id")), "abc") == 0);
    CHECK(cJSON_GetObjectItem(result(), "entities") != NULL);

    // Error mapping.
    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"entity.get\",\"params\":{\"entity\":\"sofa\"}}"));
    CHECK(error_code() == JSONRPC_COMMAND_FAILED && strcmp(data_code(), CMD_ERROR_NOT_FOUND) == 0);
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(error(), "message")), "lamp") != NULL);   // lists what exists
    CHECK(result() == NULL);

    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"entity.fly\"}"));
    CHECK(error_code() == JSONRPC_METHOD_NOT_FOUND);

    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"entity.get\",\"params\":{\"entitiy\":\"lamp\"}}"));
    CHECK(error_code() == JSONRPC_INVALID_PARAMS);

    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"entity.get\",\"params\":[\"lamp\"]}"));
    CHECK(error_code() == JSONRPC_INVALID_REQUEST);

    CHECK(call("{\"id\":6,\"method\":\"scene.describe\"}"));   // no "jsonrpc": "2.0"
    CHECK(error_code() == JSONRPC_INVALID_REQUEST);
    CHECK(cJSON_GetObjectItem(g_response, "id")->valuedouble == 6.0);

    CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"scene.describe\"}"));   // bad id type
    CHECK(error_code() == JSONRPC_INVALID_REQUEST && cJSON_IsNull(cJSON_GetObjectItem(g_response, "id")));

    CHECK(call("{oops"));
    CHECK(error_code() == JSONRPC_PARSE_ERROR && cJSON_IsNull(cJSON_GetObjectItem(g_response, "id")));

    CHECK(call("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"scene.describe\"}]"));
    CHECK(error_code() == JSONRPC_INVALID_REQUEST);

    // Notifications run but get no response.
    CHECK(!call("{\"jsonrpc\":\"2.0\",\"method\":\"entity.create\",\"params\":{\"name\":\"quiet\"}}"));
    CHECK(world_find(&w, "quiet") != ENTITY_NULL);

    // Server-pushed notifications.
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "revision", 3);
    char* note = jsonrpc_notification("scene.changed", params);
    cJSON* parsed = cJSON_Parse(note);
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(parsed, "method")), "scene.changed") == 0);
    CHECK(cJSON_GetObjectItem(parsed, "id") == NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(parsed, "params"), "revision")->valuedouble == 3.0);
    cJSON_Delete(parsed);
    cJSON_free(note);
    cJSON_Delete(params);

    cJSON_Delete(g_response);
    world_shutdown(&w);
    assets_shutdown(&assets);

    printf("jsonrpc_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
