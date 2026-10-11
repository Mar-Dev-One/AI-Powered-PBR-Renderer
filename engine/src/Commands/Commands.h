#pragma once

#include "../Core/Defines.h"
#include "../World/World.h"

#include "cJSON.h"

// The one way anything outside the engine -- the debug console, the command
// server, an MCP tool, the in-app agent -- changes a World. Every request is
// JSON in, JSON out:
//
//   request:  { "cmd": "entity.create", "args": { "name": "lamp", ... } }
//   success:  { "ok": true,  "result": { ... } }
//   failure:  { "ok": false, "error": { "code": "invalid_value", "message": "..." } }
//
// A failed command changes nothing. Error messages are written for the
// caller to act on: they name the argument or field at fault and list the
// accepted values or the existing entities.
//
// Entity arguments accept a name ("lamp") or a numeric id; results report
// both. Component data uses World/Serialize.h's format.
//
// `commands.list` describes every command and its arguments, so help text
// and tool schemas are generated from the same table the dispatcher uses.

typedef struct command_context
{
    world* w;
} command_context;

// Error codes ("code" in a failure response).
#define CMD_ERROR_BAD_REQUEST     "bad_request"       // not {"cmd": string, "args": object}
#define CMD_ERROR_UNKNOWN_COMMAND "unknown_command"
#define CMD_ERROR_INVALID_ARGS    "invalid_args"      // missing / wrong-typed argument
#define CMD_ERROR_NOT_FOUND       "not_found"         // entity or component doesn't exist
#define CMD_ERROR_INVALID_VALUE   "invalid_value"     // a component field was rejected

// Runs one request. Never returns NULL; caller owns the result (cJSON_Delete).
cJSON* cmd_execute(command_context* ctx, const cJSON* request);

// Same, from and to JSON text -- for the console and the network server.
// Caller frees the result with cJSON_free().
char* cmd_execute_text(command_context* ctx, const char* request_json);
