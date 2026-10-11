#include "JsonRpc.h"

#include <string.h>

static char*
error_response(const cJSON* id, int code, const char* message, const char* command_code)
{
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());

    cJSON* error = cJSON_AddObjectToObject(response, "error");
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    if (command_code)
        cJSON_AddStringToObject(cJSON_AddObjectToObject(error, "data"), "code", command_code);

    char* text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return text;
}

// The command layer's error codes, as JSON-RPC codes.
static int
map_error_code(const char* command_code)
{
    if (!command_code)                                     return JSONRPC_COMMAND_FAILED;
    if (strcmp(command_code, CMD_ERROR_BAD_REQUEST) == 0)     return JSONRPC_INVALID_REQUEST;
    if (strcmp(command_code, CMD_ERROR_UNKNOWN_COMMAND) == 0) return JSONRPC_METHOD_NOT_FOUND;
    if (strcmp(command_code, CMD_ERROR_INVALID_ARGS) == 0)    return JSONRPC_INVALID_PARAMS;
    return JSONRPC_COMMAND_FAILED;
}

char*
jsonrpc_handle(command_context* ctx, const char* message)
{
    cJSON* request = cJSON_Parse(message);
    if (!request)
        return error_response(NULL, JSONRPC_PARSE_ERROR, "invalid JSON", CMD_ERROR_BAD_REQUEST);

    if (cJSON_IsArray(request))
    {
        cJSON_Delete(request);
        return error_response(NULL, JSONRPC_INVALID_REQUEST,
                              "JSON-RPC batch arrays aren't supported; use the \"batch\" method", CMD_ERROR_BAD_REQUEST);
    }

    const cJSON* id      = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(request, "jsonrpc");
    const cJSON* method  = cJSON_GetObjectItemCaseSensitive(request, "method");
    const cJSON* params  = cJSON_GetObjectItemCaseSensitive(request, "params");

    // A valid id is a string or a number; anything else (or a missing
    // "jsonrpc": "2.0") makes the request invalid -- answered with id null.
    b8 id_ok = !id || cJSON_IsString(id) || cJSON_IsNumber(id) || cJSON_IsNull(id);
    if (!cJSON_IsObject(request) || !id_ok || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0
        || !cJSON_IsString(method) || (params && !cJSON_IsObject(params) && !cJSON_IsNull(params)))
    {
        char* text = error_response(id_ok ? id : NULL, JSONRPC_INVALID_REQUEST,
                                    "expected {\"jsonrpc\": \"2.0\", \"id\": ..., \"method\": \"<command>\", "
                                    "\"params\": {...}} (params by name, as an object)",
                                    CMD_ERROR_BAD_REQUEST);
        cJSON_Delete(request);
        return text;
    }

    // Translate to the command layer's request shape and run it.
    cJSON* command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "cmd", method->valuestring);
    if (params && !cJSON_IsNull(params))
        cJSON_AddItemToObject(command, "args", cJSON_Duplicate(params, true));

    cJSON* outcome = cmd_execute(ctx, command);
    cJSON_Delete(command);

    char* text = NULL;
    if (id)   // notifications get no response
    {
        if (cJSON_IsTrue(cJSON_GetObjectItem(outcome, "ok")))
        {
            cJSON* response = cJSON_CreateObject();
            cJSON_AddStringToObject(response, "jsonrpc", "2.0");
            cJSON_AddItemToObject(response, "id", cJSON_Duplicate(id, true));

            cJSON* result  = cJSON_DetachItemFromObject(outcome, "result");
            cJSON* changes = cJSON_DetachItemFromObject(outcome, "changes");
            if (changes && cJSON_IsObject(result) && !cJSON_GetObjectItem(result, "changes"))
                cJSON_AddItemToObject(result, "changes", changes);
            else
                cJSON_Delete(changes);

            cJSON_AddItemToObject(response, "result", result ? result : cJSON_CreateObject());
            text = cJSON_PrintUnformatted(response);
            cJSON_Delete(response);
        }
        else
        {
            const cJSON* error = cJSON_GetObjectItem(outcome, "error");
            const char*  code  = cJSON_GetStringValue(cJSON_GetObjectItem(error, "code"));
            const char*  msg   = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
            text = error_response(id, map_error_code(code), msg ? msg : "command failed", code);
        }
    }

    cJSON_Delete(outcome);
    cJSON_Delete(request);
    return text;
}

char*
jsonrpc_notification(const char* method, const cJSON* params)
{
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "jsonrpc", "2.0");
    cJSON_AddStringToObject(message, "method", method);
    if (params)
        cJSON_AddItemToObject(message, "params", cJSON_Duplicate(params, true));

    char* text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    return text;
}
