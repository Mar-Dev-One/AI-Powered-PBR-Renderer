#pragma once

#include "../Core/Defines.h"
#include "../Commands/Commands.h"

// A localhost WebSocket server that lets other processes -- the MCP server,
// the in-app agent, a script -- run commands (Commands/Commands.h) in the
// running app, speaking JSON-RPC 2.0 (Commands/JsonRpc.h).
//
//   ws://127.0.0.1:<port>/?token=<token>
//
// Security: it only listens on 127.0.0.1, and a connection must present the
// per-run random token in its URL (and, if it is a browser, come from a
// localhost Origin), so other machines and arbitrary web pages can't drive
// the engine. The URL is published in a small discovery file -- by default
// ~/.pbr-renderer/server.json -- which clients read instead of being
// configured by hand; it is removed again when the server stops.
//
// Threading: none. server_poll() accepts, reads, runs and replies with
// non-blocking sockets from the caller's (main) thread, once per frame --
// commands have to run on the main thread anyway, where the World and the
// GL context live.

#define SERVER_DEFAULT_PORT  7468
#define SERVER_MAX_CLIENTS   16
#define SERVER_MAX_MESSAGE   (4u * 1024u * 1024u)   // larger messages close the connection (1009)

typedef struct server_config
{
    uint16      port;             // 0 = SERVER_DEFAULT_PORT; if that is taken, any free port
    const char* discovery_path;   // NULL = ~/.pbr-renderer/server.json
} server_config;

typedef struct command_server command_server;

// Starts listening. NULL (logged) if no socket could be opened.
command_server* server_start(const server_config* config);

// Accepts new connections, completes handshakes, runs every complete
// request against `ctx` and queues the replies, then sends what it can.
// Never blocks.
void server_poll(command_server* server, command_context* ctx);

// Sends `text` (one JSON-RPC message) to every connected client.
void server_broadcast(command_server* server, const char* text);

// Pushes a JSON-RPC notification -- {"jsonrpc": "2.0", "method": `method`,
// "params": `params`} -- to every connected client. `params` may be NULL.
void server_notify(command_server* server, const char* method, const cJSON* params);

// A cmd_change_fn (Commands.h) that broadcasts each change as a
// "scene.changed" notification whose params are the change event:
//   {"revision": n, "source": "place_model ball", "changes": [...]}
// Install with ctx->on_change = server_on_change, ctx->on_change_user = server.
// The client that caused a change gets the notification too, just before
// its own response.
#define SERVER_CHANGE_NOTIFICATION "scene.changed"
void server_on_change(void* server, const cJSON* event);

uint16      server_port(const command_server* server);
const char* server_url(const command_server* server);    // ws://127.0.0.1:<port>/?token=<token>
uint32      server_client_count(const command_server* server);

// Closes every connection and removes the discovery file. NULL is a no-op.
void server_stop(command_server* server);
