#include "Inspector.h"

#include "../World/Reflect.h"

#include "cimgui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const ImVec4_c ERROR_COLOR = { 1.0f, 0.45f, 0.4f, 1.0f };

void
inspector_init(inspector* ins)
{
    memset(ins, 0, sizeof(*ins));
}

void
inspector_shutdown(inspector* ins)
{
    cJSON_Delete(ins->assets);
    ins->assets = NULL;
}

// --- Commands -------------------------------------------------------------------

// Runs {cmd, args} (takes ownership of `args`). On failure the engine's
// message goes to ins->error; on success it is cleared. *out_result (may be
// NULL) gets the result (caller frees).
static b8
run(inspector* ins, command_context* ctx, const char* cmd, cJSON* args, cJSON** out_result)
{
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", cmd);
    cJSON_AddItemToObject(request, "args", args ? args : cJSON_CreateObject());

    ins->executing = true;
    cJSON* response = cmd_execute(ctx, request);
    ins->executing = false;
    cJSON_Delete(request);

    b8 ok = cJSON_IsTrue(cJSON_GetObjectItem(response, "ok"));
    if (ok)
    {
        ins->error[0] = '\0';
        if (out_result)
            *out_result = cJSON_DetachItemFromObject(response, "result");
    }
    else
    {
        const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "message"));
        snprintf(ins->error, sizeof(ins->error), "%s: %s", cmd, message ? message : "failed");
    }

    cJSON_Delete(response);
    return ok;
}

// The first change of an edit opens a history group, so a whole drag undoes
// as one step. inspector_finish_edit() closes it.
static void
begin_edit(inspector* ins, command_context* ctx, const char* label)
{
    if (ins->editing || !ctx->history)
        return;

    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "label", label);
    ins->editing = run(ins, ctx, "history.begin", args, NULL);
}

void
inspector_finish_edit(inspector* ins, command_context* ctx)
{
    if (!ins->editing)
        return;
    ins->editing = false;

    char error[sizeof(ins->error)];
    memcpy(error, ins->error, sizeof(error));
    run(ins, ctx, "history.commit", NULL, NULL);
    if (error[0])
        memcpy(ins->error, error, sizeof(error));   // keep the edit's own error visible
}

// component.set {entity, component, fields: {name: value}} (takes `value`).
static b8
set_field(inspector* ins, command_context* ctx, entity_id e, const component_desc* component,
          const field_desc* field, cJSON* value)
{
    char label[128];
    snprintf(label, sizeof(label), "edit %s.%s.%s", world_name(ctx->w, e), component->name, field->name);
    begin_edit(ins, ctx, label);

    cJSON* args   = cJSON_CreateObject();
    cJSON* fields = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
    cJSON_AddStringToObject(args, "component", component->name);
    cJSON_AddItemToObject(fields, field->name, value);
    cJSON_AddItemToObject(args, "fields", fields);
    return run(ins, ctx, "component.set", args, NULL);
}

static cJSON*
vec3_json(const f32* v)
{
    return cJSON_CreateFloatArray(v, 3);
}

// --- Field editors ------------------------------------------------------------------

// Drag speed: a fraction of the range, or of the value's size for unbounded
// fields (a 0.2 m model and a 50 m building both drag comfortably).
static f32
drag_speed(const field_desc* field, const f32* values, int count)
{
    if (strstr(field->name, "deg"))
        return 0.5f;   // angles: half a degree per pixel

    f32 magnitude = 0.0f;
    for (int i = 0; i < count; ++i)
        magnitude = fmaxf(magnitude, fabsf(values[i]));

    f32 base = field->max > field->min ? fminf((field->max - field->min) / 1000.0f, 0.1f) : 0.01f;
    return fmaxf(base, magnitude * 0.005f);
}

// Clamping only for fields that have a range: ImGui's clamp flags would pin
// an unbounded field (min == max == 0) to zero.
static ImGuiSliderFlags
drag_flags(const field_desc* field)
{
    return field->max > field->min ? ImGuiSliderFlags_AlwaysClamp : 0;
}

