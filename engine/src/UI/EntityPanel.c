#include "EntityPanel.h"

#include "cimgui.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

// Drag-and-drop payload: one entity_id.
#define PAYLOAD_ENTITY "PBR_ENTITY"

// True if `ancestor` is `e`'s parent, grandparent, ...
static b8
is_ancestor(const world* w, entity_id ancestor, entity_id e)
{
    for (entity_id p = world_parent(w, e); p; p = world_parent(w, p))
    {
        if (p == ancestor)
            return true;
    }
    return false;
}

static b8
has_children(const world* w, entity_id e)
{
    for (entity_id child = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_PARENT)); child;
         child = world_next(w, child, COMPONENT_BIT(COMPONENT_PARENT)))
    {
        if (world_parent(w, child) == e)
            return true;
    }
    return false;
}

// `text` contains `needle`, ignoring case.
static b8
contains_nocase(const char* text, const char* needle)
{
    size_t n = strlen(needle);
    for (; *text; ++text)
    {
        size_t i = 0;
        while (i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == n)
            return true;
    }
    return n == 0;
}

// What an entity is, at a glance, after its name.
static const char*
entity_kind(const world* w, entity_id e)
{
    if (world_has(w, e, COMPONENT_LIGHT))
    {
        static const char* const TYPES[] = { "sun", "point light", "spot light" };
        uint32 type = (uint32)world_light(w, e)->type;
        return type < 3 ? TYPES[type] : "light";
    }
    if (world_has(w, e, COMPONENT_CAMERA))           return "camera";
    if (world_has(w, e, COMPONENT_REFLECTION_PROBE)) return "probe";
    if (world_has(w, e, COMPONENT_MESH_RENDERER))    return NULL;
    return "empty";
}

// Drag source, drop target and right-click menu of the item just drawn.
static void
entity_item_behaviour(const world* w, entity_id e, entity_id* selection, entity_panel_action* action)
{
    if (igBeginDragDropSource(0))
    {
        igSetDragDropPayload(PAYLOAD_ENTITY, &e, sizeof(e), 0);
        igText("Parent %s to...", world_name(w, e));
        igEndDragDropSource();
    }
    if (igBeginDragDropTarget())
    {
        const ImGuiPayload* payload = igAcceptDragDropPayload(PAYLOAD_ENTITY, 0);
        if (payload && payload->DataSize == sizeof(entity_id))
        {
            entity_id child;
            memcpy(&child, payload->Data, sizeof(child));
            if (child != e)
                *action = (entity_panel_action){ ENTITY_PANEL_REPARENT, child, e };
        }
        igEndDragDropTarget();
    }

    if (igBeginPopupContextItem(NULL, ImGuiPopupFlags_MouseButtonRight))
    {
        if (selection)
            *selection = e;
        igTextDisabled("%s", world_name(w, e));
        if (igMenuItem_Bool("Frame", "F", false, true))
            *action = (entity_panel_action){ ENTITY_PANEL_FRAME, e, ENTITY_NULL };
        if (igMenuItem_Bool("Duplicate", "Ctrl+D", false, true))
            *action = (entity_panel_action){ ENTITY_PANEL_DUPLICATE, e, ENTITY_NULL };
        if (igMenuItem_Bool("Unparent", NULL, false, world_parent(w, e) != ENTITY_NULL))
            *action = (entity_panel_action){ ENTITY_PANEL_REPARENT, e, ENTITY_NULL };
        igSeparator();
        if (igMenuItem_Bool("Delete", "Del", false, true))
            *action = (entity_panel_action){ ENTITY_PANEL_DELETE, e, ENTITY_NULL };
        igEndPopup();
    }
}

