#include "Project.h"

#include "../Assets/AssetCache.h"
#include "../Core/FileIO.h"
#include "../Core/Paths.h"
#include "../World/Serialize.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PROJECT_DEFAULT_SCENE "main"

// The folders every project has.
static const char* const PROJECT_FOLDERS[] = { "scenes", "models", "hdri", "textures" };

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

static void
join_into(char* out, size_t size, const char* directory, const char* relative)
{
    snprintf(out, size, "%s/%s", directory, relative);
}

// A path inside the project: relative, no "..", no drive or root.
static b8
relative_path_safe(const char* path)
{
    if (!path[0] || path[0] == '/' || path[0] == '\\' || strchr(path, ':'))
        return false;

    for (const char* c = path; *c;)
    {
        const char* end = c;
        while (*end && *end != '/' && *end != '\\')
            ++end;
        if (end - c == 2 && c[0] == '.' && c[1] == '.')
            return false;
        c = *end ? end + 1 : end;
    }
    return true;
}

// --- Settings -------------------------------------------------------------------

project_settings
project_settings_default(const char* name)
{
    project_settings s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "%s", name && name[0] ? name : "Untitled");
    snprintf(s.startup_scene, sizeof(s.startup_scene), "%s", PROJECT_DEFAULT_SCENE);
    s.window_width  = 1920;
    s.window_height = 1080;
    input_action_map_defaults(&s.input);
    return s;
}

b8
project_name_valid(const char* name)
{
    size_t length = name ? strlen(name) : 0;
    if (length == 0 || length >= PROJECT_NAME_MAX)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    if (name[0] == ' ' || name[0] == '.' || name[length - 1] == ' ' || name[length - 1] == '.')
        return false;

    for (const char* c = name; *c; ++c)
    {
        if ((unsigned char)*c < 32 || strchr("\\/:*?\"<>|", *c))
            return false;
    }
    return true;
}

b8
project_scene_name_valid(const char* name)
{
    size_t length = name ? strlen(name) : 0;
    if (length == 0 || length >= PROJECT_SCENE_MAX)
        return false;

    for (const char* c = name; *c; ++c)
    {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-')
            return false;
    }
    return true;
}

b8
project_exists(const char* directory)
{
    char path[PROJECT_PATH_MAX];
    join_into(path, sizeof(path), directory, PROJECT_FILE);
    return file_exists(path);
}

static b8
validate_settings(const project_settings* s, char* error, size_t error_size)
{
    if (!project_name_valid(s->name))
        return fail(error, error_size, "\"%s\" can't be a project name: use 1-63 characters, none of \\ / : * ? \" < > |",
                    s->name);
    if (!project_scene_name_valid(s->startup_scene))
        return fail(error, error_size, "startup scene \"%s\" must be 1-64 letters, digits, '_' or '-'", s->startup_scene);
    if (s->window_width < 64 || s->window_height < 64)
        return fail(error, error_size, "the window must be at least 64 x 64 pixels");
    if (s->default_hdri[0] && !relative_path_safe(s->default_hdri))
        return fail(error, error_size, "default HDRI \"%s\" must be a path inside the project", s->default_hdri);
    return true;
}

b8
project_read(const char* directory, project_settings* out, char* error, size_t error_size)
{
    char path[PROJECT_PATH_MAX];
    join_into(path, sizeof(path), directory, PROJECT_FILE);
    if (!file_exists(path))
        return fail(error, error_size, "%s has no %s, so it isn't a project", directory, PROJECT_FILE);

    char* text = file_read_to_string(path);
    cJSON* root = text ? cJSON_Parse(text) : NULL;
    free(text);

    const cJSON* format = cJSON_GetObjectItem(root, "format");
    if (!cJSON_IsString(format) || strcmp(format->valuestring, PROJECT_FORMAT) != 0)
    {
        cJSON_Delete(root);
        return fail(error, error_size, "%s isn't a project file (no \"format\": \"%s\")", path, PROJECT_FORMAT);
    }

    project_settings s = project_settings_default(NULL);

    const cJSON* name   = cJSON_GetObjectItem(root, "name");
    const cJSON* scene  = cJSON_GetObjectItem(root, "startup_scene");
    const cJSON* window = cJSON_GetObjectItem(root, "window");
    const cJSON* hdri   = cJSON_GetObjectItem(root, "default_hdri");

    char input_error[256];
    if (!input_action_map_from_json(cJSON_GetObjectItem(root, "input"), &s.input, input_error, sizeof(input_error)))
    {
        cJSON_Delete(root);
        return fail(error, error_size, "%s: input: %s", path, input_error);
    }

    if (cJSON_IsString(name))
        snprintf(s.name, sizeof(s.name), "%s", name->valuestring);
    if (cJSON_IsString(scene))
        snprintf(s.startup_scene, sizeof(s.startup_scene), "%s", scene->valuestring);
    if (cJSON_IsString(hdri))
        snprintf(s.default_hdri, sizeof(s.default_hdri), "%s", hdri->valuestring);

    const cJSON* width  = cJSON_GetObjectItem(window, "width");
    const cJSON* height = cJSON_GetObjectItem(window, "height");
    if (cJSON_IsNumber(width) && width->valuedouble >= 0 && width->valuedouble <= 16384)
        s.window_width = (uint16)width->valuedouble;
    if (cJSON_IsNumber(height) && height->valuedouble >= 0 && height->valuedouble <= 16384)
        s.window_height = (uint16)height->valuedouble;

    cJSON_Delete(root);

    char reason[256];
    if (!validate_settings(&s, reason, sizeof(reason)))
        return fail(error, error_size, "%s: %s", path, reason);

    *out = s;
    return true;
}

