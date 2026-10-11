#include "CommandConsole.h"

#include "cimgui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char*
copy_string(const char* text)
{
    size_t length = strlen(text) + 1;
    char*  copy   = malloc(length);
    if (!copy)
        FATAL("console: out of memory");
    memcpy(copy, text, length);
    return copy;
}

static void
add_entry(command_console* console, console_entry_kind kind, const char* text)
{
    if (console->entry_count == CONSOLE_MAX_ENTRIES)
    {
        free(console->entries[console->entry_start].text);
        console->entry_start = (console->entry_start + 1) % CONSOLE_MAX_ENTRIES;
        console->entry_count--;
    }

    console_entry* entry = &console->entries[(console->entry_start + console->entry_count) % CONSOLE_MAX_ENTRIES];
    entry->kind = kind;
    entry->text = copy_string(text);
    console->entry_count++;
    console->scroll_to_bottom = true;
}

static void
clear_entries(command_console* console)
{
    for (uint32 i = 0; i < console->entry_count; ++i)
        free(console->entries[(console->entry_start + i) % CONSOLE_MAX_ENTRIES].text);
    console->entry_start = 0;
    console->entry_count = 0;
}

static void
remember_input(command_console* console, const char* line)
{
    // Skip repeats of the previous line.
    if (console->history_count > 0)
    {
        const char* last = console->history[(console->history_start + console->history_count - 1) % CONSOLE_MAX_HISTORY];
        if (strcmp(last, line) == 0)
            return;
    }

    if (console->history_count == CONSOLE_MAX_HISTORY)
    {
        free(console->history[console->history_start]);
        console->history_start = (console->history_start + 1) % CONSOLE_MAX_HISTORY;
        console->history_count--;
    }
    console->history[(console->history_start + console->history_count) % CONSOLE_MAX_HISTORY] = copy_string(line);
    console->history_count++;
}

void
console_init(command_console* console)
{
    memset(console, 0, sizeof(*console));
    console->history_cursor = -1;
    console->focus_input    = true;
    add_entry(console, CONSOLE_INFO, "Type a command, e.g. place_model {\"model\": \"builtin:cube\", \"size\": 0.5}. "
                                     "\"help\" lists them; Up/Down recall, Tab completes.");
}

void
console_shutdown(command_console* console)
{
    clear_entries(console);
    for (uint32 i = 0; i < console->history_count; ++i)
        free(console->history[(console->history_start + i) % CONSOLE_MAX_HISTORY]);
    memset(console, 0, sizeof(*console));
}

// --- Output formatting ------------------------------------------------------------

// "+ created lamp [Light, Transform]" lines for a "changes" array.
static void
log_changes(command_console* console, const cJSON* changes, console_entry_kind kind)
{
    const cJSON* change;
    cJSON_ArrayForEach(change, changes)
    {
        const char* op   = cJSON_GetStringValue(cJSON_GetObjectItem(change, "op"));
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(change, "name"));
        char        components[256] = "";

        const cJSON* c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(change, "components"))
        {
            size_t used = strlen(components);
            snprintf(components + used, sizeof(components) - used, "%s%s", used ? ", " : "", c->valuestring);
        }

        char line[512];
        char sign = op && op[0] == 'c' && op[1] == 'r' ? '+' : op && op[0] == 'd' ? '-' : '~';
        snprintf(line, sizeof(line), "  %c %s %s%s%s%s", sign, op ? op : "?", name ? name : "?",
                 components[0] ? " [" : "", components, components[0] ? "]" : "");
        add_entry(console, kind, line);
    }
}

