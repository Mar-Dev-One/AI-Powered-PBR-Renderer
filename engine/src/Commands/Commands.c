#include "Commands.h"

#include "../Core/FileIO.h"
#include "../Core/Paths.h"
#include "../World/Reflect.h"
#include "../World/Serialize.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// How many existing names an "entity not found" error lists before
// summarising the rest as "and N more".
#define MAX_NAMES_IN_ERROR 20

// --- Command table types -------------------------------------------------------

typedef struct cmd_output
{
    cJSON*      result;
    const char* error_code;
    char        error[1024];
} cmd_output;

typedef b8 (*cmd_handler)(command_context* ctx, const cJSON* args, cmd_output* out);

typedef struct cmd_def
{
    const char*             name;
    const char*             doc;
    const cmd_arg*          args;
    uint32                  arg_count;
    b8                      mutates;   // snapshots, changes, undo step, world_update()
    cmd_handler             handler;   // built-in commands
    const cmd_host_command* host;      // application commands (handler == NULL)
} cmd_def;

static b8
fail(cmd_output* out, const char* code, const char* fmt, ...)
{
    out->error_code = code;

    va_list args;
    va_start(args, fmt);
    vsnprintf(out->error, sizeof(out->error), fmt, args);
    va_end(args);

    return false;
}

// --- Argument helpers ----------------------------------------------------------

static const cJSON*
arg(const cJSON* args, const char* name)
{
    return cJSON_GetObjectItemCaseSensitive(args, name);
}

static b8
arg_string(const cJSON* args, const char* name, b8 required, const char** out_value, cmd_output* out)
{
    const cJSON* item = arg(args, name);
    if (!item || cJSON_IsNull(item))
    {
        *out_value = NULL;
        return required ? fail(out, CMD_ERROR_INVALID_ARGS, "missing required argument \"%s\" (a string)", name) : true;
    }
    if (!cJSON_IsString(item))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"%s\" must be a string", name);

    *out_value = item->valuestring;
    return true;
}

static b8
arg_bool(const cJSON* args, const char* name, b8 default_value, b8* out_value, cmd_output* out)
{
    const cJSON* item = arg(args, name);
    if (!item || cJSON_IsNull(item))
    {
        *out_value = default_value;
        return true;
    }
    if (!cJSON_IsBool(item))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"%s\" must be true or false", name);

    *out_value = cJSON_IsTrue(item);
    return true;
}

// "a, b, c (and 12 more)" -- existing entity names, for not-found errors.
static void
list_entity_names(const world* w, char* buffer, size_t size)
{
    buffer[0] = '\0';
    uint32 listed = 0;

    for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
    {
        size_t used = strlen(buffer);
        if (listed == MAX_NAMES_IN_ERROR)
        {
            snprintf(buffer + used, size - used, " (and %u more)", world_count(w) - listed);
            return;
        }
        snprintf(buffer + used, size - used, "%s%s", listed ? ", " : "", world_name(w, e));
        ++listed;
    }

    if (listed == 0)
        snprintf(buffer, size, "(none -- the scene is empty)");
}

// Resolves an entity argument given as a name or a numeric id.
static b8
arg_entity(const world* w, const cJSON* args, const char* name, b8 required, entity_id* out_entity, cmd_output* out)
{
    const cJSON* item = arg(args, name);
    *out_entity = ENTITY_NULL;

    if (!item || cJSON_IsNull(item))
        return required ? fail(out, CMD_ERROR_INVALID_ARGS, "missing required argument \"%s\" (an entity name or id)", name) : true;

    if (cJSON_IsString(item))
        *out_entity = world_find(w, item->valuestring);
    else if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
        *out_entity = (entity_id)item->valuedouble;
    else
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"%s\" must be an entity name or id", name);

    if (!world_is_alive(w, *out_entity))
    {
        char names[1024];
        list_entity_names(w, names, sizeof(names));
        if (cJSON_IsString(item))
            return fail(out, CMD_ERROR_NOT_FOUND, "no entity named \"%s\"; existing entities: %s", item->valuestring, names);
        return fail(out, CMD_ERROR_NOT_FOUND, "no entity with id %.0f (it may have been destroyed); existing entities: %s",
                    item->valuedouble, names);
    }
    return true;
}

static void
list_component_names(char* buffer, size_t size, b8 settable_only)
{
    buffer[0] = '\0';
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        if (settable_only && (t == COMPONENT_NAME || t == COMPONENT_PARENT))
            continue;
        size_t used = strlen(buffer);
        snprintf(buffer + used, size - used, "%s%s", used ? ", " : "", reflect_component((component_type)t)->name);
    }
}

// Resolves a component name. Name and Parent are rejected: they have their
// own arguments ("name" / "parent") and commands.
static b8
parse_component_name(const char* name, component_type* out_type, cmd_output* out)
{
    const component_desc* c = reflect_component_by_name(name);
    if (!c || c->type == COMPONENT_NAME || c->type == COMPONENT_PARENT)
    {
        char names[256];
        list_component_names(names, sizeof(names), true);
        if (c)
            return fail(out, CMD_ERROR_INVALID_ARGS, "\"%s\" isn't set as a component; use the \"name\" / \"parent\" "
                        "arguments of entity.create or entity.update instead", name);
        return fail(out, CMD_ERROR_INVALID_ARGS, "unknown component \"%s\"; components are: %s", name, names);
    }

    *out_type = c->type;
    return true;
}

// --- Rules that span more than one field ------------------------------------------

// Checked after a component write; the caller rolls back on failure.
static b8
check_component_rules(const world* w, entity_id e, component_type type, cmd_output* out)
{
    if (type == COMPONENT_LIGHT)
    {
        const c_light* l = world_light(w, e);
        if (l->outer_cone_degrees < l->inner_cone_degrees)
            return fail(out, CMD_ERROR_INVALID_VALUE,
                        "Light: outer_cone_degrees (%g) must be at least inner_cone_degrees (%g)",
                        (double)l->outer_cone_degrees, (double)l->inner_cone_degrees);
    }
    return true;
}

// Snapshot of one component, so a write that passes field validation but
// breaks a cross-field rule can be undone. Only for components whose fields
// are all plain data (no setter-managed invariants): Transform, Light, Camera.
typedef struct component_snapshot
{
    component_type type;
    b8             existed;
    union
    {
        c_transform transform;
        c_light     light;
        c_camera    camera;
    };
} component_snapshot;

static b8
snapshot_supported(component_type type)
{
    return type == COMPONENT_TRANSFORM || type == COMPONENT_LIGHT || type == COMPONENT_CAMERA;
}

static void
take_snapshot(const world* w, entity_id e, component_type type, component_snapshot* s)
{
    s->type    = type;
    s->existed = world_has(w, e, type);
    if (!s->existed)
        return;

    switch (type)
    {
        case COMPONENT_TRANSFORM: s->transform = *world_transform(w, e); break;
        case COMPONENT_LIGHT:     s->light     = *world_light(w, e);     break;
        case COMPONENT_CAMERA:    s->camera    = *world_camera(w, e);    break;
        default:                                                         break;
    }
}

static void
restore_snapshot(world* w, entity_id e, const component_snapshot* s)
{
    if (!s->existed)
    {
        world_remove(w, e, s->type);
        return;
    }

    switch (s->type)
    {
        case COMPONENT_TRANSFORM: *world_transform(w, e) = s->transform; break;
        case COMPONENT_LIGHT:     *world_light(w, e)     = s->light;     break;
        case COMPONENT_CAMERA:    *world_camera(w, e)    = s->camera;    break;
        default:                                                         break;
    }
}

// deserialize_component() + the cross-field rules, all-or-nothing.
static b8
set_component(world* w, entity_id e, component_type type, const cJSON* fields, cmd_output* out)
{
    component_snapshot snapshot;
    b8 can_roll_back = snapshot_supported(type);
    if (can_roll_back)
        take_snapshot(w, e, type, &snapshot);

    char error[512];
    if (!deserialize_component(w, e, type, fields, error, sizeof(error)))
        return fail(out, CMD_ERROR_INVALID_VALUE, "%s", error);

    if (!check_component_rules(w, e, type, out))
    {
        if (can_roll_back)
            restore_snapshot(w, e, &snapshot);
        return false;
    }
    return true;
}

