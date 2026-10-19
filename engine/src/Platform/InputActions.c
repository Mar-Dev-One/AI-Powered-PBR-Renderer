#include "InputActions.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DT              0.1f
#define AXIS_AS_BUTTON_AT   0.5f   // an axis bound to a button action holds it from here
#define DEFAULT_DEAD_ZONE   0.2f

static b8
fail(char* error, size_t error_size, const char* fmt, ...)
{
    if (error && error_size)
    {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_size, fmt, args);
        va_end(args);
    }
    return false;
}

// --- Bindings -------------------------------------------------------------------

input_binding input_binding_key(input_key key)                      { return (input_binding){ INPUT_BINDING_KEY, key, 0, 1.0f }; }
input_binding input_binding_mouse_button(input_mouse_button button) { return (input_binding){ INPUT_BINDING_MOUSE_BUTTON, button, 0, 1.0f }; }
input_binding input_binding_pad_button(input_pad_button button)     { return (input_binding){ INPUT_BINDING_PAD_BUTTON, button, 0, 1.0f }; }
input_binding input_binding_pad_axis(input_pad_axis axis, f32 scale) { return (input_binding){ INPUT_BINDING_PAD_AXIS, axis, 0, scale }; }
input_binding input_binding_mouse_motion(int axis, f32 scale)       { return (input_binding){ INPUT_BINDING_MOUSE_MOTION, axis, 0, scale }; }

input_binding
input_binding_key_pair(input_key negative, input_key positive, f32 scale)
{
    return (input_binding){ INPUT_BINDING_KEY_PAIR, negative, positive, scale };
}

static const char* const BINDING_PREFIXES[] = {
    [INPUT_BINDING_KEY]          = "key",
    [INPUT_BINDING_MOUSE_BUTTON] = "mouse",
    [INPUT_BINDING_PAD_BUTTON]   = "pad",
    [INPUT_BINDING_KEY_PAIR]     = "keys",
    [INPUT_BINDING_PAD_AXIS]     = "pad_axis",
    [INPUT_BINDING_MOUSE_MOTION] = "mouse_motion",
};

void
input_binding_to_text(const input_binding* b, char* out, size_t size)
{
    const char* prefix = BINDING_PREFIXES[b->kind];
    int         n      = 0;

    switch (b->kind)
    {
        case INPUT_BINDING_KEY:
            n = snprintf(out, size, "%s:%s", prefix, input_key_name((input_key)b->code));
            break;
        case INPUT_BINDING_MOUSE_BUTTON:
            n = snprintf(out, size, "%s:%s", prefix, input_mouse_button_name((input_mouse_button)b->code));
            break;
        case INPUT_BINDING_PAD_BUTTON:
            n = snprintf(out, size, "%s:%s", prefix, input_pad_button_name((input_pad_button)b->code));
            break;
        case INPUT_BINDING_KEY_PAIR:
            n = snprintf(out, size, "%s:%s/%s", prefix, input_key_name((input_key)b->code),
                         input_key_name((input_key)b->code2));
            break;
        case INPUT_BINDING_PAD_AXIS:
            n = snprintf(out, size, "%s:%s", prefix, input_pad_axis_name((input_pad_axis)b->code));
            break;
        case INPUT_BINDING_MOUSE_MOTION:
            n = snprintf(out, size, "%s:%s", prefix, b->code == 0 ? "x" : "y");
            break;
    }

    if (b->scale != 1.0f && n > 0 && (size_t)n < size)
        snprintf(out + n, size - (size_t)n, "*%g", b->scale);
}

