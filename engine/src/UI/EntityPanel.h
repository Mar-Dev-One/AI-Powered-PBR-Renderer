#pragma once

#include "../World/World.h"

// ImGui window listing every entity in `w` as a parent/child tree (the
// outliner); the selected entity's fields are edited in UI/Inspector.h. Call
// between imgui_layer_new_frame() and imgui_layer_render().
//
// Selection: `selection` (may be NULL for none) is highlighted, and clicking
// an entity's name sets it. With `reveal`, the tree opens down to the
// selected entity and scrolls to it -- for a selection made elsewhere (a
// click in the viewport).
//
// The panel never changes the world itself (it only reads `w`): what the
// user asks for -- dragging an entity onto another to parent it (or onto
// "scene root" to unparent it), or an item of an entity's right-click menu
// -- comes back as an entity_panel_action for the caller to run as a
// command, so it is validated and undoable like any other edit.

typedef enum entity_panel_request
{
    ENTITY_PANEL_NONE,
    ENTITY_PANEL_REPARENT,    // entity under parent (ENTITY_NULL = the scene root)
    ENTITY_PANEL_DUPLICATE,
    ENTITY_PANEL_DELETE,
    ENTITY_PANEL_FRAME,       // point the view at it
} entity_panel_request;

typedef struct entity_panel_action
{
    entity_panel_request request;
    entity_id            entity;
    entity_id            parent;   // ENTITY_PANEL_REPARENT only
} entity_panel_action;

typedef struct entity_panel
{
    char filter[ENTITY_NAME_MAX];   // only names containing this (any case), as a flat list; "" = the tree
} entity_panel;

entity_panel_action entity_panel_draw(entity_panel* panel, const world* w, const char* window_title,
                                      entity_id* selection, b8 reveal);