static void
log_response(command_console* console, const cJSON* response)
{
    if (!cJSON_IsTrue(cJSON_GetObjectItem(response, "ok")))
    {
        const cJSON* error = cJSON_GetObjectItem(response, "error");
        char line[2048];
        snprintf(line, sizeof(line), "%s: %s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(error, "code")),
                 cJSON_GetStringValue(cJSON_GetObjectItem(error, "message")));
        add_entry(console, CONSOLE_ERROR, line);
        return;
    }

    // The result as indented JSON, minus the change list (shown compactly below).
    cJSON* result = cJSON_Duplicate(cJSON_GetObjectItem(response, "result"), true);
    cJSON_DeleteItemFromObject(result, "changes");
    if (cJSON_GetArraySize(result) > 0)
    {
        char* text = cJSON_Print(result);
        add_entry(console, CONSOLE_RESULT, text);
        cJSON_free(text);
    }
    else
    {
        add_entry(console, CONSOLE_RESULT, "ok");
    }
    cJSON_Delete(result);

    const cJSON* changes = cJSON_GetObjectItem(response, "changes");
    if (!changes)
        changes = cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "changes");
    log_changes(console, changes, CONSOLE_RESULT);
}

void
console_on_change(void* user_data, const cJSON* event)
{
    command_console* console = user_data;
    if (console->executing)
        return;   // our own command: its response already lists the changes

    char line[256];
    snprintf(line, sizeof(line), "[rev %.0f] %s", cJSON_GetNumberValue(cJSON_GetObjectItem(event, "revision")),
             cJSON_GetStringValue(cJSON_GetObjectItem(event, "source")));
    add_entry(console, CONSOLE_EVENT, line);
    log_changes(console, cJSON_GetObjectItem(event, "changes"), CONSOLE_EVENT);
}

// --- Commands ---------------------------------------------------------------------

static cJSON*
run_request(command_console* console, command_context* ctx, cJSON* request)
{
    console->executing = true;
    cJSON* response = cmd_execute(ctx, request);
    console->executing = false;
    return response;
}

static void
show_help(command_console* console, command_context* ctx, const char* topic)
{
    cJSON* request  = cJSON_Parse("{\"cmd\": \"commands.list\"}");
    cJSON* response = run_request(console, ctx, request);
    cJSON_Delete(request);

    const cJSON* commands = cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "commands");
    b8 found = false;

    const cJSON* c;
    cJSON_ArrayForEach(c, commands)
    {
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(c, "name"));
        const char* doc  = cJSON_GetStringValue(cJSON_GetObjectItem(c, "description"));

        if (!topic)
        {
            char line[512];
            snprintf(line, sizeof(line), "%-18s %s", name, doc);
            add_entry(console, CONSOLE_INFO, line);
            continue;
        }
        if (strcmp(name, topic) != 0)
            continue;

        found = true;
        char line[1024];
        snprintf(line, sizeof(line), "%s -- %s", name, doc);
        add_entry(console, CONSOLE_INFO, line);

        const cJSON* a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItem(c, "args"))
        {
            snprintf(line, sizeof(line), "  %s (%s%s): %s",
                     cJSON_GetStringValue(cJSON_GetObjectItem(a, "name")),
                     cJSON_GetStringValue(cJSON_GetObjectItem(a, "type")),
                     cJSON_IsTrue(cJSON_GetObjectItem(a, "required")) ? ", required" : "",
                     cJSON_GetStringValue(cJSON_GetObjectItem(a, "description")));
            add_entry(console, CONSOLE_INFO, line);
        }
    }

    if (topic && !found)
    {
        char line[256];
        snprintf(line, sizeof(line), "no command \"%s\" -- \"help\" lists them all", topic);
        add_entry(console, CONSOLE_ERROR, line);
    }
    cJSON_Delete(response);
}