b8
input_binding_from_text(const char* text, input_binding* out, char* error, size_t error_size)
{
    char buffer[INPUT_BINDING_TEXT_MAX];
    if (!text || strlen(text) >= sizeof(buffer))
        return fail(error, error_size, "a binding is text like \"key:w\", at most %d characters", INPUT_BINDING_TEXT_MAX - 1);
    snprintf(buffer, sizeof(buffer), "%s", text);
    for (char* c = buffer; *c; ++c)
        *c = (char)tolower((unsigned char)*c);

    input_binding b = { .scale = 1.0f };

    char* star = strchr(buffer, '*');
    if (star)
    {
        *star = '\0';
        char* end;
        b.scale = strtof(star + 1, &end);
        if (end == star + 1 || *end || !isfinite(b.scale) || b.scale == 0.0f)
            return fail(error, error_size, "\"%s\": after '*' comes a number other than 0, like *-1 or *0.1", text);
    }

    char* colon = strchr(buffer, ':');
    if (!colon)
        return fail(error, error_size, "\"%s\" has no ':' -- write it like key:w, keys:a/d, mouse:right, pad:a, "
                                       "pad_axis:left_y or mouse_motion:x", text);
    *colon = '\0';
    const char* prefix = buffer;
    const char* name   = colon + 1;

    int kind = -1;
    for (int i = 0; i < (int)(sizeof(BINDING_PREFIXES) / sizeof(BINDING_PREFIXES[0])); ++i)
        if (strcmp(prefix, BINDING_PREFIXES[i]) == 0)
            kind = i;
    b.kind = (input_binding_kind)kind;

    b8 ok = false;
    switch (kind)
    {
        case INPUT_BINDING_KEY:
            ok = input_key_from_name(name, (input_key*)&b.code);
            break;
        case INPUT_BINDING_MOUSE_BUTTON:
            ok = input_mouse_button_from_name(name, (input_mouse_button*)&b.code);
            break;
        case INPUT_BINDING_PAD_BUTTON:
            ok = input_pad_button_from_name(name, (input_pad_button*)&b.code);
            break;
        case INPUT_BINDING_PAD_AXIS:
            ok = input_pad_axis_from_name(name, (input_pad_axis*)&b.code);
            break;
        case INPUT_BINDING_MOUSE_MOTION:
            ok = (name[0] == 'x' || name[0] == 'y') && !name[1];
            b.code = name[0] == 'y';
            break;
        case INPUT_BINDING_KEY_PAIR:
        {
            char* slash = strchr(colon + 1, '/');
            if (!slash)
                return fail(error, error_size, "\"%s\": keys takes two, the one for -1 first: keys:a/d", text);
            *slash = '\0';
            ok = input_key_from_name(name, (input_key*)&b.code) && input_key_from_name(slash + 1, (input_key*)&b.code2);
            break;
        }
        default:
            return fail(error, error_size, "\"%s\": \"%s\" isn't a kind of binding (key, keys, mouse, pad, pad_axis, "
                                           "mouse_motion)", text, prefix);
    }

    if (!ok)
        return fail(error, error_size, "\"%s\": no %s called that", text,
                    kind == INPUT_BINDING_MOUSE_MOTION ? "mouse axis (x or y)" : prefix);
    *out = b;
    return true;
}

static const char* const KIND_NAMES[INPUT_ACTION_KIND_COUNT] = { "button", "axis", "delta" };

const char*
input_action_kind_name(input_action_kind kind)
{
    return (int)kind >= 0 && kind < INPUT_ACTION_KIND_COUNT ? KIND_NAMES[kind] : "";
}

b8
input_action_kind_from_name(const char* name, input_action_kind* out)
{
    for (int i = 0; name && i < INPUT_ACTION_KIND_COUNT; ++i)
        if (strcmp(name, KIND_NAMES[i]) == 0)
        {
            *out = (input_action_kind)i;
            return true;
        }
    return false;
}

// --- Action maps ------------------------------------------------------------------

b8
input_action_name_valid(const char* name)
{
    size_t length = name ? strlen(name) : 0;
    if (length == 0 || length >= INPUT_ACTION_NAME_MAX)
        return false;
    for (size_t i = 0; i < length; ++i)
        if (!isalnum((unsigned char)name[i]) && name[i] != '_')
            return false;
    return true;
}

int
input_action_map_find(const input_action_map* map, const char* name)
{
    for (int i = 0; name && i < map->count; ++i)
        if (strcmp(map->actions[i].name, name) == 0)
            return i;
    return -1;
}

int
input_action_map_add(input_action_map* map, const char* name, input_action_kind kind)
{
    if (!input_action_name_valid(name))
        return -1;

    int existing = input_action_map_find(map, name);
    if (existing >= 0)
    {
        map->actions[existing].kind = kind;
        return existing;
    }
    if (map->count == INPUT_ACTION_MAX)
        return -1;

    input_action_def* a = &map->actions[map->count];
    memset(a, 0, sizeof(*a));
    strcpy(a->name, name);
    a->kind = kind;
    return map->count++;
}

