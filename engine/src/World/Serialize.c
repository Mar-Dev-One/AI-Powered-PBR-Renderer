#include "Serialize.h"

#include "Reflect.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Most fields any component has; bounds the parsed-value buffer below.
#define MAX_FIELDS_PER_REQUEST 32

static void
set_error(char* error, size_t error_size, const char* fmt, ...)
{
    if (!error || error_size == 0)
        return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

// f32 -> double at 6 significant digits, so JSON shows 0.3 rather than the
// float's exact binary value 0.30000001192092896.
static double
round6(f32 value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.6g", (double)value);
    return strtod(buffer, NULL);
}

static cJSON*
vec3_to_json(const f32* v)
{
    cJSON* array = cJSON_CreateArray();
    for (int i = 0; i < 3; ++i)
        cJSON_AddItemToArray(array, cJSON_CreateNumber(round6(v[i])));
    return array;
}

// Entity reference as JSON: its name, or null.
static cJSON*
entity_ref_to_json(const world* w, entity_id e)
{
    const char* name = world_name(w, e);
    return name ? cJSON_CreateString(name) : cJSON_CreateNull();
}

// --- Serialize ---------------------------------------------------------------------

static cJSON*
field_to_json(const world* w, const field_desc* field, const void* component_data)
{
    const void* value = (const unsigned char*)component_data + field->offset;

    switch (field->type)
    {
        case FIELD_F32:    return cJSON_CreateNumber(round6(*(const f32*)value));
        case FIELD_BOOL:   return cJSON_CreateBool(*(const b8*)value);
        case FIELD_VEC3:
        case FIELD_COLOR3: return vec3_to_json((const f32*)value);
        case FIELD_STRING: return cJSON_CreateString((const char*)value);
        case FIELD_FLAGS:  return cJSON_CreateNumber(*(const uint32*)value);
        case FIELD_ENTITY: return entity_ref_to_json(w, *(const entity_id*)value);

        case FIELD_ENUM:
        {
            int index = *(const int*)value;
            if (index >= 0 && (uint32)index < field->enum_count)
                return cJSON_CreateString(field->enum_names[index]);
            return cJSON_CreateNumber(index);
        }

        case FIELD_ASSET:
        {
            const char* key = assets_key(w->assets, *(const asset_id*)value);
            return key ? cJSON_CreateString(key) : cJSON_CreateNull();
        }
    }

    return cJSON_CreateNull();
}

cJSON*
serialize_component(const world* w, entity_id e, component_type type)
{
    const component_desc* component = reflect_component(type);
    const void*           data      = reflect_component_data(w, e, type);
    if (!component || !data)
        return NULL;

    cJSON* object = cJSON_CreateObject();

    for (uint32 i = 0; i < component->field_count; ++i)
    {
        const field_desc* field = &component->fields[i];

        // An unset override isn't applied at draw time; leaving it out says
        // "the model's own value" without a misleading number.
        if (field->override_bit && type == COMPONENT_MESH_RENDERER
            && !(((const c_mesh_renderer*)data)->material.fields & field->override_bit))
            continue;

        cJSON_AddItemToObject(object, field->name, field_to_json(w, field, data));
    }

    return object;
}

cJSON*
serialize_entity(const world* w, entity_id e)
{
    if (!world_is_alive(w, e))
        return NULL;

    cJSON* object = cJSON_CreateObject();
    cJSON_AddNumberToObject(object, "id", (double)e);
    cJSON_AddStringToObject(object, "name", world_name(w, e));
    cJSON_AddItemToObject(object, "parent", entity_ref_to_json(w, world_parent(w, e)));

    cJSON* components = cJSON_AddObjectToObject(object, "components");
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        // Name and Parent are the top-level "name" / "parent" keys.
        if (t == COMPONENT_NAME || t == COMPONENT_PARENT)
            continue;

        cJSON* fields = serialize_component(w, e, (component_type)t);
        if (fields)
            cJSON_AddItemToObject(components, reflect_component((component_type)t)->name, fields);
    }

    return object;
}