// --- Changes ---------------------------------------------------------------------

// "Transform", "Light", ... for the COMPONENT_BIT()s in `mask`.
static cJSON*
component_names(uint32 mask)
{
    cJSON* names = cJSON_CreateArray();
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        if (mask & COMPONENT_BIT(t))
            cJSON_AddItemToArray(names, cJSON_CreateString(reflect_component((component_type)t)->name));
    }
    return names;
}

// What `changes` did, one line per entity. `reverse` describes going from
// each change's `after` back to its `before` (an undo).
static cJSON*
changes_to_json(const history_change* changes, uint32 count, b8 reverse)
{
    cJSON* list = cJSON_CreateArray();

    for (uint32 i = 0; i < count; ++i)
    {
        const entity_state* from = reverse ? &changes[i].after  : &changes[i].before;
        const entity_state* to   = reverse ? &changes[i].before : &changes[i].after;

        uint32 components;
        entity_state_differs(from, to, &components);

        const entity_state* named = to->alive ? to : from;
        const char*         op    = !from->alive ? "created" : !to->alive ? "destroyed" : "changed";

        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "op", op);
        cJSON_AddNumberToObject(entry, "id", (double)named->id);
        cJSON_AddStringToObject(entry, "name", named->name.value);
        cJSON_AddItemToObject(entry, "components", component_names(components));
        cJSON_AddItemToArray(list, entry);
    }

    return list;
}

// Bumps the revision and tells the on_change listener. Returns the change
// list (caller owns it) for the command's response.
static cJSON*
announce_changes(command_context* ctx, const char* source, const history_change* changes, uint32 count, b8 reverse)
{
    cJSON* list = changes_to_json(changes, count, reverse);
    if (count == 0)
        return list;

    ctx->revision++;

    if (ctx->on_change)
    {
        cJSON* event = cJSON_CreateObject();
        cJSON_AddNumberToObject(event, "revision", (double)ctx->revision);
        cJSON_AddStringToObject(event, "source", source);
        cJSON_AddItemReferenceToObject(event, "changes", list);
        ctx->on_change(ctx->on_change_user, event);
        cJSON_Delete(event);
    }

    return list;
}

// History label for one command: its name plus the entity it was about,
// e.g. "component.set lamp".
static void
command_label(const char* name, const cJSON* args, char* out, size_t size)
{
    const cJSON* subject = cJSON_GetObjectItemCaseSensitive(args, "entity");
    if (!cJSON_IsString(subject))
        subject = cJSON_GetObjectItemCaseSensitive(args, "name");

    if (cJSON_IsString(subject))
        snprintf(out, size, "%s %s", name, subject->valuestring);
    else
        snprintf(out, size, "%s", name);
}

static b8
require_history(command_context* ctx, cmd_output* out)
{
    if (!ctx->history)
        return fail(out, CMD_ERROR_UNAVAILABLE, "undo history isn't enabled for this world");
    return true;
}

// --- Commands -------------------------------------------------------------------

static b8 cmd_commands_list(command_context* ctx, const cJSON* args, cmd_output* out);
static b8 cmd_batch(command_context* ctx, const cJSON* args, cmd_output* out);

static b8
cmd_scene_describe(command_context* ctx, const cJSON* args, cmd_output* out)
{
    b8 summary;
    if (!arg_bool(args, "summary", false, &summary, out))
        return false;

    if (!summary)
    {
        out->result = serialize_world(ctx->w);
        cJSON_AddNumberToObject(out->result, "count", world_count(ctx->w));
        return true;
    }

    // One short line per entity: enough to plan with, cheap on context.
    out->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(out->result, "count", world_count(ctx->w));
    cJSON* list = cJSON_AddArrayToObject(out->result, "entities");

    cJSON* full     = serialize_world(ctx->w);
    cJSON* entities = cJSON_GetObjectItem(full, "entities");
    cJSON* entity;
    cJSON_ArrayForEach(entity, entities)
    {
        cJSON* line       = cJSON_CreateObject();
        cJSON* components = cJSON_GetObjectItem(entity, "components");

        cJSON_AddItemToObject(line, "id",     cJSON_Duplicate(cJSON_GetObjectItem(entity, "id"), true));
        cJSON_AddItemToObject(line, "name",   cJSON_Duplicate(cJSON_GetObjectItem(entity, "name"), true));
        cJSON_AddItemToObject(line, "parent", cJSON_Duplicate(cJSON_GetObjectItem(entity, "parent"), true));
        cJSON_AddItemToObject(line, "position",
                              cJSON_Duplicate(cJSON_GetObjectItem(cJSON_GetObjectItem(components, "Transform"), "position"), true));

        cJSON* names = cJSON_AddArrayToObject(line, "components");
        cJSON* component;
        cJSON_ArrayForEach(component, components)
            cJSON_AddItemToArray(names, cJSON_CreateString(component->string));

        cJSON_AddItemToArray(list, line);
    }
    cJSON_Delete(full);
    return true;
}

static b8
cmd_scene_clear(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    uint32 count = world_count(ctx->w);
    world_clear(ctx->w);

    out->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(out->result, "destroyed", count);
    return true;
}

static b8
cmd_entity_get(command_context* ctx, const cJSON* args, cmd_output* out)
{
    entity_id e;
    if (!arg_entity(ctx->w, args, "entity", true, &e, out))
        return false;

    out->result = serialize_entity(ctx->w, e);
    return true;
}

static b8
cmd_entity_create(command_context* ctx, const cJSON* args, cmd_output* out)
{
    world* w = ctx->w;

    const char* name;
    entity_id   parent;
    if (!arg_string(args, "name", false, &name, out) || !arg_entity(w, args, "parent", false, &parent, out))
        return false;

    const cJSON* components = arg(args, "components");
    if (components && !cJSON_IsNull(components) && !cJSON_IsObject(components))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"components\" must be an object like "
                    "{\"Transform\": {...}, \"Light\": {...}}");

    // Resolve every component name before creating anything.
    const cJSON* item;
    cJSON_ArrayForEach(item, components)
    {
        component_type type;
        if (!parse_component_name(item->string, &type, out))
            return false;
    }

    entity_id e = world_create(w, name);
    if (parent != ENTITY_NULL)
        world_set_parent(w, e, parent);   // can't cycle: `e` is brand new

    cJSON_ArrayForEach(item, components)
    {
        component_type type;
        parse_component_name(item->string, &type, out);

        if (!set_component(w, e, type, item, out))
        {
            world_destroy(w, e);   // all-or-nothing
            return false;
        }
    }

    out->result = serialize_entity(w, e);
    return true;
}

static void
collect_subtree_names(const world* w, entity_id e, cJSON* names)
{
    cJSON_AddItemToArray(names, cJSON_CreateString(world_name(w, e)));

    const uint32 mask = COMPONENT_BIT(COMPONENT_PARENT);
    for (entity_id child = world_next(w, ENTITY_NULL, mask); child; child = world_next(w, child, mask))
    {
        if (world_parent(w, child) == e)
            collect_subtree_names(w, child, names);
    }
}

static b8
cmd_entity_destroy(command_context* ctx, const cJSON* args, cmd_output* out)
{
    entity_id e;
    if (!arg_entity(ctx->w, args, "entity", true, &e, out))
        return false;

    // Reported before destroying, so the caller learns which children went too.
    out->result = cJSON_CreateObject();
    collect_subtree_names(ctx->w, e, cJSON_AddArrayToObject(out->result, "destroyed"));

    world_destroy(ctx->w, e);
    return true;
}

