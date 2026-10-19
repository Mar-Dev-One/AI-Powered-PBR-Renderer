// world_tests.c
//
// Headless tests for World/World.h: entity lifetime, names, hierarchy,
// components, iteration and light gathering. Every entity here uses
// ASSET_NONE (or none at all), so nothing touches the GPU and no window or
// GL context is needed -- this runs anywhere ctest does.

#include "Scene/OrbitCamera.h"
#include "World/Reflect.h"
#include "World/PlayerController.h"
#include "World/SceneTransition.h"
#include "World/World.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++g_checks;                                                             \
        if (!(condition))                                                       \
        {                                                                       \
            ++g_failures;                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

static b8
near(f32 a, f32 b)
{
    return fabsf(a - b) < 1e-4f;
}

static void
test_create_destroy_and_stale_ids(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id a = world_create(&w, "a");
    entity_id b = world_create(&w, "b");
    CHECK(a != ENTITY_NULL && b != ENTITY_NULL && a != b);
    CHECK(world_count(&w) == 2);
    CHECK(world_is_alive(&w, a));

    world_destroy(&w, a);
    CHECK(!world_is_alive(&w, a));
    CHECK(world_count(&w) == 1);

    // The slot is reused, but the old ID must not reach the new entity.
    entity_id c = world_create(&w, "c");
    CHECK(c != a);
    CHECK(!world_is_alive(&w, a));
    CHECK(world_transform(&w, a) == NULL);
    CHECK(world_name(&w, a) == NULL);
    CHECK(!world_rename(&w, a, "zombie"));
    CHECK(strcmp(world_name(&w, c), "c") == 0);

    world_destroy(&w, a);   // stale: must be a no-op
    CHECK(world_is_alive(&w, c) && world_count(&w) == 2);

    CHECK(!world_is_alive(&w, ENTITY_NULL));

    world_shutdown(&w);
}

static void
test_names(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id s1 = world_create(&w, "sphere");
    entity_id s2 = world_create(&w, "sphere");
    entity_id s3 = world_create(&w, "sphere");
    entity_id anon = world_create(&w, NULL);

    CHECK(strcmp(world_name(&w, s1), "sphere") == 0);
    CHECK(strcmp(world_name(&w, s2), "sphere_2") == 0);
    CHECK(strcmp(world_name(&w, s3), "sphere_3") == 0);
    CHECK(strcmp(world_name(&w, anon), "entity") == 0);

    CHECK(world_find(&w, "sphere_2") == s2);
    CHECK(world_find(&w, "nope") == ENTITY_NULL);

    CHECK(!world_rename(&w, s3, "sphere"));   // taken
    CHECK(!world_rename(&w, s3, ""));
    CHECK(world_rename(&w, s3, "ball"));
    CHECK(world_find(&w, "ball") == s3);
    CHECK(world_rename(&w, s3, "ball"));      // renaming to its own name is fine

    // A long name still gets a unique suffix within ENTITY_NAME_MAX.
    char long_name[ENTITY_NAME_MAX + 20];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    entity_id l1 = world_create(&w, long_name);
    entity_id l2 = world_create(&w, long_name);
    CHECK(strlen(world_name(&w, l1)) == ENTITY_NAME_MAX - 1);
    CHECK(strcmp(world_name(&w, l1), world_name(&w, l2)) != 0);

    world_shutdown(&w);
}

static void
test_hierarchy(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id root  = world_create(&w, "root");
    entity_id child = world_create(&w, "child");
    entity_id grand = world_create(&w, "grandchild");

    CHECK(world_set_parent(&w, child, root));
    CHECK(world_set_parent(&w, grand, child));

    // Cycles and self-parenting are rejected.
    CHECK(!world_set_parent(&w, root, grand));
    CHECK(!world_set_parent(&w, root, root));
    CHECK(world_parent(&w, root) == ENTITY_NULL);

    world_transform(&w, root)->position[0]      = 10.0f;
    world_transform(&w, root)->rotation_deg[1]  = 90.0f;   // yaw: local +X -> world -Z
    world_transform(&w, child)->position[0]     = 1.0f;
    world_transform(&w, grand)->scale[0]        = 2.0f;

    world_update(&w);

    const vec4* m = world_matrix(&w, child);
    CHECK(near(m[3][0], 10.0f) && near(m[3][1], 0.0f) && near(m[3][2], -1.0f));

    // Grandchild inherits the position and the rotation; its own scale
    // stretches its local X, which the root's yaw turns into world -Z.
    const vec4* g = world_matrix(&w, grand);
    CHECK(near(g[3][0], 10.0f) && near(g[3][2], -1.0f));
    CHECK(near(g[0][2], -2.0f));

    // A child created *before* its parent in storage still resolves.
    entity_id late_parent = world_create(&w, "late_parent");
    CHECK(world_set_parent(&w, root, late_parent));
    world_transform(&w, late_parent)->position[1] = 5.0f;
    world_update(&w);
    CHECK(near(world_matrix(&w, grand)[3][1], 5.0f));

    // Destroying a parent destroys its whole subtree.
    world_destroy(&w, root);
    CHECK(!world_is_alive(&w, root));
    CHECK(!world_is_alive(&w, child));
    CHECK(!world_is_alive(&w, grand));
    CHECK(world_is_alive(&w, late_parent));
    CHECK(world_count(&w) == 1);

    // Moving to the root works.
    entity_id x = world_create(&w, "x");
    CHECK(world_set_parent(&w, x, late_parent));
    CHECK(world_set_parent(&w, x, ENTITY_NULL));
    CHECK(!world_has(&w, x, COMPONENT_PARENT));

    world_shutdown(&w);
}

static void
test_components(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id e = world_create(&w, "lamp");
    CHECK(world_has(&w, e, COMPONENT_NAME));
    CHECK(world_has(&w, e, COMPONENT_TRANSFORM));
    CHECK(!world_has(&w, e, COMPONENT_LIGHT));
    CHECK(world_light(&w, e) == NULL);

    // Default transform is identity with unit scale.
    c_transform* t = world_transform(&w, e);
    CHECK(t->scale[0] == 1.0f && t->scale[1] == 1.0f && t->scale[2] == 1.0f);

    c_mesh_renderer* mr = world_add_mesh_renderer(&w, e, ASSET_NONE);
    CHECK(mr && mr->visible && mr->cast_shadows && mr->material.fields == 0);

    light l;
    light_init_point(&l, (vec3){ 1.0f, 0.5f, 0.2f }, 4.0f, 6.0f);
    c_light* cl = world_add_light(&w, e, l);
    CHECK(cl && cl->type == LIGHT_POINT && cl->intensity == 4.0f);

    // A model and a light on one entity -- what the old Scene couldn't do.
    CHECK(world_has(&w, e, COMPONENT_MESH_RENDERER) && world_has(&w, e, COMPONENT_LIGHT));

    world_remove(&w, e, COMPONENT_LIGHT);
    CHECK(!world_has(&w, e, COMPONENT_LIGHT));

    // Name and Transform are mandatory.
    world_remove(&w, e, COMPONENT_NAME);
    world_remove(&w, e, COMPONENT_TRANSFORM);
    CHECK(world_has(&w, e, COMPONENT_NAME) && world_has(&w, e, COMPONENT_TRANSFORM));

    // No model loaded -> no bounds.
    vec3 bmin, bmax;
    world_update(&w);
    CHECK(!world_bounds(&w, e, bmin, bmax));

    world_shutdown(&w);
}

static void
test_iteration_and_destroy_mid_loop(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    light l;
    light_init_directional(&l, (vec3){ 0.0f, -1.0f, 0.0f }, (vec3){ 1.0f, 1.0f, 1.0f }, 1.0f);

    for (int i = 0; i < 100; ++i)
    {
        entity_id e = world_create(&w, "e");
        if (i % 2 == 0)
            world_add_light(&w, e, l);
    }
    CHECK(world_count(&w) == 100);

    uint32 lights = 0;
    for (entity_id e = world_next(&w, ENTITY_NULL, COMPONENT_BIT(COMPONENT_LIGHT)); e;
         e = world_next(&w, e, COMPONENT_BIT(COMPONENT_LIGHT)))
        ++lights;
    CHECK(lights == 50);

    // Destroy every entity while iterating over them -- the iterator must
    // neither skip live entities nor revisit destroyed ones.
    uint32 visited = 0;
    for (entity_id e = world_next(&w, ENTITY_NULL, 0); e; e = world_next(&w, e, 0))
    {
        CHECK(world_is_alive(&w, e));
        world_destroy(&w, e);
        ++visited;
    }
    CHECK(visited == 100);
    CHECK(world_count(&w) == 0);
    CHECK(world_next(&w, ENTITY_NULL, 0) == ENTITY_NULL);

    world_shutdown(&w);
}

static void
test_gather_lights(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    // A spot light on a rotated, translated parent: direction rotates with
    // it, position comes from the full chain.
    entity_id rig  = world_create(&w, "rig");
    entity_id spot = world_create(&w, "spot");
    world_set_parent(&w, spot, rig);

    world_transform(&w, rig)->position[1]     = 3.0f;
    world_transform(&w, rig)->rotation_deg[1] = 90.0f;
    world_transform(&w, spot)->position[0]    = 2.0f;

    light l;
    light_init_spot(&l, (vec3){ 1.0f, 0.0f, 0.0f }, (vec3){ 1.0f, 1.0f, 1.0f }, 10.0f, 5.0f, 10.0f, 20.0f);
    world_add_light(&w, spot, l);

    world_update(&w);

    light  out[4];
    vec3   positions[4];
    uint32 count = world_gather_lights(&w, out, positions, 4);

    CHECK(count == 1);
    CHECK(near(out[0].direction[0], 0.0f) && near(out[0].direction[2], -1.0f));   // +X yawed 90 -> -Z
    CHECK(near(positions[0][0], 0.0f) && near(positions[0][1], 3.0f) && near(positions[0][2], -2.0f));

    // The cap is respected.
    for (int i = 0; i < 10; ++i)
        world_add_light(&w, world_create(&w, "extra"), l);
    world_update(&w);
    CHECK(world_gather_lights(&w, out, NULL, 4) == 4);

    world_shutdown(&w);
}

static uint32
field_size(field_type type)
{
    switch (type)
    {
        case FIELD_F32:    return sizeof(f32);
        case FIELD_BOOL:   return sizeof(b8);
        case FIELD_VEC3:
        case FIELD_COLOR3: return 3 * sizeof(f32);
        case FIELD_ENUM:   return sizeof(int);
        case FIELD_STRING: return ENTITY_NAME_MAX;
        case FIELD_ASSET:  return sizeof(asset_id);
        case FIELD_ENTITY: return sizeof(entity_id);
        case FIELD_FLAGS:  return sizeof(uint32);
    }
    return 0;
}

static void
test_reflection(asset_cache* assets)
{
    for (uint32 t = 0; t < COMPONENT_TYPE_COUNT; ++t)
    {
        const component_desc* c = reflect_component((component_type)t);
        CHECK(c && c->type == (component_type)t && c->field_count > 0);
        CHECK(reflect_component_by_name(c->name) == c);

        for (uint32 f = 0; f < c->field_count; ++f)
        {
            const field_desc* field = &c->fields[f];

            // Every field lies inside its struct, has a doc, and is findable.
            CHECK(field->offset + field_size(field->type) <= c->size);
            CHECK(field->doc && field->doc[0]);
            CHECK(reflect_field(c, field->name) == field);
            CHECK(field->type != FIELD_ENUM || (field->enum_names && field->enum_count > 0));
        }
    }

    CHECK(reflect_component_by_name("Nope") == NULL);
    CHECK(reflect_component(COMPONENT_TYPE_COUNT) == NULL);

    // Reading through reflection sees what the typed API wrote.
    world w;
    world_init(&w, assets);
    entity_id e = world_create(&w, "probe");
    world_transform(&w, e)->position[1] = 7.5f;

    const component_desc* transform = reflect_component(COMPONENT_TRANSFORM);
    const field_desc*     position  = reflect_field(transform, "position");
    const unsigned char*  data      = reflect_component_data(&w, e, COMPONENT_TRANSFORM);
    CHECK(data && ((const f32*)(data + position->offset))[1] == 7.5f);
    CHECK(reflect_component_data(&w, e, COMPONENT_LIGHT) == NULL);

    world_shutdown(&w);
}

// world_tick(): Spin turns rotation_deg by rate * dt, wraps within +-360,
// and leaves entities without a Spin alone.
static void
test_tick(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id spinner = world_create(&w, "spinner");
    entity_id still   = world_create(&w, "still");
    CHECK(world_add_spin(&w, spinner, (vec3){ 0.0f, 90.0f, -45.0f }) != NULL);
    CHECK(world_has(&w, spinner, COMPONENT_SPIN) && !world_has(&w, still, COMPONENT_SPIN));

    world_tick(&w, 0.5f);
    const f32* r = world_transform(&w, spinner)->rotation_deg;
    CHECK(fabsf(r[0]) < 1e-4f && fabsf(r[1] - 45.0f) < 1e-4f && fabsf(r[2] + 22.5f) < 1e-4f);

    // 45 + 90 * 4 = 405 -> 45; stays inside Transform's +-360 range.
    world_tick(&w, 4.0f);
    CHECK(fabsf(r[1] - 45.0f) < 1e-3f);
    CHECK(fabsf(r[2]) <= 360.0f);

    const f32* s = world_transform(&w, still)->rotation_deg;
    CHECK(s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f);

    // Removing it stops the turning.
    world_remove(&w, spinner, COMPONENT_SPIN);
    world_tick(&w, 1.0f);
    CHECK(fabsf(r[1] - 45.0f) < 1e-3f);

    world_shutdown(&w);
}

// The way an entity looks: its world matrix's -Z (what a Camera looks down).
static void
facing(world* w, entity_id e, vec3 out)
{
    world_update(w);
    const vec4* m = world_matrix(w, e);
    out[0] = -m[2][0];
    out[1] = -m[2][1];
    out[2] = -m[2][2];
    glm_vec3_normalize(out);
}

static b8
vec3_near(const f32* a, f32 x, f32 y, f32 z)
{
    return fabsf(a[0] - x) < 1e-3f && fabsf(a[1] - y) < 1e-3f && fabsf(a[2] - z) < 1e-3f;
}

// player_controller_tick(): looking turns yaw and pitch the way the input
// says, and moving goes where the entity faces (flat when walking).
static void
test_player_controller(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id player = world_create(&w, "player");
    entity_id still  = world_create(&w, "still");
    c_player_controller* pc = world_add_player_controller(&w, player, PLAYER_WALK);
    CHECK(pc && pc->move_speed == 4.0f && pc->sprint_multiplier == 2.0f && pc->look_sensitivity == 1.0f);
    c_transform* t = world_transform(&w, player);
    t->position[1] = 1.7f;

    // Forward at the start is -Z.
    player_input in = { .move_y = 1.0f };
    player_controller_tick(&w, 0.5f, &in);
    CHECK(vec3_near(t->position, 0.0f, 1.7f, -2.0f));
    CHECK(vec3_near(world_transform(&w, still)->position, 0.0f, 0.0f, 0.0f));   // only controllers move

    // Turn left 90 degrees (look_x is + right): now facing -X, and forward goes there.
    in = (player_input){ .look_x = -90.0f };
    player_controller_tick(&w, 0.016f, &in);
    CHECK(fabsf(t->rotation_deg[1] - 90.0f) < 1e-4f);
    vec3 dir;
    facing(&w, player, dir);
    CHECK(vec3_near(dir, -1.0f, 0.0f, 0.0f));
    in = (player_input){ .move_y = 1.0f };
    player_controller_tick(&w, 1.0f, &in);
    CHECK(vec3_near(t->position, -4.0f, 1.7f, -2.0f));

    // Facing -X, right is -Z (forward x up).
    in = (player_input){ .move_x = 1.0f };
    player_controller_tick(&w, 0.25f, &in);
    CHECK(vec3_near(t->position, -4.0f, 1.7f, -3.0f));

    // Look down 30 degrees (look_y is + down). Walking stays at its height,
    // whatever the view's pitch; up_down does nothing.
    in = (player_input){ .look_y = 30.0f };
    player_controller_tick(&w, 0.016f, &in);
    CHECK(fabsf(t->rotation_deg[0] + 30.0f) < 1e-4f);
    facing(&w, player, dir);
    CHECK(dir[1] < -0.49f && dir[1] > -0.51f);   // sin(-30)
    in = (player_input){ .move_y = 1.0f, .up_down = 1.0f };
    player_controller_tick(&w, 1.0f, &in);
    CHECK(fabsf(t->position[1] - 1.7f) < 1e-4f && fabsf(t->position[0] + 8.0f) < 1e-3f);

    // Flying follows the view down, and up_down rises.
    glm_vec3_copy((vec3){ 0.0f, 10.0f, 0.0f }, t->position);
    pc->mode = PLAYER_FLY;
    in = (player_input){ .move_y = 1.0f };
    player_controller_tick(&w, 1.0f, &in);
    facing(&w, player, dir);
    CHECK(vec3_near(t->position, dir[0] * 4.0f, 10.0f + dir[1] * 4.0f, dir[2] * 4.0f));
    glm_vec3_copy((vec3){ 0.0f, 10.0f, 0.0f }, t->position);
    in = (player_input){ .up_down = 1.0f };
    player_controller_tick(&w, 0.5f, &in);
    CHECK(vec3_near(t->position, 0.0f, 12.0f, 0.0f));

    // Sprinting doubles the speed; a diagonal isn't faster than straight on.
    t->rotation_deg[0] = t->rotation_deg[1] = 0.0f;
    glm_vec3_copy((vec3){ 0.0f, 0.0f, 0.0f }, t->position);
    in = (player_input){ .move_y = 1.0f, .sprint = true };
    player_controller_tick(&w, 1.0f, &in);
    CHECK(vec3_near(t->position, 0.0f, 0.0f, -8.0f));
    glm_vec3_copy((vec3){ 0.0f, 0.0f, 0.0f }, t->position);
    in = (player_input){ .move_x = 1.0f, .move_y = 1.0f };
    player_controller_tick(&w, 1.0f, &in);
    CHECK(fabsf(glm_vec3_norm(t->position) - 4.0f) < 1e-3f);

    // The view never flips over the top; sensitivity and invert_look_y scale the turn.
    in = (player_input){ .look_y = -500.0f };
    player_controller_tick(&w, 0.016f, &in);
    CHECK(t->rotation_deg[0] == 89.0f);
    t->rotation_deg[0] = 0.0f;
    pc->look_sensitivity = 0.5f;
    pc->invert_look_y    = true;
    in = (player_input){ .look_x = 20.0f, .look_y = 20.0f };
    player_controller_tick(&w, 0.016f, &in);
    CHECK(fabsf(t->rotation_deg[1] + 10.0f) < 1e-4f && fabsf(t->rotation_deg[0] - 10.0f) < 1e-4f);

    // Yaw stays inside Transform's +-360.
    in = (player_input){ .look_x = -1000.0f };
    player_controller_tick(&w, 0.016f, &in);
    CHECK(fabsf(t->rotation_deg[1]) <= 360.0f);

    world_shutdown(&w);
}

// SceneTransition: walking into a trigger fades out, asks for its scene once,
// puts the player at the spawn after the load and fades back in.
static void
test_scene_transition(asset_cache* assets)
{
    world w;
    world_init(&w, assets);

    entity_id player = world_create(&w, "player");
    world_add_player_controller(&w, player, PLAYER_WALK);
    entity_id door = world_create(&w, "door");
    world_transform(&w, door)->position[2] = -5.0f;
    c_scene_trigger* st = world_add_scene_trigger(&w, door, (vec3){ 1.0f, 2.0f, 1.0f });
    CHECK(st && st->fade_seconds == 0.5f && st->scene[0] == '\0' && st->spawn[0] == '\0');
    entity_id empty_door = world_create(&w, "empty_door");
    world_transform(&w, empty_door)->position[0] = 5.0f;
    world_add_scene_trigger(&w, empty_door, (vec3){ 1.0f, 2.0f, 1.0f });
    snprintf(st->scene, sizeof(st->scene), "level_2");
    snprintf(st->spawn, sizeof(st->spawn), "arrival");

    world_update(&w);
    scene_transition t;
    scene_transition_reset(&t, &w);
    CHECK(!scene_transition_busy(&t) && scene_transition_fade(&t) == 0.0f);
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL && !scene_transition_busy(&t));

    // A trigger without a scene does nothing.
    world_transform(&w, player)->position[0] = 5.0f;
    world_update(&w);
    CHECK(scene_trigger_holding_player(&w) == empty_door);
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL && !scene_transition_busy(&t));

    // Into the door: the fade starts, and the scene is asked for once it's dark.
    glm_vec3_copy((vec3){ 0.5f, 0.0f, -4.5f }, world_transform(&w, player)->position);
    world_update(&w);
    CHECK(scene_trigger_holding_player(&w) == door);
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL && scene_transition_busy(&t));
    CHECK(scene_transition_fade(&t) == 0.0f);
    CHECK(scene_transition_update(&t, &w, 0.25f) == NULL && fabsf(scene_transition_fade(&t) - 0.5f) < 1e-4f);
    const char* next = scene_transition_update(&t, &w, 0.25f);
    CHECK(next && strcmp(next, "level_2") == 0 && scene_transition_fade(&t) == 1.0f);
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL);   // asked once; waits for the load

    // "level_2" loads: its own player, a spawn turned to face +X, and a door
    // back right where the spawn is.
    world_clear(&w);
    player = world_create(&w, "player2");
    world_add_player_controller(&w, player, PLAYER_WALK);
    entity_id arrival = world_create(&w, "arrival");
    glm_vec3_copy((vec3){ 10.0f, 1.7f, 3.0f }, world_transform(&w, arrival)->position);
    world_transform(&w, arrival)->rotation_deg[1] = -90.0f;
    entity_id back = world_create(&w, "back");
    glm_vec3_copy((vec3){ 10.0f, 1.0f, 3.0f }, world_transform(&w, back)->position);
    st = world_add_scene_trigger(&w, back, (vec3){ 1.0f, 2.0f, 1.0f });
    snprintf(st->scene, sizeof(st->scene), "main");

    scene_transition_loaded(&t, &w, true);
    const c_transform* pt = world_transform(&w, player);
    CHECK(fabsf(pt->position[0] - 10.0f) < 1e-4f && fabsf(pt->position[1] - 1.7f) < 1e-4f
          && fabsf(pt->position[2] - 3.0f) < 1e-4f);
    CHECK(pt->rotation_deg[1] == -90.0f);
    CHECK(fabsf(scene_transition_fade(&t) - 1.0f) < 1e-4f && scene_transition_busy(&t));

    // Fading in; arriving inside the door back doesn't send the player back.
    CHECK(scene_transition_update(&t, &w, 0.25f) == NULL && fabsf(scene_transition_fade(&t) - 0.5f) < 1e-4f);
    CHECK(scene_transition_update(&t, &w, 0.25f) == NULL && !scene_transition_busy(&t));
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL && !scene_transition_busy(&t));

    // Out of it and back in: now it does -- and with a 0 s fade, at once.
    st->fade_seconds = 0.0f;
    world_transform(&w, player)->position[0] = 0.0f;
    world_update(&w);
    CHECK(scene_transition_update(&t, &w, 0.1f) == NULL);
    world_transform(&w, player)->position[0] = 10.0f;
    world_update(&w);
    next = scene_transition_update(&t, &w, 0.016f);
    CHECK(next && strcmp(next, "main") == 0 && scene_transition_fade(&t) == 1.0f);

    // A scene that doesn't load: the player stays put and the view comes back.
    scene_transition_loaded(&t, &w, false);
    CHECK(fabsf(world_transform(&w, player)->position[0] - 10.0f) < 1e-4f);
    CHECK(scene_transition_fade(&t) == 0.0f);
    scene_transition_update(&t, &w, 0.016f);
    CHECK(!scene_transition_busy(&t));

    world_shutdown(&w);
}

