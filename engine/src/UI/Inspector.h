#pragma once

#include "../Commands/Commands.h"
#include "../World/World.h"

#include "cJSON.h"

// The selected entity's components as editing fields, in an ImGui window.
// Every editor is generated from World/Reflect.h -- a field's type picks the
// widget (drag, color, checkbox, combo, text) and its min/max the range -- so
// a new component or field gets an editor without touching this code.
//
// Every edit runs as a command through the given context (component.set,
// entity.update, component.remove, entity.destroy), exactly as the console or
// the agent would: it is validated, undoable, and reported as a change. A
// drag (or any continuous edit) is one undo step: the first change opens a
// history group, which is committed once no widget is active any more. A
// rejected value is shown in the window and changes nothing.

typedef struct inspector
{
    b8     editing;                         // a history group is open for the edit in progress
    b8     executing;                       // inside one of our own commands (the host may skip logging it)
    char   error[512];                      // the last rejected edit, until the next one succeeds
    entity_id name_entity;                  // whose name is in name_buffer
    char   name_buffer[ENTITY_NAME_MAX];
    cJSON* assets;                          // assets.list result, refreshed when the model combo opens
} inspector;

void inspector_init(inspector* ins);

// Draws the window for `*selected` in ctx->w (nothing selected: a hint).
// Clears *selected if the entity is deleted from here. Call between
// imgui_layer_new_frame() and imgui_layer_render().
void inspector_draw(inspector* ins, command_context* ctx, entity_id* selected, const char* window_title);

// Commits an edit still in progress (call before running other commands that
// shouldn't join it, and at shutdown).
void inspector_finish_edit(inspector* ins, command_context* ctx);

void inspector_shutdown(inspector* ins);
