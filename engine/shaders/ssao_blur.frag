#version 460 core

// SSAO, cleaned up (Renderer/PostProcess.h post_ssao()): the 4 x 4 average
// that cancels ssao.frag's tiled rotations, skipping texels whose depth is
// far from this pixel's so occlusion doesn't bleed across silhouettes.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_ssao;     // ssao.frag's output
uniform sampler2D u_depth;    // the scene's depth buffer
uniform mat4      u_inverse_projection;

float view_z(ivec2 pixel)
{
    vec2  size  = vec2(textureSize(u_depth, 0));
    float depth = texelFetch(u_depth, clamp(pixel, ivec2(0), ivec2(size) - 1), 0).r;
    vec4  ndc   = vec4((vec2(pixel) + 0.5) / size * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4  view  = u_inverse_projection * ndc;
    return view.z / view.w;
}

void main()
{
    ivec2 pixel    = ivec2(gl_FragCoord.xy);
    ivec2 size     = textureSize(u_ssao, 0);
    float center_z = view_z(pixel);

    // Within about 5% of the pixel's distance counts as the same surface.
    float tolerance = 0.05 * abs(center_z) + 0.01;

    float sum = 0.0, weight = 0.0;
    for (int y = -2; y < 2; ++y)
    {
        for (int x = -2; x < 2; ++x)
        {
            ivec2 at = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
            float w  = abs(view_z(at) - center_z) < tolerance ? 1.0 : 0.0;
            sum    += texelFetch(u_ssao, at, 0).r * w;
            weight += w;
        }
    }

    float ao = weight > 0.0 ? sum / weight : texelFetch(u_ssao, pixel, 0).r;
    frag_color = vec4(vec3(ao), 1.0);
}