static void
edit_asset(inspector* ins, command_context* ctx, entity_id e, const component_desc* component,
           const field_desc* field, asset_id current)
{
    const char* key = assets_key(ctx->w->assets, current);
    if (!igBeginCombo(field->name, key ? key : "(none)", 0))
        return;

    // Fresh on every open: models can be dropped into assets/ while running.
    if (igIsWindowAppearing())
    {
        cJSON_Delete(ins->assets);
        ins->assets = NULL;
        run(ins, ctx, "assets.list", NULL, &ins->assets);
    }

    if (igSelectable_Bool("(none)", key == NULL, 0, (ImVec2_c){ 0, 0 }))
        set_field(ins, ctx, e, component, field, cJSON_CreateNull());

    const char* lists[] = { "builtins", "models" };
    for (int l = 0; l < 2; ++l)
    {
        const cJSON* item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(ins->assets, lists[l]))
        {
            const char* option = cJSON_GetStringValue(item);
            if (option && igSelectable_Bool(option, key && strcmp(key, option) == 0, 0, (ImVec2_c){ 0, 0 }))
                set_field(ins, ctx, e, component, field, cJSON_CreateString(option));
        }
    }
    igEndCombo();
}

// A material override field: a checkbox turns the override on (from the
// stored value, or white for an unset color) or off (back to the model's own).
static b8
edit_override_toggle(inspector* ins, command_context* ctx, entity_id e, const component_desc* component,
                     const field_desc* field, const void* value)
{
    const c_mesh_renderer* mr = world_mesh_renderer(ctx->w, e);
    b8 on = mr && (mr->material.fields & field->override_bit);

    igPushID_Str(field->name);
    b8 toggled = igCheckbox("##override", &on);
    igPopID();
    if (igIsItemHovered(0))
        igSetTooltip("Override the model's own %s", field->name);
    igSameLine(0.0f, -1.0f);

    if (toggled)
    {
        cJSON* json = NULL;
        if (!on)
            json = cJSON_CreateNull();
        else if (field->type == FIELD_F32)
            json = cJSON_CreateNumber(*(const f32*)value);
        else
        {
            const f32* v     = value;
            b8         black = v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
            f32        white[3] = { 1.0f, 1.0f, 1.0f };
            json = vec3_json(black && strstr(field->name, "base_color") ? white : v);
        }
        set_field(ins, ctx, e, component, field, json);
    }

    if (!on && !toggled)
    {
        igTextDisabled("%s: (model's own)", field->name);
        return false;
    }
    return true;
}

static void
edit_field(inspector* ins, command_context* ctx, entity_id e, const component_desc* component,
           const field_desc* field, const void* data)
{
    const void* stored = (const unsigned char*)data + field->offset;

    if (field->override_bit && component->type == COMPONENT_MESH_RENDERER
        && !edit_override_toggle(ins, ctx, e, component, field, stored))
        return;

    switch (field->type)
    {
        case FIELD_F32:
        {
            f32 v = *(const f32*)stored;
            if (igDragFloat(field->name, &v, drag_speed(field, &v, 1), field->min, field->max, "%.3f", drag_flags(field)))
                set_field(ins, ctx, e, component, field, cJSON_CreateNumber(v));
            break;
        }

        case FIELD_VEC3:
        {
            f32 v[3];
            memcpy(v, stored, sizeof(v));
            if (igDragFloat3(field->name, v, drag_speed(field, v, 3), field->min, field->max, "%.3f", drag_flags(field)))
                set_field(ins, ctx, e, component, field, vec3_json(v));
            break;
        }

        case FIELD_COLOR3:
        {
            // Linear RGB; values above 1 (emissive) need the HDR picker.
            f32 v[3];
            memcpy(v, stored, sizeof(v));
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | (field->max > 1.0f ? ImGuiColorEditFlags_HDR : 0);
            if (igColorEdit3(field->name, v, flags))
            {
                for (int i = 0; i < 3; ++i)
                    v[i] = fminf(fmaxf(v[i], field->min), field->max > field->min ? field->max : v[i]);
                set_field(ins, ctx, e, component, field, vec3_json(v));
            }
            break;
        }

        case FIELD_BOOL:
        {
            b8 v = *(const b8*)stored;
            if (igCheckbox(field->name, &v))
                set_field(ins, ctx, e, component, field, cJSON_CreateBool(v));
            break;
        }

        case FIELD_ENUM:
        {
            int v = *(const int*)stored;
            if (igCombo_Str_arr(field->name, &v, field->enum_names, (int)field->enum_count, -1))
                set_field(ins, ctx, e, component, field, cJSON_CreateString(field->enum_names[v]));
            break;
        }

        case FIELD_ASSET:
            edit_asset(ins, ctx, e, component, field, *(const asset_id*)stored);
            break;

        case FIELD_STRING:
        case FIELD_ENTITY:
        case FIELD_FLAGS:
        default:
            // Name and Parent have their own editors at the top; nothing else uses these yet.
            igTextDisabled("%s: (not editable here)", field->name);
            break;
    }

    if (field->doc && igIsItemHovered(0))
        igSetTooltip("%s", field->doc);
}

