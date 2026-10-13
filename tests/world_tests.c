// world_tests.c
//
// Headless tests for World/World.h: entity lifetime, names, hierarchy,
// components, iteration and light gathering. Every entity here uses
// ASSET_NONE (or none at all), so nothing touches the GPU and no window or
// GL context is needed -- this runs anywhere ctest does.

#include "World/Reflect.h"
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

    assets_shutdown(&assets);

    printf("world_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
