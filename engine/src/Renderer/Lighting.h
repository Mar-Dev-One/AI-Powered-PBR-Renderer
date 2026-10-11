#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../World/World.h"

// Bridges World.h's Light entities to pbr.frag's u_lights[]/u_light_count
// uniforms -- the multi-light equivalent of Renderer/IBL.h's ibl_bind(),
// just for direct light instead of indirect.

// Upper bound on lights pbr.frag's u_lights[] array holds. Reaches GLSL as
// MAX_LIGHTS through scene_renderer_shader_defines() (SceneRenderer.h), so
// this is the only place to change it.
#define LIGHTING_MAX_LIGHTS 8

// Gathers every Light entity in `w` (world_gather_lights(), World.h) and
// uploads them to `shader`'s u_lights[] array plus u_light_count, in the
// layout pbr.frag's scene_light struct expects. Lights beyond
// LIGHTING_MAX_LIGHTS are silently dropped. Requires world_update() on `w`
// this frame.
//
// Shadowing: pbr.frag only shadow-tests against the cascades Renderer/
// Shadow.h builds for one directional light (see csm_update()). This sets
// u_shadow_light_index to the first LIGHT_DIRECTIONAL in world storage
// order (World.h), so pass that same light's direction to csm_update(); -1
// (no directional light) leaves every light unshadowed. Point and spot
// lights cast no shadows until per-light shadow maps exist.
void lighting_bind(rhi_shader shader, const world* w);