#pragma once

#include "../Core/Defines.h"

// Move / rotate / scale handles drawn over the 3D view: a C face on
// ImGuizmo (engine/deps/ImGuizmo, C++), the way cimgui is one on Dear ImGui.
// Only what the editor uses. Call between imgui_layer_new_frame() and
// imgui_layer_render(); matrices are column-major (cglm's layout).

typedef enum gizmo_operation
{
    GIZMO_TRANSLATE,
    GIZMO_ROTATE,
    GIZMO_SCALE
} gizmo_operation;

typedef enum gizmo_space
{
    GIZMO_LOCAL,   // handles follow the object's own axes
    GIZMO_WORLD    // handles follow the world axes (scale is always local)
} gizmo_space;

// Once per frame, before gizmo_manipulate().
void gizmo_begin_frame(void);

// Draws the handles for `matrix` (a world matrix) into `draw_list` (an
// ImDrawList*), over the screen rectangle (x, y, width, height) the camera
// renders to, and while the user drags one, edits `matrix` in place --
// returning true on the frames it changed. `snap` (NULL = off) is one value
// for every axis: meters for translate, degrees for rotate, a factor step for
// scale. With `interactive` false the handles are drawn but ignore the mouse
// (it's over a panel).
//
// `view_item` is the ImGui ID of an item covering the view to take its
// mouse input (the editor's invisible button), or 0. ImGuizmo won't start a
// drag while any item is hovered or active, which that item always is; it's
// hidden from that check -- and only it -- so the handles still take clicks.
b8 gizmo_manipulate(const f32 view[16], const f32 projection[16], gizmo_operation operation, gizmo_space space,
                    f32 matrix[16], const f32* snap, f32 x, f32 y, f32 width, f32 height, void* draw_list,
                    b8 interactive, uint32 view_item);

// The mouse is over a handle (as of the last gizmo_manipulate()) -- a click
// there belongs to the gizmo, not to camera orbit or picking.
b8 gizmo_is_over(void);

// A handle is being dragged.
b8 gizmo_is_using(void);
