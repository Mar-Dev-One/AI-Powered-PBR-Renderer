#include "Mesh.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

mesh mesh_create(const mesh_vertex* vertices, uint32 vertex_count,
                  const uint32* indices, uint32 index_count)
{
    rhi_vertex_attribute attrs[] = {
        { .location = 0, .component_count = 3, .offset = offsetof(mesh_vertex, position) },
        { .location = 1, .component_count = 3, .offset = offsetof(mesh_vertex, normal) },
        { .location = 2, .component_count = 2, .offset = offsetof(mesh_vertex, uv) },
        { .location = 3, .component_count = 4, .offset = offsetof(mesh_vertex, tangent) },
    };

    rhi_vertex_layout layout = {
        .attributes = attrs,
        .attribute_count = 4,
        .stride = sizeof(mesh_vertex)
    };

    mesh m = { 0 };

    m.vertex_buffer = rhi_vertex_buffer_create(vertices, (uint64)vertex_count * sizeof(mesh_vertex),
                                                &layout, RHI_USAGE_STATIC);
    m.index_buffer  = rhi_index_buffer_create(indices, (uint64)index_count * sizeof(uint32), RHI_USAGE_STATIC);
    m.index_count   = index_count;
    m.vertex_count  = vertex_count;

    if (vertex_count > 0)
    {
        glm_vec3_copy((f32*)vertices[0].position, m.bounds_min);
        glm_vec3_copy((f32*)vertices[0].position, m.bounds_max);

        for (uint32 i = 1; i < vertex_count; ++i)
        {
            glm_vec3_minv(m.bounds_min, (f32*)vertices[i].position, m.bounds_min);
            glm_vec3_maxv(m.bounds_max, (f32*)vertices[i].position, m.bounds_max);
        }
    }

    return m;
}

mesh mesh_create_uv_sphere(uint32 segments, uint32 rings)
{
    if (segments < 3) segments = 3;
    if (rings < 2)    rings = 2;

    uint32 vertex_count = (segments + 1) * (rings + 1);   // +1 column duplicates the seam so UVs can wrap 0 -> 1
    uint32 index_count  = segments * rings * 6;

    mesh_vertex* vertices = malloc((uint64)vertex_count * sizeof(mesh_vertex));
    uint32*      indices  = malloc((uint64)index_count * sizeof(uint32));
    if (!vertices || !indices)
        FATAL("mesh_create_uv_sphere: out of memory");

    uint32 v = 0;
    for (uint32 r = 0; r <= rings; ++r)
    {
        f32 theta = GLM_PIf * (f32)r / (f32)rings;   // 0 at the +Y pole, pi at -Y
        f32 sin_t = sinf(theta), cos_t = cosf(theta);

        for (uint32 s = 0; s <= segments; ++s)
        {
            f32 phi = 2.0f * GLM_PIf * (f32)s / (f32)segments;
            f32 sin_p = sinf(phi), cos_p = cosf(phi);

            mesh_vertex* vert = &vertices[v++];

            // On a unit sphere the position *is* the normal.
            vert->position[0] = vert->normal[0] = sin_t * cos_p;
            vert->position[1] = vert->normal[1] = cos_t;
            vert->position[2] = vert->normal[2] = sin_t * sin_p;

            // Bottom-left UV origin: v = 0 at the south pole.
            vert->uv[0] = (f32)s / (f32)segments;
            vert->uv[1] = 1.0f - (f32)r / (f32)rings;

            // d(position)/d(phi), i.e. the direction of increasing u.
            // Exactly (0,0,0) at the poles, so fall back to the value it
            // approaches just off the pole.
            vert->tangent[0] = -sin_p;
            vert->tangent[1] = 0.0f;
            vert->tangent[2] = cos_p;
            vert->tangent[3] = 1.0f;
        }
    }

    uint32 i = 0;
    for (uint32 r = 0; r < rings; ++r)
    {
        for (uint32 s = 0; s < segments; ++s)
        {
            uint32 a = r * (segments + 1) + s;   // this ring
            uint32 b = a + segments + 1;         // the ring below

            // Counter-clockwise when seen from outside the sphere.
            indices[i++] = a;  indices[i++] = a + 1;  indices[i++] = b + 1;
            indices[i++] = a;  indices[i++] = b + 1;  indices[i++] = b;
        }
    }

    mesh m = mesh_create(vertices, vertex_count, indices, index_count);

    free(vertices);
    free(indices);
    return m;
}

