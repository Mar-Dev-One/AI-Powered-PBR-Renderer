#pragma once

#include "Commands.h"

// JSON-RPC 2.0 on top of cmd_execute() -- the wire format of the command
// server (Net/CommandServer.h), so any JSON-RPC client library can drive
// the engine. A command name is the method, its arguments are the params:
//
//   -> {"jsonrpc": "2.0", "id": 7, "method": "entity.get", "params": {"entity": "lamp"}}
//   <- {"jsonrpc": "2.0", "id": 7, "result": { ...entity..., "changes": [...] }}
//   <- {"jsonrpc": "2.0", "id": 7, "error": {"code": -32001, "message": "no entity named ...",
//                                           "data": {"code": "not_found"}}}
//
// "result" is the command's result object; a command that changed the
// world also carries its "changes" list there. Errors use the standard
// codes where they fit (-32700 parse error, -32600 invalid request, -32601
// unknown method, -32602 invalid params) and -32001 for the rest; the
// command layer's own error code ("not_found", "batch_failed", ...) is in
// error.data.code either way. A request without "id" is a notification: it
// runs, and nothing is sent back. JSON-RPC batch arrays aren't supported --
// the "batch" method does the same, all-or-nothing.

#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602
#define JSONRPC_COMMAND_FAILED   -32001

// Handles one JSON-RPC message. Returns the response text (caller frees
// with cJSON_free), or NULL for a notification.
char* jsonrpc_handle(command_context* ctx, const char* message);

// A notification to push to clients unprompted, e.g. method "scene.changed".
// Caller frees with cJSON_free.
char* jsonrpc_notification(const char* method, const cJSON* params);