// --- Name, parent, components ----------------------------------------------------------

static void
edit_name(inspector* ins, command_context* ctx, entity_id e)
{
    // Edited in a buffer and applied on Enter or leaving the field, so a
    // half-typed name never collides with another entity's.
    if (ins->name_entity != e || !igIsAnyItemActive())
    {
        snprintf(ins->name_buffer, sizeof(ins->name_buffer), "%s", world_name(ctx->w, e));
        ins->name_entity = e;
    }

    igInputText("Name", ins->name_buffer, sizeof(ins->name_buffer), 0, NULL, NULL);
    if (igIsItemDeactivatedAfterEdit() && strcmp(ins->name_buffer, world_name(ctx->w, e)) != 0)
    {
        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
        cJSON_AddStringToObject(args, "name", ins->name_buffer);
        run(ins, ctx, "entity.update", args, NULL);
        ins->name_entity = ENTITY_NULL;   // reload from the world (the engine may have refused)
    }
}

static void
edit_parent(inspector* ins, command_context* ctx, entity_id e)
{
    entity_id   parent = world_parent(ctx->w, e);
    const char* name   = parent ? world_name(ctx->w, parent) : NULL;
    if (!igBeginCombo("Parent", name ? name : "(scene root)", 0))
        return;

    entity_id choice   = ENTITY_NULL;
    b8        clicked  = false;
    if (igSelectable_Bool("(scene root)", parent == ENTITY_NULL, 0, (ImVec2_c){ 0, 0 }))
        clicked = true;
    for (entity_id other = world_next(ctx->w, ENTITY_NULL, 0); other; other = world_next(ctx->w, other, 0))
    {
        if (other != e && igSelectable_Bool(world_name(ctx->w, other), other == parent, 0, (ImVec2_c){ 0, 0 }))
        {
            choice  = other;
            clicked = true;
        }
    }
    igEndCombo();

    if (clicked && choice != parent)
    {
        // The engine refuses a cycle (a child of its own descendant) and says so.
        cJSON* args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
        if (choice)
            cJSON_AddStringToObject(args, "parent", world_name(ctx->w, choice));
        else
            cJSON_AddNullToObject(args, "parent");
        run(ins, ctx, "entity.update", args, NULL);
    }
}