static void
serialize_subtree(const world* w, entity_id e, cJSON* array)
{
    cJSON_AddItemToArray(array, serialize_entity(w, e));

    const uint32 mask = COMPONENT_BIT(COMPONENT_PARENT);
    for (entity_id child = world_next(w, ENTITY_NULL, mask); child; child = world_next(w, child, mask))
    {
        if (world_parent(w, child) == e)
            serialize_subtree(w, child, array);
    }
}

cJSON*
serialize_world(const world* w)
{
    cJSON* object   = cJSON_CreateObject();
    cJSON* entities = cJSON_AddArrayToObject(object, "entities");

    // Depth-first from each root, so every parent precedes its children --
    // the order a loader needs to recreate the hierarchy.
    for (entity_id e = world_next(w, ENTITY_NULL, 0); e; e = world_next(w, e, 0))
    {
        if (!world_is_alive(w, world_parent(w, e)))
            serialize_subtree(w, e, entities);
    }

    return object;
}

cJSON*
serialize_scene_file(const world* w, const scene_environment* env)
{
    cJSON* scene = serialize_world(w);
    cJSON_AddStringToObject(scene, "format", SCENE_FORMAT);
    cJSON_AddNumberToObject(scene, "version", SCENE_VERSION);
    if (env)
        cJSON_AddItemToObject(scene, "environment", serialize_environment(env));
    return scene;
}

// --- Deserialize ---------------------------------------------------------------------

// One validated field from the request, ready to apply.
typedef struct parsed_field
{
    const field_desc* field;
    b8                is_null;   // override fields: clear; entity/asset refs: none
    union
    {
        f32       f;
        b8        b;
        vec3      v;
        int       enum_value;
        char      str[ENTITY_NAME_MAX];
        asset_id  asset;
        entity_id entity;
    };
} parsed_field;

// "type, color, intensity, ..." -- the valid field names, for error messages.
static void
list_fields(const component_desc* component, char* out, size_t out_size)
{
    out[0] = '\0';
    for (uint32 i = 0; i < component->field_count; ++i)
    {
        size_t used = strlen(out);
        snprintf(out + used, out_size - used, "%s%s", i ? ", " : "", component->fields[i].name);
    }
}

static void
list_enum(const field_desc* field, char* out, size_t out_size)
{
    out[0] = '\0';
    for (uint32 i = 0; i < field->enum_count; ++i)
    {
        size_t used = strlen(out);
        snprintf(out + used, out_size - used, "%s\"%s\"", i ? ", " : "", field->enum_names[i]);
    }
}

static b8
has_range(const field_desc* field)
{
    return field->min < field->max;
}

// Resolves an entity reference (name string or numeric ID) to a live entity.
static b8
parse_entity_ref(const world* w, const cJSON* item, entity_id* out)
{
    if (cJSON_IsString(item))
        *out = world_find(w, item->valuestring);
    else if (cJSON_IsNumber(item) && item->valuedouble >= 0.0)
        *out = (entity_id)item->valuedouble;
    else
        return false;

    return world_is_alive(w, *out);
}

