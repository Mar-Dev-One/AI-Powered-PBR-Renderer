#include "ChatSession.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Items ---------------------------------------------------------------------------

static char*
copy_string(const char* text)
{
    size_t length = strlen(text) + 1;
    char*  copy   = malloc(length);
    if (!copy)
        FATAL("chat: out of memory");
    memcpy(copy, text, length);
    return copy;
}

static void
free_item(chat_item* item)
{
    free(item->text);
    free(item->tool_input);
    free(item->tool_result);
}

static chat_item*
add_item(chat_session* chat, chat_item_kind kind, uint32 turn, const char* text)
{
    if (chat->item_count == CHAT_MAX_ITEMS)
    {
        // Drop the oldest line. The conversation the agent remembers is its
        // own; this is only what the panel can scroll back to.
        free_item(&chat->items[0]);
        memmove(chat->items, chat->items + 1, sizeof(chat_item) * (chat->item_count - 1));
        chat->item_count--;
    }
    if (chat->item_count == chat->item_capacity)
    {
        uint32     capacity = chat->item_capacity ? chat->item_capacity * 2 : 64;
        chat_item* grown    = realloc(chat->items, sizeof(chat_item) * capacity);
        if (!grown)
            FATAL("chat: out of memory");
        chat->items         = grown;
        chat->item_capacity = capacity;
    }

    chat_item* item = &chat->items[chat->item_count++];
    memset(item, 0, sizeof(*item));
    item->kind   = kind;
    item->turn   = turn;
    item->text   = copy_string(text);
    item->length = strlen(text);
    chat->version++;
    return item;
}

static void
append_text(chat_session* chat, chat_item* item, const char* text)
{
    size_t more  = strlen(text);
    char*  grown = realloc(item->text, item->length + more + 1);
    if (!grown)
        FATAL("chat: out of memory");
    memcpy(grown + item->length, text, more + 1);
    item->text    = grown;
    item->length += more;
    chat->version++;
}

static chat_item*
find_tool_item(chat_session* chat, uint32 turn, const char* id)
{
    for (uint32 i = chat->item_count; i-- > 0;)
    {
        chat_item* item = &chat->items[i];
        if (item->turn != turn)
            break;
        if (item->kind == CHAT_ITEM_TOOL && strcmp(item->tool_id, id) == 0)
            return item;
    }
    return NULL;
}

// --- Notifications -----------------------------------------------------------------------

// Pushes {"turn": turn, "type": type, ...fields} to every client as chat.event.
// Takes ownership of `fields` (may be NULL).
static void
broadcast(chat_session* chat, uint32 turn, const char* type, cJSON* fields)
{
    cJSON* params = fields ? fields : cJSON_CreateObject();
    cJSON_DeleteItemFromObject(params, "turn");
    cJSON_DeleteItemFromObject(params, "type");
    cJSON_AddNumberToObject(params, "turn", turn);
    cJSON_AddStringToObject(params, "type", type);

    server_notify(chat->server, CHAT_EVENT_METHOD, params);
    cJSON_Delete(params);
}

// --- Turns ---------------------------------------------------------------------------------

// Closes the running turn's transaction and remembers it for "Undo turn" if
// it recorded a step. `event` is broadcast unless NULL (the agent's own
// done/error events are rebroadcast by their handler); `message`, if given,
// is added to the conversation as `kind`.
static void
end_turn(chat_session* chat, const char* event, chat_item_kind kind, const char* message)
{
    if (!chat->active_turn)
        return;

    uint32 turn = chat->active_turn;
    if (message)
        add_item(chat, kind, turn, message);

    command_context* ctx = chat->turn_ctx;
    if (ctx && ctx->history && history_in_transaction(ctx->history))
    {
        char label[HISTORY_LABEL_MAX];
        snprintf(label, sizeof(label), "%s", ctx->history->label);

        const history_change* changes;
        uint32                count = 0;
        history_commit(ctx->history, ctx->w, &changes, &count);

        if (ctx->history->depth == 0 && count > 0)
        {
            chat->undo_turn = turn;
            chat->undo_ctx  = ctx;
            snprintf(chat->undo_label, sizeof(chat->undo_label), "%s", label);
        }
    }

    chat->active_turn = 0;
    chat->turn_ctx    = NULL;
    chat->version++;

    if (event)
        broadcast(chat, turn, event, NULL);
}

