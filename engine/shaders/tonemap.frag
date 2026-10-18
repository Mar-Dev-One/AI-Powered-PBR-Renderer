#version 460 core

// Final display pass (Renderer/PostProcess.h): turns the linear HDR scene
// target into display-ready color -- bloom, exposure, tone mapping (ACES,
// AgX or Khronos PBR Neutral), then gamma encoding for the (non-sRGB)
// default framebuffer.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_scene;      // RGBA16F, linear radiance
uniform sampler2D u_bloom;      // the scene blurred (post_bloom()), half size
uniform float     u_bloom_strength;   // 0 = none
uniform float     u_bloom_scale;      // 1 / the chain's levels: u_bloom holds every level's blur added up
uniform float     u_exposure;
uniform int       u_tonemap;    // World/Environment.h's scene_tonemap: 0 ACES, 1 AgX, 2 PBR Neutral
uniform int       u_raw;        // a debug view's data (0..1): gamma only, no exposure or tone mapping
uniform int       u_luma_alpha; // write the pixel's luma to alpha, for fxaa.frag (otherwise alpha = 1)

// --- ACES ---------------------------------------------------------------------
// Narkowicz's fit of the ACES filmic curve: contrasty, and pushes bright
// saturated colors towards yellow and white.
vec3 aces_tonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// --- AgX ----------------------------------------------------------------------
// Troy Sobotka's AgX, as Benjamin Wrensch's minimal version: works in log
// space after a slight desaturating "inset", so very bright colors fade to
// white along their own hue instead of skewing. Softer than ACES.
vec3 agx_contrast(vec3 x)
{
    // Polynomial fit of AgX's default contrast curve (mean error^2 3.7e-6).
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 agx_tonemap(vec3 color)
{
    const mat3 inset = mat3(0.842479062253094,  0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772,  0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3( 1.19687900512017,  -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417,   -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;

    color = inset * max(color, vec3(1e-10));   // log2 of 0 or less is -inf / NaN
    color = clamp(log2(color), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);
    color = agx_contrast(color);
    color = outset * color;

    // AgX's curve ends display-encoded; back to linear, for the shared
    // gamma encoding below.
    return pow(clamp(color, 0.0, 1.0), vec3(2.2));
}

// --- Khronos PBR Neutral ------------------------------------------------------
// Khronos's reference implementation (2024): leaves colors exactly as
// authored up to about 0.8 and only compresses above that, desaturating
// the brightest -- a material's base color shows as itself, which suits
// product shots and color-critical work.
vec3 neutral_tonemap(vec3 color)
{
    const float start_compression = 0.8 - 0.04;
    const float desaturation      = 0.15;

    float x      = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
        return color;

    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

vec3 tonemap(vec3 color)
{
    switch (u_tonemap)
    {
        case 1:  return agx_tonemap(color);
        case 2:  return clamp(neutral_tonemap(color), 0.0, 1.0);
        default: return aces_tonemap(color);
    }
}

void main()
{
    vec3 color = texture(u_scene, v_uv).rgb;

    // A blend rather than an addition, so bloom moves light around instead
    // of adding more of it: the picture's overall brightness stays put.
    if (u_bloom_strength > 0.0)
        color = mix(color, texture(u_bloom, v_uv).rgb * u_bloom_scale, u_bloom_strength);

    color = u_raw != 0 ? clamp(color, 0.0, 1.0) : tonemap(color * u_exposure);
    color = pow(color, vec3(1.0 / 2.2));

    // FXAA finds edges by luma, measured on the gamma-encoded picture as
    // the eye sees it.
    float alpha = u_luma_alpha != 0 ? dot(color, vec3(0.299, 0.587, 0.114)) : 1.0;
    frag_color = vec4(color, alpha);
}