static void
draw_entity(const world* w, entity_id e, entity_id* selection, b8 reveal, b8 nested, entity_panel_action* action)
{
    entity_id selected = selection ? *selection : ENTITY_NULL;

    // Clicking the name selects; the arrow (or a double-click) opens.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!nested || !has_children(w, e))
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (e == selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (nested && reveal && selected && is_ancestor(w, e, selected))
        igSetNextItemOpen(true, ImGuiCond_Always);

    // The ID in the label: what tools (and the LLM) will refer to.
    const char* kind = entity_kind(w, e);
    b8 open = igTreeNodeEx_Ptr((const void*)(uintptr_t)e, flags, "%s%s%s%s  #%u.%u", world_name(w, e),
                               kind ? "  (" : "", kind ? kind : "", kind ? ")" : "",
                               entity_slot(e), e >> ENTITY_INDEX_BITS);

    if (selection && igIsItemClicked(ImGuiMouseButton_Left) && !igIsItemToggledOpen())
        *selection = e;
    if (reveal && e == selected)
        igSetScrollHereY(0.5f);
    entity_item_behaviour(w, e, selection, action);

    if (!open)
        return;

    // Children, as nested nodes.
    if (nested)
    {
        for (entity_id child = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_PARENT)); child;
             child = world_next(w, child, COMPONENT_BIT(COMPONENT_PARENT)))
        {
            if (world_parent(w, child) == e)
                draw_entity(w, child, selection, reveal, true, action);
        }
    }

    igTreePop();
}

entity_panel_action
entity_panel_draw(entity_panel* panel, const world* w, const char* window_title, entity_id* selection, b8 reveal)
{
    entity_panel_action action = { ENTITY_PANEL_NONE, ENTITY_NULL, ENTITY_NULL };

    // First run only (after that imgui.ini remembers where the user put it):
    // a tall column at the right edge, clear of the app's own panels.
    const ImGuiViewport* viewport = igGetMainViewport();
    igSetNextWindowPos((ImVec2_c){ viewport->WorkPos.x + viewport->WorkSize.x - 310.0f, viewport->WorkPos.y + 10.0f },
                       ImGuiCond_FirstUseEver, (ImVec2_c){ 0.0f, 0.0f });
    igSetNextWindowSize((ImVec2_c){ 300.0f, viewport->WorkSize.y - 20.0f }, ImGuiCond_FirstUseEver);

    if (!igBegin(window_title, NULL, 0))
    {
        igEnd();
        return action;
    }

    igText("%u entities", world_count(w));
    if (selection && *selection)
        igText("Selected: %s", world_name(w, *selection));
    else if (selection)
        igTextDisabled("Nothing selected: click an entity here or in the view");

    igSetNextItemWidth(-1.0f);
    igInputTextWithHint("##filter", "Filter by name", panel->filter, sizeof(panel->filter), 0, NULL, NULL);

    // While an entity is dragged: somewhere to drop it to unparent it.
    const ImGuiPayload* dragging = igGetDragDropPayload();
    if (dragging && dragging->DataType[0] && strcmp(dragging->DataType, PAYLOAD_ENTITY) == 0)
    {
        igSelectable_Bool("(drop here: scene root)", false, 0, (ImVec2_c){ 0, 0 });
        if (igBeginDragDropTarget())
        {
            const ImGuiPayload* payload = igAcceptDragDropPayload(PAYLOAD_ENTITY, 0);
            if (payload && payload->DataSize == sizeof(entity_id))
            {
                entity_id child;
                memcpy(&child, payload->Data, sizeof(child));
                action = (entity_panel_action){ ENTITY_PANEL_REPARENT, child, ENTITY_NULL };
            }
            igEndDragDropTarget();
        }
    }
    igSeparator();

    igBeginChild_Str("##entities", (ImVec2_c){ 0, 0 }, 0, 0);
    if (panel->filter[0])
    {
        // Matches as a flat list, wherever they sit in the tree.
        uint32 matches = 0;
        for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
        {
            if (contains_nocase(world_name(w, e), panel->filter))
            {
                draw_entity(w, e, selection, reveal, false, &action);
                ++matches;
            }
        }
        if (matches == 0)
            igTextDisabled("No entity name contains \"%s\".", panel->filter);
    }
    else
    {
        // Roots: no parent, or a parent that no longer exists.
        for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
        {
            if (!world_is_alive(w, world_parent(w, e)))
                draw_entity(w, e, selection, reveal, true, &action);
        }
    }
    igEndChild();

    igEnd();
    return action;
}
