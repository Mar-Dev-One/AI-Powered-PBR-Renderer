#pragma once

#include "../Core/Defines.h"

#include "cJSON.h"

// How a scene is lit and backed beyond its entities: the HDRI, whether it
// lights the scene and shows as the sky, and the exposure / ambient
// strength. Saved as a scene file's "environment" object (World/Serialize.h),
// so the editor and the game runtime show a scene the same way.
//
// Format (every key optional when read; omitted ones keep their value):
//   { "hdri": "hdri/sky.hdr" | null, "use_ibl": true, "show_skybox": true,
//     "sky_blur": 0, "ambient_strength": 0.5, "exposure": 1, "bloom": 0.04,
//     "tonemap": "aces" | "agx" | "neutral", "auto_exposure": false,
//     "ssao": true, "ssao_radius": 0.5 }

#define ENVIRONMENT_HDRI_MAX 256

// How light becomes display color (engine/shaders/tonemap.frag): the curve
// that rolls off what's brighter than the screen can show.
typedef enum scene_tonemap
{
    SCENE_TONEMAP_ACES,      // contrasty filmic; bright saturated colors skew to yellow/white
    SCENE_TONEMAP_AGX,       // softer; bright colors fade to white along their own hue
    SCENE_TONEMAP_NEUTRAL,   // Khronos PBR Neutral: colors as authored up to ~0.8, then compressed
    SCENE_TONEMAP_COUNT
} scene_tonemap;

// "aces", "agx", "neutral"; NULL out of range.
const char* scene_tonemap_name(scene_tonemap tonemap);

// The inverse; false if `name` isn't one.
b8 scene_tonemap_from_name(const char* name, scene_tonemap* out_tonemap);

typedef struct scene_environment
{
    char hdri[ENVIRONMENT_HDRI_MAX];   // Radiance .hdr under assets/; "" = none (hemisphere ambient)
    b8   use_ibl;                      // light with the HDRI when it loads
    b8   show_skybox;                  // draw the HDRI as the background
    f32  sky_blur;                     // 0 (sharp) .. 1 (blurriest)
    f32  ambient_strength;             // 0 .. 4
    f32  exposure;                     // (0, 16]; with auto_exposure, compensation on top of it
    b8   auto_exposure;                // adapt to the scene's brightness, as an eye does (Renderer/PostProcess.h)
    f32  bloom;                        // 0 (none) .. 1: how much bright light spreads (Renderer/PostProcess.h)
    scene_tonemap tonemap;
    b8   ssao;                         // ambient occlusion: creases and contacts darken (Renderer/PostProcess.h)
    f32  ssao_radius;                  // [0.05, 5] meters: how far nearby geometry darkens a point
} scene_environment;

// No HDRI, IBL and skybox on (for when one is set), no blur, ambient 0.5,
// exposure 1 (no auto-exposure), bloom 0.04, ACES, SSAO on with a 0.5 m radius.
scene_environment scene_environment_default(void);

// Caller owns the result (cJSON_Delete).
cJSON* serialize_environment(const scene_environment* env);

// Writes the keys present in `fields` into `env`. All-or-nothing: on false
// `env` is unchanged and `error` (may be NULL) names the key and the accepted
// values. The HDRI path must stay inside assets/ (no "..", not absolute).
b8 deserialize_environment(const cJSON* fields, scene_environment* env, char* error, size_t error_size);