b8
input_action_map_bind(input_action_map* map, int index, input_binding binding, char* error, size_t error_size)
{
    if (index < 0 || index >= map->count)
        return fail(error, error_size, "no such action");

    input_action_def* a = &map->actions[index];
    if (a->binding_count == INPUT_ACTION_MAX_BINDINGS)
        return fail(error, error_size, "\"%s\" has all %d bindings it can hold", a->name, INPUT_ACTION_MAX_BINDINGS);
    if (binding.kind == INPUT_BINDING_MOUSE_MOTION && a->kind != INPUT_ACTION_AXIS_DELTA)
        return fail(error, error_size, "mouse motion only drives a delta axis (\"%s\" is a %s)", a->name,
                    input_action_kind_name(a->kind));

    a->bindings[a->binding_count++] = binding;
    return true;
}

void
input_action_map_remove(input_action_map* map, int index)
{
    if (index < 0 || index >= map->count)
        return;
    memmove(&map->actions[index], &map->actions[index + 1], (size_t)(map->count - index - 1) * sizeof(map->actions[0]));
    --map->count;
}

void
input_action_map_defaults(input_action_map* map)
{
    memset(map, 0, sizeof(*map));
    map->stick_dead_zone = DEFAULT_DEAD_ZONE;

    const f32 look_degrees_per_pixel  = 0.1f;
    const f32 look_degrees_per_second = 150.0f;

    int a = input_action_map_add(map, "move_x", INPUT_ACTION_AXIS);
    input_action_map_bind(map, a, input_binding_key_pair(INPUT_KEY_A, INPUT_KEY_D, 1.0f), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_axis(INPUT_PAD_LEFT_X, 1.0f), NULL, 0);

    a = input_action_map_add(map, "move_y", INPUT_ACTION_AXIS);
    input_action_map_bind(map, a, input_binding_key_pair(INPUT_KEY_S, INPUT_KEY_W, 1.0f), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_axis(INPUT_PAD_LEFT_Y, -1.0f), NULL, 0);   // the stick is + down

    a = input_action_map_add(map, "look_x", INPUT_ACTION_AXIS_DELTA);
    input_action_map_bind(map, a, input_binding_mouse_motion(0, look_degrees_per_pixel), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_axis(INPUT_PAD_RIGHT_X, look_degrees_per_second), NULL, 0);

    a = input_action_map_add(map, "look_y", INPUT_ACTION_AXIS_DELTA);
    input_action_map_bind(map, a, input_binding_mouse_motion(1, look_degrees_per_pixel), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_axis(INPUT_PAD_RIGHT_Y, look_degrees_per_second), NULL, 0);

    a = input_action_map_add(map, "up_down", INPUT_ACTION_AXIS);
    input_binding down = input_binding_pad_button(INPUT_PAD_LEFT_BUMPER);
    down.scale = -1.0f;
    input_action_map_bind(map, a, input_binding_key_pair(INPUT_KEY_Q, INPUT_KEY_E, 1.0f), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_button(INPUT_PAD_RIGHT_BUMPER), NULL, 0);
    input_action_map_bind(map, a, down, NULL, 0);

    a = input_action_map_add(map, "jump", INPUT_ACTION_BUTTON);
    input_action_map_bind(map, a, input_binding_key(INPUT_KEY_SPACE), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_button(INPUT_PAD_A), NULL, 0);

    a = input_action_map_add(map, "sprint", INPUT_ACTION_BUTTON);
    input_action_map_bind(map, a, input_binding_key(INPUT_KEY_LEFT_SHIFT), NULL, 0);
    input_action_map_bind(map, a, input_binding_pad_button(INPUT_PAD_LEFT_THUMB), NULL, 0);
}

cJSON*
input_action_map_to_json(const input_action_map* map)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "stick_dead_zone", map->stick_dead_zone);

    cJSON* actions = cJSON_AddObjectToObject(root, "actions");
    for (int i = 0; i < map->count; ++i)
    {
        const input_action_def* a = &map->actions[i];
        cJSON* action = cJSON_AddObjectToObject(actions, a->name);
        cJSON_AddStringToObject(action, "type", input_action_kind_name(a->kind));

        cJSON* bindings = cJSON_AddArrayToObject(action, "bindings");
        for (int j = 0; j < a->binding_count; ++j)
        {
            char text[INPUT_BINDING_TEXT_MAX];
            input_binding_to_text(&a->bindings[j], text, sizeof(text));
            cJSON_AddItemToArray(bindings, cJSON_CreateString(text));
        }
    }
    return root;
}

