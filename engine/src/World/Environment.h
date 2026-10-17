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
//     "sky_blur": 0, "ambient_strength": 0.5, "exposure": 1, "bloom": 0.04 }

#define ENVIRONMENT_HDRI_MAX 256

typedef struct scene_environment
{
    char hdri[ENVIRONMENT_HDRI_MAX];   // Radiance .hdr under assets/; "" = none (hemisphere ambient)
    b8   use_ibl;                      // light with the HDRI when it loads
    b8   show_skybox;                  // draw the HDRI as the background
    f32  sky_blur;                     // 0 (sharp) .. 1 (blurriest)
    f32  ambient_strength;             // 0 .. 4
    f32  exposure;                     // (0, 16]
    f32  bloom;                        // 0 (none) .. 1: how much bright light spreads (Renderer/PostProcess.h)
} scene_environment;

// No HDRI, IBL and skybox on (for when one is set), no blur, ambient 0.5,
// exposure 1, bloom 0.04.
scene_environment scene_environment_default(void);

// Caller owns the result (cJSON_Delete).
cJSON* serialize_environment(const scene_environment* env);

// Writes the keys present in `fields` into `env`. All-or-nothing: on false
// `env` is unchanged and `error` (may be NULL) names the key and the accepted
// values. The HDRI path must stay inside assets/ (no "..", not absolute).
b8 deserialize_environment(const cJSON* fields, scene_environment* env, char* error, size_t error_size);
