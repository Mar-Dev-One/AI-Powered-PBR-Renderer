#pragma once

#include "../World/World.h"

// Read-only ImGui window listing every entity in `w` as a parent/child tree,
// with each component's fields. Built entirely from World/Reflect.h, so a new
// component or field shows up here without touching this code. Call between
// imgui_layer_new_frame() and imgui_layer_render().
//
// Selection: `selection` (may be NULL for none) is highlighted, and clicking
// an entity's name sets it. With `reveal`, the tree opens down to the
// selected entity and scrolls to it -- for a selection made elsewhere (a
// click in the viewport).
void entity_panel_draw(const world* w, const char* window_title, entity_id* selection, b8 reveal);
