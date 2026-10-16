#version 460 core

// Bake pass 1: resamples an equirectangular (lat/long) HDR panorama into a
// cube map face. The panorama is uploaded with row 0 at the bottom (see
// Core/Image.c), so v = 1 is straight up (+Y).

in vec3 v_dir;

out vec4 frag_color;

uniform sampler2D u_equirect;

const vec2 INV_ATAN = vec2(0.15915494, 0.31830989);   // 1 / (2 pi), 1 / pi

// Largest finite half-float is 65504; the cube map is RGBA16F, so an
// out-of-range texel (some HDRIs store the sun at 1e6 and up) would become
// infinity and poison every convolution that samples it.
const float MAX_RADIANCE = 60000.0;

void main()
{
    vec3 d = normalize(v_dir);
    vec2 uv = vec2(atan(d.z, d.x), asin(clamp(d.y, -1.0, 1.0))) * INV_ATAN + 0.5;

    vec3 radiance = texture(u_equirect, uv).rgb;
    frag_color = vec4(min(radiance, vec3(MAX_RADIANCE)), 1.0);
}