static void
edit_components(inspector* ins, command_context* ctx, entity_id e)
{
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        if (t == COMPONENT_NAME || t == COMPONENT_PARENT || !world_has(ctx->w, e, (component_type)t))
            continue;

        const component_desc* component = reflect_component((component_type)t);
        igPushID_Int((int)t);

        // Every component but Transform can be removed with the header's X.
        b8 keep = true;
        b8 open = t == COMPONENT_TRANSFORM
                ? igCollapsingHeader_TreeNodeFlags(component->name, ImGuiTreeNodeFlags_DefaultOpen)
                : igCollapsingHeader_BoolPtr(component->name, &keep, ImGuiTreeNodeFlags_DefaultOpen);

        if (!keep)
        {
            cJSON* args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
            cJSON_AddStringToObject(args, "component", component->name);
            run(ins, ctx, "component.remove", args, NULL);
        }
        else if (open)
        {
            // Read after any edit above, so the fields show this frame's state.
            const void* data = reflect_component_data(ctx->w, e, (component_type)t);
            for (uint32 f = 0; data && f < component->field_count; ++f)
                edit_field(ins, ctx, e, component, &component->fields[f], data);
        }

        igPopID();
    }
}

static void
add_component_menu(inspector* ins, command_context* ctx, entity_id e)
{
    if (!igBeginCombo("##add", "Add component...", 0))
        return;

    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        if (t == COMPONENT_NAME || t == COMPONENT_PARENT || t == COMPONENT_TRANSFORM || world_has(ctx->w, e, (component_type)t))
            continue;

        const char* name = reflect_component((component_type)t)->name;
        if (igSelectable_Bool(name, false, 0, (ImVec2_c){ 0, 0 }))
        {
            // Added with its defaults; the fields then show up to edit.
            cJSON* args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
            cJSON_AddStringToObject(args, "component", name);
            cJSON_AddItemToObject(args, "fields", cJSON_CreateObject());
            run(ins, ctx, "component.set", args, NULL);
        }
    }
    igEndCombo();
}

void
inspector_draw(inspector* ins, command_context* ctx, entity_id* selected, const char* window_title)
{
    const ImGuiViewport* viewport = igGetMainViewport();
    igSetNextWindowPos((ImVec2_c){ viewport->WorkPos.x + viewport->WorkSize.x - 640.0f, viewport->WorkPos.y + 10.0f },
                       ImGuiCond_FirstUseEver, (ImVec2_c){ 0.0f, 0.0f });
    igSetNextWindowSize((ImVec2_c){ 320.0f, viewport->WorkSize.y * 0.6f }, ImGuiCond_FirstUseEver);

    b8 window_open = igBegin(window_title, NULL, 0);
    entity_id e    = selected ? *selected : ENTITY_NULL;

    if (window_open)
    {
        if (!world_is_alive(ctx->w, e))
        {
            igTextDisabled("Nothing selected.");
            igTextDisabled("Click an object in the view or a name in Entities.");
        }
        else
        {
            igPushID_Int((int)e);
            edit_name(ins, ctx, e);
            edit_parent(ins, ctx, e);
            igTextDisabled("#%u.%u", entity_slot(e), e >> ENTITY_INDEX_BITS);

            edit_components(ins, ctx, e);

            igSeparator();
            add_component_menu(ins, ctx, e);
            igSameLine(0.0f, -1.0f);
            if (igButton("Delete", (ImVec2_c){ 0, 0 }))
            {
                inspector_finish_edit(ins, ctx);
                cJSON* args = cJSON_CreateObject();
                cJSON_AddStringToObject(args, "entity", world_name(ctx->w, e));
                if (run(ins, ctx, "entity.destroy", args, NULL) && selected)
                    *selected = ENTITY_NULL;
            }

            vec3 lo, hi;
            if (world_is_alive(ctx->w, e) && world_bounds(ctx->w, e, lo, hi))
            {
                igSeparatorText("Bounds (world, read-only)");
                igText("min: (%.3f, %.3f, %.3f)", lo[0], lo[1], lo[2]);
                igText("max: (%.3f, %.3f, %.3f)", hi[0], hi[1], hi[2]);
            }
            igPopID();
        }

        if (ins->error[0])
        {
            igSeparator();
            igPushTextWrapPos(0.0f);
            igTextColored(ERROR_COLOR, "%s", ins->error);
            igPopTextWrapPos();
        }
    }
    igEnd();

    // The edit is over once nothing is being dragged or typed in: close its
    // history group, making it one undo step.
    if (ins->editing && !igIsAnyItemActive())
        inspector_finish_edit(ins, ctx);
}
