#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../Scene/Scene.h"

// Bridges Scene.h's light entities to pbr.frag's u_lights[]/u_light_count
// uniforms -- the multi-light equivalent of Renderer/IBL.h's ibl_bind(),
// just for direct light instead of indirect.

// Upper bound on lights pbr.frag's u_lights[] array holds. Keep this in
// sync with pbr.frag's own MAX_LIGHTS -- there is no shared header between
// C and GLSL here, so the two are defined separately and must be edited
// together.
#define LIGHTING_MAX_LIGHTS 8

// Gathers every ENTITY_LIGHT in `s` (scene_gather_lights(), Scene.h) and
// uploads them to `shader`'s u_lights[] array plus u_light_count, in the
// layout pbr.frag's scene_light struct expects. Lights beyond
// LIGHTING_MAX_LIGHTS are silently dropped (the same cap
// scene_gather_lights() enforces). Requires scene_update_world_transforms()
// to have been called on `s` this frame -- same requirement as
// scene_gather_lights() itself.
//
// Shadowing: pbr.frag only shadow-tests against the cascades Renderer/
// Shadow.h builds for one directional light (see csm_update()). This sets
// u_shadow_light_index to the first LIGHT_DIRECTIONAL in scene storage
// order (Scene.h), so pass that same light's direction to csm_update(); -1
// (no directional light) leaves every light unshadowed. Point and spot
// lights cast no shadows until per-light shadow maps exist.
void lighting_bind(rhi_shader shader, const scene* s);