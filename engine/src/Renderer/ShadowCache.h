#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../World/World.h"

#include <cglm/cglm.h>

// Shadow maps for spot and point lights (SpotShadow.h, PointShadow.h) are
// kept from frame to frame and drawn again only when what they show changed.
//
// A map shows the casters the light can see, as the light sees them -- so
// its signature is a hash of the light's own view plus every shadow caster
// that can reach it: which entity, which model, its world matrix (visible /
// cast_shadows decide whether it counts at all). Same signature, same
// picture: the tile is left as it is.
//
// Casters that move every frame -- a Spin component on them or on an
// ancestor (a flock of gulls circling a lamp) -- would otherwise make a map
// redraw everything near them, every frame. So each map is two layers: the
// static casters, kept in a second atlas and redrawn only when they change,
// and the moving ones, drawn each frame on top of a copy of that layer.
// Everything else that changes (a gizmo drag, an agent's edit) just redraws
// the static layer when it happens.

// Which casters.
typedef enum shadow_casters
{
    SHADOW_CASTERS_ALL,
    SHADOW_CASTERS_STATIC,   // no Spin on them or above them
    SHADOW_CASTERS_MOVING    // turned by a Spin, theirs or an ancestor's
} shadow_casters;

// FNV-1a over `size` bytes, continuing from `hash` (start with
// SHADOW_SIGNATURE_SEED).
#define SHADOW_SIGNATURE_SEED 0xcbf29ce484222325ull
uint64 shadow_signature_bytes(uint64 hash, const void* data, size_t size);

// Adds every caster of `which` that would be drawn into a map: visible,
// casting shadows, with a loaded model, and -- when given -- inside `planes`
// (a frustum, glm_frustum_planes()) or touching `sphere` (center xyz, radius
// w). Entities without bounds count wherever they are, as the map draws
// them. *out_count (may be NULL) gets how many there were.
uint64 shadow_signature_casters(uint64 hash, const world* w, const vec4* planes, const f32* sphere,
                                shadow_casters which, uint32* out_count);

// Draws the casters of `which` inside `planes` with the bound depth shader
// `depth_shader`, into whatever region is set.
void shadow_draw_casters(const world* w, rhi_shader depth_shader, const mat4 view_projection, const vec4* planes,
                         shadow_casters which);

// One map's layers: what it should show this frame, and what's in its tile.
// shadow_layers_plan() fills the first three from the light's own parameters
// (any bytes that change its picture) and its casters.
typedef struct shadow_layers
{
    uint64 static_signature;   // the light and its static casters
    uint64 signature;          // ...and its moving casters too: the whole picture
    b8     moving;             // it has moving casters
    uint64 static_drawn;       // what the static atlas's tile holds; 0 = nothing valid
    uint64 drawn;              // what the live atlas's tile holds; 0 = nothing valid
} shadow_layers;

void shadow_layers_plan(shadow_layers* layers, const void* light_key, size_t key_size, const world* w,
                        const vec4* planes, const f32* sphere);

// What a tile needs this frame.
typedef enum shadow_work
{
    SHADOW_WORK_NONE,           // it already shows this
    SHADOW_WORK_DRAW_ALL,       // no moving casters: clear and draw the static ones into the live tile
    SHADOW_WORK_DRAW_STATIC,    // redraw the static layer, then copy it and draw the moving ones
    SHADOW_WORK_COPY_STATIC     // copy the (unchanged) static layer, then draw the moving ones
} shadow_work;

// Decides, and marks the layers as drawn (the caller then does the work).
shadow_work shadow_layers_work(shadow_layers* layers);
