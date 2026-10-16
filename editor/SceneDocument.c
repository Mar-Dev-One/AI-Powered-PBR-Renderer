#include "SceneDocument.h"

#include "World/Serialize.h"

#include <stdio.h>
#include <string.h>

void
doc_init(scene_document* doc, asset_cache* assets, command_context commands)
{
    world_init(&doc->scene, assets);
    history_init(&doc->history);

    doc->commands             = commands;
    doc->commands.w           = &doc->scene;
    doc->commands.history     = &doc->history;
    doc->commands.environment = &doc->env;

    doc->env       = scene_environment_default();
    doc->name[0]   = '\0';
    doc_mark_saved(doc, NULL);
}

void
doc_shutdown(scene_document* doc)
{
    history_shutdown(&doc->history, &doc->scene);   // the history holds asset references too
    world_shutdown(&doc->scene);
}

static b8
environment_equal(const scene_environment* a, const scene_environment* b)
{
    return strcmp(a->hdri, b->hdri) == 0 && a->use_ibl == b->use_ibl && a->show_skybox == b->show_skybox
        && a->sky_blur == b->sky_blur && a->ambient_strength == b->ambient_strength && a->exposure == b->exposure;
}

b8
doc_dirty(const scene_document* doc)
{
    return doc->commands.revision != doc->saved_revision || !environment_equal(&doc->env, &doc->saved_env);
}

void
doc_mark_saved(scene_document* doc, const char* name)
{
    if (name)
        snprintf(doc->name, sizeof(doc->name), "%s", name);
    doc->saved_revision = doc->commands.revision;
    doc->saved_env      = doc->env;
}

void
doc_reset_history(scene_document* doc)
{
    history_shutdown(&doc->history, &doc->scene);
    history_init(&doc->history);
}

b8
doc_read(const scene_document* doc, const cJSON* scene_json, const char* default_hdri, world* out_world,
         scene_environment* out_env, char* error, size_t error_size)
{
    *out_env = scene_environment_default();
    snprintf(out_env->hdri, sizeof(out_env->hdri), "%s", default_hdri ? default_hdri : "");

    const cJSON* env_json = cJSON_GetObjectItem(scene_json, "environment");
    if (env_json && !deserialize_environment(env_json, out_env, error, error_size))
        return false;

    world_init(out_world, doc->scene.assets);
    if (!deserialize_scene(out_world, scene_json, NULL, NULL, error, error_size))
    {
        world_shutdown(out_world);
        return false;
    }
    return true;
}

uint64
doc_replace(scene_document* doc, world* next, const scene_environment* env, const char* name)
{
    doc_reset_history(doc);
    world_shutdown(&doc->scene);
    doc->scene = *next;   // commands.w still points here
    world_update(&doc->scene);

    doc->env = *env;
    ++doc->commands.revision;
    doc_mark_saved(doc, name);
    return doc->commands.revision;
}