// One flat quad per face: `normal` faces out, `u_axis` is the direction of
// increasing u (the tangent), and v runs along cross(normal, u_axis) so the
// tangent handedness is always +1. Corners go (-u,-v) (+u,-v) (+u,+v)
// (-u,+v), counter-clockwise seen from outside since u x v = normal.
typedef struct face_desc
{
    vec3 normal;
    vec3 u_axis;
} face_desc;

static mesh
mesh_create_faces(const face_desc* faces, uint32 face_count)
{
    mesh_vertex vertices[6 * 4];
    uint32      indices[6 * 6];

    static const f32 CORNERS[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };

    for (uint32 f = 0; f < face_count; ++f)
    {
        vec3 n, u, v;
        glm_vec3_copy((f32*)faces[f].normal, n);
        glm_vec3_copy((f32*)faces[f].u_axis, u);
        glm_vec3_cross(n, u, v);

        for (uint32 c = 0; c < 4; ++c)
        {
            mesh_vertex* vert = &vertices[f * 4 + c];

            // Face centre sits half a unit out along the normal.
            for (int k = 0; k < 3; ++k)
                vert->position[k] = n[k] * 0.5f + u[k] * CORNERS[c][0] + v[k] * CORNERS[c][1];

            glm_vec3_copy(n, vert->normal);
            vert->uv[0] = CORNERS[c][0] + 0.5f;
            vert->uv[1] = CORNERS[c][1] + 0.5f;
            vert->tangent[0] = u[0];
            vert->tangent[1] = u[1];
            vert->tangent[2] = u[2];
            vert->tangent[3] = 1.0f;
        }

        uint32 base = f * 4;
        uint32* idx = &indices[f * 6];
        idx[0] = base; idx[1] = base + 1; idx[2] = base + 2;
        idx[3] = base; idx[4] = base + 2; idx[5] = base + 3;
    }

    return mesh_create(vertices, face_count * 4, indices, face_count * 6);
}

mesh mesh_create_cube(void)
{
    static const face_desc FACES[6] = {
        { {  1.0f,  0.0f,  0.0f }, {  0.0f, 0.0f, -1.0f } },
        { { -1.0f,  0.0f,  0.0f }, {  0.0f, 0.0f,  1.0f } },
        { {  0.0f,  1.0f,  0.0f }, {  1.0f, 0.0f,  0.0f } },
        { {  0.0f, -1.0f,  0.0f }, {  1.0f, 0.0f,  0.0f } },
        { {  0.0f,  0.0f,  1.0f }, {  1.0f, 0.0f,  0.0f } },
        { {  0.0f,  0.0f, -1.0f }, { -1.0f, 0.0f,  0.0f } },
    };

    return mesh_create_faces(FACES, 6);
}

mesh mesh_create_plane(void)
{
    mesh_vertex vertices[4];
    static const f32 CORNERS[4][2] = { { -0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.5f, -0.5f }, { -0.5f, -0.5f } };
    for (uint32 c = 0; c < 4; ++c)
    {
        vertices[c] = (mesh_vertex){
            .position = { CORNERS[c][0], 0.0f, CORNERS[c][1] },
            .normal   = { 0.0f, 1.0f, 0.0f },
            .uv       = { CORNERS[c][0] + 0.5f, 0.5f - CORNERS[c][1] },
            .tangent  = { 1.0f, 0.0f, 0.0f, 1.0f }
        };
    }

    // (-x,+z) (+x,+z) (+x,-z) (-x,-z): counter-clockwise seen from above.
    const uint32 indices[6] = { 0, 1, 2, 0, 2, 3 };
    return mesh_create(vertices, 4, indices, 6);
}

void mesh_compute_smooth_normals(mesh_vertex* vertices, uint32 vertex_count,
                                  const uint32* indices, uint32 index_count)
{
    for (uint32 i = 0; i < vertex_count; ++i)
        glm_vec3_zero(vertices[i].normal);

    for (uint32 i = 0; i + 2 < index_count; i += 3)
    {
        mesh_vertex* v0 = &vertices[indices[i + 0]];
        mesh_vertex* v1 = &vertices[indices[i + 1]];
        mesh_vertex* v2 = &vertices[indices[i + 2]];

        vec3 e1, e2, face_normal;
        glm_vec3_sub(v1->position, v0->position, e1);
        glm_vec3_sub(v2->position, v0->position, e2);

        // Un-normalised cross product: its length is twice the triangle's
        // area, so summing it directly gives area-weighted smoothing for free.
        glm_vec3_cross(e1, e2, face_normal);

        glm_vec3_add(v0->normal, face_normal, v0->normal);
        glm_vec3_add(v1->normal, face_normal, v1->normal);
        glm_vec3_add(v2->normal, face_normal, v2->normal);
    }

    for (uint32 i = 0; i < vertex_count; ++i)
    {
        if (glm_vec3_norm2(vertices[i].normal) > 1e-20f)
            glm_vec3_normalize(vertices[i].normal);
        else
            glm_vec3_copy((vec3){ 0.0f, 1.0f, 0.0f }, vertices[i].normal);   // unreferenced / degenerate
    }
}

