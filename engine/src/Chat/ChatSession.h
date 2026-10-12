#pragma once

#include "../Core/Defines.h"
#include "../Commands/Commands.h"
#include "../Net/CommandServer.h"

// The in-app chat: one conversation between the user and an AI agent that
// builds the scene (docs/ROADMAP.md, Phase 5). The engine is the hub; the
// agent is a client of the command server (agent/pbr_agent/agent.py) that
// claimed the role "chat_agent" with server.hello:
//
//   panel / script --chat.send--> engine --"chat.request"--> agent
//   agent --chat.event (text, tool calls, done)--> engine --"chat.event"--> every client
//
// The agent changes the scene with ordinary commands over the same
// connection, so those changes reach every client as scene.changed events,
// exactly as an MCP client's do.
//
// Turns: chat.send starts one. The engine wraps it in a history transaction
// so the whole turn undoes as one step, and ends it itself -- when the agent
// reports "done" or "error", when Stop is pressed, or when the agent
// disconnects -- so a crashed agent can't leave the transaction (and with
// it undo) blocked. Only one turn runs at a time.
//
// Commands (install chat_commands() as a context's module table, with the
// session as module_user):
//   chat.send    {text}             starts a turn; fails while one runs or with no agent
//   chat.stop    {}                 ends the running turn now; the agent is told to stop
//   chat.undo_turn {}               undoes the last turn's changes ("Undo turn") and tells the agent
//   chat.event   {turn, type, ...}  the agent reports progress (CHAT_EVENT_* below)
//   chat.status  {model, problem}   the agent describes itself
//   chat.history {}                 the conversation so far, and whether a turn runs
//
// Everything that happens in the conversation is pushed to every client as a
// "chat.event" notification: {"turn": n, "type": ..., ...}.

#define CHAT_AGENT_ROLE       "chat_agent"
#define CHAT_REQUEST_METHOD   "chat.request"   // engine -> agent: {turn, text, notes?}
#define CHAT_STOP_METHOD      "chat.stop"      // engine -> agent: {turn}
#define CHAT_RESET_METHOD     "chat.reset"     // engine -> agent: forget the conversation
#define CHAT_EVENT_METHOD     "chat.event"     // engine -> every client

// chat.event types. From the agent:
#define CHAT_EVENT_TEXT        "text"          // {text}: more of the reply, streamed
#define CHAT_EVENT_PROGRESS    "progress"      // {text}: a note between tool calls
#define CHAT_EVENT_TOOL_CALL   "tool_call"     // {id, name, input}
#define CHAT_EVENT_TOOL_RESULT "tool_result"   // {id, ok, result}: result is a short summary
#define CHAT_EVENT_DONE        "done"          // {}: the turn is over
#define CHAT_EVENT_ERROR       "error"         // {message}: the turn failed and is over
// From the engine:
#define CHAT_EVENT_USER        "user"          // {text}: a turn started
#define CHAT_EVENT_STOPPED     "stopped"       // {}: Stop was pressed
#define CHAT_EVENT_AGENT_LOST  "agent_lost"    // {}: the agent disconnected mid-turn
#define CHAT_EVENT_UNDONE      "undone"        // {}: the turn's changes were undone

#define CHAT_MAX_ITEMS 1000

typedef enum chat_item_kind
{
    CHAT_ITEM_USER,
    CHAT_ITEM_ASSISTANT,
    CHAT_ITEM_PROGRESS,
    CHAT_ITEM_TOOL,
    CHAT_ITEM_ERROR,
    CHAT_ITEM_INFO
} chat_item_kind;

typedef enum chat_tool_state
{
    CHAT_TOOL_RUNNING,
    CHAT_TOOL_OK,
    CHAT_TOOL_FAILED
} chat_tool_state;

// One line of the conversation as the panel shows it.
typedef struct chat_item
{
    chat_item_kind  kind;
    uint32          turn;          // 0 for lines outside any turn
    char*           text;          // message text; for a tool call, the tool's name
    size_t          length;

    // CHAT_ITEM_TOOL only
    char            tool_id[64];
    char*           tool_input;    // JSON
    char*           tool_result;   // the agent's summary; NULL while running
    chat_tool_state tool_state;
} chat_item;

typedef struct chat_session
{
    command_server* server;        // may be NULL: then no agent is ever online

    chat_item*      items;         // oldest first
    uint32          item_count;
    uint32          item_capacity;
    uint64          version;       // bumped on every change, for redraw/scroll decisions

    uint32          last_turn;     // turns are numbered from 1
    uint32          active_turn;   // 0 = idle
    command_context* turn_ctx;     // where the running turn's transaction is open (NULL = none)

    // The last finished turn that changed the world, while undoing it is
    // still the next undo step of `undo_ctx`.
    uint32           undo_turn;
    command_context* undo_ctx;
    char             undo_label[HISTORY_LABEL_MAX];

    char pending_note[512];        // told to the agent with the next request

    b8   agent_online;             // as of the last chat_poll()
    char agent_model[64];
    char agent_problem[256];       // why the agent can't work (e.g. no API key); "" = fine
} chat_session;

void chat_init(chat_session* chat, command_server* server);

// Ends a running turn (keeping its changes) and frees everything. Call
// before the command contexts' histories are shut down.
void chat_shutdown(chat_session* chat);

// The chat.* commands, for command_context.module_commands.
const cmd_host_command* chat_commands(uint32* out_count);

// Once per frame: notices the agent connecting or leaving, and ends a turn
// whose agent has gone.
void chat_poll(chat_session* chat);

// Starts a turn running against `ctx`. False (with the reason in `error`)
// if a turn is already running or no agent is connected.
b8 chat_send(chat_session* chat, command_context* ctx, const char* text, char* error, size_t error_size);

// Ends the running turn now, keeping what it changed. No-op when idle.
void chat_stop(chat_session* chat);

b8 chat_busy(const chat_session* chat);
b8 chat_agent_online(const chat_session* chat);

// "Undo turn": true while the last finished turn's changes are still the
// next undo step and nothing is running.
b8 chat_can_undo_turn(const chat_session* chat);
b8 chat_undo_turn(chat_session* chat, char* error, size_t error_size);

// Forgets the conversation (here and in the agent). No-op while a turn runs.
void chat_clear(chat_session* chat);

// Adds a grey informational line, e.g. about starting the agent.
void chat_info(chat_session* chat, const char* text);