b8
input_action_map_from_json(const cJSON* json, input_action_map* out, char* error, size_t error_size)
{
    if (!json)
    {
        input_action_map_defaults(out);
        return true;
    }
    if (!cJSON_IsObject(json))
        return fail(error, error_size, "\"input\" must be an object");

    // Built in a scratch copy, so a bad value leaves `out` alone.
    input_action_map* map = calloc(1, sizeof(*map));
    map->custom          = true;
    map->stick_dead_zone = DEFAULT_DEAD_ZONE;

    b8 ok = true;
    const cJSON* dead_zone = cJSON_GetObjectItem(json, "stick_dead_zone");
    if (dead_zone)
    {
        if (!cJSON_IsNumber(dead_zone) || dead_zone->valuedouble < 0.0 || dead_zone->valuedouble > 0.9)
            ok = fail(error, error_size, "\"stick_dead_zone\" must be a number from 0 to 0.9");
        else
            map->stick_dead_zone = (f32)dead_zone->valuedouble;
    }

    const cJSON* actions = cJSON_GetObjectItem(json, "actions");
    if (ok && actions && !cJSON_IsObject(actions))
        ok = fail(error, error_size, "\"actions\" must be an object of actions by name");

    const cJSON* action;
    cJSON_ArrayForEach(action, ok ? actions : NULL)
    {
        const char* name = action->string;
        if (!input_action_name_valid(name))
        {
            ok = fail(error, error_size, "action \"%s\": a name is 1-%d letters, digits or '_'", name ? name : "",
                      INPUT_ACTION_NAME_MAX - 1);
            break;
        }
        if (input_action_map_find(map, name) >= 0)
        {
            ok = fail(error, error_size, "action \"%s\" is there twice", name);
            break;
        }

        input_action_kind kind;
        const char*       type = cJSON_GetStringValue(cJSON_GetObjectItem(action, "type"));
        if (!input_action_kind_from_name(type, &kind))
        {
            ok = fail(error, error_size, "action \"%s\": \"type\" must be \"button\", \"axis\" or \"delta\"", name);
            break;
        }

        int index = input_action_map_add(map, name, kind);
        if (index < 0)
        {
            ok = fail(error, error_size, "more than %d actions", INPUT_ACTION_MAX);
            break;
        }

        const cJSON* bindings = cJSON_GetObjectItem(action, "bindings");
        if (bindings && !cJSON_IsArray(bindings))
        {
            ok = fail(error, error_size, "action \"%s\": \"bindings\" must be a list", name);
            break;
        }

        const cJSON* item;
        cJSON_ArrayForEach(item, bindings)
        {
            char          reason[160];
            input_binding binding;
            if (!cJSON_IsString(item))
                ok = fail(error, error_size, "action \"%s\": each binding is text like \"key:w\"", name);
            else if (!input_binding_from_text(item->valuestring, &binding, reason, sizeof(reason))
                     || !input_action_map_bind(map, index, binding, reason, sizeof(reason)))
                ok = fail(error, error_size, "action \"%s\": %s", name, reason);
            if (!ok)
                break;
        }
        if (!ok)
            break;
    }

    if (ok)
        *out = *map;
    free(map);
    return ok;
}

// --- The live actions -----------------------------------------------------------

typedef struct action_slot
{
    input_action_def def;

    // input_actions_update()
    b8  down, pressed, released;
    f32 value;
} action_slot;

static struct
{
    action_slot actions[INPUT_ACTION_MAX];
    int         count;
} live;

static const action_slot*
slot_at(input_action action)
{
    return action >= 0 && action < live.count ? &live.actions[action] : NULL;
}

void
input_actions_load(const input_action_map* map)
{
    memset(&live, 0, sizeof(live));
    live.count = map->count;
    for (int i = 0; i < map->count; ++i)
        live.actions[i].def = map->actions[i];
    input_set_stick_dead_zone(map->stick_dead_zone);
}

void
input_actions_set_defaults(void)
{
    static input_action_map defaults;   // 10 KB: kept off the stack
    input_action_map_defaults(&defaults);
    input_actions_load(&defaults);
}

void
input_actions_clear(void)
{
    memset(&live, 0, sizeof(live));
}

input_action
input_action_find(const char* name)
{
    for (int i = 0; name && i < live.count; ++i)
        if (strcmp(live.actions[i].def.name, name) == 0)
            return i;
    return INPUT_ACTION_NONE;
}

