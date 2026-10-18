#version 460 core

// Auto-exposure, step 2 (Renderer/PostProcess.h post_meter_exposure()):
// halves u_source. Drawn at exactly half its size, each output pixel's
// centre lands on the corner shared by four source texels, so one bilinear
// read is their average.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_source;

void main()
{
    frag_color = texture(u_source, v_uv);
}