static b8
cmd_entity_update(command_context* ctx, const cJSON* args, cmd_output* out)
{
    world* w = ctx->w;

    entity_id e;
    if (!arg_entity(w, args, "entity", true, &e, out))
        return false;

    const cJSON* name_item   = arg(args, "name");
    const cJSON* parent_item = arg(args, "parent");
    if (!name_item && !parent_item)
        return fail(out, CMD_ERROR_INVALID_ARGS, "nothing to update: pass \"name\" and/or \"parent\" (null = root)");

    // Validate both before applying either.
    cJSON* name_fields   = NULL;
    cJSON* parent_fields = NULL;
    if (name_item)
    {
        name_fields = cJSON_CreateObject();
        cJSON_AddItemToObject(name_fields, "value", cJSON_Duplicate(name_item, true));
    }
    if (parent_item)
    {
        parent_fields = cJSON_CreateObject();
        cJSON_AddItemToObject(parent_fields, "parent", cJSON_Duplicate(parent_item, true));
    }

    // Parent first: it can't make the name invalid, while renaming first
    // would leave a renamed entity behind if the parent was then rejected.
    entity_id old_parent = world_parent(w, e);
    b8 ok = true;
    if (parent_fields)
        ok = set_component(w, e, COMPONENT_PARENT, parent_fields, out);
    if (ok && name_fields && !set_component(w, e, COMPONENT_NAME, name_fields, out))
    {
        world_set_parent(w, e, old_parent);   // roll the parent change back
        ok = false;
    }

    cJSON_Delete(name_fields);
    cJSON_Delete(parent_fields);
    if (!ok)
        return false;

    out->result = serialize_entity(w, e);
    return true;
}

static b8
cmd_component_set(command_context* ctx, const cJSON* args, cmd_output* out)
{
    entity_id   e;
    const char* component_name;
    if (!arg_entity(ctx->w, args, "entity", true, &e, out) || !arg_string(args, "component", true, &component_name, out))
        return false;

    component_type type;
    if (!parse_component_name(component_name, &type, out))
        return false;

    const cJSON* fields = arg(args, "fields");
    if (!cJSON_IsObject(fields))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"fields\" must be an object of field values, "
                    "e.g. {\"intensity\": 5}");

    if (!set_component(ctx->w, e, type, fields, out))
        return false;

    out->result = serialize_entity(ctx->w, e);
    return true;
}

static b8
cmd_component_remove(command_context* ctx, const cJSON* args, cmd_output* out)
{
    entity_id   e;
    const char* component_name;
    if (!arg_entity(ctx->w, args, "entity", true, &e, out) || !arg_string(args, "component", true, &component_name, out))
        return false;

    component_type type;
    if (!parse_component_name(component_name, &type, out))
        return false;

    if (type == COMPONENT_TRANSFORM)
        return fail(out, CMD_ERROR_INVALID_ARGS, "every entity has a Transform; it can't be removed");

    if (!world_has(ctx->w, e, type))
        return fail(out, CMD_ERROR_NOT_FOUND, "\"%s\" has no %s component", world_name(ctx->w, e), component_name);

    world_remove(ctx->w, e, type);
    out->result = serialize_entity(ctx->w, e);
    return true;
}

// assets.list: what can be put in a MeshRenderer's "model".
typedef struct model_listing
{
    cJSON*      array;
    const char* prefix;   // "models/" or "models/<subdir>/"
    int         depth;
} model_listing;

static b8
is_model_file(const char* name)
{
    const char* dot = strrchr(name, '.');
    return dot && (strcmp(dot, ".glb") == 0 || strcmp(dot, ".gltf") == 0 || strcmp(dot, ".obj") == 0);
}

static void
visit_model_entry(const char* name, b8 is_directory, void* user_data)
{
    model_listing* listing = user_data;

    char key[512];
    snprintf(key, sizeof(key), "%s%s", listing->prefix, name);

    if (is_directory && listing->depth < 2)
    {
        // glTF models often come as a folder (model.gltf + .bin + textures).
        char subdir[512], prefix[512];
        char* path = asset_path(key);
        snprintf(subdir, sizeof(subdir), "%s", path);
        free(path);
        snprintf(prefix, sizeof(prefix), "%s/", key);

        model_listing nested = { listing->array, prefix, listing->depth + 1 };
        file_list_directory(subdir, visit_model_entry, &nested);
    }
    else if (!is_directory && is_model_file(name))
    {
        cJSON_AddItemToArray(listing->array, cJSON_CreateString(key));
    }
}

static b8
cmd_assets_list(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    out->result = cJSON_CreateObject();

    cJSON* builtins = cJSON_AddArrayToObject(out->result, "builtins");
    cJSON_AddItemToArray(builtins, cJSON_CreateString("builtin:cube"));
    cJSON_AddItemToArray(builtins, cJSON_CreateString("builtin:sphere"));
    cJSON_AddItemToArray(builtins, cJSON_CreateString("builtin:plane"));
    cJSON_AddStringToObject(out->result, "builtins_note",
                            "1 unit across, centred on the origin; the plane lies on XZ facing +Y");

    model_listing listing = { cJSON_AddArrayToObject(out->result, "models"), "models/", 0 };
    char* models_dir = asset_path("models");
    file_list_directory(models_dir, visit_model_entry, &listing);
    free(models_dir);

    // Already in memory, with how many entities use each.
    cJSON* loaded = cJSON_AddArrayToObject(out->result, "loaded");
    const asset_cache* cache = ctx->w->assets;
    for (uint32 i = 0; i < cache->capacity; ++i)
    {
        if (!cache->entries[i].key)
            continue;
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "key", cache->entries[i].key);
        cJSON_AddNumberToObject(entry, "users", cache->entries[i].ref_count);
        cJSON_AddItemToArray(loaded, entry);
    }
    return true;
}

static b8
cmd_history_step(command_context* ctx, cmd_output* out, b8 redo)
{
    if (!require_history(ctx, out))
        return false;
    if (history_in_transaction(ctx->history))
        return fail(out, CMD_ERROR_CONFLICT, "a transaction is open; history.commit or history.rollback it first");

    const history_step* step = redo ? history_redo(ctx->history, ctx->w) : history_undo(ctx->history, ctx->w);
    if (!step)
        return fail(out, CMD_ERROR_NOT_FOUND, redo ? "nothing to redo" : "nothing to undo");

    char source[HISTORY_LABEL_MAX + 16];
    snprintf(source, sizeof(source), "%s %s", redo ? "redo" : "undo", step->label);

    out->result = cJSON_CreateObject();
    cJSON_AddStringToObject(out->result, redo ? "redone" : "undone", step->label);
    cJSON_AddItemToObject(out->result, "changes",
                          announce_changes(ctx, source, step->changes, step->change_count, !redo));
    cJSON_AddBoolToObject(out->result, "can_undo", history_can_undo(ctx->history));
    cJSON_AddBoolToObject(out->result, "can_redo", history_can_redo(ctx->history));
    return true;
}

static b8 cmd_history_undo(command_context* ctx, const cJSON* args, cmd_output* out) { (void)args; return cmd_history_step(ctx, out, false); }
static b8 cmd_history_redo(command_context* ctx, const cJSON* args, cmd_output* out) { (void)args; return cmd_history_step(ctx, out, true); }

static b8
cmd_history_begin(command_context* ctx, const cJSON* args, cmd_output* out)
{
    const char* label;
    if (!require_history(ctx, out) || !arg_string(args, "label", false, &label, out))
        return false;

    history_begin(ctx->history, ctx->w, label);

    out->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(out->result, "depth", ctx->history->depth);
    return true;
}

static b8
cmd_history_commit(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    if (!require_history(ctx, out))
        return false;

    char label[HISTORY_LABEL_MAX];
    snprintf(label, sizeof(label), "%s", ctx->history->label);

    const history_change* changes;
    uint32                count;
    if (!history_commit(ctx->history, ctx->w, &changes, &count))
        return fail(out, CMD_ERROR_CONFLICT, "no transaction is open");

    // The individual commands already announced these; this only reports
    // what the recorded step contains.
    out->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(out->result, "depth", ctx->history->depth);
    if (ctx->history->depth == 0)
    {
        cJSON_AddStringToObject(out->result, "recorded", count ? label : "");
        cJSON_AddItemToObject(out->result, "changes", changes_to_json(changes, count, false));
    }
    return true;
}

static b8
cmd_history_rollback(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    if (!require_history(ctx, out))
        return false;

    char label[HISTORY_LABEL_MAX];
    snprintf(label, sizeof(label), "rollback %s", ctx->history->label);

    history_change* changes;
    uint32          count;
    if (!history_rollback(ctx->history, ctx->w, &changes, &count))
        return fail(out, CMD_ERROR_CONFLICT, "no transaction is open");

    out->result = cJSON_CreateObject();
    cJSON_AddItemToObject(out->result, "changes", announce_changes(ctx, label, changes, count, false));
    free(changes);
    return true;
}