// Validates one JSON value against its field and fills `out`. Everything
// that could make the apply step fail is checked here.
static b8
parse_field(world* w, entity_id e, const component_desc* component, const field_desc* field,
            const cJSON* item, parsed_field* out, char* error, size_t error_size)
{
    out->field   = field;
    out->is_null = cJSON_IsNull(item);

    const char* c = component->name;
    const char* f = field->name;

    if (out->is_null)
    {
        if (field->override_bit || field->type == FIELD_ASSET || field->type == FIELD_ENTITY)
            return true;

        set_error(error, error_size, "%s.%s: null is not allowed here", c, f);
        return false;
    }

    switch (field->type)
    {
        case FIELD_F32:
        {
            if (!cJSON_IsNumber(item))
            {
                set_error(error, error_size, "%s.%s: expected a number", c, f);
                return false;
            }
            out->f = (f32)item->valuedouble;
            if (has_range(field) && (out->f < field->min || out->f > field->max))
            {
                set_error(error, error_size, "%s.%s: %g is out of range [%g, %g]",
                          c, f, item->valuedouble, (double)field->min, (double)field->max);
                return false;
            }
            return true;
        }

        case FIELD_BOOL:
            if (!cJSON_IsBool(item))
            {
                set_error(error, error_size, "%s.%s: expected true or false", c, f);
                return false;
            }
            out->b = cJSON_IsTrue(item);
            return true;

        case FIELD_VEC3:
        case FIELD_COLOR3:
        {
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3)
            {
                set_error(error, error_size, "%s.%s: expected an array of 3 numbers", c, f);
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                const cJSON* n = cJSON_GetArrayItem(item, i);
                if (!cJSON_IsNumber(n))
                {
                    set_error(error, error_size, "%s.%s: expected an array of 3 numbers", c, f);
                    return false;
                }
                out->v[i] = (f32)n->valuedouble;
                if (has_range(field) && (out->v[i] < field->min || out->v[i] > field->max))
                {
                    set_error(error, error_size, "%s.%s[%d]: %g is out of range [%g, %g]",
                              c, f, i, n->valuedouble, (double)field->min, (double)field->max);
                    return false;
                }
            }
            if (field->nonzero && glm_vec3_norm2(out->v) < 1e-12f)
            {
                set_error(error, error_size, "%s.%s: must not be [0, 0, 0]", c, f);
                return false;
            }
            return true;
        }

        case FIELD_ENUM:
        {
            if (cJSON_IsString(item))
            {
                for (uint32 i = 0; i < field->enum_count; ++i)
                {
                    if (strcmp(item->valuestring, field->enum_names[i]) == 0)
                    {
                        out->enum_value = (int)i;
                        return true;
                    }
                }
            }
            char options[256];
            list_enum(field, options, sizeof(options));
            set_error(error, error_size, "%s.%s: expected one of %s", c, f, options);
            return false;
        }

        case FIELD_STRING:
        {
            if (!cJSON_IsString(item) || !item->valuestring[0] || strlen(item->valuestring) >= ENTITY_NAME_MAX)
            {
                set_error(error, error_size, "%s.%s: expected a non-empty string under %d characters",
                          c, f, ENTITY_NAME_MAX);
                return false;
            }
            snprintf(out->str, sizeof(out->str), "%s", item->valuestring);

            // The only string field is the entity name, which must be unique.
            if (component->type == COMPONENT_NAME)
            {
                entity_id holder = world_find(w, out->str);
                if (holder != ENTITY_NULL && holder != e)
                {
                    set_error(error, error_size, "%s.%s: the name \"%s\" is already used by another entity",
                              c, f, out->str);
                    return false;
                }
            }
            return true;
        }

        case FIELD_ASSET:
        {
            if (!cJSON_IsString(item))
            {
                set_error(error, error_size, "%s.%s: expected an asset key such as \"builtin:cube\" or "
                          "\"models/Avocado.glb\", or null", c, f);
                return false;
            }
            out->asset = assets_resolve(w->assets, item->valuestring);
            if (out->asset == ASSET_NONE)
            {
                if (assets_is_builtin_key(item->valuestring))
                    set_error(error, error_size, "%s.%s: unknown built-in \"%s\"; expected \"builtin:cube\", "
                              "\"builtin:sphere\" or \"builtin:plane\"", c, f, item->valuestring);
                else
                    set_error(error, error_size, "%s.%s: could not load the model \"%s\"", c, f, item->valuestring);
                return false;
            }
            return true;
        }

        case FIELD_ENTITY:
        {
            if (!parse_entity_ref(w, item, &out->entity))
            {
                set_error(error, error_size, "%s.%s: no entity matches that name or id", c, f);
                return false;
            }

            // The only entity field is the parent: no self- or cycle-parenting.
            if (component->type == COMPONENT_PARENT)
            {
                for (entity_id a = out->entity; a != ENTITY_NULL; a = world_parent(w, a))
                {
                    if (a == e)
                    {
                        set_error(error, error_size, "%s.%s: \"%s\" can't be parented to itself or one of "
                                  "its own descendants", c, f, world_name(w, e));
                        return false;
                    }
                }
            }
            return true;
        }

        case FIELD_FLAGS:
            set_error(error, error_size, "%s.%s: can't be set directly", c, f);
            return false;
    }

    return false;
}