void
console_submit(command_console* console, command_context* ctx, const char* line)
{
    while (isspace((unsigned char)*line))
        ++line;
    if (!*line)
        return;

    remember_input(console, line);
    add_entry(console, CONSOLE_INPUT, line);

    if (strcmp(line, "clear") == 0)
    {
        clear_entries(console);
        return;
    }
    if (strcmp(line, "help") == 0)
    {
        show_help(console, ctx, NULL);
        return;
    }
    if (strncmp(line, "help ", 5) == 0)
    {
        show_help(console, ctx, line + 5);
        return;
    }

    cJSON* request;
    if (line[0] == '{')
    {
        request = cJSON_Parse(line);
        if (!request)
        {
            add_entry(console, CONSOLE_ERROR, "not valid JSON");
            return;
        }
    }
    else
    {
        // "name {args}" -- the name runs to the first space.
        const char* space = line;
        while (*space && !isspace((unsigned char)*space))
            ++space;

        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(space - line), line);

        while (isspace((unsigned char)*space))
            ++space;

        cJSON* args = NULL;
        if (*space)
        {
            args = cJSON_Parse(space);
            if (!cJSON_IsObject(args))
            {
                cJSON_Delete(args);
                add_entry(console, CONSOLE_ERROR, "arguments must be a JSON object, e.g. {\"entity\": \"lamp\"}");
                return;
            }
        }

        request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "cmd", name);
        if (args)
            cJSON_AddItemToObject(request, "args", args);
    }

    cJSON* response = run_request(console, ctx, request);
    log_response(console, response);
    cJSON_Delete(response);
    cJSON_Delete(request);
}

// --- Input callbacks ----------------------------------------------------------------

static void
replace_input(ImGuiInputTextCallbackData* data, const char* text)
{
    ImGuiInputTextCallbackData_DeleteChars(data, 0, data->BufTextLen);
    ImGuiInputTextCallbackData_InsertChars(data, 0, text, NULL);
}

// Completes the command name being typed (only the first word).
static void
complete_command(command_console* console, ImGuiInputTextCallbackData* data)
{
    if (!console->completion_ctx)
        return;

    int word_end = 0;
    while (word_end < data->BufTextLen && !isspace((unsigned char)data->Buf[word_end]))
        ++word_end;
    if (data->CursorPos > word_end || word_end == 0)
        return;

    cJSON* request  = cJSON_Parse("{\"cmd\": \"commands.list\"}");
    cJSON* response = run_request(console, console->completion_ctx, request);
    cJSON_Delete(request);

    // Collect matches and their common prefix.
    char   common[128] = "";
    uint32 matches     = 0;
    char   listing[1024] = "";

    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "commands"))
    {
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(c, "name"));
        if (strncmp(name, data->Buf, (size_t)word_end) != 0)
            continue;

        if (matches++ == 0)
        {
            snprintf(common, sizeof(common), "%s", name);
        }
        else
        {
            size_t i = 0;
            while (common[i] && common[i] == name[i])
                ++i;
            common[i] = '\0';
        }

        size_t used = strlen(listing);
        snprintf(listing + used, sizeof(listing) - used, "%s%s", used ? "  " : "", name);
    }
    cJSON_Delete(response);

    if (matches == 0)
        return;

    ImGuiInputTextCallbackData_DeleteChars(data, 0, word_end);
    ImGuiInputTextCallbackData_InsertChars(data, 0, common, NULL);
    if (matches == 1 && data->BufTextLen == (int)strlen(common))
        ImGuiInputTextCallbackData_InsertChars(data, data->BufTextLen, " ", NULL);
    else if (matches > 1)
        add_entry(console, CONSOLE_INFO, listing);
}

static int
input_callback(ImGuiInputTextCallbackData* data)
{
    command_console* console = data->UserData;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        complete_command(console, data);
        return 0;
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && console->history_count > 0)
    {
        int previous = console->history_cursor;
        if (data->EventKey == ImGuiKey_UpArrow)
        {
            if (console->history_cursor == -1)
                console->history_cursor = (int)console->history_count - 1;
            else if (console->history_cursor > 0)
                console->history_cursor--;
        }
        else if (data->EventKey == ImGuiKey_DownArrow && console->history_cursor != -1)
        {
            if (++console->history_cursor >= (int)console->history_count)
                console->history_cursor = -1;
        }

        if (previous != console->history_cursor)
        {
            const char* text = console->history_cursor >= 0
                ? console->history[(console->history_start + (uint32)console->history_cursor) % CONSOLE_MAX_HISTORY]
                : "";
            replace_input(data, text);
        }
    }
    return 0;
}

