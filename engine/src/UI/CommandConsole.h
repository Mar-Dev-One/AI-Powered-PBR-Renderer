#pragma once

#include "../Commands/Commands.h"

// An ImGui window for typing commands (Commands/Commands.h) by hand: the
// same JSON requests the command server and the AI tools will send, so
// anything they can do can be tried here first.
//
// Input forms:
//   place_model {"model": "builtin:cube", "on_ground": true}
//   scene.describe                       (no arguments)
//   {"cmd": "entity.get", "args": {"entity": "lamp"}}   (a full request)
//   help / help <command> / clear
// Up/Down recall earlier input; Tab completes command names.
//
// Change events from other sources (a command server, an agent) are shown
// too, if console_on_change is installed as the context's on_change.

#define CONSOLE_MAX_ENTRIES 400
#define CONSOLE_MAX_HISTORY 64
#define CONSOLE_INPUT_MAX   2048

typedef enum console_entry_kind
{
    CONSOLE_INPUT,
    CONSOLE_RESULT,
    CONSOLE_ERROR,
    CONSOLE_EVENT,
    CONSOLE_INFO
} console_entry_kind;

typedef struct console_entry
{
    console_entry_kind kind;
    char*              text;
} console_entry;

typedef struct command_console
{
    console_entry entries[CONSOLE_MAX_ENTRIES];   // ring buffer
    uint32        entry_start;
    uint32        entry_count;

    char*  history[CONSOLE_MAX_HISTORY];          // ring buffer of past input, oldest first
    uint32 history_start;
    uint32 history_count;
    int    history_cursor;                        // -1 = editing a new line

    char input[CONSOLE_INPUT_MAX];
    b8   scroll_to_bottom;
    b8   executing;                               // own commands' events are shown via their responses
    b8   focus_input;

    command_context* completion_ctx;              // for Tab completion, valid during console_draw()
} command_console;

void console_init(command_console* console);
void console_shutdown(command_console* console);

// Draws the window and runs whatever is submitted against `ctx`.
void console_draw(command_console* console, command_context* ctx, const char* window_title);

// Runs one line of input exactly as if typed.
void console_submit(command_console* console, command_context* ctx, const char* line);

// A cmd_change_fn: install with ctx->on_change = console_on_change and
// ctx->on_change_user = console to see changes made by anything else.
void console_on_change(void* console, const cJSON* event);
