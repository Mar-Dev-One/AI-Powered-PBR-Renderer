#include "AssetCache.h"

#include "../Core/Paths.h"

#include <string.h>

#define ASSET_INDEX_BITS 20u
#define ASSET_INDEX_MASK ((1u << ASSET_INDEX_BITS) - 1u)
#define ASSET_GEN_MASK   0xFFFu

static const char* BUILTIN_KEYS[BUILTIN_SHAPE_COUNT] = {
    "builtin:cube",
    "builtin:sphere",
    "builtin:plane",
};

static asset_id
make_id(uint32 index, uint32 generation)
{
    return (generation << ASSET_INDEX_BITS) | index;
}

// The entry `id` names, or NULL if it's ASSET_NONE, out of range or stale.
static asset_entry*
lookup(const asset_cache* cache, asset_id id)
{
    if (id == ASSET_NONE)
        return NULL;

    uint32 index = id & ASSET_INDEX_MASK;
    if (index >= cache->capacity)
        return NULL;

    asset_entry* entry = &cache->entries[index];
    if (!entry->key || entry->generation != (id >> ASSET_INDEX_BITS))
        return NULL;

    return entry;
}

static asset_id
find_key(const asset_cache* cache, const char* key)
{
    for (uint32 i = 0; i < cache->capacity; ++i)
    {
        const asset_entry* entry = &cache->entries[i];
        if (entry->key && strcmp(entry->key, key) == 0)
            return make_id(i, entry->generation);
    }
    return ASSET_NONE;
}

// Stores `m` under `key` in a free slot (growing the table if needed).
static asset_id
insert(asset_cache* cache, const char* key, model m)
{
    uint32 index = cache->capacity;
    for (uint32 i = 0; i < cache->capacity; ++i)
    {
        if (!cache->entries[i].key)
        {
            index = i;
            break;
        }
    }

    if (index == cache->capacity)
    {
        uint32       new_capacity = cache->capacity ? cache->capacity * 2 : 16;
        asset_entry* grown        = realloc(cache->entries, new_capacity * sizeof(*grown));
        if (!grown)
            FATAL("assets: out of memory");

        memset(grown + cache->capacity, 0, (new_capacity - cache->capacity) * sizeof(*grown));
        for (uint32 i = cache->capacity; i < new_capacity; ++i)
            grown[i].generation = 1;

        cache->entries  = grown;
        cache->capacity = new_capacity;
    }

    asset_entry* entry = &cache->entries[index];

    size_t key_length = strlen(key) + 1;
    entry->key = malloc(key_length);
    if (!entry->key)
        FATAL("assets: out of memory");
    memcpy(entry->key, key, key_length);

    entry->model     = m;
    entry->ref_count = 0;

    return make_id(index, entry->generation);
}

static void
free_entry(asset_entry* entry)
{
    model_destroy(&entry->model);
    free(entry->key);
    entry->key       = NULL;
    entry->ref_count = 0;

    // Bump the generation so IDs handed out for this slot go stale; skip 0
    // so a recycled slot can never produce ASSET_NONE.
    entry->generation = (entry->generation + 1) & ASSET_GEN_MASK;
    if (entry->generation == 0)
        entry->generation = 1;
}

void
assets_init(asset_cache* cache)
{
    memset(cache, 0, sizeof(*cache));
}

void
assets_shutdown(asset_cache* cache)
{
    for (uint32 i = 0; i < cache->capacity; ++i)
    {
        if (cache->entries[i].key)
            free_entry(&cache->entries[i]);
    }

    free(cache->entries);
    memset(cache, 0, sizeof(*cache));
}

asset_id
assets_load_model(asset_cache* cache, const char* relative_path)
{
    asset_id existing = find_key(cache, relative_path);
    if (existing != ASSET_NONE)
        return existing;

    char* path = asset_path(relative_path);
    model m    = model_load(path);
    free(path);

    if (m.submesh_count == 0)
    {
        LOG_ERROR("assets: failed to load model '%s'\n", relative_path);
        model_destroy(&m);
        return ASSET_NONE;
    }

    return insert(cache, relative_path, m);
}

asset_id
assets_builtin(asset_cache* cache, builtin_shape shape)
{
    if ((uint32)shape >= BUILTIN_SHAPE_COUNT)
        return ASSET_NONE;

    asset_id existing = find_key(cache, BUILTIN_KEYS[shape]);
    if (existing != ASSET_NONE)
        return existing;

    model m;
    switch (shape)
    {
        case BUILTIN_CUBE:
            m = model_from_mesh(mesh_create_cube(), NULL);
            break;

        case BUILTIN_SPHERE:
        {
            // mesh_create_uv_sphere() is unit *radius*; halve it so every
            // primitive is 1 unit across.
            mat4 half;
            glm_scale_make(half, (vec3){ 0.5f, 0.5f, 0.5f });
            m = model_from_mesh(mesh_create_uv_sphere(64, 32), half);
            break;
        }

        case BUILTIN_PLANE:
        default:
            m = model_from_mesh(mesh_create_plane(), NULL);
            break;
    }

    return insert(cache, BUILTIN_KEYS[shape], m);
}

asset_id
assets_add_model(asset_cache* cache, const char* key, model m)
{
    asset_id existing = find_key(cache, key);
    if (existing != ASSET_NONE)
    {
        model_destroy(&m);
        return existing;
    }
    return insert(cache, key, m);
}

b8
assets_is_builtin_key(const char* key)
{
    return key && strncmp(key, "builtin:", 8) == 0;
}

asset_id
assets_resolve(asset_cache* cache, const char* key)
{
    if (!key || !key[0])
        return ASSET_NONE;

    if (assets_is_builtin_key(key))
    {
        for (uint32 s = 0; s < BUILTIN_SHAPE_COUNT; ++s)
        {
            if (strcmp(key, BUILTIN_KEYS[s]) == 0)
                return assets_builtin(cache, (builtin_shape)s);
        }
        return ASSET_NONE;
    }

    return assets_load_model(cache, key);
}

const model*
assets_model(const asset_cache* cache, asset_id id)
{
    asset_entry* entry = lookup(cache, id);
    return entry ? &entry->model : NULL;
}

const char*
assets_key(const asset_cache* cache, asset_id id)
{
    asset_entry* entry = lookup(cache, id);
    return entry ? entry->key : NULL;
}

b8
assets_is_valid(const asset_cache* cache, asset_id id)
{
    return lookup(cache, id) != NULL;
}

void
assets_retain(asset_cache* cache, asset_id id)
{
    asset_entry* entry = lookup(cache, id);
    if (entry)
        entry->ref_count++;
}

void
assets_release(asset_cache* cache, asset_id id)
{
    asset_entry* entry = lookup(cache, id);
    if (entry && entry->ref_count > 0)
        entry->ref_count--;
}

uint32
assets_collect_unused(asset_cache* cache)
{
    uint32 collected = 0;

    for (uint32 i = 0; i < cache->capacity; ++i)
    {
        asset_entry* entry = &cache->entries[i];
        if (entry->key && entry->ref_count == 0)
        {
            free_entry(entry);
            ++collected;
        }
    }

    return collected;
}