// Play mode support: world_copy() gives an independent copy with the same
// IDs, holding its own references to the models it uses.
static void
test_copy(asset_cache* assets)
{
    model box = { 0 };
    glm_vec3_fill(box.bounds_min, -0.5f);
    glm_vec3_fill(box.bounds_max,  0.5f);
    asset_id box_id = assets_add_model(assets, "test:copy_box", box);

    world src;
    world_init(&src, assets);
    entity_id pivot = world_create(&src, "pivot");
    entity_id child = world_create(&src, "child");
    entity_id gone  = world_create(&src, "gone");
    world_set_parent(&src, child, pivot);
    world_add_mesh_renderer(&src, child, box_id);
    world_add_spin(&src, pivot, (vec3){ 0.0f, 90.0f, 0.0f });
    world_destroy(&src, gone);
    world_update(&src);

    world dst;
    world_copy(&dst, &src);
    CHECK(world_count(&dst) == 2);
    CHECK(world_is_alive(&dst, pivot) && world_is_alive(&dst, child) && !world_is_alive(&dst, gone));
    CHECK(world_parent(&dst, child) == pivot);
    CHECK(world_find(&dst, "child") == child);
    CHECK(world_mesh_renderer(&dst, child)->model == box_id);

    // Same free list: both hand out the same next ID (the slot "gone" left).
    CHECK(world_create(&src, "next") == world_create(&dst, "next"));

    // Independent: ticking the copy leaves the original as it was.
    world_tick(&dst, 1.0f);
    CHECK(near(world_transform(&dst, pivot)->rotation_deg[1], 90.0f));
    CHECK(near(world_transform(&src, pivot)->rotation_deg[1], 0.0f));
    world_destroy(&dst, child);
    CHECK(world_is_alive(&src, child));

    // The original going away doesn't free a model the copy uses.
    entity_id user = world_create(&dst, "user");
    world_add_mesh_renderer(&dst, user, box_id);
    world_shutdown(&src);
    assets_collect_unused(assets);
    CHECK(assets_is_valid(assets, box_id));

    world_shutdown(&dst);
    assets_collect_unused(assets);
    CHECK(!assets_is_valid(assets, box_id));

    // An empty world copies too.
    world empty, empty_copy;
    world_init(&empty, assets);
    world_copy(&empty_copy, &empty);
    CHECK(world_count(&empty_copy) == 0 && world_create(&empty_copy, "a") != ENTITY_NULL);
    world_shutdown(&empty_copy);
    world_shutdown(&empty);
}

