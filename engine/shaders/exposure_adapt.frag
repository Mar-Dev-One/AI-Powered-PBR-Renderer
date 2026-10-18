#version 460 core

// Auto-exposure, step 3 (Renderer/PostProcess.h post_meter_exposure()):
// moves the adapted brightness (1 x 1, log2 luminance in r) part of the way
// towards this frame's measured one, as an eye adjusts -- u_blend is that
// part, from the frame time (1 jumps straight there).

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_measured;   // 1 x 1: (weighted log2 luma, weight), averaged
uniform sampler2D u_previous;   // 1 x 1: the adapted log2 luminance so far
uniform float     u_blend;

void main()
{
    vec2  m        = texelFetch(u_measured, ivec2(0), 0).rg;
    float measured = m.r / max(m.g, 1e-6);
    float previous = texelFetch(u_previous, ivec2(0), 0).r;

    frag_color = vec4(mix(previous, measured, u_blend), 0.0, 0.0, 1.0);
}
