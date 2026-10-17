#version 460 core

// Bloom, going down (Renderer/PostProcess.h post_bloom()): halves u_source
// into the next smaller target with the 13-tap filter from Jimenez,
// "Next Generation Post Processing in Call of Duty: Advanced Warfare"
// (SIGGRAPH 2014) -- five overlapping 2x2 box filters, so the blur stays
// smooth instead of turning blocky as the chain gets small.
//
// u_first marks the pass that reads the HDR scene itself: there each box
// is weighted by 1 / (1 + luma) (Karis's average), so one pixel far
// brighter than the rest -- a specular glint on a single pixel -- can't
// spread into a flickering blob.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_source;
uniform vec2      u_source_texel;   // 1 / u_source's size
uniform int       u_first;

vec3 tap(float x, float y)
{
    // max(): a NaN or negative value in the scene would otherwise spread.
    return max(texture(u_source, v_uv + vec2(x, y) * u_source_texel).rgb, vec3(0.0));
}

float karis_weight(vec3 c)
{
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    return 1.0 / (1.0 + luma);
}

void main()
{
    // a . b . c
    // . j . k .
    // d . e . f
    // . l . m .
    // g . h . i
    vec3 a = tap(-2.0,  2.0), b = tap(0.0,  2.0), c = tap(2.0,  2.0);
    vec3 d = tap(-2.0,  0.0), e = tap(0.0,  0.0), f = tap(2.0,  0.0);
    vec3 g = tap(-2.0, -2.0), h = tap(0.0, -2.0), i = tap(2.0, -2.0);
    vec3 j = tap(-1.0,  1.0), k = tap(1.0,  1.0);
    vec3 l = tap(-1.0, -1.0), m = tap(1.0, -1.0);

    vec3 result;
    if (u_first != 0)
    {
        // The five boxes, each averaged, then weighted by Karis's term
        // (the centre box counts half, the four corner boxes an eighth each).
        vec3 box_centre = (j + k + l + m) * 0.25;
        vec3 box_tl     = (a + b + d + e) * 0.25;
        vec3 box_tr     = (b + c + e + f) * 0.25;
        vec3 box_bl     = (d + e + g + h) * 0.25;
        vec3 box_br     = (e + f + h + i) * 0.25;

        float w_centre = 0.5   * karis_weight(box_centre);
        float w_tl     = 0.125 * karis_weight(box_tl);
        float w_tr     = 0.125 * karis_weight(box_tr);
        float w_bl     = 0.125 * karis_weight(box_bl);
        float w_br     = 0.125 * karis_weight(box_br);

        result = (box_centre * w_centre + box_tl * w_tl + box_tr * w_tr + box_bl * w_bl + box_br * w_br)
               / (w_centre + w_tl + w_tr + w_bl + w_br);
    }
    else
    {
        result = e * 0.125
               + (a + c + g + i) * 0.03125
               + (b + d + f + h) * 0.0625
               + (j + k + l + m) * 0.125;
    }

    frag_color = vec4(result, 1.0);
}
