#pragma once

#include "../Core/Defines.h"
#include "../Scene/Model.h"

// Load-once, reference-counted models, keyed by their path under assets/
// ("models/Avocado.glb") or by a built-in name ("builtin:cube"). Two
// entities that use the same model share one GPU copy.
//
// Reference counts are driven by World (World/World.h): giving an entity a
// MeshRenderer retains its asset, removing it (or destroying the entity)
// releases it. An asset whose count drops to zero stays loaded until
// assets_collect_unused() runs, so swapping a model out and straight back
// in doesn't reload it from disk.
//
// asset_id is [generation:12 | index:20], like World's entity_id, so an ID
// kept after its asset was collected is detected instead of silently naming
// whatever reused the slot. ASSET_NONE (0) is never a valid ID.

typedef uint32 asset_id;
#define ASSET_NONE 0

typedef enum builtin_shape
{
    BUILTIN_CUBE,     // 1 x 1 x 1, centred on the origin
    BUILTIN_SPHERE,   // diameter 1, centred on the origin
    BUILTIN_PLANE,    // 1 x 1 on XZ, centred on the origin, facing +Y
    BUILTIN_SHAPE_COUNT
} builtin_shape;

typedef struct asset_entry
{
    char*  key;          // NULL = free slot
    model  model;
    uint32 ref_count;
    uint32 generation;
} asset_entry;

typedef struct asset_cache
{
    asset_entry* entries;
    uint32       capacity;
} asset_cache;

void assets_init(asset_cache* cache);

// Destroys every model, whatever its reference count. Call after every
// World using the cache is gone.
void assets_shutdown(asset_cache* cache);

// The model at `relative_path` under assets/, loaded on first request.
// ASSET_NONE if it fails to load (logged; a failed load isn't cached, so a
// fixed file loads on the next request).
asset_id assets_load_model(asset_cache* cache, const char* relative_path);

// A built-in primitive, created on first request.
asset_id assets_builtin(asset_cache* cache, builtin_shape shape);

// Registers an already-built model under `key` (taking ownership of it), or
// returns the existing ID if `key` is taken (and destroys `m`). For models
// made in code -- and for tests, which register bounds-only models without a
// GPU.
asset_id assets_add_model(asset_cache* cache, const char* key, model m);

// Either of the above by key: "builtin:cube" / "builtin:sphere" /
// "builtin:plane", or a path under assets/. ASSET_NONE for an unknown
// built-in name or a model that fails to load.
asset_id assets_resolve(asset_cache* cache, const char* key);

// True if `key` names a built-in ("builtin:..."), known or not -- lets a
// caller reject a typo'd built-in without trying to load it from disk.
b8 assets_is_builtin_key(const char* key);

// NULL for ASSET_NONE or a stale ID.
const model* assets_model(const asset_cache* cache, asset_id id);

// The path / "builtin:..." name an asset was requested by; NULL if stale.
const char* assets_key(const asset_cache* cache, asset_id id);

b8   assets_is_valid(const asset_cache* cache, asset_id id);
void assets_retain(asset_cache* cache, asset_id id);    // no-op for ASSET_NONE / stale
void assets_release(asset_cache* cache, asset_id id);   // no-op for ASSET_NONE / stale

// Destroys every asset with a reference count of zero. Returns how many.
uint32 assets_collect_unused(asset_cache* cache);
