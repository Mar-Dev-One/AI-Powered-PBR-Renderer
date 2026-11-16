#version 460 core

// Depth of field, first pass (Renderer/PostProcess.h post_depth_of_field()):
// the scene at half size, each texel the average of its 2 x 2 pixels, with
// the circle of confusion in alpha -- how wide that spot is blurred to, in
// half-size texels, signed: negative in front of the focus, positive behind.
// Of the four pixels' blurs the nearest one is kept, so a blurred
// foreground's edge reaches as far as it should.

out vec4 frag_color;

uniform sampler2D u_scene;    // linear HDR, full size
uniform sampler2D u_depth;    // the scene's depth buffer, full size
uniform vec2      u_planes;   // the camera's near, far
uniform float     u_coc_scale;   // blur of a point infinitely far behind the focus, in full-size pixels
uniform float     u_focus;       // meters
uniform float     u_max_coc;     // full-size pixels

float view_depth(float depth)
{
    float z = depth * 2.0 - 1.0;
    return 2.0 * u_planes.x * u_planes.y / (u_planes.y + u_planes.x - z * (u_planes.y - u_planes.x));
}

// Thin lens (Scene/Camera.h camera_circle_of_confusion()): c = K (1 - s / d).
float circle_of_confusion(float depth)
{
    return clamp(u_coc_scale * (1.0 - u_focus / view_depth(depth)), -u_max_coc, u_max_coc);
}

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 2;
    ivec2 last = textureSize(u_scene, 0) - 1;

    vec3  color = vec3(0.0);
    float coc   = u_max_coc;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 p = min(base + ivec2(i & 1, i >> 1), last);
        color += max(texelFetch(u_scene, p, 0).rgb, vec3(0.0));
        coc    = min(coc, circle_of_confusion(texelFetch(u_depth, p, 0).r));
    }

    frag_color = vec4(color * 0.25, coc * 0.5);
}