void mesh_compute_tangents(mesh_vertex* vertices, uint32 vertex_count,
                            const uint32* indices, uint32 index_count)
{
    // Two accumulators per vertex (tangent, bitangent), packed into one
    // allocation: [0, n) tangents, [n, 2n) bitangents.
    vec3* accum = calloc((uint64)vertex_count * 2, sizeof(vec3));
    if (!accum)
        FATAL("mesh_compute_tangents: out of memory");

    vec3* tan_sum   = accum;
    vec3* bitan_sum = accum + vertex_count;

    for (uint32 i = 0; i + 2 < index_count; i += 3)
    {
        uint32 i0 = indices[i + 0], i1 = indices[i + 1], i2 = indices[i + 2];
        const mesh_vertex* v0 = &vertices[i0];
        const mesh_vertex* v1 = &vertices[i1];
        const mesh_vertex* v2 = &vertices[i2];

        vec3 e1, e2;
        glm_vec3_sub((f32*)v1->position, (f32*)v0->position, e1);
        glm_vec3_sub((f32*)v2->position, (f32*)v0->position, e2);

        f32 du1 = v1->uv[0] - v0->uv[0], dv1 = v1->uv[1] - v0->uv[1];
        f32 du2 = v2->uv[0] - v0->uv[0], dv2 = v2->uv[1] - v0->uv[1];

        f32 det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-12f)
            continue;   // degenerate UV mapping for this triangle -- contributes nothing

        f32 r = 1.0f / det;

        vec3 t, b;
        for (int k = 0; k < 3; ++k)
        {
            t[k] = (e1[k] * dv2 - e2[k] * dv1) * r;
            b[k] = (e2[k] * du1 - e1[k] * du2) * r;
        }

        uint32 corner[3] = { i0, i1, i2 };
        for (int c = 0; c < 3; ++c)
        {
            glm_vec3_add(tan_sum[corner[c]],   t, tan_sum[corner[c]]);
            glm_vec3_add(bitan_sum[corner[c]], b, bitan_sum[corner[c]]);
        }
    }

    for (uint32 i = 0; i < vertex_count; ++i)
    {
        mesh_vertex* v = &vertices[i];

        // Gram-Schmidt: strip whatever part of the tangent lies along the
        // normal so the (T, B, N) frame is orthogonal.
        vec3 t;
        glm_vec3_scale(v->normal, glm_vec3_dot(v->normal, tan_sum[i]), t);
        glm_vec3_sub(tan_sum[i], t, t);

        if (glm_vec3_norm2(t) < 1e-20f)
        {
            // No usable UV data: pick any vector perpendicular to the
            // normal, starting from whichever world axis is least aligned.
            vec3 axis = { 1.0f, 0.0f, 0.0f };
            if (fabsf(v->normal[0]) > 0.9f)
                glm_vec3_copy((vec3){ 0.0f, 1.0f, 0.0f }, axis);

            glm_vec3_cross(v->normal, axis, t);
        }

        glm_vec3_normalize(t);

        vec3 n_cross_t;
        glm_vec3_cross(v->normal, t, n_cross_t);

        v->tangent[0] = t[0];
        v->tangent[1] = t[1];
        v->tangent[2] = t[2];
        v->tangent[3] = (glm_vec3_dot(n_cross_t, bitan_sum[i]) < 0.0f) ? -1.0f : 1.0f;
    }

    free(accum);
}

void mesh_draw(const mesh* m)
{
    rhi_draw_indexed(m->vertex_buffer, m->index_buffer, m->index_count);
}

void mesh_destroy(mesh* m)
{
    if (!m)
        return;

    rhi_buffer_destroy(m->vertex_buffer);
    rhi_buffer_destroy(m->index_buffer);

    m->vertex_buffer = NULL;
    m->index_buffer  = NULL;
    m->index_count   = 0;
}