// Adds `type` to `e` with defaults, if it isn't there yet.
static void
ensure_component(world* w, entity_id e, component_type type)
{
    if (world_has(w, e, type))
        return;

    switch (type)
    {
        case COMPONENT_MESH_RENDERER:
            world_add_mesh_renderer(w, e, ASSET_NONE);
            break;

        case COMPONENT_LIGHT:
        {
            light l;
            light_init_point(&l, (vec3){ 1.0f, 1.0f, 1.0f }, 1.0f, 0.0f);
            glm_vec3_copy((vec3){ 0.0f, -1.0f, 0.0f }, l.direction);   // used if it becomes directional/spot
            l.inner_cone_degrees = 20.0f;
            l.outer_cone_degrees = 30.0f;
            world_add_light(w, e, l);
            break;
        }

        case COMPONENT_CAMERA:
            world_add_camera(w, e, 45.0f, 0.1f, 100.0f);
            break;

        case COMPONENT_SPIN:
            world_add_spin(w, e, (vec3){ 0.0f, 30.0f, 0.0f });   // a slow turn about Y
            break;

        default:
            break;   // Name and Transform always exist; Parent is added by world_set_parent()
    }
}

static void
apply_field(world* w, entity_id e, component_type type, const parsed_field* p)
{
    const field_desc* field = p->field;

    // Fields with an invariant go through World.
    if (field->via_setter)
    {
        switch (field->type)
        {
            case FIELD_STRING: world_rename(w, e, p->str);                                      return;
            case FIELD_ENTITY: world_set_parent(w, e, p->is_null ? ENTITY_NULL : p->entity);    return;
            case FIELD_ASSET:  world_set_model(w, e, p->is_null ? ASSET_NONE : p->asset);       return;
            default:           return;
        }
    }

    unsigned char* data  = reflect_component_data(w, e, type);
    void*          value = data + field->offset;

    if (field->override_bit)
    {
        c_mesh_renderer* mr = (c_mesh_renderer*)data;
        if (p->is_null)
        {
            mr->material.fields &= ~field->override_bit;
            return;
        }
        mr->material.fields |= field->override_bit;
    }

    switch (field->type)
    {
        case FIELD_F32:    *(f32*)value = p->f;                 break;
        case FIELD_BOOL:   *(b8*)value  = p->b;                 break;
        case FIELD_VEC3:
        case FIELD_COLOR3: glm_vec3_copy((f32*)p->v, (f32*)value); break;
        case FIELD_ENUM:   *(int*)value = p->enum_value;        break;
        default:                                                break;
    }
}