b8
project_write(const char* directory, const project_settings* settings, char* error, size_t error_size)
{
    if (!validate_settings(settings, error, error_size))
        return false;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", PROJECT_FORMAT);
    cJSON_AddNumberToObject(root, "version", PROJECT_VERSION);
    cJSON_AddStringToObject(root, "name", settings->name);
    cJSON_AddStringToObject(root, "startup_scene", settings->startup_scene);

    cJSON* window = cJSON_AddObjectToObject(root, "window");
    cJSON_AddNumberToObject(window, "width", settings->window_width);
    cJSON_AddNumberToObject(window, "height", settings->window_height);

    if (settings->default_hdri[0])
        cJSON_AddStringToObject(root, "default_hdri", settings->default_hdri);
    else
        cJSON_AddNullToObject(root, "default_hdri");

    if (settings->input.custom)
        cJSON_AddItemToObject(root, "input", input_action_map_to_json(&settings->input));

    char path[PROJECT_PATH_MAX];
    join_into(path, sizeof(path), directory, PROJECT_FILE);

    char* text = cJSON_Print(root);
    b8    ok   = file_write_string(path, text);
    cJSON_free(text);
    cJSON_Delete(root);

    return ok || fail(error, error_size, "could not write %s", path);
}

// --- Starter scene ----------------------------------------------------------------

static cJSON*
vec3_json(f32 x, f32 y, f32 z)
{
    const f32 v[3] = { x, y, z };
    return cJSON_CreateFloatArray(v, 3);
}

// { "name": name, "parent": null, "components": { "Transform": {...} } };
// returns the components object to add more to.
static cJSON*
add_entity(cJSON* entities, const char* name, cJSON* position, cJSON* rotation, cJSON* scale)
{
    cJSON* entity = cJSON_CreateObject();
    cJSON_AddStringToObject(entity, "name", name);
    cJSON_AddNullToObject(entity, "parent");

    cJSON* components = cJSON_AddObjectToObject(entity, "components");
    cJSON* transform  = cJSON_AddObjectToObject(components, "Transform");
    cJSON_AddItemToObject(transform, "position", position);
    cJSON_AddItemToObject(transform, "rotation_deg", rotation);
    cJSON_AddItemToObject(transform, "scale", scale);

    cJSON_AddItemToArray(entities, entity);
    return components;
}

