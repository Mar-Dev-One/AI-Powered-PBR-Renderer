#include "Environment.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Same 6-significant-digit rounding as Serialize.c, so 0.3f saves as 0.3.
static double
round6(f32 value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.6g", (double)value);
    return strtod(buffer, NULL);
}

static const char* const TONEMAP_NAMES[SCENE_TONEMAP_COUNT] = { "aces", "agx", "neutral" };

const char*
scene_tonemap_name(scene_tonemap tonemap)
{
    return (uint32)tonemap < SCENE_TONEMAP_COUNT ? TONEMAP_NAMES[tonemap] : NULL;
}

b8
scene_tonemap_from_name(const char* name, scene_tonemap* out_tonemap)
{
    for (uint32 i = 0; name && i < SCENE_TONEMAP_COUNT; ++i)
    {
        if (strcmp(name, TONEMAP_NAMES[i]) == 0)
        {
            *out_tonemap = (scene_tonemap)i;
            return true;
        }
    }
    return false;
}

scene_environment
scene_environment_default(void)
{
    scene_environment env = { 0 };
    env.use_ibl          = true;
    env.show_skybox      = true;
    env.sky_blur         = 0.0f;
    env.ambient_strength = 0.5f;
    env.exposure         = 1.0f;
    env.bloom            = 0.04f;
    env.tonemap          = SCENE_TONEMAP_ACES;
    return env;
}

cJSON*
serialize_environment(const scene_environment* env)
{
    cJSON* object = cJSON_CreateObject();
    if (env->hdri[0])
        cJSON_AddStringToObject(object, "hdri", env->hdri);
    else
        cJSON_AddNullToObject(object, "hdri");
    cJSON_AddBoolToObject(object, "use_ibl", env->use_ibl);
    cJSON_AddBoolToObject(object, "show_skybox", env->show_skybox);
    cJSON_AddNumberToObject(object, "sky_blur", round6(env->sky_blur));
    cJSON_AddNumberToObject(object, "ambient_strength", round6(env->ambient_strength));
    cJSON_AddNumberToObject(object, "exposure", round6(env->exposure));
    cJSON_AddNumberToObject(object, "bloom", round6(env->bloom));
    cJSON_AddStringToObject(object, "tonemap", scene_tonemap_name(env->tonemap));
    return object;
}

// A number in [lo, hi] (or (lo, hi] when `lo_open`), into *out.
static b8
read_number(const cJSON* fields, const char* key, f64 lo, b8 lo_open, f64 hi, f32* out, char* error, size_t error_size)
{
    const cJSON* item = cJSON_GetObjectItem(fields, key);
    if (!item)
        return true;

    if (!cJSON_IsNumber(item) || item->valuedouble > hi || (lo_open ? item->valuedouble <= lo : item->valuedouble < lo))
    {
        set_error(error, error_size, "environment: \"%s\" must be a number in %s%g, %g]", key, lo_open ? "(" : "[", lo, hi);
        return false;
    }

    *out = (f32)item->valuedouble;
    return true;
}

static b8
read_bool(const cJSON* fields, const char* key, b8* out, char* error, size_t error_size)
{
    const cJSON* item = cJSON_GetObjectItem(fields, key);
    if (!item)
        return true;

    if (!cJSON_IsBool(item))
    {
        set_error(error, error_size, "environment: \"%s\" must be true or false", key);
        return false;
    }

    *out = cJSON_IsTrue(item);
    return true;
}

b8
deserialize_environment(const cJSON* fields, scene_environment* env, char* error, size_t error_size)
{
    if (!cJSON_IsObject(fields))
    {
        set_error(error, error_size, "environment: expected an object");
        return false;
    }

    scene_environment next = *env;

    const cJSON* hdri = cJSON_GetObjectItem(fields, "hdri");
    if (cJSON_IsNull(hdri))
    {
        next.hdri[0] = '\0';
    }
    else if (hdri)
    {
        const char* path = cJSON_GetStringValue(hdri);
        size_t      length = path ? strlen(path) : 0;
        if (!path || length == 0 || length >= ENVIRONMENT_HDRI_MAX)
        {
            set_error(error, error_size, "environment: \"hdri\" must be a path under assets/ (e.g. \"hdri/sky.hdr\") or null");
            return false;
        }
        if (strstr(path, "..") || path[0] == '/' || path[0] == '\\' || strchr(path, ':'))
        {
            set_error(error, error_size, "environment: \"hdri\" \"%s\" must be a relative path inside assets/", path);
            return false;
        }
        memcpy(next.hdri, path, length + 1);
    }

    if (!read_bool(fields, "use_ibl", &next.use_ibl, error, error_size)
        || !read_bool(fields, "show_skybox", &next.show_skybox, error, error_size)
        || !read_number(fields, "sky_blur", 0.0, false, 1.0, &next.sky_blur, error, error_size)
        || !read_number(fields, "ambient_strength", 0.0, false, 4.0, &next.ambient_strength, error, error_size)
        || !read_number(fields, "exposure", 0.0, true, 16.0, &next.exposure, error, error_size)
        || !read_number(fields, "bloom", 0.0, false, 1.0, &next.bloom, error, error_size))
        return false;

    const cJSON* tonemap = cJSON_GetObjectItem(fields, "tonemap");
    if (tonemap && !scene_tonemap_from_name(cJSON_GetStringValue(tonemap), &next.tonemap))
    {
        set_error(error, error_size, "environment: \"tonemap\" must be \"aces\", \"agx\" or \"neutral\"");
        return false;
    }

    *env = next;
    return true;
}