static b8
cmd_history_list(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    if (!require_history(ctx, out))
        return false;

    const command_history* h = ctx->history;

    out->result = cJSON_CreateObject();
    cJSON* steps = cJSON_AddArrayToObject(out->result, "steps");
    for (uint32 i = 0; i < h->step_count; ++i)
    {
        cJSON* step = cJSON_CreateObject();
        cJSON_AddStringToObject(step, "label", h->steps[i].label);
        cJSON_AddNumberToObject(step, "entities_changed", h->steps[i].change_count);
        cJSON_AddBoolToObject(step, "applied", i < h->cursor);
        cJSON_AddItemToArray(steps, step);
    }
    cJSON_AddBoolToObject(out->result, "can_undo", history_can_undo(h));
    cJSON_AddBoolToObject(out->result, "can_redo", history_can_redo(h));
    cJSON_AddNumberToObject(out->result, "transaction_depth", h->depth);
    return true;
}

// --- Placement --------------------------------------------------------------------
// LLMs are bad at 3D arithmetic, so placement is by intent -- "on the
// ground", "on top of the table", "left of the sofa" -- and the numbers are
// worked out here from bounding boxes. World space, Y up, ground at y = 0;
// "front" is +Z (towards the default camera), "left" is -X.

typedef struct aabb
{
    vec3 min;
    vec3 max;
} aabb;

static const char* const SIDE_NAMES[] = { "on_top", "left", "right", "front", "behind" };
#define SIDE_COUNT 5

// Model bounds after `transform`'s rotation and scale (not its position):
// the box relative to the entity's origin once placed.
static void
oriented_bounds(const model* m, const c_transform* transform, aabb* out)
{
    c_transform no_position = *transform;
    glm_vec3_zero(no_position.position);

    mat4 matrix;
    glm_translate_make(matrix, no_position.position);
    glm_rotate_y(matrix, glm_rad(no_position.rotation_deg[1]), matrix);
    glm_rotate_x(matrix, glm_rad(no_position.rotation_deg[0]), matrix);
    glm_rotate_z(matrix, glm_rad(no_position.rotation_deg[2]), matrix);
    glm_scale(matrix, no_position.scale);

    for (int corner = 0; corner < 8; ++corner)
    {
        vec3 p = {
            (corner & 1) ? m->bounds_max[0] : m->bounds_min[0],
            (corner & 2) ? m->bounds_max[1] : m->bounds_min[1],
            (corner & 4) ? m->bounds_max[2] : m->bounds_min[2],
        };
        vec3 q;
        glm_mat4_mulv3(matrix, p, 1.0f, q);

        if (corner == 0)
        {
            glm_vec3_copy(q, out->min);
            glm_vec3_copy(q, out->max);
        }
        else
        {
            glm_vec3_minv(out->min, q, out->min);
            glm_vec3_maxv(out->max, q, out->max);
        }
    }
}

// An entity's own box relative to its origin: its model's oriented bounds,
// or a point if it has no model.
static void
entity_own_bounds(const world* w, entity_id e, aabb* out)
{
    const c_mesh_renderer* mr = world_mesh_renderer(w, e);
    const model*           m  = mr ? assets_model(w->assets, mr->model) : NULL;

    if (m)
        oriented_bounds(m, world_transform(w, e), out);
    else
        memset(out, 0, sizeof(*out));
}

// Where another entity is: its world bounds, or its position as a point.
static void
target_bounds(const world* w, entity_id e, aabb* out)
{
    if (world_bounds(w, e, out->min, out->max))
        return;

    const vec4* m = world_matrix(w, e);
    vec3 p = { m[3][0], m[3][1], m[3][2] };
    glm_vec3_copy(p, out->min);
    glm_vec3_copy(p, out->max);
}

static b8
parse_vec3(const cJSON* item, vec3 out)
{
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        const cJSON* n = cJSON_GetArrayItem(item, i);
        if (!cJSON_IsNumber(n))
            return false;
        out[i] = (f32)n->valuedouble;
    }
    return true;
}

// Turns the placement arguments ("position", "on_ground", "relative_to")
// into the world position an object with box `own` should have. `current`
// is used for whatever the arguments leave open; with no placement
// arguments at all, `default_on_ground` decides between "rest on the ground
// at current x/z" and "stay at current".
static b8
resolve_placement(const world* w, entity_id self, const cJSON* args, const aabb* own,
                  const vec3 current, b8 default_on_ground, vec3 out_position, cmd_output* out)
{
    const cJSON* position_item = arg(args, "position");
    const cJSON* relative_item = arg(args, "relative_to");

    b8 on_ground;
    if (!arg_bool(args, "on_ground", false, &on_ground, out))
        return false;

    b8 has_position = position_item && !cJSON_IsNull(position_item);
    b8 has_relative = relative_item && !cJSON_IsNull(relative_item);

    if (has_relative && (has_position || on_ground))
        return fail(out, CMD_ERROR_INVALID_ARGS, "use either \"relative_to\" or \"position\"/\"on_ground\", not both");

    glm_vec3_copy((f32*)current, out_position);

    if (has_position && !parse_vec3(position_item, out_position))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"position\" must be [x, y, z] in meters");

    if (!has_position && !has_relative && !on_ground)
        on_ground = default_on_ground;

    if (on_ground)
    {
        out_position[1] = -own->min[1];   // lowest point at y = 0; x/z from "position" or as they were
        return true;
    }

    if (!has_relative)
        return true;

    // --- relative_to: { "entity", "side", "gap" } ---
    if (!cJSON_IsObject(relative_item))
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"relative_to\" must be {\"entity\": ..., \"side\": ..., \"gap\": meters}");

    entity_id target;
    if (!arg_entity(w, relative_item, "entity", true, &target, out))
        return false;
    if (target == self)
        return fail(out, CMD_ERROR_INVALID_ARGS, "an entity can't be placed relative to itself");

    const char* side_name;
    if (!arg_string(relative_item, "side", true, &side_name, out))
        return false;

    int side = -1;
    for (int s = 0; s < SIDE_COUNT; ++s)
    {
        if (strcmp(side_name, SIDE_NAMES[s]) == 0)
            side = s;
    }
    if (side < 0)
        return fail(out, CMD_ERROR_INVALID_ARGS, "relative_to.side \"%s\" is not one of \"on_top\", \"left\" (-X), "
                    "\"right\" (+X), \"front\" (+Z) or \"behind\" (-Z)", side_name);

    const cJSON* gap_item = arg(relative_item, "gap");
    f32 gap = side == 0 ? 0.0f : 0.05f;
    if (gap_item && !cJSON_IsNull(gap_item))
    {
        if (!cJSON_IsNumber(gap_item) || gap_item->valuedouble < 0.0)
            return fail(out, CMD_ERROR_INVALID_ARGS, "relative_to.gap must be a distance in meters, 0 or more");
        gap = (f32)gap_item->valuedouble;
    }

    aabb t;
    target_bounds(w, target, &t);

    // Centred on the target and standing on the same floor, then pushed out
    // to the requested side.
    vec3 target_center, own_center;
    glm_vec3_center(t.min, t.max, target_center);
    glm_vec3_center((f32*)own->min, (f32*)own->max, own_center);

    out_position[0] = target_center[0] - own_center[0];
    out_position[1] = t.min[1] - own->min[1];
    out_position[2] = target_center[2] - own_center[2];

    switch (side)
    {
        case 0: out_position[1] = t.max[1] + gap - own->min[1]; break;   // on_top
        case 1: out_position[0] = t.min[0] - gap - own->max[0]; break;   // left (-X)
        case 2: out_position[0] = t.max[0] + gap - own->min[0]; break;   // right (+X)
        case 3: out_position[2] = t.max[2] + gap - own->min[2]; break;   // front (+Z)
        case 4: out_position[2] = t.min[2] - gap - own->max[2]; break;   // behind (-Z)
    }
    return true;
}

