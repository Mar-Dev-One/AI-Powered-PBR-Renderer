#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"
#include "../World/World.h"

// Bridges World.h's Light entities to pbr.frag's LightBlock uniform block
// (u_lights[], u_light_count, u_shadow_light_index) -- the multi-light
// equivalent of Renderer/IBL.h's ibl_bind(), just for direct light instead
// of indirect.

// Upper bound on lights pbr.frag's u_lights[] array holds. Reaches GLSL as
// MAX_LIGHTS through scene_renderer_shader_defines() (SceneRenderer.h), so
// this is the only place to change it.
#define LIGHTING_MAX_LIGHTS 8

struct gpu_light_block;   // Renderer/FrameData.h

// Gathers every Light entity in `w` (world_gather_lights(), World.h) into
// `block`, the CPU copy of pbr.frag's LightBlock, for SceneRenderer to
// upload. Lights beyond LIGHTING_MAX_LIGHTS are silently dropped. Requires
// world_update() on `w` this frame.
//
// Shadowing: u_shadow_light_index is the first LIGHT_DIRECTIONAL with
// cast_shadows in world storage order (World.h) -- the light the cascades
// (Renderer/Shadow.h) are fit to; -1 if there's none. Spot and point light
// shadows find their lights themselves (SpotShadow.h, PointShadow.h), in
// the same order.
void lighting_fill(const world* w, struct gpu_light_block* block);
