#include "LogPanel.h"
#include "cimgui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// "C:/x/y/Renderer.c" -> "Renderer.c": the folders only add width.
static const char*
base_name(const char* path)
{
    const char* name = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/' || *c == '\\')
            name = c + 1;
    return name;
}

static void
on_log(log_level level, const char* file, int line, const char* message, void* user_data)
{
    log_panel* panel = user_data;

    char* text;
    if (file && file[0])
    {
        size_t size = strlen(file) + strlen(message) + 32;
        text = malloc(size);
        if (text)
            snprintf(text, size, "%s:%d: %s", base_name(file), line, message);
    }
    else
    {
        size_t size = strlen(message) + 1;
        text = malloc(size);
        if (text)
            memcpy(text, message, size);
    }
    if (!text)
        return;   // no memory: the terminal still has it

    // Full: the oldest goes.
    if (panel->entry_count == LOG_PANEL_MAX_ENTRIES)
    {
        free(panel->entries[panel->entry_start].text);
        panel->entry_start = (panel->entry_start + 1) % LOG_PANEL_MAX_ENTRIES;
        --panel->entry_count;
    }

    log_panel_entry* entry = &panel->entries[(panel->entry_start + panel->entry_count) % LOG_PANEL_MAX_ENTRIES];
    ++panel->entry_count;

    time_t     now = time(NULL);
    struct tm* t   = localtime(&now);
    if (t)
        strftime(entry->time, sizeof(entry->time), "%H:%M:%S", t);
    else
        snprintf(entry->time, sizeof(entry->time), "--:--:--");

    entry->level = level;
    entry->text  = text;

    if (level >= LOG_LEVEL_ERROR)
        ++panel->new_errors;
}

void
log_panel_init(log_panel* panel)
{
    memset(panel, 0, sizeof(*panel));
    panel->show_info        = true;
    panel->show_warnings    = true;
    panel->show_errors      = true;
    panel->scroll_to_bottom = true;
    log_set_sink(on_log, panel);
}

static void
clear_entries(log_panel* panel)
{
    for (uint32 i = 0; i < panel->entry_count; ++i)
        free(panel->entries[(panel->entry_start + i) % LOG_PANEL_MAX_ENTRIES].text);
    panel->entry_start = 0;
    panel->entry_count = 0;
}

void
log_panel_shutdown(log_panel* panel)
{
    log_set_sink(NULL, NULL);
    clear_entries(panel);
}

uint32
log_panel_take_new_errors(log_panel* panel)
{
    uint32 count = panel->new_errors;
    panel->new_errors = 0;
    return count;
}

const log_panel_entry*
log_panel_entry_at(const log_panel* panel, uint32 index)
{
    return index < panel->entry_count ? &panel->entries[(panel->entry_start + index) % LOG_PANEL_MAX_ENTRIES] : NULL;
}

const char*
log_level_name(log_level level)
{
    static const char* const names[] = { "trace", "debug", "info", "warn", "error", "fatal" };
    return (uint32)level <= LOG_LEVEL_FATAL ? names[level] : "?";
}

static ImVec4_c
level_color(log_level level)
{
    switch (level)
    {
        case LOG_LEVEL_TRACE: return (ImVec4_c){ 0.55f, 0.55f, 0.55f, 1.0f };
        case LOG_LEVEL_DEBUG: return (ImVec4_c){ 0.50f, 0.80f, 0.85f, 1.0f };
        case LOG_LEVEL_WARN:  return (ImVec4_c){ 1.00f, 0.80f, 0.35f, 1.0f };
        case LOG_LEVEL_ERROR: return (ImVec4_c){ 1.00f, 0.45f, 0.40f, 1.0f };
        case LOG_LEVEL_FATAL: return (ImVec4_c){ 1.00f, 0.40f, 0.90f, 1.0f };
        case LOG_LEVEL_INFO:
        default:              return (ImVec4_c){ 0.90f, 0.90f, 0.90f, 1.0f };
    }
}