// The entity's JSON plus its world-space box, so the caller can place the
// next thing relative to it without asking again.
static cJSON*
entity_with_bounds(const world* w, entity_id e)
{
    cJSON* json = serialize_entity(w, e);
    vec3   bmin, bmax;
    if (world_bounds(w, e, bmin, bmax))
    {
        cJSON* bounds = cJSON_AddObjectToObject(json, "bounds");
        for (int k = 0; k < 2; ++k)
        {
            const f32* v = k ? bmax : bmin;
            char buffer[3][32];
            cJSON* array = cJSON_AddArrayToObject(bounds, k ? "max" : "min");
            for (int i = 0; i < 3; ++i)
            {
                snprintf(buffer[i], sizeof(buffer[i]), "%.4g", (double)v[i]);
                cJSON_AddItemToArray(array, cJSON_CreateNumber(strtod(buffer[i], NULL)));
            }
        }
    }
    return json;
}

// "models/Desk Lamp.glb" -> "desk_lamp"; "builtin:cube" -> "cube".
static void
default_name_from_key(const char* key, char* out, size_t size)
{
    const char* base = key;
    const char* colon = strrchr(base, ':');
    if (colon) base = colon + 1;
    const char* slash = strrchr(base, '/');
    if (slash) base = slash + 1;

    size_t n = 0;
    for (const char* c = base; *c && *c != '.' && n + 1 < size; ++c)
        out[n++] = (*c == ' ' || *c == '-') ? '_' : (char)tolower((unsigned char)*c);
    out[n] = '\0';

    if (n == 0)
        snprintf(out, size, "model");
}

static b8
cmd_place_model(command_context* ctx, const cJSON* args, cmd_output* out)
{
    world* w = ctx->w;

    const char* key;
    const char* name;
    if (!arg_string(args, "model", true, &key, out) || !arg_string(args, "name", false, &name, out))
        return false;

    asset_id asset = assets_resolve(w->assets, key);
    const model* m = assets_model(w->assets, asset);
    if (!m)
    {
        if (assets_is_builtin_key(key))
            return fail(out, CMD_ERROR_INVALID_VALUE, "unknown built-in \"%s\"; built-ins are \"builtin:cube\", "
                        "\"builtin:sphere\" and \"builtin:plane\"", key);
        return fail(out, CMD_ERROR_INVALID_VALUE, "could not load the model \"%s\"; assets.list shows what exists", key);
    }

    c_transform transform;
    glm_vec3_zero(transform.position);
    glm_vec3_zero(transform.rotation_deg);
    glm_vec3_one(transform.scale);

    // --- size / scale ---
    const cJSON* size_item  = arg(args, "size");
    const cJSON* scale_item = arg(args, "scale");
    b8 has_size  = size_item  && !cJSON_IsNull(size_item);
    b8 has_scale = scale_item && !cJSON_IsNull(scale_item);
    if (has_size && has_scale)
        return fail(out, CMD_ERROR_INVALID_ARGS, "use either \"size\" or \"scale\", not both");

    if (has_size)
    {
        if (!cJSON_IsNumber(size_item) || size_item->valuedouble <= 0.0)
            return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"size\" must be a length in meters, more than 0");

        vec3 extent;
        glm_vec3_sub((f32*)m->bounds_max, (f32*)m->bounds_min, extent);
        f32 largest = glm_vec3_max(extent);
        glm_vec3_fill(transform.scale, largest > 1e-8f ? (f32)size_item->valuedouble / largest : 1.0f);
    }
    else if (has_scale)
    {
        if (cJSON_IsNumber(scale_item))
            glm_vec3_fill(transform.scale, (f32)scale_item->valuedouble);
        else if (!parse_vec3(scale_item, transform.scale))
            return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"scale\" must be a number or [x, y, z]");

        if (transform.scale[0] <= 0.0f || transform.scale[1] <= 0.0f || transform.scale[2] <= 0.0f)
            return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"scale\" must be more than 0 on every axis");
    }

    const cJSON* rotation_item = arg(args, "rotation_deg");
    if (rotation_item && !cJSON_IsNull(rotation_item))
    {
        if (cJSON_IsNumber(rotation_item))
            transform.rotation_deg[1] = (f32)rotation_item->valuedouble;   // a lone number is a yaw
        else if (!parse_vec3(rotation_item, transform.rotation_deg))
            return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"rotation_deg\" must be a yaw in degrees or [pitch, yaw, roll]");
    }

    // --- material: {"base_color": ..., "roughness": ...} -> "material.*" fields ---
    const cJSON* material_item = arg(args, "material");
    cJSON*       material_fields = NULL;
    if (material_item && !cJSON_IsNull(material_item))
    {
        if (!cJSON_IsObject(material_item))
            return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"material\" must be an object like "
                        "{\"base_color\": [0.8, 0.1, 0.1], \"roughness\": 0.4}");

        material_fields = cJSON_CreateObject();
        const cJSON* field;
        cJSON_ArrayForEach(field, material_item)
        {
            char prefixed[64];
            snprintf(prefixed, sizeof(prefixed), "%s%s", strncmp(field->string, "material.", 9) == 0 ? "" : "material.",
                     field->string);
            cJSON_AddItemToObject(material_fields, prefixed, cJSON_Duplicate(field, true));
        }
    }

    // --- where ---
    aabb own;
    oriented_bounds(m, &transform, &own);

    vec3 origin = { 0.0f, 0.0f, 0.0f };
    if (!resolve_placement(w, ENTITY_NULL, args, &own, origin, true, transform.position, out))
    {
        cJSON_Delete(material_fields);
        return false;
    }

    // --- create (the dispatcher undoes all of it if anything below fails) ---
    char default_name[ENTITY_NAME_MAX];
    default_name_from_key(key, default_name, sizeof(default_name));

    entity_id e = world_create(w, name ? name : default_name);
    *world_transform(w, e) = transform;
    world_add_mesh_renderer(w, e, asset);

    if (material_fields)
    {
        char error[512];
        b8   ok = deserialize_component(w, e, COMPONENT_MESH_RENDERER, material_fields, error, sizeof(error));
        cJSON_Delete(material_fields);
        if (!ok)
            return fail(out, CMD_ERROR_INVALID_VALUE, "material: %s", error);
    }

    world_update(w);
    out->result = entity_with_bounds(w, e);
    return true;
}

static b8
cmd_entity_place(command_context* ctx, const cJSON* args, cmd_output* out)
{
    world* w = ctx->w;

    entity_id e;
    if (!arg_entity(w, args, "entity", true, &e, out))
        return false;

    if (world_parent(w, e) != ENTITY_NULL)
        return fail(out, CMD_ERROR_CONFLICT, "\"%s\" is parented to \"%s\" and placement works in world space; "
                    "set its Transform directly, or entity.update it to parent null first",
                    world_name(w, e), world_name(w, world_parent(w, e)));

    if (!arg(args, "position") && !arg(args, "on_ground") && !arg(args, "relative_to"))
        return fail(out, CMD_ERROR_INVALID_ARGS, "say where: \"position\", \"on_ground\" and/or \"relative_to\"");

    aabb own;
    entity_own_bounds(w, e, &own);

    c_transform* t = world_transform(w, e);
    vec3 position;
    if (!resolve_placement(w, e, args, &own, t->position, false, position, out))
        return false;

    glm_vec3_copy(position, t->position);
    world_update(w);
    out->result = entity_with_bounds(w, e);
    return true;
}

// --- Scene files ------------------------------------------------------------------

#define SCENE_FORMAT  "pbr-scene"
#define SCENE_VERSION 1

// The scenes folder, created if needed. Caller frees.
static char*
scenes_directory(const command_context* ctx)
{
    char* dir;
    if (ctx->scenes_dir)
    {
        size_t length = strlen(ctx->scenes_dir) + 1;
        dir = malloc(length);
        if (!dir)
            FATAL("commands: out of memory");
        memcpy(dir, ctx->scenes_dir, length);
    }
    else
    {
        dir = asset_path("scenes");
    }

    file_make_directory(dir);
    return dir;
}

