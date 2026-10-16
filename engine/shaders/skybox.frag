#version 460 core

// Environment background. Writes linear HDR radiance, like pbr.frag, so the
// sky and the lit model go through the same tone-mapping pass
// (Renderer/PostProcess.h) and share one look.

in vec3 v_dir;

out vec4 frag_color;

uniform samplerCube u_environment;
uniform float u_lod;   // mip level to read: 0 = sharp, higher = blurrier

void main()
{
    vec3 color = textureLod(u_environment, normalize(v_dir), u_lod).rgb;

    frag_color = vec4(color, 1.0);
}