b8 input_action_down(input_action action)     { const action_slot* a = slot_at(action); return a && a->down; }
b8 input_action_pressed(input_action action)  { const action_slot* a = slot_at(action); return a && a->pressed; }
b8 input_action_released(input_action action) { const action_slot* a = slot_at(action); return a && a->released; }
f32 input_action_value(input_action action)   { const action_slot* a = slot_at(action); return a ? a->value : 0.0f; }

int
input_action_count(void)
{
    return live.count;
}

const char*
input_action_name(input_action action)
{
    const action_slot* a = slot_at(action);
    return a ? a->def.name : "";
}

// --- Each frame -----------------------------------------------------------------

// Held now, -1..1 before its scale (mouse motion: none, it has no "held").
static f32
binding_held(const input_binding* b)
{
    switch (b->kind)
    {
        case INPUT_BINDING_KEY:          return input_key_down((input_key)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_MOUSE_BUTTON: return input_mouse_down((input_mouse_button)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_PAD_BUTTON:   return input_pad_down(INPUT_PAD_ANY, (input_pad_button)b->code) ? 1.0f : 0.0f;
        case INPUT_BINDING_KEY_PAIR:
            return (input_key_down((input_key)b->code2) ? 1.0f : 0.0f) - (input_key_down((input_key)b->code) ? 1.0f : 0.0f);
        case INPUT_BINDING_PAD_AXIS:     return input_pad_axis_value(INPUT_PAD_ANY, (input_pad_axis)b->code);
        case INPUT_BINDING_MOUSE_MOTION: return 0.0f;
    }
    return 0.0f;
}

// Went down this frame, even if it's up again already (a tap shorter than a frame).
static b8
binding_pressed(const input_binding* b)
{
    switch (b->kind)
    {
        case INPUT_BINDING_KEY:          return input_key_pressed((input_key)b->code);
        case INPUT_BINDING_MOUSE_BUTTON: return input_mouse_pressed((input_mouse_button)b->code);
        case INPUT_BINDING_PAD_BUTTON:   return input_pad_pressed(INPUT_PAD_ANY, (input_pad_button)b->code);
        case INPUT_BINDING_KEY_PAIR:
            return input_key_pressed((input_key)b->code) || input_key_pressed((input_key)b->code2);
        default:                         return false;
    }
}

static f32
mouse_motion(const input_binding* b)
{
    // A free cursor is pointing at things, not looking around.
    if (input_cursor_mode() != WINDOW_CURSOR_LOCKED)
        return 0.0f;

    f32 dx, dy;
    input_mouse_delta(&dx, &dy);
    return (b->code == 0 ? dx : dy) * b->scale;
}

static void
update_button(action_slot* a)
{
    b8 was_down = a->down;
    b8 down     = false;
    b8 tapped   = false;

    for (int i = 0; i < a->def.binding_count; ++i)
    {
        const input_binding* b = &a->def.bindings[i];
        down   = down || fabsf(binding_held(b) * b->scale) >= AXIS_AS_BUTTON_AT;
        tapped = tapped || binding_pressed(b);
    }

    a->down     = down;
    a->pressed  = !was_down && (down || tapped);
    a->released = (was_down && !down) || (a->pressed && !down);
    a->value    = down ? 1.0f : 0.0f;
}

static void
update_axis(action_slot* a, f32 dt)
{
    f32 furthest = 0.0f;
    f32 mouse    = 0.0f;

    for (int i = 0; i < a->def.binding_count; ++i)
    {
        const input_binding* b = &a->def.bindings[i];
        if (b->kind == INPUT_BINDING_MOUSE_MOTION)
        {
            mouse += mouse_motion(b);
            continue;
        }

        f32 v = binding_held(b) * b->scale;
        if (fabsf(v) > fabsf(furthest))
            furthest = v;
    }

    if (a->def.kind == INPUT_ACTION_AXIS_DELTA)
        a->value = furthest * dt + mouse;
    else
        a->value = fminf(fmaxf(furthest, -1.0f), 1.0f);

    a->down    = a->value != 0.0f;
    a->pressed = a->released = false;
}

void
input_actions_update(f32 dt)
{
    dt = fminf(fmaxf(dt, 0.0f), MAX_DT);

    for (int i = 0; i < live.count; ++i)
    {
        action_slot* a = &live.actions[i];
        if (a->def.kind == INPUT_ACTION_BUTTON)
            update_button(a);
        else
            update_axis(a, dt);
    }
}
