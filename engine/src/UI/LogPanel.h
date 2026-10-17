#pragma once

#include "../Core/Defines.h"

// An ImGui window showing the engine's log (Core/Defines.h's LOG_* and
// FATAL) as it happens: the last LOG_PANEL_MAX_ENTRIES messages, colored
// by level, with level toggles, a text filter, Copy and Clear. What a
// terminal shows, inside the app -- a shader that fails to compile on hot
// reload, a model that won't load.
//
// log_panel_init() makes the panel the log's sink (log_set_sink()), so it
// collects from then on, drawn or not; init it as early as possible to
// catch startup messages.

#define LOG_PANEL_MAX_ENTRIES 500

typedef struct log_panel_entry
{
    log_level level;
    char      time[9];    // "HH:MM:SS", local
    char*     text;       // "File.c:42: message" from WARN up, else the message
} log_panel_entry;

typedef struct log_panel
{
    log_panel_entry entries[LOG_PANEL_MAX_ENTRIES];   // ring buffer, oldest first
    uint32          entry_start;
    uint32          entry_count;

    uint32 new_errors;      // ERROR / FATAL since log_panel_take_new_errors()
    b8     show_info;       // TRACE, DEBUG, INFO
    b8     show_warnings;
    b8     show_errors;     // ERROR, FATAL
    char   filter[128];     // case-insensitive substring; "" = everything
    b8     scroll_to_bottom;
} log_panel;

// Starts collecting: becomes the log's sink.
void log_panel_init(log_panel* panel);

// Stops collecting (if it's still the sink) and frees the entries.
void log_panel_shutdown(log_panel* panel);

// Draws the window. `open` (may be NULL) gets the title bar's close button.
void log_panel_draw(log_panel* panel, const char* window_title, b8* open);

// How many errors arrived since the last call, e.g. to open the window when
// something goes wrong.
uint32 log_panel_take_new_errors(log_panel* panel);

// The `index`th entry, oldest first (index < entry_count), or NULL.
const log_panel_entry* log_panel_entry_at(const log_panel* panel, uint32 index);

// "trace", "debug", "info", "warn", "error", "fatal".
const char* log_level_name(log_level level);