cJSON*
project_starter_scene(const project_settings* settings)
{
    cJSON* scene    = cJSON_CreateObject();
    cJSON* entities = cJSON_AddArrayToObject(scene, "entities");
    cJSON_AddStringToObject(scene, "format", SCENE_FORMAT);
    cJSON_AddNumberToObject(scene, "version", SCENE_VERSION);

    // A 20 x 20 m dim, rough ground for shadows to land on.
    cJSON* ground = add_entity(entities, "ground", vec3_json(0, 0, 0), vec3_json(0, 0, 0), vec3_json(20, 1, 20));
    cJSON* ground_mr = cJSON_AddObjectToObject(ground, "MeshRenderer");
    cJSON_AddStringToObject(ground_mr, "model", "builtin:plane");
    cJSON_AddBoolToObject(ground_mr, "cast_shadows", false);
    cJSON_AddItemToObject(ground_mr, "material.base_color", vec3_json(0.18f, 0.18f, 0.18f));
    cJSON_AddNumberToObject(ground_mr, "material.metallic", 0);
    cJSON_AddNumberToObject(ground_mr, "material.roughness", 0.9);

    // Warm late-morning sun from the front left.
    cJSON* sun = add_entity(entities, "sun", vec3_json(0, 0, 0), vec3_json(0, 0, 0), vec3_json(1, 1, 1));
    cJSON* light = cJSON_AddObjectToObject(sun, "Light");
    cJSON_AddStringToObject(light, "type", "directional");
    cJSON_AddItemToObject(light, "color", vec3_json(1.0f, 0.97f, 0.9f));
    cJSON_AddNumberToObject(light, "intensity", 2.5);
    cJSON_AddItemToObject(light, "direction", vec3_json(-0.4f, -0.7f, -0.5f));

    // Something to look at, resting on the ground.
    cJSON* cube = add_entity(entities, "cube", vec3_json(0, 0.5f, 0), vec3_json(0, 0, 0), vec3_json(1, 1, 1));
    cJSON* cube_mr = cJSON_AddObjectToObject(cube, "MeshRenderer");
    cJSON_AddStringToObject(cube_mr, "model", "builtin:cube");
    cJSON_AddItemToObject(cube_mr, "material.base_color", vec3_json(0.8f, 0.25f, 0.2f));
    cJSON_AddNumberToObject(cube_mr, "material.roughness", 0.5);

    // 6 m back, 2 m up, pitched down at the cube (atan(1.5 / 6) ~ 14 deg).
    cJSON* camera = add_entity(entities, "camera", vec3_json(0, 2, 6), vec3_json(-14, 0, 0), vec3_json(1, 1, 1));
    cJSON* cam = cJSON_AddObjectToObject(camera, "Camera");
    cJSON_AddNumberToObject(cam, "fov_degrees", 45);
    cJSON_AddNumberToObject(cam, "near_plane", 0.1);
    cJSON_AddNumberToObject(cam, "far_plane", 200);
    cJSON* player = cJSON_AddObjectToObject(camera, "PlayerController");   // walk around in the game
    cJSON_AddStringToObject(player, "mode", "walk");

    scene_environment env = scene_environment_default();
    if (settings && settings->default_hdri[0])
        snprintf(env.hdri, sizeof(env.hdri), "%s", settings->default_hdri);
    cJSON_AddItemToObject(scene, "environment", serialize_environment(&env));

    return scene;
}

static void
count_visit(const char* name, b8 is_directory, void* user_data)
{
    (void)name; (void)is_directory;
    ++*(uint32*)user_data;
}

b8
project_create(const char* directory, const char* name, char* error, size_t error_size)
{
    if (!directory || !directory[0])
        return fail(error, error_size, "no folder given for the project");
    if (!project_name_valid(name))
        return fail(error, error_size, "\"%s\" can't be a project name: use 1-63 characters, none of \\ / : * ? \" < > |",
                    name ? name : "");

    if (file_exists(directory))
        return fail(error, error_size, "%s is a file, not a folder", directory);
    if (file_is_directory(directory))
    {
        uint32 entries = 0;
        file_list_directory(directory, count_visit, &entries);
        if (entries > 0)
            return fail(error, error_size, "%s already exists and isn't empty; pick a new folder", directory);
    }

    if (!file_make_directories(directory))
        return fail(error, error_size, "could not create %s", directory);

    char path[PROJECT_PATH_MAX];
    for (size_t i = 0; i < sizeof(PROJECT_FOLDERS) / sizeof(PROJECT_FOLDERS[0]); ++i)
    {
        join_into(path, sizeof(path), directory, PROJECT_FOLDERS[i]);
        if (!file_make_directory(path))
            return fail(error, error_size, "could not create %s", path);
    }

    project_settings settings = project_settings_default(name);
    if (!project_write(directory, &settings, error, error_size))
        return false;

    cJSON* scene = project_starter_scene(&settings);
    char*  text  = cJSON_Print(scene);
    snprintf(path, sizeof(path), "%s/scenes/%s.json", directory, settings.startup_scene);
    b8 ok = file_write_string(path, text);
    cJSON_free(text);
    cJSON_Delete(scene);

    return ok || fail(error, error_size, "could not write %s", path);
}

// --- Recent projects --------------------------------------------------------------

static char*
recent_file_path(const char* file)
{
    if (file)
    {
        size_t length = strlen(file) + 1;
        char*  copy   = malloc(length);
        if (!copy)
            FATAL("project: out of memory");
        memcpy(copy, file, length);
        return copy;
    }
    return user_data_path("recent_projects.json");
}

static b8
same_path(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        char x = *a == '\\' ? '/' : *a;
        char y = *b == '\\' ? '/' : *b;
#ifdef _WIN32
        x = (char)tolower((unsigned char)x);   // Windows paths aren't case-sensitive
        y = (char)tolower((unsigned char)y);
#endif
        if (x != y)
            return false;
    }
    return *a == *b;
}

