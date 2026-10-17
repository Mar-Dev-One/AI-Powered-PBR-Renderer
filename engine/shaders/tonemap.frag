#version 460 core

// Final display pass (Renderer/PostProcess.h): turns the linear HDR scene
// target into display-ready color -- bloom, exposure, ACES tone mapping,
// then gamma encoding for the (non-sRGB) default framebuffer.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_scene;      // RGBA16F, linear radiance
uniform sampler2D u_bloom;      // the scene blurred (post_bloom()), half size
uniform float     u_bloom_strength;   // 0 = none
uniform float     u_bloom_scale;      // 1 / the chain's levels: u_bloom holds every level's blur added up
uniform float     u_exposure;
uniform int       u_raw;        // a debug view's data (0..1): gamma only, no exposure or ACES
uniform int       u_luma_alpha; // write the pixel's luma to alpha, for fxaa.frag (otherwise alpha = 1)

// Narkowicz's fit of the ACES filmic curve.
vec3 aces_tonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec3 color = texture(u_scene, v_uv).rgb;

    // A blend rather than an addition, so bloom moves light around instead
    // of adding more of it: the picture's overall brightness stays put.
    if (u_bloom_strength > 0.0)
        color = mix(color, texture(u_bloom, v_uv).rgb * u_bloom_scale, u_bloom_strength);

    color = u_raw != 0 ? clamp(color, 0.0, 1.0) : aces_tonemap(color * u_exposure);
    color = pow(color, vec3(1.0 / 2.2));

    // FXAA finds edges by luma, measured on the gamma-encoded picture as
    // the eye sees it.
    float alpha = u_luma_alpha != 0 ? dot(color, vec3(0.299, 0.587, 0.114)) : 1.0;
    frag_color = vec4(color, alpha);
}
