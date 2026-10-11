#pragma once

#include "../Core/Defines.h"
#include "../World/World.h"
#include "History.h"

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
//
// Changes: every successful command that changes the world adds
//   "changes": [ { "op": "created" | "changed" | "destroyed",
//                  "id": ..., "name": "lamp", "components": ["Light"] } ]
// to its response, bumps `revision`, calls `on_change` with
//   { "revision": n, "source": "component.set lamp", "changes": [...] }
// and -- with a history attached -- records an undo step. `history.begin` /
// `history.commit` group several commands into one step; `batch` runs a list
// of commands all-or-nothing as one step.

// Called after every change to the world, from whichever command made it
// (including undo, redo and rollbacks). `event` is only valid during the call.
typedef void (*cmd_change_fn)(void* user_data, const cJSON* event);

typedef struct command_context
{
    world*           w;
    command_history* history;          // optional: NULL disables the history.* commands
    uint64           revision;         // bumped once per change to the world
    cmd_change_fn    on_change;        // optional
    void*            on_change_user;

    uint32           batch_depth;      // internal: > 0 while a batch runs its commands
} command_context;

// Error codes ("code" in a failure response).
#define CMD_ERROR_BAD_REQUEST     "bad_request"       // not {"cmd": string, "args": object}
#define CMD_ERROR_UNKNOWN_COMMAND "unknown_command"
#define CMD_ERROR_INVALID_ARGS    "invalid_args"      // missing / wrong-typed argument
#define CMD_ERROR_NOT_FOUND       "not_found"         // entity or component doesn't exist
#define CMD_ERROR_INVALID_VALUE   "invalid_value"     // a component field was rejected
#define CMD_ERROR_CONFLICT        "conflict"          // not possible in the current state (e.g. undo inside a transaction)
#define CMD_ERROR_UNAVAILABLE     "unavailable"       // feature not enabled for this context (e.g. no history)
#define CMD_ERROR_BATCH_FAILED    "batch_failed"      // one command of a batch failed; nothing changed

// Runs one request. Never returns NULL; caller owns the result (cJSON_Delete).
cJSON* cmd_execute(command_context* ctx, const cJSON* request);

// Same, from and to JSON text -- for the console and the network server.
// Caller frees the result with cJSON_free().
char* cmd_execute_text(command_context* ctx, const char* request_json);