// Case-insensitive: does `text` contain `needle`?
static b8
contains(const char* text, const char* needle)
{
    if (!needle[0])
        return true;
    for (; *text; ++text)
    {
        const char* a = text;
        const char* b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            ++a, ++b;
        if (!*b)
            return true;
    }
    return false;
}

static b8
shown(const log_panel* panel, const log_panel_entry* entry)
{
    b8 level_on = entry->level >= LOG_LEVEL_ERROR  ? panel->show_errors
                : entry->level == LOG_LEVEL_WARN   ? panel->show_warnings
                                                   : panel->show_info;
    return level_on && contains(entry->text, panel->filter);
}

// Every shown entry as "HH:MM:SS [level] text" lines, onto the clipboard.
static void
copy_shown(const log_panel* panel)
{
    size_t size = 1;
    for (uint32 i = 0; i < panel->entry_count; ++i)
    {
        const log_panel_entry* entry = log_panel_entry_at(panel, i);
        if (shown(panel, entry))
            size += strlen(entry->text) + 24;
    }

    char* text = malloc(size);
    if (!text)
        return;
    size_t used = 0;
    text[0] = '\0';
    for (uint32 i = 0; i < panel->entry_count; ++i)
    {
        const log_panel_entry* entry = log_panel_entry_at(panel, i);
        if (shown(panel, entry))
            used += (size_t)snprintf(text + used, size - used, "%s [%s] %s\n", entry->time, log_level_name(entry->level),
                                     entry->text);
    }
    igSetClipboardText(text);
    free(text);
}

void
log_panel_draw(log_panel* panel, const char* window_title, b8* open)
{
    const ImGuiViewport* viewport = igGetMainViewport();
    igSetNextWindowSize((ImVec2_c){ viewport->WorkSize.x * 0.5f, viewport->WorkSize.y * 0.3f }, ImGuiCond_FirstUseEver);

    if (!igBegin(window_title, open, 0))
    {
        igEnd();
        return;
    }

    uint32 warnings = 0, errors = 0;
    for (uint32 i = 0; i < panel->entry_count; ++i)
    {
        log_level level = log_panel_entry_at(panel, i)->level;
        warnings += level == LOG_LEVEL_WARN;
        errors   += level >= LOG_LEVEL_ERROR;
    }

    if (igButton("Clear", (ImVec2_c){ 0, 0 }))
        clear_entries(panel);
    igSameLine(0.0f, -1.0f);
    if (igButton("Copy", (ImVec2_c){ 0, 0 }))
        copy_shown(panel);
    if (igIsItemHovered(0))
        igSetTooltip("The lines shown, to the clipboard");
    igSameLine(0.0f, -1.0f);
    igCheckbox("Info", &panel->show_info);
    igSameLine(0.0f, -1.0f);
    char label[48];
    snprintf(label, sizeof(label), "Warnings (%u)###warnings", warnings);
    igCheckbox(label, &panel->show_warnings);
    igSameLine(0.0f, -1.0f);
    snprintf(label, sizeof(label), "Errors (%u)###errors", errors);
    igCheckbox(label, &panel->show_errors);
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(-1.0f);
    igInputTextWithHint("##filter", "filter", panel->filter, sizeof(panel->filter), 0, NULL, NULL);

    igBeginChild_Str("entries", (ImVec2_c){ 0.0f, 0.0f }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

    // Follow new lines only while scrolled to the bottom, so reading
    // something further up isn't interrupted.
    b8 at_bottom = igGetScrollY() >= igGetScrollMaxY() - 1.0f;

    for (uint32 i = 0; i < panel->entry_count; ++i)
    {
        const log_panel_entry* entry = log_panel_entry_at(panel, i);
        if (!shown(panel, entry))
            continue;
        igTextDisabled("%s", entry->time);
        igSameLine(0.0f, -1.0f);
        igPushStyleColor_Vec4(ImGuiCol_Text, level_color(entry->level));
        igTextUnformatted(entry->text, NULL);
        igPopStyleColor(1);
    }

    if (panel->scroll_to_bottom || at_bottom)
        igSetScrollHereY(1.0f);
    panel->scroll_to_bottom = false;
    igEndChild();

    igEnd();
}