// --- Window ------------------------------------------------------------------------

static ImVec4_c
entry_color(console_entry_kind kind)
{
    switch (kind)
    {
        case CONSOLE_INPUT:  return (ImVec4_c){ 0.55f, 0.80f, 1.00f, 1.0f };
        case CONSOLE_ERROR:  return (ImVec4_c){ 1.00f, 0.45f, 0.40f, 1.0f };
        case CONSOLE_EVENT:  return (ImVec4_c){ 0.75f, 0.70f, 1.00f, 1.0f };
        case CONSOLE_INFO:   return (ImVec4_c){ 0.65f, 0.65f, 0.65f, 1.0f };
        case CONSOLE_RESULT:
        default:             return (ImVec4_c){ 0.90f, 0.90f, 0.90f, 1.0f };
    }
}

void
console_draw(command_console* console, command_context* ctx, const char* window_title)
{
    const ImGuiViewport* viewport = igGetMainViewport();
    igSetNextWindowPos((ImVec2_c){ viewport->WorkPos.x + 10.0f, viewport->WorkPos.y + viewport->WorkSize.y * 0.62f },
                       ImGuiCond_FirstUseEver, (ImVec2_c){ 0.0f, 0.0f });
    igSetNextWindowSize((ImVec2_c){ viewport->WorkSize.x * 0.6f, viewport->WorkSize.y * 0.36f }, ImGuiCond_FirstUseEver);

    if (!igBegin(window_title, NULL, 0))
    {
        igEnd();
        return;
    }

    b8 can_undo = ctx->history && history_can_undo(ctx->history);
    b8 can_redo = ctx->history && history_can_redo(ctx->history);

    igBeginDisabled(!can_undo);
    if (igButton("Undo", (ImVec2_c){ 0, 0 }))
        console_submit(console, ctx, "history.undo");
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    igBeginDisabled(!can_redo);
    if (igButton("Redo", (ImVec2_c){ 0, 0 }))
        console_submit(console, ctx, "history.redo");
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (igButton("Clear", (ImVec2_c){ 0, 0 }))
        clear_entries(console);
    igSameLine(0.0f, -1.0f);
    igTextDisabled("revision %llu, %u entities", (unsigned long long)ctx->revision, world_count(ctx->w));

    // Log, filling everything above the input line.
    igBeginChild_Str("log", (ImVec2_c){ 0.0f, -igGetFrameHeightWithSpacing() }, ImGuiChildFlags_Borders, 0);
    for (uint32 i = 0; i < console->entry_count; ++i)
    {
        const console_entry* entry = &console->entries[(console->entry_start + i) % CONSOLE_MAX_ENTRIES];
        igPushStyleColor_Vec4(ImGuiCol_Text, entry_color(entry->kind));
        if (entry->kind == CONSOLE_INPUT)
        {
            igText("> %s", entry->text);
        }
        else
        {
            igTextUnformatted(entry->text, NULL);
        }
        igPopStyleColor(1);
    }
    if (console->scroll_to_bottom)
    {
        igSetScrollHereY(1.0f);
        console->scroll_to_bottom = false;
    }
    igEndChild();

    // Input line.
    console->completion_ctx = ctx;
    igPushItemWidth(-1.0f);
    if (console->focus_input)
    {
        igSetKeyboardFocusHere(0);
        console->focus_input = false;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory
                              | ImGuiInputTextFlags_CallbackCompletion;
    if (igInputText("##input", console->input, sizeof(console->input), flags, input_callback, console))
    {
        console_submit(console, ctx, console->input);
        console->input[0]       = '\0';
        console->history_cursor = -1;
        console->focus_input    = true;   // keep typing after Enter
    }
    igPopItemWidth();
    console->completion_ctx = NULL;

    igEnd();
}
