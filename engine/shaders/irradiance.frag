#version 460 core

// Bake pass 2: diffuse irradiance. For every direction N, integrates the
// environment's radiance over the hemisphere around N, weighted by cos(theta).
// Stored divided by pi, so pbr.frag can multiply it straight by the diffuse
// color (Lambert's albedo / pi already folded in).

in vec3 v_dir;

out vec4 frag_color;

uniform samplerCube u_environment;

const float PI = 3.14159265359;

// Angular step of the brute-force hemisphere walk, in radians. The source
// is read from the mip level whose texel size matches this step, which
// pre-blurs it: point-sampling the full-resolution sky would alias hard on
// small bright lights (the sun) at this step size.
const float SAMPLE_DELTA = 0.05;

void main()
{
    vec3 N = normalize(v_dir);

    // Tangent basis around N. Y-up is degenerate when N is (anti)parallel to
    // it, so fall back to Z there.
    vec3 helper = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right  = normalize(cross(helper, N));
    vec3 up     = cross(N, right);

    // A 90 degree cube face at 32 texels across is ~2.8 degrees per texel
    // (0.049 rad) -- the mip matching SAMPLE_DELTA.
    float source_size = float(textureSize(u_environment, 0).x);
    float lod = max(log2(source_size / 32.0), 0.0);

    vec3  sum = vec3(0.0);
    float count = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += SAMPLE_DELTA)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += SAMPLE_DELTA)
        {
            // Spherical to cartesian in tangent space, then into world space.
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 dir = t.x * right + t.y * up + t.z * N;

            sum += textureLod(u_environment, dir, lod).rgb * cos(theta) * sin(theta);
            count += 1.0;
        }
    }

    frag_color = vec4(PI * sum / count, 1.0);
}