// Gizmo support: transform_from_matrix() undoes transform_to_matrix() --
// compared as matrices, since different Euler triples can give the same one.
static b8
same_matrix(const mat4 a, const mat4 b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (fabsf(a[c][r] - b[c][r]) > 1e-4f)
                return false;
    return true;
}

static void
test_transform_matrix_round_trip(void)
{
    const c_transform cases[] = {
        { { 0, 0, 0 }, { 0, 0, 0 },       { 1, 1, 1 } },
        { { 1, 2, 3 }, { 30, 45, 60 },    { 2, 0.5f, 3 } },
        { { -4, 0, 9 }, { -80, 170, -120 }, { 0.1f, 0.1f, 0.1f } },
        { { 0, 1, 0 }, { 90, 30, 0 },     { 1, 2, 1 } },     // gimbal lock: pitch +90
        { { 0, 1, 0 }, { -90, 0, 45 },    { 1, 1, 1 } },     // gimbal lock: pitch -90
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        mat4 m, again;
        transform_to_matrix(&cases[i], m);

        c_transform back;
        transform_from_matrix(m, &back);
        transform_to_matrix(&back, again);
        CHECK(same_matrix(m, again));

        for (int axis = 0; axis < 3; ++axis)
        {
            CHECK(fabsf(back.position[axis] - cases[i].position[axis]) < 1e-4f);
            CHECK(fabsf(back.scale[axis] - cases[i].scale[axis]) < 1e-4f);
            CHECK(back.rotation_deg[axis] >= -180.0001f && back.rotation_deg[axis] <= 180.0001f);
        }
    }

    // Away from gimbal lock the angles themselves come back.
    c_transform back;
    mat4 m;
    transform_to_matrix(&cases[1], m);
    transform_from_matrix(m, &back);
    CHECK(fabsf(back.rotation_deg[0] - 30.0f) < 1e-3f && fabsf(back.rotation_deg[1] - 45.0f) < 1e-3f
          && fabsf(back.rotation_deg[2] - 60.0f) < 1e-3f);
}

