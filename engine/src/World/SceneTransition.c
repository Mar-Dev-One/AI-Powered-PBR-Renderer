#include "SceneTransition.h"

#include <math.h>
#include <string.h>

static void
world_position(const world* w, entity_id e, vec3 out)
{
    const vec4* m = world_matrix(w, e);
    glm_vec3_copy((f32*)m[3], out);
}

entity_id
scene_trigger_holding_player(const world* w)
{
    const uint32 players  = COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER);
    const uint32 triggers = COMPONENT_BIT(COMPONENT_SCENE_TRIGGER);

    for (entity_id p = world_next(w, ENTITY_NULL, players); p; p = world_next(w, p, players))
    {
        vec3 at;
        world_position(w, p, at);

        for (entity_id t = world_next(w, ENTITY_NULL, triggers); t; t = world_next(w, t, triggers))
        {
            const c_scene_trigger* st = world_scene_trigger(w, t);
            vec3 center;
            world_position(w, t, center);
            if (fabsf(at[0] - center[0]) <= st->extents[0] && fabsf(at[1] - center[1]) <= st->extents[1]
                && fabsf(at[2] - center[2]) <= st->extents[2])
                return t;
        }
    }
    return ENTITY_NULL;
}

void
scene_transition_reset(scene_transition* t, const world* w)
{
    memset(t, 0, sizeof(*t));
    t->inside = scene_trigger_holding_player(w);
}

const char*
scene_transition_update(scene_transition* t, const world* w, f32 dt)
{
    t->time += dt;

    switch (t->phase)
    {
        case SCENE_TRANSITION_IDLE:
        {
            entity_id holder = scene_trigger_holding_player(w);
            entity_id was    = t->inside;
            t->inside        = holder;

            const c_scene_trigger* st = holder && holder != was ? world_scene_trigger(w, holder) : NULL;
            if (!st || !st->scene[0])
                return NULL;

            snprintf(t->scene, sizeof(t->scene), "%s", st->scene);
            snprintf(t->spawn, sizeof(t->spawn), "%s", st->spawn);
            t->seconds = st->fade_seconds;
            t->phase   = SCENE_TRANSITION_FADING_OUT;
            t->time    = 0.0f;
            LOG_INFO("scene trigger '%s': going to '%s'\n", world_name(w, holder), t->scene);
        }
            // fall through: a 0 s fade loads this same frame
        case SCENE_TRANSITION_FADING_OUT:
            if (t->time < t->seconds)
                return NULL;
            t->phase = SCENE_TRANSITION_LOAD;
            return t->scene;

        case SCENE_TRANSITION_LOAD:
            return NULL;   // still waiting for scene_transition_loaded()

        case SCENE_TRANSITION_FADING_IN:
            if (t->time >= t->seconds)
                t->phase = SCENE_TRANSITION_IDLE;
            return NULL;
    }
    return NULL;
}

// Puts the first PlayerController entity where `spawn` is, facing its way.
static void
place_at_spawn(world* w, const char* spawn)
{
    entity_id at     = world_find(w, spawn);
    entity_id player = world_next(w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_PLAYER_CONTROLLER));
    if (!at)
    {
        LOG_WARN("scene trigger: this scene has no entity '%s' to arrive at -- the player starts where it was saved\n",
                 spawn);
        return;
    }
    if (!player || player == at)
        return;

    vec3 position;
    world_position(w, at, position);

    // The player's Transform is local to its parent, if it has one.
    entity_id parent = world_parent(w, player);
    if (parent)
    {
        mat4 inverse;
        glm_mat4_inv((vec4*)world_matrix(w, parent), inverse);
        glm_mat4_mulv3(inverse, position, 1.0f, position);
    }

    c_transform*       t    = world_transform(w, player);
    const c_transform* from = world_transform(w, at);
    glm_vec3_copy(position, t->position);
    t->rotation_deg[0] = fminf(fmaxf(from->rotation_deg[0], -89.0f), 89.0f);
    t->rotation_deg[1] = from->rotation_deg[1];
    t->rotation_deg[2] = 0.0f;
    world_update(w);
}

void
scene_transition_loaded(scene_transition* t, world* w, b8 loaded)
{
    if (t->phase != SCENE_TRANSITION_LOAD)
        return;

    world_update(w);
    if (loaded && t->spawn[0])
        place_at_spawn(w, t->spawn);

    t->inside = scene_trigger_holding_player(w);   // arriving in a doorway doesn't send it straight back
    t->phase  = SCENE_TRANSITION_FADING_IN;
    t->time   = 0.0f;
}

f32
scene_transition_fade(const scene_transition* t)
{
    if (t->seconds <= 0.0f)
        return t->phase == SCENE_TRANSITION_LOAD ? 1.0f : 0.0f;

    f32 x = fminf(t->time / t->seconds, 1.0f);
    switch (t->phase)
    {
        case SCENE_TRANSITION_FADING_OUT: return x;
        case SCENE_TRANSITION_LOAD:       return 1.0f;
        case SCENE_TRANSITION_FADING_IN:  return 1.0f - x;
        default:                          return 0.0f;
    }
}

b8
scene_transition_busy(const scene_transition* t)
{
    return t->phase != SCENE_TRANSITION_IDLE;
}