// Scene names are plain file names -- letters, digits, '_' and '-' -- so a
// request can never reach outside the scenes folder.
static b8
scene_path(const command_context* ctx, const char* name, char* out_path, size_t size, cmd_output* out)
{
    size_t length = strlen(name);
    if (length == 0 || length > 64)
        return fail(out, CMD_ERROR_INVALID_ARGS, "scene name must be 1 to 64 characters");

    for (const char* c = name; *c; ++c)
    {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-')
            return fail(out, CMD_ERROR_INVALID_ARGS, "scene name \"%s\" may only contain letters, digits, '_' and '-'", name);
    }

    char* dir = scenes_directory(ctx);
    snprintf(out_path, size, "%s/%s.json", dir, name);
    free(dir);
    return true;
}

static b8
cmd_scene_save(command_context* ctx, const cJSON* args, cmd_output* out)
{
    const char* name;
    char        path[1024];
    if (!arg_string(args, "name", true, &name, out) || !scene_path(ctx, name, path, sizeof(path), out))
        return false;

    cJSON* scene = serialize_world(ctx->w);
    cJSON_AddStringToObject(scene, "format", SCENE_FORMAT);
    cJSON_AddNumberToObject(scene, "version", SCENE_VERSION);

    char* text = cJSON_Print(scene);
    b8    ok   = file_write_string(path, text);
    cJSON_free(text);
    cJSON_Delete(scene);

    if (!ok)
        return fail(out, CMD_ERROR_UNAVAILABLE, "could not write %s", path);

    out->result = cJSON_CreateObject();
    cJSON_AddStringToObject(out->result, "name", name);
    cJSON_AddNumberToObject(out->result, "entities", world_count(ctx->w));
    return true;
}

static void
visit_scene_entry(const char* name, b8 is_directory, void* user_data)
{
    size_t length = strlen(name);
    if (is_directory || length < 6 || strcmp(name + length - 5, ".json") != 0)
        return;

    char stem[128];
    snprintf(stem, sizeof(stem), "%.*s", (int)(length - 5), name);
    cJSON_AddItemToArray((cJSON*)user_data, cJSON_CreateString(stem));
}

static b8
cmd_scene_list(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;
    out->result = cJSON_CreateObject();
    cJSON* names = cJSON_AddArrayToObject(out->result, "scenes");

    char* dir = scenes_directory(ctx);
    file_list_directory(dir, visit_scene_entry, names);
    free(dir);
    return true;
}

static b8
cmd_scene_load(command_context* ctx, const cJSON* args, cmd_output* out)
{
    world* w = ctx->w;

    const char* name;
    char        path[1024];
    b8          replace;
    if (!arg_string(args, "name", true, &name, out) || !scene_path(ctx, name, path, sizeof(path), out)
        || !arg_bool(args, "replace", true, &replace, out))
        return false;

    char* text = file_exists(path) ? file_read_to_string(path) : NULL;
    if (!text)
        return fail(out, CMD_ERROR_NOT_FOUND, "no saved scene \"%s\"; scene.list shows the saved ones", name);

    cJSON* scene = cJSON_Parse(text);
    free(text);

    const cJSON* format   = cJSON_GetObjectItem(scene, "format");
    const cJSON* entities = cJSON_GetObjectItem(scene, "entities");
    if (!scene || !cJSON_IsString(format) || strcmp(format->valuestring, SCENE_FORMAT) != 0 || !cJSON_IsArray(entities))
    {
        cJSON_Delete(scene);
        return fail(out, CMD_ERROR_INVALID_VALUE, "\"%s\" is not a scene file", name);
    }

    if (replace)
        world_clear(w);

    // Names in the file -> the entities made for them (names may change
    // when merging into a scene that already uses them).
    uint32     count     = (uint32)cJSON_GetArraySize(entities);
    entity_id* created   = calloc(count ? count : 1, sizeof(*created));
    if (!created)
        FATAL("commands: out of memory");

    out->result = cJSON_CreateObject();
    cJSON* names = cJSON_AddArrayToObject(out->result, "entities");

    b8     ok    = true;
    uint32 index = 0;
    const cJSON* entity;
    cJSON_ArrayForEach(entity, entities)
    {
        const char* entity_name = cJSON_GetStringValue(cJSON_GetObjectItem(entity, "name"));
        entity_id   e           = world_create(w, entity_name);
        created[index] = e;

        // Parents come earlier in the file (serialize_world()'s order).
        const char* parent_name = cJSON_GetStringValue(cJSON_GetObjectItem(entity, "parent"));
        if (parent_name)
        {
            entity_id parent = ENTITY_NULL;
            uint32    i      = 0;
            const cJSON* earlier;
            cJSON_ArrayForEach(earlier, entities)
            {
                if (i >= index)
                    break;
                const char* earlier_name = cJSON_GetStringValue(cJSON_GetObjectItem(earlier, "name"));
                if (earlier_name && strcmp(earlier_name, parent_name) == 0)
                    parent = created[i];
                ++i;
            }
            if (parent == ENTITY_NULL)
            {
                ok = fail(out, CMD_ERROR_INVALID_VALUE, "entity \"%s\": parent \"%s\" isn't defined before it",
                          entity_name ? entity_name : "?", parent_name);
                break;
            }
            world_set_parent(w, e, parent);
        }

        const cJSON* component;
        cJSON_ArrayForEach(component, cJSON_GetObjectItem(entity, "components"))
        {
            component_type type;
            char error[512];
            if (!parse_component_name(component->string, &type, out))
            {
                ok = false;
                break;
            }
            if (!deserialize_component(w, e, type, component, error, sizeof(error)))
            {
                ok = fail(out, CMD_ERROR_INVALID_VALUE, "entity \"%s\": %s", world_name(w, e), error);
                break;
            }
        }
        if (!ok)
            break;

        cJSON_AddItemToArray(names, cJSON_CreateString(world_name(w, e)));
        ++index;
    }

    free(created);
    cJSON_Delete(scene);

    if (!ok)
    {
        cJSON_Delete(out->result);
        out->result = NULL;
        return false;   // the dispatcher restores the world as it was
    }

    cJSON_AddNumberToObject(out->result, "loaded", index);
    return true;
}

// --- The table ------------------------------------------------------------------------

static const cmd_arg ARGS_NONE[1] = { { 0 } };

static const cmd_arg ARGS_DESCRIBE[] = {
    { "summary", "boolean", false, "true = one short line per entity (id, name, parent, position, component names) "
                                   "instead of every field; use for large scenes" },
};

static const cmd_arg ARGS_ENTITY[] = {
    { "entity", "entity", true, "Name or id of the entity" },
};

static const cmd_arg ARGS_CREATE[] = {
    { "name",       "string", false, "Unique name; a taken name gets _2, _3... (the result says which)" },
    { "parent",     "entity", false, "Entity to place this one relative to; omit for the scene root" },
    { "components", "object", false, "Initial components by name, e.g. {\"Transform\": {\"position\": [0, 1, 0]}, "
                                     "\"MeshRenderer\": {\"model\": \"builtin:cube\"}}" },
};

static const cmd_arg ARGS_UPDATE[] = {
    { "entity", "entity", true,  "Name or id of the entity" },
    { "name",   "string", false, "New unique name" },
    { "parent", "entity", false, "New parent; null moves it to the scene root" },
};

static const cmd_arg ARGS_COMPONENT_SET[] = {
    { "entity",    "entity",    true, "Name or id of the entity" },
    { "component", "component", true, "Transform, MeshRenderer, Light or Camera; added if missing" },
    { "fields",    "object",    true, "Only the fields to change, e.g. {\"intensity\": 5}" },
};

static const cmd_arg ARGS_COMPONENT_REMOVE[] = {
    { "entity",    "entity",    true, "Name or id of the entity" },
    { "component", "component", true, "MeshRenderer, Light or Camera" },
};

#define PLACEMENT_ARGS \
    { "position",    "vec3",    false, "[x, y, z] in meters (Y up). With on_ground, only x and z are used" }, \
    { "on_ground",   "boolean", false, "true = rest its lowest point on the ground (y = 0)" }, \
    { "relative_to", "object",  false, "{\"entity\": name, \"side\": \"on_top\" | \"left\" | \"right\" | \"front\" | \"behind\", " \
                                       "\"gap\": meters}: next to or on top of another entity, without overlapping it. " \
                                       "left = -X, right = +X, front = +Z (towards the default camera), behind = -Z" }