// True if `path` is `folder` or somewhere under it, compared as same_path()
// does ('/' and '\' alike; case-insensitive on Windows).
static b8
path_within(const char* path, const char* folder)
{
    size_t length = strlen(folder);
    while (length > 0 && (folder[length - 1] == '/' || folder[length - 1] == '\\'))
        --length;
    if (strlen(path) < length)
        return false;

    for (size_t i = 0; i < length; ++i)
    {
        char a = path[i] == '\\' ? '/' : path[i];
        char b = folder[i] == '\\' ? '/' : folder[i];
#ifdef _WIN32
        a = (char)tolower((unsigned char)a);
        b = (char)tolower((unsigned char)b);
#endif
        if (a != b)
            return false;
    }
    return path[length] == '\0' || path[length] == '/' || path[length] == '\\';
}

void
project_recent_load(project_recent* out, const char* file)
{
    memset(out, 0, sizeof(*out));

    char*  path = recent_file_path(file);
    char*  text = file_exists(path) ? file_read_to_string(path) : NULL;
    cJSON* root = text ? cJSON_Parse(text) : NULL;
    free(text);
    free(path);

    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "projects"))
    {
        if (out->count >= PROJECT_RECENT_MAX)
            break;
        if (cJSON_IsString(item) && strlen(item->valuestring) < PROJECT_PATH_MAX && project_exists(item->valuestring))
            snprintf(out->paths[out->count++], PROJECT_PATH_MAX, "%s", item->valuestring);
    }
    cJSON_Delete(root);
}

void
project_recent_add(project_recent* list, const char* directory, const char* file)
{
    if (!directory || !directory[0] || strlen(directory) >= PROJECT_PATH_MAX)
        return;

    // Drop it where it was, shift everything down one, put it first.
    uint32 kept = 0;
    char   (*paths)[PROJECT_PATH_MAX] = list->paths;
    for (uint32 i = 0; i < list->count; ++i)
    {
        if (!same_path(paths[i], directory))
        {
            if (kept != i)
                memcpy(paths[kept], paths[i], PROJECT_PATH_MAX);
            ++kept;
        }
    }
    if (kept == PROJECT_RECENT_MAX)
        --kept;
    memmove(paths[1], paths[0], (size_t)kept * PROJECT_PATH_MAX);
    snprintf(paths[0], PROJECT_PATH_MAX, "%s", directory);
    list->count = kept + 1;

    cJSON* root     = cJSON_CreateObject();
    cJSON* projects = cJSON_AddArrayToObject(root, "projects");
    for (uint32 i = 0; i < list->count; ++i)
        cJSON_AddItemToArray(projects, cJSON_CreateString(paths[i]));

    char* path = recent_file_path(file);
    char* text = cJSON_Print(root);
    file_write_string(path, text);
    cJSON_free(text);
    free(path);
    cJSON_Delete(root);
}

// --- Importing assets ---------------------------------------------------------------

// The files an import or export copies: source -> destination, both full
// paths, each destination once. Checked in full before anything is copied.
typedef struct import_plan
{
    char   (*sources)[PROJECT_PATH_MAX];
    char   (*destinations)[PROJECT_PATH_MAX];
    uint32 count;
    uint32 capacity;
} import_plan;

static b8
plan_add(import_plan* plan, const char* source, const char* destination, char* error, size_t error_size)
{
    (void)error; (void)error_size;
    for (uint32 i = 0; i < plan->count; ++i)
    {
        if (same_path(plan->destinations[i], destination))
            return true;   // named twice (two images sharing a file, two scenes using a model)
    }

    if (plan->count == plan->capacity)
    {
        plan->capacity     = plan->capacity ? plan->capacity * 2 : 64;
        plan->sources      = realloc(plan->sources, sizeof(*plan->sources) * plan->capacity);
        plan->destinations = realloc(plan->destinations, sizeof(*plan->destinations) * plan->capacity);
        if (!plan->sources || !plan->destinations)
            FATAL("project: out of memory");
    }

    snprintf(plan->sources[plan->count], PROJECT_PATH_MAX, "%s", source);
    snprintf(plan->destinations[plan->count], PROJECT_PATH_MAX, "%s", destination);
    ++plan->count;
    return true;
}

static void
plan_free(import_plan* plan)
{
    free(plan->sources);
    free(plan->destinations);
    *plan = (import_plan){ 0 };
}

// "a%20b.png" -> "a b.png" (glTF URIs are percent-encoded).
static void
percent_decode(const char* in, char* out, size_t size)
{
    size_t n = 0;
    for (const char* c = in; *c && n + 1 < size; ++c)
    {
        if (c[0] == '%' && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2]))
        {
            char hex[3] = { c[1], c[2], 0 };
            out[n++] = (char)strtol(hex, NULL, 16);
            c += 2;
        }
        else
        {
            out[n++] = *c;
        }
    }
    out[n] = '\0';
}

