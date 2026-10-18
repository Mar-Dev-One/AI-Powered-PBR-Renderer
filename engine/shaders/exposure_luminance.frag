#version 460 core

// Auto-exposure, step 1 (Renderer/PostProcess.h post_meter_exposure()):
// the scene's brightness as log2 luminance, drawn at a small fixed size,
// weighted towards the middle of the frame (where the subject usually is).
// Output (log2 luma * weight, weight): halving the target down to 1 x 1
// then averages both, and r / g is the weighted average log luminance.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_scene;        // RGBA16F, linear radiance
uniform vec2      u_scene_texel;  // 1 / the scene's size

float log_luma(vec2 uv)
{
    vec3  c    = texture(u_scene, uv).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Floor and ceiling: a black pixel would drag the average to -inf, the
    // sun's disc to the top of the range.
    return log2(clamp(luma, 1.0 / 1024.0, 1024.0));
}

void main()
{
    // Four taps across this texel's patch of the full-size scene, so small
    // bright things are seen rather than skipped between samples.
    vec2  o = u_scene_texel * 2.0;
    float l = 0.25 * (log_luma(v_uv + vec2(-o.x, -o.y)) + log_luma(v_uv + vec2(o.x, -o.y))
                    + log_luma(v_uv + vec2(-o.x,  o.y)) + log_luma(v_uv + vec2(o.x,  o.y)));

    // The centre counts about 2.7x as much as the corners.
    vec2  d = v_uv - 0.5;
    float w = exp(-dot(d, d) * 4.0);

    frag_color = vec4(l * w, w, 0.0, 1.0);
}
