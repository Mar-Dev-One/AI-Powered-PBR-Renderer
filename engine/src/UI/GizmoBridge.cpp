// GizmoBridge.cpp
//
// UI/Gizmo.h's C functions, implemented on ImGuizmo (C++). The only C++ in
// the engine's own sources: everything else reaches ImGui through cimgui.

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiContext's hovered/active item, for view_item
#include "ImGuizmo.h"

extern "C" {
#include "Gizmo.h"
}

extern "C" void
gizmo_begin_frame(void)
{
    ImGuizmo::BeginFrame();
}

extern "C" b8
gizmo_manipulate(const f32 view[16], const f32 projection[16], gizmo_operation operation, gizmo_space space,
                 f32 matrix[16], const f32* snap, f32 x, f32 y, f32 width, f32 height, void* draw_list,
                 b8 interactive, uint32 view_item)
{
    static const ImGuizmo::OPERATION OPERATIONS[] = { ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE };

    ImGuizmo::Enable(interactive);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(static_cast<ImDrawList*>(draw_list));
    ImGuizmo::SetRect(x, y, width, height);

    // ImGuizmo snaps per axis.
    f32 snap3[3];
    if (snap)
        snap3[0] = snap3[1] = snap3[2] = *snap;

    // Scaling along world axes isn't meaningful for a rotated object.
    ImGuizmo::MODE mode = (space == GIZMO_WORLD && operation != GIZMO_SCALE) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

    // ImGuizmo starts a drag only when no ImGui item is hovered or active
    // (its CanActivate()). The view's own input item always is, so for this
    // call it's taken out of ImGui's hovered/active state -- only when it's
    // that item, so any other widget still blocks the gizmo -- and put back.
    ImGuiContext& g          = *ImGui::GetCurrentContext();
    const ImGuiID hovered    = g.HoveredId;
    const ImGuiID hovered_pf = g.HoveredIdPreviousFrame;
    const ImGuiID active     = g.ActiveId;
    if (view_item)
    {
        if (g.HoveredId == view_item)              g.HoveredId = 0;
        if (g.HoveredIdPreviousFrame == view_item) g.HoveredIdPreviousFrame = 0;
        if (g.ActiveId == view_item)               g.ActiveId = 0;
    }

    b8 changed = ImGuizmo::Manipulate(view, projection, OPERATIONS[operation], mode, matrix, nullptr,
                                      snap ? snap3 : nullptr);

    g.HoveredId              = hovered;
    g.HoveredIdPreviousFrame = hovered_pf;
    g.ActiveId               = active;
    return changed;
}

extern "C" b8
gizmo_is_over(void)
{
    return ImGuizmo::IsOver();
}

extern "C" b8
gizmo_is_using(void)
{
    return ImGuizmo::IsUsing();
}