// Adds a file the model refers to by `relative` (to the model's folder),
// copied to the same relative place under `destination_dir`.
static b8
plan_dependency(import_plan* plan, const char* source_dir, const char* destination_dir, const char* relative,
                const char* model_name, char* error, size_t error_size)
{
    if (!relative_path_safe(relative))
        return fail(error, error_size, "%s names \"%s\", outside its own folder; only files beside or under it can be imported",
                    model_name, relative);

    char source[PROJECT_PATH_MAX], destination[PROJECT_PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s", source_dir, relative);
    snprintf(destination, sizeof(destination), "%s/%s", destination_dir, relative);
    if (!file_exists(source))
        return fail(error, error_size, "%s needs \"%s\", which isn't there (%s)", model_name, relative, source);

    return plan_add(plan, source, destination, error, error_size);
}

static b8
plan_gltf(import_plan* plan, const char* source, const char* source_dir, const char* destination_dir,
          const char* file_name, char* error, size_t error_size)
{
    char*  text = file_read_to_string(source);
    cJSON* gltf = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!gltf)
        return fail(error, error_size, "%s isn't a readable glTF (JSON) file", file_name);

    b8 ok = true;
    static const char* const ARRAYS[] = { "buffers", "images" };
    for (int a = 0; a < 2 && ok; ++a)
    {
        const cJSON* item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(gltf, ARRAYS[a]))
        {
            const char* uri = cJSON_GetStringValue(cJSON_GetObjectItem(item, "uri"));
            if (!uri || strncmp(uri, "data:", 5) == 0)
                continue;   // embedded, or an image inside a buffer view

            char decoded[PROJECT_PATH_MAX];
            percent_decode(uri, decoded, sizeof(decoded));
            ok = plan_dependency(plan, source_dir, destination_dir, decoded, file_name, error, error_size);
            if (!ok)
                break;
        }
    }
    cJSON_Delete(gltf);
    return ok;
}