b8
chat_send(chat_session* chat, command_context* ctx, const char* text, char* error, size_t error_size)
{
    while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')
        ++text;

    if (!*text)
    {
        snprintf(error, error_size, "the message is empty");
        return false;
    }
    if (chat->active_turn)
    {
        snprintf(error, error_size, "the agent is still working on the last message; wait or stop it first");
        return false;
    }
    if (!server_has_role(chat->server, CHAT_AGENT_ROLE))
    {
        snprintf(error, error_size, "no chat agent is connected (start agent/pbr_agent/agent.py)");
        return false;
    }

    uint32 turn = ++chat->last_turn;

    cJSON* request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "turn", turn);
    cJSON_AddStringToObject(request, "text", text);
    if (chat->pending_note[0])
        cJSON_AddStringToObject(request, "notes", chat->pending_note);
    b8 sent = server_notify_role(chat->server, CHAT_AGENT_ROLE, CHAT_REQUEST_METHOD, request);
    cJSON_Delete(request);

    if (!sent)
    {
        snprintf(error, error_size, "the chat agent couldn't be reached");
        return false;
    }

    chat->pending_note[0] = '\0';
    chat->active_turn     = turn;
    chat->turn_ctx        = ctx;
    chat->undo_ctx        = NULL;   // a new turn replaces the one "Undo turn" offered

    // The whole turn is one undo step, labelled with what was asked.
    if (ctx->history)
    {
        char label[HISTORY_LABEL_MAX];
        snprintf(label, sizeof(label), "chat: %s", text);
        for (char* c = label; *c; ++c)
        {
            if (*c == '\n' || *c == '\r' || *c == '\t')
                *c = ' ';
        }
        history_begin(ctx->history, ctx->w, label);
    }

    add_item(chat, CHAT_ITEM_USER, turn, text);

    cJSON* fields = cJSON_CreateObject();
    cJSON_AddStringToObject(fields, "text", text);
    broadcast(chat, turn, CHAT_EVENT_USER, fields);
    return true;
}

void
chat_stop(chat_session* chat)
{
    if (!chat->active_turn)
        return;

    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "turn", chat->active_turn);
    server_notify_role(chat->server, CHAT_AGENT_ROLE, CHAT_STOP_METHOD, params);
    cJSON_Delete(params);

    end_turn(chat, CHAT_EVENT_STOPPED, CHAT_ITEM_INFO, "Stopped. What was already done stays (Undo turn reverts it).");
}

void
chat_poll(chat_session* chat)
{
    b8 online = server_has_role(chat->server, CHAT_AGENT_ROLE);
    if (online == chat->agent_online)
        return;

    chat->agent_online = online;
    chat->version++;
    if (!online)
    {
        chat->agent_model[0]   = '\0';
        chat->agent_problem[0] = '\0';
        end_turn(chat, CHAT_EVENT_AGENT_LOST, CHAT_ITEM_ERROR,
                 "The agent disconnected before it finished. What it already did stays (Undo turn reverts it).");
    }
}

b8
chat_busy(const chat_session* chat)
{
    return chat->active_turn != 0;
}

b8
chat_agent_online(const chat_session* chat)
{
    return chat->agent_online;
}

b8
chat_can_undo_turn(const chat_session* chat)
{
    const command_context* ctx = chat->undo_ctx;
    if (chat->active_turn || !ctx || !ctx->history || history_in_transaction(ctx->history))
        return false;

    const command_history* h = ctx->history;
    return h->cursor > 0 && strcmp(h->steps[h->cursor - 1].label, chat->undo_label) == 0;
}

b8
chat_undo_turn(chat_session* chat, char* error, size_t error_size)
{
    if (!chat_can_undo_turn(chat))
    {
        snprintf(error, error_size, "the last turn's changes are no longer the latest undo step");
        return false;
    }

    // Through the command layer, so the undo is announced like any other.
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "history.undo");
    cJSON* response = cmd_execute(chat->undo_ctx, request);
    cJSON_Delete(request);

    b8 ok = cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
    if (!ok)
    {
        const cJSON* message = cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message");
        snprintf(error, error_size, "%s", cJSON_IsString(message) ? message->valuestring : "undo failed");
    }
    cJSON_Delete(response);

    if (ok)
    {
        uint32 turn = chat->undo_turn;
        chat->undo_ctx = NULL;
        add_item(chat, CHAT_ITEM_INFO, turn, "Undid the changes from that turn.");
        snprintf(chat->pending_note, sizeof(chat->pending_note),
                 "The user pressed Undo turn: everything you changed in the previous turn has been undone, "
                 "so the scene is back to how it was before that request.");
        broadcast(chat, turn, CHAT_EVENT_UNDONE, NULL);
    }
    return ok;
}

void
chat_clear(chat_session* chat)
{
    if (chat->active_turn)
        return;

    for (uint32 i = 0; i < chat->item_count; ++i)
        free_item(&chat->items[i]);
    chat->item_count      = 0;
    chat->undo_ctx        = NULL;
    chat->pending_note[0] = '\0';
    chat->version++;

    server_notify_role(chat->server, CHAT_AGENT_ROLE, CHAT_RESET_METHOD, NULL);
}

