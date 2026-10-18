#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../World/World.h"

#include <cglm/cglm.h>

// Local reflections: each ReflectionProbe entity (World/Components.h) is
// a cube map of the scene seen from its position, GGX-prefiltered like the
// environment's specular map (one roughness per mip), all of them layers of
// one cube map array -- a single texture unit, PROBE_SLOT, which pbr.frag
// otherwise never reads (IBL.h uses it only while drawing the sky).
//
// pbr.frag reflects through the probe whose box holds the surface (the
// smallest, where boxes nest), with the reflected ray bent to where it would
// meet the box's walls (box projection, "parallax correction") so the
// reflection lines up with the room. Inside its box a probe applies fully;
// it fades to the environment's reflection over half a meter outside.
//
// Captures are baked, not live: SceneRenderer re-bakes when the probes
// (their positions and boxes) or the environment change, and on request
// (scene_renderer_bake_probes()). Moving lights or objects needs a re-bake
// to show in the reflections.

#define PROBE_MAX  8      // layers of the array
#define PROBE_SIZE 128    // texels along a face's edge
#define PROBE_MIPS 5      // roughness levels: 128, 64, 32, 16, 8
#define PROBE_SLOT 11     // IBL_SLOT_ENVIRONMENT -- free while pbr.frag draws

typedef struct reflection_probes
{
    rhi_texture     maps;          // cube map array: PROBE_MAX layers, PROBE_MIPS mips, RGBA16F
    rhi_texture     capture_cube;  // one probe's raw capture, with its full mip chain, before prefiltering
    rhi_framebuffer capture;       // RGBA16F + depth, PROBE_SIZE square: where one face is drawn

    // The probes in the world, in world order (from reflection_probes_gather()).
    uint32 count;
    vec3   position[PROBE_MAX];
    vec3   box_min[PROBE_MAX];
    vec3   box_max[PROBE_MAX];

    uint64 baked_signature;        // what the maps were baked from; 0 = nothing baked
    b8     force_bake;             // scene_renderer_bake_probes() asked for one
    b8     warned_count;           // logged that probes beyond PROBE_MAX are ignored

    // pbr.frag's uniform names, built once.
    char   names[PROBE_MAX][3][40];
} reflection_probes;

// The array and the capture targets. False (and `rp` safe to destroy) on
// failure.
b8 reflection_probes_create(reflection_probes* rp);

// This frame's probes from `w` (world positions -- world_update() first --
// and boxes), and a signature of them combined with `environment_key` (what
// identifies the lighting environment), to compare with baked_signature.
uint64 reflection_probes_gather(reflection_probes* rp, const world* w, uint64 environment_key);

// Binds the array to PROBE_SLOT and sets pbr.frag's u_probe_* uniforms;
// `enabled` false (while baking, or with nothing baked) makes pbr.frag
// ignore every probe.
void reflection_probes_bind(const reflection_probes* rp, rhi_shader pbr_shader, b8 enabled);

// Safe on a zeroed / failed / already-destroyed reflection_probes.
void reflection_probes_destroy(reflection_probes* rp);
