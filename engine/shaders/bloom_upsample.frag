#version 460 core

// Bloom, coming back up (Renderer/PostProcess.h post_bloom()): blurs the
// smaller target u_source with a 3x3 tent filter while drawing it into the
// next larger one, which is additively blended -- so each level ends up
// holding its own blur plus every wider one below it.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_source;
uniform vec2      u_radius;   // the tent's half-width in UV units (same visual size at every level)

void main()
{
    float x = u_radius.x, y = u_radius.y;

    vec3 a = texture(u_source, v_uv + vec2(-x,  y)).rgb;
    vec3 b = texture(u_source, v_uv + vec2(0.0, y)).rgb;
    vec3 c = texture(u_source, v_uv + vec2( x,  y)).rgb;
    vec3 d = texture(u_source, v_uv + vec2(-x, 0.0)).rgb;
    vec3 e = texture(u_source, v_uv).rgb;
    vec3 f = texture(u_source, v_uv + vec2( x, 0.0)).rgb;
    vec3 g = texture(u_source, v_uv + vec2(-x, -y)).rgb;
    vec3 h = texture(u_source, v_uv + vec2(0.0, -y)).rgb;
    vec3 i = texture(u_source, v_uv + vec2( x, -y)).rgb;

    // 1 2 1 / 2 4 2 / 1 2 1, over 16.
    vec3 result = e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i);
    frag_color = vec4(result / 16.0, 1.0);
}
