#include "Commands.h"

#include "../Core/FileIO.h"
#include "../Core/Paths.h"
#include "../World/Reflect.h"
#include "../World/Serialize.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// How many existing names an "entity not found" error lists before
// summarising the rest as "and N more".
#define MAX_NAMES_IN_ERROR 20

// --- Command table types -------------------------------------------------------

typedef struct cmd_arg
{
    const char* name;
    const char* type;       // "string", "entity", "component", "object", "boolean"
    b8          required;
    const char* doc;
} cmd_arg;

typedef struct cmd_output
{
    cJSON*      result;
    const char* error_code;
    char        error[1024];
} cmd_output;

typedef b8 (*cmd_handler)(command_context* ctx, const cJSON* args, cmd_output* out);

typedef struct cmd_def
{
    const char*    name;
    const char*    doc;
    const cmd_arg* args;
    uint32         arg_count;
    b8             mutates;   // world_update() after success
    cmd_handler    handler;
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

// --- Commands -------------------------------------------------------------------

static b8 cmd_commands_list(command_context* ctx, const cJSON* args, cmd_output* out);

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
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static b8
cmd_commands_list(command_context* ctx, const cJSON* args, cmd_output* out)
{
    (void)ctx;
    (void)args;

    out->result = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(out->result, "commands");

    for (uint32 i = 0; i < COMMAND_COUNT; ++i)
    {
        const cmd_def* def = &COMMANDS[i];
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", def->name);
        cJSON_AddStringToObject(entry, "description", def->doc);

        cJSON* arg_list = cJSON_AddArrayToObject(entry, "args");
        for (uint32 a = 0; a < def->arg_count; ++a)
        {
            cJSON* a_json = cJSON_CreateObject();
            cJSON_AddStringToObject(a_json, "name", def->args[a].name);
            cJSON_AddStringToObject(a_json, "type", def->args[a].type);
            cJSON_AddBoolToObject(a_json, "required", def->args[a].required);
            cJSON_AddStringToObject(a_json, "description", def->args[a].doc);
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

static const cmd_def*
find_command(const char* name)
{
    for (uint32 i = 0; i < COMMAND_COUNT; ++i)
    {
        if (strcmp(COMMANDS[i].name, name) == 0)
            return &COMMANDS[i];
    }
    return NULL;
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

    const cmd_def* def = find_command(cmd->valuestring);
    if (!def)
    {
        char message[1024];
        int  used = snprintf(message, sizeof(message), "unknown command \"%s\"; commands are: ", cmd->valuestring);
        for (uint32 i = 0; i < COMMAND_COUNT && used < (int)sizeof(message); ++i)
            used += snprintf(message + used, sizeof(message) - (size_t)used, "%s%s", i ? ", " : "", COMMANDS[i].name);
        return error_response(CMD_ERROR_UNKNOWN_COMMAND, message);
    }

    // A missing "args" is the same as {}.
    cJSON* empty = NULL;
    if (!args || cJSON_IsNull(args))
        args = empty = cJSON_CreateObject();

    cmd_output out = { 0 };
    b8 ok = check_unknown_args(def, args, &out) && def->handler(ctx, args, &out);
    cJSON_Delete(empty);

    if (!ok)
    {
        cJSON_Delete(out.result);
        return error_response(out.error_code ? out.error_code : CMD_ERROR_INVALID_ARGS, out.error);
    }

    if (def->mutates)
        world_update(ctx->w);

    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddItemToObject(response, "result", out.result ? out.result : cJSON_CreateObject());
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