static const cmd_arg ARGS_PLACE_MODEL[] = {
    { "model",        "string", true,  "Asset key from assets.list, e.g. \"builtin:cube\" or \"models/Avocado.glb\"" },
    { "name",         "string", false, "Unique name; defaults to the model's file name" },
    { "size",         "number", false, "Length in meters of its longest side -- the easy way to get real-world size, "
                                       "since model files come in arbitrary units" },
    { "scale",        "vec3",   false, "Scale factor (a number or [x, y, z]) instead of size" },
    { "rotation_deg", "vec3",   false, "Yaw in degrees (a number), or [pitch, yaw, roll]" },
    PLACEMENT_ARGS,
    { "material",     "object", false, "Overrides of the model's own material: base_color [r, g, b] (linear 0-1), "
                                       "metallic 0-1, roughness 0-1, emissive [r, g, b]" },
};

static const cmd_arg ARGS_ENTITY_PLACE[] = {
    { "entity", "entity", true, "Name or id of an unparented entity to move" },
    PLACEMENT_ARGS,
};

static const cmd_arg ARGS_SCENE_NAME[] = {
    { "name", "string", true, "Scene file name: letters, digits, '_' and '-'" },
};

static const cmd_arg ARGS_SCENE_LOAD[] = {
    { "name",    "string",  true,  "Scene file name, as scene.list shows it" },
    { "replace", "boolean", false, "true (default) = clear the current scene first; false = add to it" },
};

static const cmd_arg ARGS_HISTORY_BEGIN[] = {
    { "label", "string", false, "What the grouped change does, shown in history.list and when undone, "
                                "e.g. \"add a reading lamp\"" },
};

static const cmd_arg ARGS_BATCH[] = {
    { "commands", "array",  true,  "Requests to run in order, each {\"cmd\": ..., \"args\": {...}}; if one fails, "
                                   "every earlier one is undone and nothing changes" },
    { "label",    "string", false, "Name of the single undo step the batch records" },
};

#define ARGS(array) array, (uint32)(sizeof(array) / sizeof((array)[0]))
#define NO_ARGS     ARGS_NONE, 0

static const cmd_def COMMANDS[] = {
    { "commands.list",    "Describes every command and its arguments",                       NO_ARGS,                     false, cmd_commands_list },
    { "scene.describe",   "Every entity with its components",                                ARGS(ARGS_DESCRIBE),         false, cmd_scene_describe },
    { "scene.clear",      "Destroys every entity",                                           NO_ARGS,                     true,  cmd_scene_clear },
    { "entity.get",       "One entity with all its components",                              ARGS(ARGS_ENTITY),           false, cmd_entity_get },
    { "entity.create",    "Creates an entity, optionally with a parent and components",     ARGS(ARGS_CREATE),           true,  cmd_entity_create },
    { "entity.update",    "Renames an entity and/or changes its parent",                     ARGS(ARGS_UPDATE),           true,  cmd_entity_update },
    { "entity.destroy",   "Destroys an entity and everything parented to it",                ARGS(ARGS_ENTITY),           true,  cmd_entity_destroy },
    { "component.set",    "Sets fields of a component, adding the component if missing",    ARGS(ARGS_COMPONENT_SET),    true,  cmd_component_set },
    { "component.remove", "Removes a component from an entity",                              ARGS(ARGS_COMPONENT_REMOVE), true,  cmd_component_remove },
    { "assets.list",      "Models that can be used in a MeshRenderer's \"model\" field",     NO_ARGS,                     false, cmd_assets_list },
    { "place_model",      "Adds a model sized in meters and placed by intent: on the ground, on top of or beside another entity",
                                                                                              ARGS(ARGS_PLACE_MODEL),      true,  cmd_place_model },
    { "entity.place",     "Moves an entity: onto the ground, on top of or beside another entity, or to a position",
                                                                                              ARGS(ARGS_ENTITY_PLACE),     true,  cmd_entity_place },
    { "scene.save",       "Saves every entity to a named scene file",                       ARGS(ARGS_SCENE_NAME),       false, cmd_scene_save },
    { "scene.load",       "Loads a saved scene, replacing or adding to the current one",    ARGS(ARGS_SCENE_LOAD),       true,  cmd_scene_load },
    { "scene.list",       "Names of the saved scenes",                                       NO_ARGS,                     false, cmd_scene_list },
    { "batch",            "Runs several commands as one all-or-nothing, single-undo change", ARGS(ARGS_BATCH),            false, cmd_batch },
    { "history.undo",     "Undoes the last change (or the last committed transaction)",     NO_ARGS,                     false, cmd_history_undo },
    { "history.redo",     "Redoes the last undone change",                                   NO_ARGS,                     false, cmd_history_redo },
    { "history.begin",    "Starts grouping the following commands into one undo step",       ARGS(ARGS_HISTORY_BEGIN),    false, cmd_history_begin },
    { "history.commit",   "Ends the group started by history.begin and records it",          NO_ARGS,                     false, cmd_history_commit },
    { "history.rollback", "Abandons the open group, undoing everything done since history.begin", NO_ARGS,               false, cmd_history_rollback },
    { "history.list",     "Recorded undo steps, oldest first",                               NO_ARGS,                     false, cmd_history_list },
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static b8
cmd_commands_list(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)args;

    out->result = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(out->result, "commands");

    uint32 total = (uint32)COMMAND_COUNT + ctx->host_command_count;
    for (uint32 i = 0; i < total; ++i)
    {
        const char*    name;
        const char*    doc;
        const cmd_arg* def_args;
        uint32         arg_count;
        if (i < COMMAND_COUNT)
        {
            name = COMMANDS[i].name; doc = COMMANDS[i].doc; def_args = COMMANDS[i].args; arg_count = COMMANDS[i].arg_count;
        }
        else
        {
            const cmd_host_command* h = &ctx->host_commands[i - COMMAND_COUNT];
            name = h->name; doc = h->doc; def_args = h->args; arg_count = h->arg_count;
        }

        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name);
        cJSON_AddStringToObject(entry, "description", doc);

        cJSON* arg_list = cJSON_AddArrayToObject(entry, "args");
        for (uint32 a = 0; a < arg_count; ++a)
        {
            cJSON* a_json = cJSON_CreateObject();
            cJSON_AddStringToObject(a_json, "name", def_args[a].name);
            cJSON_AddStringToObject(a_json, "type", def_args[a].type);
            cJSON_AddBoolToObject(a_json, "required", def_args[a].required);
            cJSON_AddStringToObject(a_json, "description", def_args[a].doc);
            cJSON_AddItemToArray(arg_list, a_json);
        }
        cJSON_AddItemToArray(list, entry);
    }
    return true;
}

// --- Dispatch -------------------------------------------------------------------------

