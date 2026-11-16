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

// Every field of World/Environment.h's scene_environment: whatever the scene
// file saves.
static b8
environment_equal(const scene_environment* a, const scene_environment* b)
{
    return strcmp(a->hdri, b->hdri) == 0 && a->use_ibl == b->use_ibl && a->show_skybox == b->show_skybox
        && a->sky_blur == b->sky_blur && a->ambient_strength == b->ambient_strength && a->exposure == b->exposure
        && a->auto_exposure == b->auto_exposure && a->bloom == b->bloom && a->tonemap == b->tonemap
        && a->ssao == b->ssao && a->ssao_radius == b->ssao_radius;
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
doc_scene_file(scene_document* doc, cmd_scene_file_op op, const char* name)
{
    if (op == CMD_SCENE_LOADED)
    {
        snprintf(doc->name_before_load, sizeof(doc->name_before_load), "%s", doc->name);
        snprintf(doc->name_loaded, sizeof(doc->name_loaded), "%s", name);
        doc->load_watched = true;
        doc->load_undone  = false;

        // Outside a transaction its step was just recorded: the newest. In
        // one (a chat turn), doc_follow_history() finds it after the commit.
        const command_history* h = &doc->history;
        doc->load_step = !history_in_transaction(h) && h->cursor ? h->steps[h->cursor - 1].changes : NULL;
    }
    doc_mark_saved(doc, name);
}

// The document's name is now `name`, and the file doesn't match what's on
// screen (an undo or redo changed it).
static void
rename_unsaved(scene_document* doc, const char* name)
{
    snprintf(doc->name, sizeof(doc->name), "%s", name);
    doc->saved_revision = doc->commands.revision - 1;
}

void
doc_follow_history(scene_document* doc)
{
    const command_history* h = &doc->history;
    if (!doc->load_watched || history_in_transaction(h))
        return;

    // The load's step: the newest one, the first time we look after the
    // load (a chat turn's load is recorded when the turn commits).
    if (!doc->load_step)
    {
        if (h->cursor == 0)
        {
            doc->load_watched = false;   // nothing recorded (the turn was rolled back): nothing to follow
            return;
        }
        doc->load_step = h->steps[h->cursor - 1].changes;
    }

    uint32 index = h->step_count;
    for (uint32 i = 0; i < h->step_count; ++i)
        if (h->steps[i].changes == doc->load_step)
            index = i;
    if (index == h->step_count)
    {
        doc->load_watched = false;   // gone: the redo tail was replaced, or it aged out of the history
        return;
    }

    b8 applied = index < h->cursor;
    if (!applied && !doc->load_undone)
    {
        rename_unsaved(doc, doc->name_before_load);
        doc->load_undone = true;
    }
    else if (applied && doc->load_undone)
    {
        rename_unsaved(doc, doc->name_loaded);
        doc->load_undone = false;
    }
}

void
doc_reset_history(scene_document* doc)
{
    history_shutdown(&doc->history, &doc->scene);
    history_init(&doc->history);
    doc->load_watched = false;
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