// The rest of a line after `keyword` and whitespace, trimmed; NULL if the
// line doesn't start with it.
static const char*
line_argument(char* line, const char* keyword)
{
    size_t length = strlen(keyword);
    if (strncmp(line, keyword, length) != 0 || (line[length] != ' ' && line[length] != '\t'))
        return NULL;

    char* value = line + length;
    while (*value == ' ' || *value == '\t')
        ++value;
    size_t end = strlen(value);
    while (end > 0 && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r'))
        value[--end] = '\0';
    return value[0] ? value : NULL;
}

// The texture file of an .mtl map line: its last word (options like
// "-bm 0.5" come first).
static const char*
mtl_texture(char* line)
{
    static const char* const MAPS[] = { "map_Ka", "map_Kd", "map_Ks", "map_Ke", "map_Ns", "map_d", "map_Bump",
                                        "map_bump", "bump", "norm", "disp", "map_Pr", "map_Pm" };
    for (size_t i = 0; i < sizeof(MAPS) / sizeof(MAPS[0]); ++i)
    {
        const char* value = line_argument(line, MAPS[i]);
        if (value)
        {
            const char* last = strrchr(value, ' ');
            return last ? last + 1 : value;
        }
    }
    return NULL;
}

static b8
plan_obj(import_plan* plan, const char* source, const char* source_dir, const char* destination_dir,
         const char* file_name, char* error, size_t error_size)
{
    char* text = file_read_to_string(source);
    if (!text)
        return fail(error, error_size, "%s can't be read", file_name);

    b8 ok = true;
    for (char* line = strtok(text, "\n"); line && ok; line = strtok(NULL, "\n"))
    {
        const char* mtl = line_argument(line, "mtllib");
        if (!mtl)
            continue;

        ok = plan_dependency(plan, source_dir, destination_dir, mtl, file_name, error, error_size);
        if (!ok)
            break;

        // The material file's own textures, relative to the model's folder too.
        char mtl_path[PROJECT_PATH_MAX];
        snprintf(mtl_path, sizeof(mtl_path), "%s/%s", source_dir, mtl);
        char* mtl_text = file_read_to_string(mtl_path);
        if (!mtl_text)
            continue;

        // Its own tokenizer state: strtok is busy with the .obj.
        for (char* cursor = mtl_text; cursor && *cursor && ok;)
        {
            char* next = strchr(cursor, '\n');
            if (next)
                *next++ = '\0';
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;

            const char* texture = mtl_texture(cursor);
            if (texture)
                ok = plan_dependency(plan, source_dir, destination_dir, texture, mtl, error, error_size);
            cursor = next;
        }
        free(mtl_text);
    }
    free(text);
    return ok;
}

static b8
has_extension(const char* name, const char* extension)
{
    size_t a = strlen(name), b = strlen(extension);
    if (a <= b)
        return false;
    for (size_t i = 0; i < b; ++i)
    {
        if (tolower((unsigned char)name[a - b + i]) != tolower((unsigned char)extension[i]))
            return false;
    }
    return true;
}

// Makes the folder `path` lives in.
static b8
make_parent(const char* path)
{
    char folder[PROJECT_PATH_MAX];
    snprintf(folder, sizeof(folder), "%s", path);
    char* slash = strrchr(folder, '/');
    if (!slash)
        return true;
    *slash = '\0';
    return file_make_directories(folder);
}

static uint64
file_size(const char* path)
{
    struct stat info;
    return stat(path, &info) == 0 ? (uint64)info.st_size : 0;
}

// Copies every file of the plan, making folders as needed, and adds the
// bytes copied to *out_bytes (may be NULL).
static b8
plan_copy(const import_plan* plan, uint64* out_bytes, char* error, size_t error_size)
{
    for (uint32 i = 0; i < plan->count; ++i)
    {
        if (!make_parent(plan->destinations[i]) || !file_copy(plan->sources[i], plan->destinations[i]))
            return fail(error, error_size, "could not copy %s to %s", plan->sources[i], plan->destinations[i]);
        if (out_bytes)
            *out_bytes += file_size(plan->destinations[i]);
    }
    return true;
}

b8
project_import_asset(const char* project_dir, const char* source, char* out_key, size_t key_size,
                     char* error, size_t error_size)
{
    char source_path[PROJECT_PATH_MAX];
    snprintf(source_path, sizeof(source_path), "%s", source ? source : "");
    for (char* c = source_path; *c; ++c)
        if (*c == '\\')
            *c = '/';

    if (!file_exists(source_path))
        return fail(error, error_size, "no file at %s", source_path[0] ? source_path : "(no path given)");

    // Folder and file name; the stem names a multi-file model's folder.
    char  source_dir[PROJECT_PATH_MAX];
    char* slash     = strrchr(source_path, '/');
    const char* file_name = slash ? slash + 1 : source_path;
    if (slash)
        snprintf(source_dir, sizeof(source_dir), "%.*s", (int)(slash - source_path), source_path);
    else
        snprintf(source_dir, sizeof(source_dir), ".");

    char stem[256];
    const char* dot = strrchr(file_name, '.');
    snprintf(stem, sizeof(stem), "%.*s", (int)(dot ? dot - file_name : (ptrdiff_t)strlen(file_name)), file_name);

    // Where it goes, by type.
    b8   is_gltf = has_extension(file_name, ".gltf");
    b8   is_obj  = has_extension(file_name, ".obj");
    char key[PROJECT_PATH_MAX];
    if (has_extension(file_name, ".hdr"))
        snprintf(key, sizeof(key), "hdri/%s", file_name);
    else if (has_extension(file_name, ".glb"))
        snprintf(key, sizeof(key), "models/%s", file_name);
    else if (is_gltf || is_obj)
        snprintf(key, sizeof(key), "models/%s/%s", stem, file_name);
    else
        return fail(error, error_size, "%s: only .glb, .gltf, .obj and .hdr files can be imported", file_name);

    char destination[PROJECT_PATH_MAX];
    snprintf(destination, sizeof(destination), "%s/%s", project_dir, key);
    if (file_exists(destination))
        return fail(error, error_size, "the project already has %s; rename the file to import it again", key);

    import_plan plan = { 0 };
    char        destination_dir[PROJECT_PATH_MAX];
    snprintf(destination_dir, sizeof(destination_dir), "%s/models/%s", project_dir, stem);

    b8 ok = plan_add(&plan, source_path, destination, error, error_size);
    if (ok && is_gltf)
        ok = plan_gltf(&plan, source_path, source_dir, destination_dir, file_name, error, error_size);
    if (ok && is_obj)
        ok = plan_obj(&plan, source_path, source_dir, destination_dir, file_name, error, error_size);

    // Everything checked; now copy.
    ok = ok && plan_copy(&plan, NULL, error, error_size);
    plan_free(&plan);

    if (ok && out_key)
        snprintf(out_key, key_size, "%s", key);
    return ok;
}

// --- Exporting the game --------------------------------------------------------------

#define EXPORT_MARKER ".pbr-export"

// Plans `relative` (a path a scene uses, e.g. "models/lamp/lamp.gltf") from
// the project into the export, with the files a .gltf / .obj names.
// `scene` names the scene file for the error message.
static b8
plan_project_asset(import_plan* plan, const char* project_dir, const char* out_dir, const char* scene_path,
                   const char* scene, char* error, size_t error_size)
{
    // "models\lamp\lamp.gltf" works on Windows too; the folder lookups below
    // need '/'.
    char relative[PROJECT_PATH_MAX];
    snprintf(relative, sizeof(relative), "%s", scene_path);
    for (char* c = relative; *c; ++c)
        if (*c == '\\')
            *c = '/';

    if (!relative_path_safe(relative))
        return fail(error, error_size, "%s uses \"%s\", which is outside the project; only files in the project "
                    "can be exported (File > Import Asset brings one in)", scene, relative);

    char source[PROJECT_PATH_MAX], destination[PROJECT_PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s", project_dir, relative);
    snprintf(destination, sizeof(destination), "%s/%s", out_dir, relative);
    if (!file_exists(source))
        return fail(error, error_size, "%s uses \"%s\", which isn't in the project (%s)", scene, relative, source);

    if (!plan_add(plan, source, destination, error, error_size))
        return false;

    // Its folder, both sides, and its name: what plan_gltf() / plan_obj() work from.
    char source_dir[PROJECT_PATH_MAX], destination_dir[PROJECT_PATH_MAX];
    snprintf(source_dir, sizeof(source_dir), "%s", source);
    snprintf(destination_dir, sizeof(destination_dir), "%s", destination);
    *strrchr(source_dir, '/')      = '\0';
    *strrchr(destination_dir, '/') = '\0';
    const char* file_name = strrchr(source, '/') + 1;

    if (has_extension(file_name, ".gltf"))
        return plan_gltf(plan, source, source_dir, destination_dir, file_name, error, error_size);
    if (has_extension(file_name, ".obj"))
        return plan_obj(plan, source, source_dir, destination_dir, file_name, error, error_size);
    return true;
}

typedef struct export_scan
{
    import_plan* plan;
    const char*  project_dir;
    const char*  out_dir;
    char*        error;
    size_t       error_size;
    b8           ok;
    uint32       scenes;
} export_scan;

// One scene file: itself, every model its MeshRenderers use, its HDRI.
static void
visit_export_scene(const char* name, b8 is_directory, void* user_data)
{
    export_scan* scan   = user_data;
    size_t       length = strlen(name);
    if (!scan->ok || is_directory || length < 6 || strcmp(name + length - 5, ".json") != 0)
        return;

    char relative[PROJECT_PATH_MAX], source[PROJECT_PATH_MAX], destination[PROJECT_PATH_MAX];
    snprintf(relative, sizeof(relative), "scenes/%s", name);
    snprintf(source, sizeof(source), "%s/%s", scan->project_dir, relative);
    snprintf(destination, sizeof(destination), "%s/%s", scan->out_dir, relative);

    char*  text  = file_read_to_string(source);
    cJSON* scene = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsArray(cJSON_GetObjectItem(scene, "entities")))
    {
        cJSON_Delete(scene);
        scan->ok = fail(scan->error, scan->error_size, "%s isn't a readable scene file", relative);
        return;
    }

    b8 ok = plan_add(scan->plan, source, destination, scan->error, scan->error_size);

    const cJSON* entity;
    cJSON_ArrayForEach(entity, cJSON_GetObjectItem(scene, "entities"))
    {
        const cJSON* mesh  = cJSON_GetObjectItem(cJSON_GetObjectItem(entity, "components"), "MeshRenderer");
        const char*  model = cJSON_GetStringValue(cJSON_GetObjectItem(mesh, "model"));
        if (ok && model && !assets_is_builtin_key(model))
            ok = plan_project_asset(scan->plan, scan->project_dir, scan->out_dir, model, relative, scan->error, scan->error_size);
    }

    const char* hdri = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(scene, "environment"), "hdri"));
    if (ok && hdri && hdri[0])
        ok = plan_project_asset(scan->plan, scan->project_dir, scan->out_dir, hdri, relative, scan->error, scan->error_size);

    cJSON_Delete(scene);
    scan->ok = ok;
    ++scan->scenes;
}

