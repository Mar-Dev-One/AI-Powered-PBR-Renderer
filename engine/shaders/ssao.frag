#version 460 core

// Screen-space ambient occlusion (Renderer/PostProcess.h post_ssao()): how
// much of the sky each visible point can see, from the depth pre-pass
// alone. For each pixel: its view-space position and normal (rebuilt from
// depth), then SAMPLES points in the hemisphere above it, within u_radius;
// every one that lies behind the depth buffer counts as blocked. pbr.frag
// multiplies the result into its ambient light only -- direct light isn't
// occluded by nearby geometry this way.
//
// The kernel turns by one of 16 angles chosen by the pixel's place in a
// 4 x 4 tile; ssao_blur.frag averages exactly that tile, so the noise
// cancels out instead of being smeared.

in vec2 v_uv;

out vec4 frag_color;

uniform sampler2D u_depth;               // the scene's depth buffer
uniform mat4      u_projection;
uniform mat4      u_inverse_projection;
uniform float     u_radius;              // view-space meters

const int   SAMPLES = 16;
const float PI      = 3.14159265359;

vec3 view_position(ivec2 pixel)
{
    vec2  size  = vec2(textureSize(u_depth, 0));
    float depth = texelFetch(u_depth, clamp(pixel, ivec2(0), ivec2(size) - 1), 0).r;
    vec4  ndc   = vec4((vec2(pixel) + 0.5) / size * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4  view  = u_inverse_projection * ndc;
    return view.xyz / view.w;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_depth, pixel, 0).r;
    if (depth >= 1.0)
    {
        frag_color = vec4(1.0);   // sky: nothing to occlude
        return;
    }

    vec3 p = view_position(pixel);

    // The normal from neighbouring depths, taking on each axis the side
    // whose depth is closer -- across a silhouette, the other side belongs
    // to a different surface.
    vec3 l = view_position(pixel + ivec2(-1, 0)), r = view_position(pixel + ivec2(1, 0));
    vec3 d = view_position(pixel + ivec2(0, -1)), u = view_position(pixel + ivec2(0, 1));
    vec3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
    vec3 dy = abs(u.z - p.z) < abs(p.z - d.z) ? u - p : p - d;
    vec3 n  = normalize(cross(dx, dy));
    if (dot(n, -p) < 0.0)
        n = -n;   // face the camera

    // A basis around the normal, turned by this pixel's angle in the tile.
    int   tile  = (pixel.x & 3) + 4 * (pixel.y & 3);
    float angle = float(tile) * (2.0 * PI / 16.0);
    vec3  turn  = vec3(cos(angle), sin(angle), 0.0);
    vec3  t     = normalize(turn - n * dot(turn, n));
    vec3  b     = cross(n, t);
    mat3  tbn   = mat3(t, b, n);

    // Depth differences this small are the surface itself, not an occluder.
    float bias = 0.03 * u_radius;

    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; ++i)
    {
        // Spread over the hemisphere on a golden-angle spiral, more of them
        // close to the point, where occlusion matters most.
        float k   = (float(i) + 0.5) / float(SAMPLES);
        float z   = 1.0 - k;
        float phi = float(i) * 2.39996323;
        vec3  dir = vec3(cos(phi) * sqrt(1.0 - z * z), sin(phi) * sqrt(1.0 - z * z), z);
        vec3  s   = p + tbn * dir * (u_radius * mix(0.1, 1.0, k * k));

        vec4 clip = u_projection * vec4(s, 1.0);
        vec2 uv   = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
            continue;

        float scene_z = view_position(ivec2(uv * vec2(textureSize(u_depth, 0)))).z;

        // Something far in front of the point (a pole in the foreground)
        // doesn't occlude it: fade with distance beyond the radius.
        float range = smoothstep(0.0, 1.0, u_radius / abs(p.z - scene_z));
        occlusion  += (scene_z >= s.z + bias ? 1.0 : 0.0) * range;
    }

    frag_color = vec4(vec3(1.0 - occlusion / float(SAMPLES)), 1.0);
}
