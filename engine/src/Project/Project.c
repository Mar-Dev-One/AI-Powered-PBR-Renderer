#include "Project.h"

#include "../Core/FileIO.h"
#include "../Core/Paths.h"
#include "../World/Serialize.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define IMPORT_MAX_FILES 256

// The files one import copies: source -> destination, both absolute.
typedef struct import_plan
{
    char   (*sources)[PROJECT_PATH_MAX];
    char   (*destinations)[PROJECT_PATH_MAX];
    uint32 count;
} import_plan;

static b8
plan_add(import_plan* plan, const char* source, const char* destination, char* error, size_t error_size)
{
    for (uint32 i = 0; i < plan->count; ++i)
    {
        if (same_path(plan->destinations[i], destination))
            return true;   // named twice (two images sharing a file)
    }
    if (plan->count >= IMPORT_MAX_FILES)
        return fail(error, error_size, "the model names more than %d files", IMPORT_MAX_FILES);

    snprintf(plan->sources[plan->count], PROJECT_PATH_MAX, "%s", source);
    snprintf(plan->destinations[plan->count], PROJECT_PATH_MAX, "%s", destination);
    ++plan->count;
    return true;
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
    plan.sources      = malloc(sizeof(*plan.sources) * IMPORT_MAX_FILES);
    plan.destinations = malloc(sizeof(*plan.destinations) * IMPORT_MAX_FILES);
    if (!plan.sources || !plan.destinations)
        FATAL("project: out of memory");

    char destination_dir[PROJECT_PATH_MAX];
    snprintf(destination_dir, sizeof(destination_dir), "%s/models/%s", project_dir, stem);

    b8 ok = plan_add(&plan, source_path, destination, error, error_size);
    if (ok && is_gltf)
        ok = plan_gltf(&plan, source_path, source_dir, destination_dir, file_name, error, error_size);
    if (ok && is_obj)
        ok = plan_obj(&plan, source_path, source_dir, destination_dir, file_name, error, error_size);

    // Everything checked; now copy.
    for (uint32 i = 0; ok && i < plan.count; ++i)
    {
        ok = make_parent(plan.destinations[i]) && file_copy(plan.sources[i], plan.destinations[i]);
        if (!ok)
            fail(error, error_size, "could not copy %s to %s", plan.sources[i], plan.destinations[i]);
    }

    free(plan.sources);
    free(plan.destinations);

    if (ok && out_key)
        snprintf(out_key, key_size, "%s", key);
    return ok;
}