b8
deserialize_component(world* w, entity_id e, component_type type, const cJSON* fields,
                      char* error, size_t error_size)
{
    const component_desc* component = reflect_component(type);

    if (!world_is_alive(w, e))
    {
        set_error(error, error_size, "no such entity");
        return false;
    }
    if (!component)
    {
        set_error(error, error_size, "unknown component");
        return false;
    }
    if (!cJSON_IsObject(fields))
    {
        set_error(error, error_size, "%s: expected an object of fields", component->name);
        return false;
    }

    // Pass 1: validate every field without touching the world.
    parsed_field parsed[MAX_FIELDS_PER_REQUEST];
    uint32       count = 0;

    const cJSON* item;
    cJSON_ArrayForEach(item, fields)
    {
        const field_desc* field = reflect_field(component, item->string);
        if (!field)
        {
            char names[512];
            list_fields(component, names, sizeof(names));
            set_error(error, error_size, "%s has no field \"%s\"; its fields are: %s",
                      component->name, item->string, names);
            return false;
        }

        if (count == MAX_FIELDS_PER_REQUEST)
        {
            set_error(error, error_size, "%s: too many fields in one request", component->name);
            return false;
        }

        if (!parse_field(w, e, component, field, item, &parsed[count], error, error_size))
            return false;

        ++count;
    }

    // Pass 2: nothing can fail from here on.
    ensure_component(w, e, type);
    for (uint32 i = 0; i < count; ++i)
        apply_field(w, e, type, &parsed[i]);

    return true;
}

// True for a JSON [0, 0, 0].
static b8
is_zero_vec3(const cJSON* item)
{
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != 3)
        return false;

    const cJSON* v;
    cJSON_ArrayForEach(v, item)
    {
        if (!cJSON_IsNumber(v) || v->valuedouble != 0.0)
            return false;
    }
    return true;
}

b8
deserialize_scene(world* w, const cJSON* scene, entity_id* out_created, uint32* out_count,
                  char* error, size_t error_size)
{
    if (out_count)
        *out_count = 0;

    const cJSON* format   = cJSON_GetObjectItem(scene, "format");
    const cJSON* entities = cJSON_GetObjectItem(scene, "entities");
    if (!cJSON_IsString(format) || strcmp(format->valuestring, SCENE_FORMAT) != 0 || !cJSON_IsArray(entities))
    {
        set_error(error, error_size, "not a scene file");
        return false;
    }

    // Names in the file -> the entities made for them (names may change
    // when merging into a world that already uses them).
    uint32     count   = (uint32)cJSON_GetArraySize(entities);
    entity_id* created = calloc(count ? count : 1, sizeof(*created));
    if (!created)
        FATAL("serialize: out of memory");

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
                set_error(error, error_size, "entity \"%s\": parent \"%s\" isn't defined before it",
                          entity_name ? entity_name : "?", parent_name);
                ok = false;
                break;
            }
            world_set_parent(w, e, parent);
        }

        const cJSON* component;
        cJSON_ArrayForEach(component, cJSON_GetObjectItem(entity, "components"))
        {
            // Name and Parent are the entity's "name" / "parent" keys, never components here.
            const component_desc* c = reflect_component_by_name(component->string);
            if (!c || c->type == COMPONENT_NAME || c->type == COMPONENT_PARENT)
            {
                set_error(error, error_size, "entity \"%s\": unknown component \"%s\"", world_name(w, e),
                          component->string);
                ok = false;
                break;
            }

            // Scenes saved before light_init_point() gave point lights a real
            // direction hold [0, 0, 0] there; read it as the default (straight
            // down), which a point light ignores anyway. Only for scene files:
            // commands still reject a zero direction.
            cJSON* fixed = NULL;
            if (c->type == COMPONENT_LIGHT && is_zero_vec3(cJSON_GetObjectItem(component, "direction")))
            {
                fixed = cJSON_Duplicate(component, true);
                cJSON_ReplaceItemInObject(fixed, "direction", vec3_to_json((vec3){ 0.0f, -1.0f, 0.0f }));
            }

            char component_error[512];
            b8   component_ok = deserialize_component(w, e, c->type, fixed ? fixed : component, component_error,
                                                      sizeof(component_error));
            cJSON_Delete(fixed);
            if (!component_ok)
            {
                set_error(error, error_size, "entity \"%s\": %s", world_name(w, e), component_error);
                ok = false;
                break;
            }
        }
        if (!ok)
            break;

        ++index;
    }

    if (out_created)
        memcpy(out_created, created, index * sizeof(*created));
    if (out_count)
        *out_count = index;

    free(created);
    return ok;
}