void
chat_info(chat_session* chat, const char* text)
{
    add_item(chat, CHAT_ITEM_INFO, 0, text);
}

void
chat_init(chat_session* chat, command_server* server)
{
    memset(chat, 0, sizeof(*chat));
    chat->server = server;
}

void
chat_shutdown(chat_session* chat)
{
    end_turn(chat, NULL, CHAT_ITEM_INFO, NULL);
    for (uint32 i = 0; i < chat->item_count; ++i)
        free_item(&chat->items[i]);
    free(chat->items);
    memset(chat, 0, sizeof(*chat));
}

// --- Commands ------------------------------------------------------------------------------

static const char*
string_arg(const cJSON* args, const char* name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, name));
}

static b8
cmd_chat_send(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    chat_session* chat = user_data;
    const char*   text = string_arg(args, "text");
    if (!text)
    {
        snprintf(error, error_size, "\"text\" must be the message, as a string");
        return false;
    }
    if (!chat_send(chat, ctx, text, error, error_size))
        return false;

    *out_result = cJSON_CreateObject();
    cJSON_AddNumberToObject(*out_result, "turn", chat->active_turn);
    return true;
}

static b8
cmd_chat_stop(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    chat_session* chat = user_data;

    uint32 turn = chat->active_turn;
    chat_stop(chat);

    *out_result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out_result, "stopped", turn != 0);
    return true;
}

static b8
cmd_chat_undo_turn(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)out_result;
    return chat_undo_turn(user_data, error, error_size);
}

static b8
cmd_chat_event(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)out_result;
    chat_session* chat = user_data;

    const cJSON* turn_item = cJSON_GetObjectItemCaseSensitive(args, "turn");
    const char*  type      = string_arg(args, "type");
    if (!cJSON_IsNumber(turn_item) || !type)
    {
        snprintf(error, error_size, "chat.event needs \"turn\" (number) and \"type\" (string)");
        return false;
    }

    uint32 turn = (uint32)turn_item->valuedouble;
    if (turn == 0 || turn != chat->active_turn)
    {
        // Normal after Stop: the agent may still be finishing a sentence.
        snprintf(error, error_size, "turn %u isn't running (it was stopped or has ended)", turn);
        return false;
    }

    const char* text = string_arg(args, "text");
    const char* id   = string_arg(args, "id");

    if (strcmp(type, CHAT_EVENT_TEXT) == 0 || strcmp(type, CHAT_EVENT_PROGRESS) == 0)
    {
        if (!text)
        {
            snprintf(error, error_size, "\"%s\" events need \"text\"", type);
            return false;
        }

        // Streamed text continues the reply it belongs to, unless something
        // (a tool call) came in between.
        b8         is_text = strcmp(type, CHAT_EVENT_TEXT) == 0;
        chat_item* last    = chat->item_count ? &chat->items[chat->item_count - 1] : NULL;
        if (is_text && last && last->turn == turn && last->kind == CHAT_ITEM_ASSISTANT)
            append_text(chat, last, text);
        else
            add_item(chat, is_text ? CHAT_ITEM_ASSISTANT : CHAT_ITEM_PROGRESS, turn, text);
    }
    else if (strcmp(type, CHAT_EVENT_TOOL_CALL) == 0)
    {
        const char*  name  = string_arg(args, "name");
        const cJSON* input = cJSON_GetObjectItemCaseSensitive(args, "input");
        if (!id || !name)
        {
            snprintf(error, error_size, "\"tool_call\" events need \"id\" and \"name\"");
            return false;
        }

        chat_item* item = add_item(chat, CHAT_ITEM_TOOL, turn, name);
        snprintf(item->tool_id, sizeof(item->tool_id), "%s", id);
        item->tool_input = input ? cJSON_PrintUnformatted(input) : copy_string("{}");
        item->tool_state = CHAT_TOOL_RUNNING;
    }
    else if (strcmp(type, CHAT_EVENT_TOOL_RESULT) == 0)
    {
        chat_item* item = id ? find_tool_item(chat, turn, id) : NULL;
        if (!item)
        {
            snprintf(error, error_size, "no tool call \"%s\" in turn %u", id ? id : "?", turn);
            return false;
        }

        const cJSON* ok     = cJSON_GetObjectItemCaseSensitive(args, "ok");
        const char*  result = string_arg(args, "result");
        free(item->tool_result);
        item->tool_result = copy_string(result ? result : "");
        item->tool_state  = cJSON_IsFalse(ok) ? CHAT_TOOL_FAILED : CHAT_TOOL_OK;
        chat->version++;
    }
    else if (strcmp(type, CHAT_EVENT_DONE) == 0)
    {
        end_turn(chat, NULL, CHAT_ITEM_INFO, NULL);
    }
    else if (strcmp(type, CHAT_EVENT_ERROR) == 0)
    {
        const char* message = string_arg(args, "message");
        end_turn(chat, NULL, CHAT_ITEM_ERROR, message ? message : "The agent failed.");
    }
    else
    {
        snprintf(error, error_size, "unknown event type \"%s\"; expected text, progress, tool_call, tool_result, "
                 "done or error", type);
        return false;
    }

    // Everyone else sees it too.
    broadcast(chat, turn, type, cJSON_Duplicate(args, true));
    return true;
}

