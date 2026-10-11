#include "ModelInternal.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

char* model_duplicate_string(const char* s)
{
    // malloc+memcpy rather than POSIX strdup, so this doesn't depend on
    // which libc extensions are visible under -std=c11.
    size_t len  = strlen(s) + 1;
    char*  copy = malloc(len);

    if (copy)
        memcpy(copy, s, len);

    return copy;
}

char* model_directory_of(const char* path)
{
    const char* sep1 = strrchr(path, '/');
    const char* sep2 = strrchr(path, '\\');
    const char* sep  = sep1 > sep2 ? sep1 : sep2;

    size_t len = sep ? (size_t)(sep - path) + 1 : 0;
    char*  dir = malloc(len + 1);
    if (!dir)
        FATAL("model: out of memory");

    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

rhi_texture model_upload_image(const image* img, b8 srgb,
                                rhi_texture_filter filter, rhi_texture_wrap wrap)
{
    rhi_texture_desc desc = {
        .width            = img->width,
        .height           = img->height,
        .format           = srgb ? RHI_FORMAT_RGBA8_SRGB : RHI_FORMAT_RGBA8,
        .filter           = filter,
        .wrap             = wrap,
        .pixels           = img->pixels,
        .generate_mipmaps = true
    };

    return rhi_texture_create(desc);
}

// Grow-by-one realloc. Model loads are one-off and these arrays hold at
// most thousands of entries, so simple beats clever here.
static void*
grow_array(void* array, uint32 old_count, size_t element_size)
{
    void* grown = realloc(array, ((uint64)old_count + 1) * element_size);
    if (!grown)
        FATAL("model: out of memory growing array");

    return grown;
}

uint32 model_add_texture(model* m, rhi_texture texture)
{
    m->textures = grow_array(m->textures, m->texture_count, sizeof(*m->textures));
    m->textures[m->texture_count] = texture;
    return m->texture_count++;
}

uint32 model_add_material(model* m, const material* mat)
{
    m->materials = grow_array(m->materials, m->material_count, sizeof(*m->materials));
    m->materials[m->material_count] = *mat;
    return m->material_count++;
}

uint32 model_add_mesh(model* m, mesh msh)
{
    m->meshes = grow_array(m->meshes, m->mesh_count, sizeof(*m->meshes));
    m->meshes[m->mesh_count] = msh;
    return m->mesh_count++;
}

void model_add_submesh(model* m, uint32 mesh_index, int32 material_index, const mat4 transform)
{
    m->submeshes = grow_array(m->submeshes, m->submesh_count, sizeof(*m->submeshes));

    model_submesh* sub = &m->submeshes[m->submesh_count++];
    sub->mesh_index     = mesh_index;
    sub->material_index = material_index;
    glm_mat4_copy((vec4*)transform, sub->transform);
}

void model_finalize(model* m)
{
    m->vertex_count   = 0;
    m->triangle_count = 0;

    for (uint32 i = 0; i < m->mesh_count; ++i)
        m->vertex_count += m->meshes[i].vertex_count;

    b8 have_bounds = false;

    for (uint32 i = 0; i < m->submesh_count; ++i)
    {
        const model_submesh* sub = &m->submeshes[i];
        const mesh*          msh = &m->meshes[sub->mesh_index];

        m->triangle_count += msh->index_count / 3;

        // Transform all 8 corners of the mesh's box and grow the model
        // box around them -- exact for translation/scale, conservative
        // (still contains the mesh) once rotation is involved.
        for (int corner = 0; corner < 8; ++corner)
        {
            vec3 p = {
                (corner & 1) ? msh->bounds_max[0] : msh->bounds_min[0],
                (corner & 2) ? msh->bounds_max[1] : msh->bounds_min[1],
                (corner & 4) ? msh->bounds_max[2] : msh->bounds_min[2],
            };

            vec3 world;
            glm_mat4_mulv3((vec4*)sub->transform, p, 1.0f, world);

            if (!have_bounds)
            {
                glm_vec3_copy(world, m->bounds_min);
                glm_vec3_copy(world, m->bounds_max);
                have_bounds = true;
            }
            else
            {
                glm_vec3_minv(m->bounds_min, world, m->bounds_min);
                glm_vec3_maxv(m->bounds_max, world, m->bounds_max);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static b8
has_extension(const char* path, const char* extension)
{
    size_t path_len = strlen(path);
    size_t ext_len  = strlen(extension);

    if (path_len < ext_len)
        return false;

    const char* tail = path + path_len - ext_len;

    for (size_t i = 0; i < ext_len; ++i)
        if (tolower((unsigned char)tail[i]) != extension[i])
            return false;

    return true;
}

model model_load(const char* path)
{
    if (has_extension(path, ".obj"))
        return model_load_obj(path);

    if (has_extension(path, ".gltf") || has_extension(path, ".glb"))
        return model_load_gltf(path);

    LOG_ERROR("model_load: unsupported model format '%s' (expected .obj, .gltf or .glb)\n", path);
    return (model){ 0 };
}

void model_get_fit_transform(const model* m, f32 target_size, mat4 out)
{
    vec3 extent, center;
    glm_vec3_sub((f32*)m->bounds_max, (f32*)m->bounds_min, extent);
    glm_vec3_add((f32*)m->bounds_max, (f32*)m->bounds_min, center);
    glm_vec3_scale(center, 0.5f, center);

    f32 largest = fmaxf(extent[0], fmaxf(extent[1], extent[2]));
    f32 scale   = (largest > 1e-8f) ? (target_size / largest) : 1.0f;

    // out = Scale * Translate(-center): centre first, then scale about the origin.
    vec3 neg_center;
    glm_vec3_negate_to(center, neg_center);

    glm_mat4_identity(out);
    glm_scale_uni(out, scale);
    glm_translate(out, neg_center);
}

model model_from_mesh(mesh msh, const mat4 transform)
{
    model m = { 0 };

    mat4 identity;
    glm_mat4_identity(identity);

    model_add_mesh(&m, msh);
    model_add_submesh(&m, 0, -1, transform ? transform : identity);
    material_init_default(&m.default_material);
    model_finalize(&m);

    return m;
}

void model_draw(const model* m, rhi_shader shader, const mat4 world)
{
    model_draw_ex(m, shader, world, NULL, MODEL_PASS_ALL);
}

void model_draw_ex(const model* m, rhi_shader shader, const mat4 world,
                   const material_override* override, model_draw_pass passes)
{
    // Pass 0: opaque + alpha-masked. Pass 1: blended, after everything
    // solid is in the depth buffer so it composites over it correctly.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (!(passes & (pass == 0 ? MODEL_PASS_OPAQUE : MODEL_PASS_BLEND)))
            continue;

        for (uint32 i = 0; i < m->submesh_count; ++i)
        {
            const model_submesh* sub = &m->submeshes[i];
            const material* mat = sub->material_index >= 0
                ? &m->materials[sub->material_index]
                : &m->default_material;

            b8 is_blend = (mat->alpha_mode == MATERIAL_ALPHA_BLEND);
            if ((pass == 1) != is_blend)
                continue;

            // A shallow copy is enough: material_bind() only reads it, and
            // the borrowed texture pointers stay the model's.
            material overridden;
            if (override && override->fields)
            {
                overridden = *mat;
                material_apply_override(&overridden, override);
                mat = &overridden;
            }

            mat4 model_matrix;
            glm_mat4_mul((vec4*)world, (vec4*)sub->transform, model_matrix);

            // Inverse-transpose keeps normals correct under non-uniform scale.
            mat4 normal_matrix;
            glm_mat4_inv(model_matrix, normal_matrix);
            glm_mat4_transpose(normal_matrix);

            rhi_shader_set_mat4(shader, "u_model", (const f32*)model_matrix);
            rhi_shader_set_mat4(shader, "u_normal_matrix", (const f32*)normal_matrix);

            // A mirrored placement (negative determinant) reverses winding.
            material_bind(mat, shader, glm_mat4_det(model_matrix) < 0.0f);

            mesh_draw(&m->meshes[sub->mesh_index]);
        }
    }

    rhi_set_render_state(rhi_render_state_default());
}

void model_draw_depth(const model* m, rhi_shader shader, const mat4 world, const mat4 light_view_projection)
{
    for (uint32 i = 0; i < m->submesh_count; ++i)
    {
        const model_submesh* sub = &m->submeshes[i];
        const material* mat = sub->material_index >= 0
            ? &m->materials[sub->material_index]
            : &m->default_material;

        if (mat->alpha_mode == MATERIAL_ALPHA_BLEND)
            continue;

        mat4 model_matrix;
        glm_mat4_mul((vec4*)world, (vec4*)sub->transform, model_matrix);

        rhi_shader_set_mat4(shader, "u_model", (const f32*)model_matrix);
        rhi_shader_set_mat4(shader, "u_light_view_projection", (const f32*)light_view_projection);

        // Still respect double-sided / mirrored-transform winding here --
        // culling the wrong faces would punch holes in the shadow.
        rhi_render_state state = rhi_render_state_default();
        state.cull_back_faces = !mat->double_sided;
        state.front_face_cw   = glm_mat4_det(model_matrix) < 0.0f;
        rhi_set_render_state(state);

        mesh_draw(&m->meshes[sub->mesh_index]);
    }

    rhi_set_render_state(rhi_render_state_default());
}

void model_destroy(model* m)
{
    if (!m)
        return;

    for (uint32 i = 0; i < m->mesh_count; ++i)
        mesh_destroy(&m->meshes[i]);
    free(m->meshes);

    for (uint32 i = 0; i < m->material_count; ++i)
        material_destroy(&m->materials[i]);
    free(m->materials);

    for (uint32 i = 0; i < m->texture_count; ++i)
        rhi_texture_destroy(m->textures[i]);
    free(m->textures);

    free(m->submeshes);

    material_destroy(&m->default_material);

    memset(m, 0, sizeof(*m));
}
