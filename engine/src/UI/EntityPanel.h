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
void entity_panel_draw(const world* w, const char* window_title, entity_id* selection, b8 reveal);