// Selection support: a screen ray from the camera, and world_pick() taking
// the nearest hit -- boxes by their bounds, lights by a marker sphere,
// hidden entities and boxes around the ray's start ignored.
static void
test_pick(asset_cache* assets)
{
    // The centre of the screen looks straight at the target.
    camera cam;
    camera_init(&cam, (vec3){ 0.0f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, 45.0f, 2.0f, 0.1f, 100.0f);
    vec3 origin, direction;
    camera_screen_ray(&cam, 400.0f, 200.0f, 800.0f, 400.0f, origin, direction);
    CHECK(fabsf(direction[0]) < 1e-4f && fabsf(direction[1]) < 1e-4f && fabsf(direction[2] + 1.0f) < 1e-4f);
    CHECK(fabsf(origin[2] - 9.9f) < 1e-3f);   // starts on the near plane

    // world -> screen is the inverse: the origin projects to the centre.
    f32 sx, sy;
    CHECK(camera_world_to_screen(&cam, (vec3){ 0.0f, 0.0f, 0.0f }, 800.0f, 400.0f, &sx, &sy));
    CHECK(fabsf(sx - 400.0f) < 0.01f && fabsf(sy - 200.0f) < 0.01f);
    CHECK(!camera_world_to_screen(&cam, (vec3){ 0.0f, 0.0f, 20.0f }, 800.0f, 400.0f, &sx, &sy));   // behind

    // A 1 m box model, bounds only (no GPU).
    model box = { 0 };
    glm_vec3_fill(box.bounds_min, -0.5f);
    glm_vec3_fill(box.bounds_max,  0.5f);
    asset_id box_id = assets_add_model(assets, "test:pick_box", box);

    world w;
    world_init(&w, assets);
    entity_id front = world_create(&w, "front");
    entity_id back  = world_create(&w, "back");
    entity_id lamp  = world_create(&w, "lamp");
    world_add_mesh_renderer(&w, front, box_id);
    world_add_mesh_renderer(&w, back, box_id);
    world_transform(&w, front)->position[2] = 2.0f;
    world_transform(&w, back)->position[2]  = -2.0f;

    light l;
    light_init_point(&l, (vec3){ 1.0f, 1.0f, 1.0f }, 1.0f, 0.0f);
    world_add_light(&w, lamp, l);
    world_transform(&w, lamp)->position[0] = 3.0f;
    world_update(&w);

    // Down the -Z axis from z = 10: the nearer box wins, at its front face.
    f32 t = 0.0f;
    CHECK(world_pick(&w, (vec3){ 0.0f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, &t) == front);
    CHECK(fabsf(t - 7.5f) < 1e-4f);

    // Hidden: the ray goes through to the one behind.
    world_mesh_renderer(&w, front)->visible = false;
    CHECK(world_pick(&w, (vec3){ 0.0f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, NULL) == back);
    world_mesh_renderer(&w, front)->visible = true;

    // A light has no bounds: its marker sphere is what gets hit.
    CHECK(world_pick(&w, (vec3){ 3.0f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, NULL) == lamp);
    CHECK(world_pick(&w, (vec3){ 3.5f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, NULL) == ENTITY_NULL);

    // Starting inside a box (a room around the camera) doesn't pick it.
    CHECK(world_pick(&w, (vec3){ 0.0f, 0.0f, 2.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, NULL) == back);

    // Missing everything.
    CHECK(world_pick(&w, (vec3){ 0.0f, 5.0f, 10.0f }, (vec3){ 0.0f, 0.0f, -1.0f }, 0.25f, NULL) == ENTITY_NULL);

    world_shutdown(&w);
}

// world_framing_bounds(): visible boxes, leaving out a flat ground unless
// it's all there is; and orbit_fit() backing off until those bounds fit.
static void
test_framing_bounds(asset_cache* assets)
{
    model box = { 0 };
    glm_vec3_fill(box.bounds_min, -0.5f);
    glm_vec3_fill(box.bounds_max,  0.5f);
    asset_id box_id = assets_add_model(assets, "test:frame_box", box);

    model ground = { 0 };
    glm_vec3_copy((vec3){ -10.0f, 0.0f, -10.0f }, ground.bounds_min);
    glm_vec3_copy((vec3){  10.0f, 0.0f,  10.0f }, ground.bounds_max);
    asset_id ground_id = assets_add_model(assets, "test:frame_ground", ground);

    world w;
    world_init(&w, assets);
    vec3 lo, hi;
    world_update(&w);
    CHECK(!world_framing_bounds(&w, lo, hi));   // nothing to frame

    entity_id floor = world_create(&w, "floor");
    world_add_mesh_renderer(&w, floor, ground_id);
    world_update(&w);
    CHECK(world_framing_bounds(&w, lo, hi));    // the ground alone still counts
    CHECK(near(lo[0], -10.0f) && near(hi[0], 10.0f));

    entity_id a = world_create(&w, "a");
    entity_id b = world_create(&w, "b");
    world_add_mesh_renderer(&w, a, box_id);
    world_add_mesh_renderer(&w, b, box_id);
    world_transform(&w, a)->position[0] = -2.0f;
    world_transform(&w, b)->position[0] = 2.0f;
    world_update(&w);
    CHECK(world_framing_bounds(&w, lo, hi));    // the boxes, not the ground
    CHECK(near(lo[0], -2.5f) && near(hi[0], 2.5f) && near(lo[1], -0.5f) && near(hi[1], 0.5f));

    world_mesh_renderer(&w, b)->visible = false;
    world_update(&w);
    CHECK(world_framing_bounds(&w, lo, hi));    // hidden ones don't count
    CHECK(near(hi[0], -1.5f));

    // Fit: aimed at the middle, at radius / sin(fov / 2), keeping the angle.
    camera cam;
    camera_init(&cam, (vec3){ 0.0f, 0.0f, 10.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, 60.0f, 1.0f, 0.1f, 100.0f);
    orbit_camera orbit = { .yaw = 90.0f, .pitch = 0.0f, .distance = 1.0f };
    vec3 box_lo = { 1.0f, 1.0f, 1.0f }, box_hi = { 3.0f, 3.0f, 3.0f };
    orbit_fit(&orbit, &cam, box_lo, box_hi, 1.0f);
    f32 radius = sqrtf(3.0f);
    CHECK(near(cam.target[0], 2.0f) && near(cam.target[1], 2.0f) && near(cam.target[2], 2.0f));
    CHECK(near(orbit.distance, radius / 0.5f));
    CHECK(near(cam.position[0], 2.0f + orbit.distance) && near(cam.position[2], 2.0f));   // yaw 90: from +X

    // And back: the orbit read off the camera is the same orbit.
    orbit_camera read;
    orbit_from_camera(&read, &cam);
    CHECK(near(read.yaw, 90.0f) && near(read.pitch, 0.0f) && near(read.distance, orbit.distance));

    // A long drag down: the pitch stops short of the pole, as zoom stops at its limit.
    orbit_drag(&orbit, &cam, 0.0f, 10000.0f);
    CHECK(near(orbit.pitch, -ORBIT_PITCH_LIMIT));
    orbit_zoom(&orbit, &cam, 1000.0f);
    CHECK(near(orbit.distance, ORBIT_MIN_DISTANCE));

    world_shutdown(&w);
}

int
main(void)
{
    asset_cache assets;
    assets_init(&assets);

    test_create_destroy_and_stale_ids(&assets);
    test_names(&assets);
    test_hierarchy(&assets);
    test_components(&assets);
    test_iteration_and_destroy_mid_loop(&assets);
    test_gather_lights(&assets);
    test_reflection(&assets);
    test_tick(&assets);
    test_player_controller(&assets);
    test_scene_transition(&assets);
    test_pick(&assets);
    test_framing_bounds(&assets);
    test_copy(&assets);
    test_transform_matrix_round_trip();

    assets_shutdown(&assets);

    printf("world_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