static cJSON*
error_response(const char* code, const char* message)
{
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", false);
    cJSON* error = cJSON_AddObjectToObject(response, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return response;
}

// Built-in commands first, then the application's. Fills `out` (a view
// for host commands) and returns false if `name` is neither.
static b8
find_command(const command_context* ctx, const char* name, cmd_def* out)
{
    for (uint32 i = 0; i < COMMAND_COUNT; ++i)
    {
        if (strcmp(COMMANDS[i].name, name) == 0)
        {
            *out = COMMANDS[i];
            return true;
        }
    }
    for (uint32 i = 0; i < ctx->host_command_count; ++i)
    {
        const cmd_host_command* h = &ctx->host_commands[i];
        if (strcmp(h->name, name) == 0)
        {
            *out = (cmd_def){ h->name, h->doc, h->args, h->arg_count, h->mutates_world, NULL, h };
            return true;
        }
    }
    return false;
}

// Rejects arguments the command doesn't take -- a typo like "entitiy"
// would otherwise be silently ignored.
static b8
check_unknown_args(const cmd_def* def, const cJSON* args, cmd_output* out)
{
    const cJSON* item;
    cJSON_ArrayForEach(item, args)
    {
        b8 known = false;
        for (uint32 a = 0; a < def->arg_count && !known; ++a)
            known = strcmp(def->args[a].name, item->string) == 0;

        if (!known)
        {
            char names[256] = "";
            for (uint32 a = 0; a < def->arg_count; ++a)
            {
                size_t used = strlen(names);
                snprintf(names + used, sizeof(names) - used, "%s%s", a ? ", " : "", def->args[a].name);
            }
            return fail(out, CMD_ERROR_INVALID_ARGS, "%s doesn't take an argument \"%s\"; its arguments are: %s",
                        def->name, item->string, def->arg_count ? names : "(none)");
        }
    }
    return true;
}

cJSON*
cmd_execute(command_context* ctx, const cJSON* request)
{
    const cJSON* cmd  = cJSON_GetObjectItemCaseSensitive(request, "cmd");
    const cJSON* args = cJSON_GetObjectItemCaseSensitive(request, "args");

    if (!cJSON_IsObject(request) || !cJSON_IsString(cmd))
        return error_response(CMD_ERROR_BAD_REQUEST, "expected {\"cmd\": \"<command>\", \"args\": {...}}");
    if (args && !cJSON_IsObject(args) && !cJSON_IsNull(args))
        return error_response(CMD_ERROR_BAD_REQUEST, "\"args\" must be an object");

    cmd_def found;
    if (!find_command(ctx, cmd->valuestring, &found))
    {
        char   message[2048];
        size_t used  = (size_t)snprintf(message, sizeof(message), "unknown command \"%s\"; commands are: ", cmd->valuestring);
        uint32 total = (uint32)COMMAND_COUNT + ctx->host_command_count;
        for (uint32 i = 0; i < total && used < sizeof(message); ++i)
        {
            const char* name = i < COMMAND_COUNT ? COMMANDS[i].name : ctx->host_commands[i - COMMAND_COUNT].name;
            used += (size_t)snprintf(message + used, sizeof(message) - used, "%s%s", i ? ", " : "", name);
        }
        return error_response(CMD_ERROR_UNKNOWN_COMMAND, message);
    }
    const cmd_def* def = &found;

    // A missing "args" is the same as {}.
    cJSON* empty = NULL;
    if (!args || cJSON_IsNull(args))
        args = empty = cJSON_CreateObject();

    cmd_output out = { 0 };
    if (!check_unknown_args(def, args, &out))
    {
        cJSON_Delete(empty);
        return error_response(out.error_code, out.error);
    }

    // Commands that change the world run between two snapshots: the diff
    // is the undo step, the change event and the response's "changes".
    entity_state* before = NULL;
    uint32        before_count = 0;
    if (def->mutates)
        history_capture(ctx->w, &before, &before_count);

    b8 ok;
    if (def->handler)
    {
        ok = def->handler(ctx, args, &out);
    }
    else
    {
        out.error_code = CMD_ERROR_INVALID_VALUE;
        ok = def->host->fn(ctx->host_user, args, &out.result, out.error, sizeof(out.error));
    }

    history_change* changes = NULL;
    uint32          change_count = 0;
    if (def->mutates)
    {
        entity_state* after;
        uint32        after_count;
        history_capture(ctx->w, &after, &after_count);
        change_count = history_diff(before, before_count, after, after_count, &changes);
        free(before);
        free(after);

        // Commands validate before changing anything, but if one ever fails
        // half-way, put the world back rather than leave it half-changed.
        if (!ok && change_count > 0)
            history_apply(ctx->w, changes, change_count, false);
        world_update(ctx->w);
    }

    if (!ok)
    {
        free(changes);
        cJSON_Delete(empty);
        cJSON_Delete(out.result);
        return error_response(out.error_code ? out.error_code : CMD_ERROR_INVALID_ARGS, out.error);
    }

    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddItemToObject(response, "result", out.result ? out.result : cJSON_CreateObject());

    if (def->mutates)
    {
        char label[HISTORY_LABEL_MAX];
        command_label(def->name, args, label, sizeof(label));

        cJSON_AddItemToObject(response, "changes", announce_changes(ctx, label, changes, change_count, false));

        // Inside a transaction or batch, the step is recorded when it ends.
        b8 record = ctx->history && change_count > 0
                 && !history_in_transaction(ctx->history) && ctx->batch_depth == 0;
        if (record)
            history_push(ctx->history, ctx->w, label, changes, change_count);
        else
            free(changes);
    }

    cJSON_Delete(empty);
    return response;
}

char*
cmd_execute_text(command_context* ctx, const char* request_json)
{
    cJSON* request = cJSON_Parse(request_json);
    cJSON* response;

    if (!request)
    {
        const char* where = cJSON_GetErrorPtr();
        char message[160];
        snprintf(message, sizeof(message), "invalid JSON near: %.40s", where ? where : "(start)");
        response = error_response(CMD_ERROR_BAD_REQUEST, message);
    }
    else
    {
        response = cmd_execute(ctx, request);
        cJSON_Delete(request);
    }

    char* text = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return text;
}

// Placed after cmd_execute(), which it calls for each command.
static b8
cmd_batch(command_context* ctx, const cJSON* args, cmd_output* out)
{
    const cJSON* commands = arg(args, "commands");
    const char*  label;
    if (!cJSON_IsArray(commands) || cJSON_GetArraySize(commands) == 0)
        return fail(out, CMD_ERROR_INVALID_ARGS, "argument \"commands\" must be a non-empty array of "
                    "{\"cmd\": ..., \"args\": {...}} requests");
    if (!arg_string(args, "label", false, &label, out))
        return false;

    // Batches and history control can't nest inside a batch: the batch is
    // itself the transaction.
    int index = 0;
    const cJSON* request;
    cJSON_ArrayForEach(request, commands)
    {
        const cJSON* cmd = cJSON_GetObjectItemCaseSensitive(request, "cmd");
        if (cJSON_IsString(cmd) && (strcmp(cmd->valuestring, "batch") == 0 || strncmp(cmd->valuestring, "history.", 8) == 0))
            return fail(out, CMD_ERROR_INVALID_ARGS, "commands[%d]: \"%s\" can't be used inside a batch", index, cmd->valuestring);
        ++index;
    }

    entity_state* start;
    uint32        start_count;
    history_capture(ctx->w, &start, &start_count);

    cJSON* results = cJSON_CreateArray();
    b8     ok      = true;

    ctx->batch_depth++;
    index = 0;
    cJSON_ArrayForEach(request, commands)
    {
        cJSON* response = cmd_execute(ctx, request);

        if (!cJSON_IsTrue(cJSON_GetObjectItem(response, "ok")))
        {
            const cJSON* error   = cJSON_GetObjectItem(response, "error");
            const cJSON* message = cJSON_GetObjectItem(error, "message");
            fail(out, CMD_ERROR_BATCH_FAILED, "commands[%d] (%s) failed, so the whole batch was undone: %s",
                 index, cJSON_GetStringValue(cJSON_GetObjectItem(request, "cmd")),
                 message ? message->valuestring : "unknown error");
            cJSON_Delete(response);
            ok = false;
            break;
        }

        cJSON_AddItemToArray(results, cJSON_DetachItemFromObject(response, "result"));
        cJSON_Delete(response);
        ++index;
    }
    ctx->batch_depth--;

    entity_state* now;
    uint32        now_count;
    history_capture(ctx->w, &now, &now_count);

    history_change* changes;
    uint32          count = history_diff(start, start_count, now, now_count, &changes);
    free(start);
    free(now);

    if (!ok)
    {
        // Each command already announced its own change; announce the undo too.
        if (count > 0)
        {
            history_apply(ctx->w, changes, count, false);
            cJSON_Delete(announce_changes(ctx, "batch rolled back", changes, count, true));
        }
        free(changes);
        cJSON_Delete(results);
        return false;
    }

    out->result = cJSON_CreateObject();
    cJSON_AddItemToObject(out->result, "results", results);
    cJSON_AddItemToObject(out->result, "changes", changes_to_json(changes, count, false));

    b8 record = ctx->history && count > 0 && !history_in_transaction(ctx->history) && ctx->batch_depth == 0;
    if (record)
        history_push(ctx->history, ctx->w, label ? label : "batch", changes, count);
    else
        free(changes);

    return true;
}