static b8
cmd_chat_status(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)out_result; (void)error; (void)error_size;
    chat_session* chat = user_data;

    const char* model   = string_arg(args, "model");
    const char* problem = string_arg(args, "problem");
    snprintf(chat->agent_model, sizeof(chat->agent_model), "%s", model ? model : "");
    snprintf(chat->agent_problem, sizeof(chat->agent_problem), "%s", problem ? problem : "");
    chat->version++;
    return true;
}

static const char* const KIND_NAMES[] = { "user", "assistant", "progress", "tool", "error", "info" };
static const char* const TOOL_STATES[] = { "running", "ok", "failed" };

static b8
cmd_chat_history(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)ctx; (void)args; (void)error; (void)error_size;
    chat_session* chat = user_data;

    *out_result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out_result, "agent_online", chat->agent_online);
    cJSON_AddNumberToObject(*out_result, "active_turn", chat->active_turn);
    cJSON_AddBoolToObject(*out_result, "can_undo_turn", chat_can_undo_turn(chat));

    cJSON* items = cJSON_AddArrayToObject(*out_result, "items");
    for (uint32 i = 0; i < chat->item_count; ++i)
    {
        const chat_item* item = &chat->items[i];
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "kind", KIND_NAMES[item->kind]);
        cJSON_AddNumberToObject(entry, "turn", item->turn);
        cJSON_AddStringToObject(entry, "text", item->text);
        if (item->kind == CHAT_ITEM_TOOL)
        {
            cJSON_AddStringToObject(entry, "id", item->tool_id);
            cJSON_AddStringToObject(entry, "state", TOOL_STATES[item->tool_state]);
            if (item->tool_result)
                cJSON_AddStringToObject(entry, "result", item->tool_result);
        }
        cJSON_AddItemToArray(items, entry);
    }
    return true;
}

static const cmd_arg ARGS_SEND[] = {
    { "text", "string", true, "What to ask the agent" },
};

static const cmd_arg ARGS_EVENT[] = {
    { "turn",    "number",  true,  "The turn this belongs to (from chat.request)" },
    { "type",    "string",  true,  "text, progress, tool_call, tool_result, done or error" },
    { "text",    "string",  false, "text/progress: the text" },
    { "id",      "string",  false, "tool_call/tool_result: the call's id" },
    { "name",    "string",  false, "tool_call: the tool's name" },
    { "input",   "object",  false, "tool_call: its arguments" },
    { "ok",      "boolean", false, "tool_result: whether it succeeded" },
    { "result",  "string",  false, "tool_result: a short summary of the result" },
    { "message", "string",  false, "error: what went wrong" },
};

static const cmd_arg ARGS_STATUS[] = {
    { "model",   "string", false, "The model the agent uses" },
    { "problem", "string", false, "Why the agent can't work right now (e.g. no API key); omit when it can" },
};

#define CHAT_ARGS(array) array, (uint32)(sizeof(array) / sizeof((array)[0]))

static const cmd_host_command CHAT_COMMANDS[] = {
    { "chat.send",      "Asks the chat agent to do something; the whole turn is one undo step",     CHAT_ARGS(ARGS_SEND),   false, cmd_chat_send },
    { "chat.stop",      "Stops the chat agent's running turn, keeping what it already did",         NULL, 0,                false, cmd_chat_stop },
    { "chat.undo_turn", "Undoes everything the chat agent changed in its last turn, and tells it so", NULL, 0,              false, cmd_chat_undo_turn },
    { "chat.event",     "For the chat agent: reports a turn's progress to the app",                 CHAT_ARGS(ARGS_EVENT),  false, cmd_chat_event },
    { "chat.status",    "For the chat agent: describes it (model, or why it can't work)",           CHAT_ARGS(ARGS_STATUS), false, cmd_chat_status },
    { "chat.history",   "The chat conversation so far, and whether a turn is running",              NULL, 0,                false, cmd_chat_history },
};

const cmd_host_command*
chat_commands(uint32* out_count)
{
    *out_count = (uint32)(sizeof(CHAT_COMMANDS) / sizeof(CHAT_COMMANDS[0]));
    return CHAT_COMMANDS;
}