static void
visit_marker_check(const char* name, b8 is_directory, void* user_data)
{
    (void)is_directory;
    uint32* counts = user_data;
    ++counts[0];
    if (strcmp(name, EXPORT_MARKER) == 0)
        ++counts[1];
}

typedef struct shader_scan
{
    import_plan* plan;
    const char*  from;
    const char*  to;
} shader_scan;

static void
visit_shader(const char* name, b8 is_directory, void* user_data)
{
    shader_scan* scan = user_data;
    if (is_directory)
        return;
    char source[PROJECT_PATH_MAX], destination[PROJECT_PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s", scan->from, name);
    snprintf(destination, sizeof(destination), "%s/%s", scan->to, name);
    plan_add(scan->plan, source, destination, NULL, 0);
}

// The last part of a path ("C:/x/vcruntime140.dll" -> "vcruntime140.dll").
static const char*
base_name(const char* path)
{
    const char* slash     = strrchr(path, '/');
    const char* backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash))
        slash = backslash;
    return slash ? slash + 1 : path;
}

b8
project_export(const char* project_dir, const char* out_dir, const project_export_options* options,
               project_export_report* report, char* error, size_t error_size)
{
    memset(report, 0, sizeof(*report));

    project_settings settings;
    if (!project_read(project_dir, &settings, error, error_size))
        return false;
    if (!options->runtime_exe || !file_exists(options->runtime_exe))
        return fail(error, error_size, "no game executable at %s; build the Game target",
                    options->runtime_exe ? options->runtime_exe : "(none given)");

    char shaders_dir[PROJECT_PATH_MAX];
    snprintf(shaders_dir, sizeof(shaders_dir), "%s/shaders", options->engine_dir ? options->engine_dir : "");
    if (!file_is_directory(shaders_dir))
        return fail(error, error_size, "no engine shaders at %s", shaders_dir);

    // Not into the project itself, and not over something that isn't an
    // earlier export (whose files get replaced).
    if (path_within(out_dir, project_dir))
        return fail(error, error_size, "export somewhere outside the project folder");
    if (file_exists(out_dir))
        return fail(error, error_size, "%s is a file, not a folder", out_dir);
    uint32 counts[2] = { 0, 0 };   // entries, markers
    if (file_is_directory(out_dir))
        file_list_directory(out_dir, visit_marker_check, counts);
    if (counts[0] > 0 && counts[1] == 0)
        return fail(error, error_size, "%s already has files in it that aren't an earlier export; pick an empty or new folder",
                    out_dir);

    import_plan plan = { 0 };
    char        source[PROJECT_PATH_MAX], destination[PROJECT_PATH_MAX];

    // The game, named after the project, and the libraries it needs.
    snprintf(report->executable, sizeof(report->executable), "%s/%s.exe", out_dir, settings.name);
    plan_add(&plan, options->runtime_exe, report->executable, NULL, 0);
    for (uint32 i = 0; i < options->extra_count; ++i)
    {
        if (!file_exists(options->extra_files[i]))
        {
            plan_free(&plan);
            return fail(error, error_size, "missing %s, which the game needs next to it", options->extra_files[i]);
        }
        snprintf(destination, sizeof(destination), "%s/%s", out_dir, base_name(options->extra_files[i]));
        plan_add(&plan, options->extra_files[i], destination, NULL, 0);
    }

    // The engine's shaders, where the game looks for them (Core/Paths.h).
    snprintf(destination, sizeof(destination), "%s/engine/shaders", out_dir);
    shader_scan shaders = { &plan, shaders_dir, destination };
    file_list_directory(shaders_dir, visit_shader, &shaders);

    // The project: its settings, every scene, and what the scenes use.
    snprintf(source, sizeof(source), "%s/%s", project_dir, PROJECT_FILE);
    snprintf(destination, sizeof(destination), "%s/%s", out_dir, PROJECT_FILE);
    plan_add(&plan, source, destination, NULL, 0);

    export_scan scan = { &plan, project_dir, out_dir, error, error_size, true, 0 };
    snprintf(source, sizeof(source), "%s/scenes", project_dir);
    file_list_directory(source, visit_export_scene, &scan);
    b8 ok = scan.ok;

    char startup[PROJECT_PATH_MAX];
    snprintf(startup, sizeof(startup), "%s/scenes/%s.json", project_dir, settings.startup_scene);
    if (ok && !file_exists(startup))
        ok = fail(error, error_size, "the startup scene \"%s\" isn't saved (Project Settings picks it)", settings.startup_scene);
    if (ok && settings.default_hdri[0])
        ok = plan_project_asset(&plan, project_dir, out_dir, settings.default_hdri, PROJECT_FILE, error, error_size);

    // Everything found and checked: copy it, then mark the folder as an export.
    if (ok && !file_make_directories(out_dir))
        ok = fail(error, error_size, "could not create %s", out_dir);
    ok = ok && plan_copy(&plan, &report->bytes, error, error_size);
    if (ok)
    {
        report->files  = plan.count;
        report->scenes = scan.scenes;

        cJSON* marker = cJSON_CreateObject();
        cJSON_AddStringToObject(marker, "project", settings.name);
        cJSON_AddStringToObject(marker, "from", project_dir);
        cJSON_AddNumberToObject(marker, "files", plan.count);
        char* text = cJSON_Print(marker);
        snprintf(destination, sizeof(destination), "%s/%s", out_dir, EXPORT_MARKER);
        file_write_string(destination, text);
        cJSON_free(text);
        cJSON_Delete(marker);
    }

    plan_free(&plan);
    return ok;
}
