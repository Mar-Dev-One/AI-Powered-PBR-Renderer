#include "EntityPanel.h"

#include "../World/Reflect.h"

#include "cimgui.h"

#include <stdint.h>

// One field's current value, formatted per its reflected type.
static void
draw_field(const world* w, const component_desc* component, const field_desc* field, const void* data)
{
    const unsigned char* base  = (const unsigned char*)data;
    const void*          value = base + field->offset;

    // A material override field whose bit is clear isn't applied at draw
    // time, so its stored number would be misleading.
    if (field->override_bit && component->type == COMPONENT_MESH_RENDERER)
    {
        const c_mesh_renderer* mr = (const c_mesh_renderer*)data;
        if (!(mr->material.fields & field->override_bit))
        {
            igTextDisabled("%s: (model's own)", field->name);
            return;
        }
    }

    switch (field->type)
    {
        case FIELD_F32:
            igText("%s: %.3f", field->name, *(const f32*)value);
            break;

        case FIELD_BOOL:
            igText("%s: %s", field->name, *(const b8*)value ? "true" : "false");
            break;

        case FIELD_VEC3:
        case FIELD_COLOR3:
        {
            const f32* v = (const f32*)value;
            igText("%s: (%.3f, %.3f, %.3f)", field->name, v[0], v[1], v[2]);
            break;
        }

        case FIELD_ENUM:
        {
            int index = *(const int*)value;
            const char* name = (index >= 0 && (uint32)index < field->enum_count) ? field->enum_names[index] : "?";
            igText("%s: %s", field->name, name);
            break;
        }

        case FIELD_STRING:
            igText("%s: %s", field->name, (const char*)value);
            break;

        case FIELD_ASSET:
        {
            const char* key = assets_key(w->assets, *(const asset_id*)value);
            igText("%s: %s", field->name, key ? key : "(none)");
            break;
        }

        case FIELD_ENTITY:
        {
            const char* name = world_name(w, *(const entity_id*)value);
            igText("%s: %s", field->name, name ? name : "(none)");
            break;
        }

        case FIELD_FLAGS:
            igText("%s: 0x%X", field->name, *(const uint32*)value);
            break;
    }
}

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

static void
draw_entity(const world* w, entity_id e, entity_id* selection, b8 reveal)
{
    entity_id selected = selection ? *selection : ENTITY_NULL;

    // Clicking the name selects; the arrow (or a double-click) opens.
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (e == selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (reveal && selected && is_ancestor(w, e, selected))
        igSetNextItemOpen(true, ImGuiCond_Always);

    // The ID in the label: what tools (and the LLM) will refer to.
    b8 open = igTreeNodeEx_Ptr((const void*)(uintptr_t)e, flags, "%s  #%u.%u",
                               world_name(w, e), entity_slot(e), e >> ENTITY_INDEX_BITS);

    if (selection && igIsItemClicked(ImGuiMouseButton_Left) && !igIsItemToggledOpen())
        *selection = e;
    if (reveal && e == selected)
        igSetScrollHereY(0.5f);

    if (!open)
        return;

    // Name is the node label already, and Parent is the tree structure itself.
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        if (t == COMPONENT_NAME || t == COMPONENT_PARENT || !world_has(w, e, (component_type)t))
            continue;

        const component_desc* component = reflect_component((component_type)t);
        const void*           data      = reflect_component_data(w, e, (component_type)t);

        igSeparatorText(component->name);
        for (uint32 f = 0; f < component->field_count; ++f)
            draw_field(w, component, &component->fields[f], data);
    }

    vec3 bmin, bmax;
    if (world_bounds(w, e, bmin, bmax))
    {
        igSeparatorText("Bounds (world)");
        igText("min: (%.3f, %.3f, %.3f)", bmin[0], bmin[1], bmin[2]);
        igText("max: (%.3f, %.3f, %.3f)", bmax[0], bmax[1], bmax[2]);
    }

    // Children, as nested nodes.
    for (entity_id child = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_PARENT)); child;
         child = world_next(w, child, COMPONENT_BIT(COMPONENT_PARENT)))
    {
        if (world_parent(w, child) == e)
            draw_entity(w, child, selection, reveal);
    }

    igTreePop();
}

void
entity_panel_draw(const world* w, const char* window_title, entity_id* selection, b8 reveal)
{
    // First run only (after that imgui.ini remembers where the user put it):
    // a tall column at the right edge, clear of the app's own panels.
    const ImGuiViewport* viewport = igGetMainViewport();
    igSetNextWindowPos((ImVec2_c){ viewport->WorkPos.x + viewport->WorkSize.x - 310.0f, viewport->WorkPos.y + 10.0f },
                       ImGuiCond_FirstUseEver, (ImVec2_c){ 0.0f, 0.0f });
    igSetNextWindowSize((ImVec2_c){ 300.0f, viewport->WorkSize.y - 20.0f }, ImGuiCond_FirstUseEver);

    if (!igBegin(window_title, NULL, 0))
    {
        igEnd();
        return;
    }

    igText("%u entities", world_count(w));
    if (selection && *selection)
        igText("Selected: %s", world_name(w, *selection));
    else if (selection)
        igTextDisabled("Nothing selected: click an entity here or in the view");
    igSeparator();

    // Roots: no parent, or a parent that no longer exists.
    for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
    {
        if (!world_is_alive(w, world_parent(w, e)))
            draw_entity(w, e, selection, reveal);
    }

    igEnd();
}
