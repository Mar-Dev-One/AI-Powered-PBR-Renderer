#version 460 core

in vec3 v_world_pos;
in vec3 v_normal;
in vec2 v_uv;

out vec4 frag_color;

// Solid base color. Swapping this for a sampler2D albedo texture later is
// a one-line change (multiply u_albedo by a texture() sample) -- kept as
// a uniform for now so this shader doesn't force every lit draw call to
// also bind a texture, the way textured.frag does.
uniform vec3 u_albedo;

// Single directional light (e.g. a sun) -- no attenuation, since
// direction alone is already what a directional light is. Swap for a
// position + falloff term if/when a point light is needed.
uniform vec3  u_light_dir;    // direction the light travels *toward* the surface, normalized
uniform vec3  u_light_color;
uniform vec3  u_view_pos;
uniform float u_ambient_strength;
uniform float u_specular_strength;
uniform float u_shininess;

void main()
{
    vec3 normal  = normalize(v_normal);
    vec3 to_light = normalize(-u_light_dir);
    vec3 to_view  = normalize(u_view_pos - v_world_pos);
    vec3 halfway  = normalize(to_light + to_view);

    vec3 ambient = u_ambient_strength * u_light_color;

    float diffuse_factor = max(dot(normal, to_light), 0.0);
    vec3  diffuse        = diffuse_factor * u_light_color;

    // Blinn-Phong specular term (dot with the halfway vector rather than
    // the reflection vector) -- cheaper than classic Phong and doesn't
    // need a reflect() call, at the cost of a slightly different
    // highlight shape for the same shininess value.
    float specular_factor = (diffuse_factor > 0.0)
        ? pow(max(dot(normal, halfway), 0.0), u_shininess)
        : 0.0;   // no specular on faces already facing away from the light
    vec3 specular = u_specular_strength * specular_factor * u_light_color;

    vec3 result = (ambient + diffuse + specular) * u_albedo;
    frag_color  = vec4(result, 1.0);
}
