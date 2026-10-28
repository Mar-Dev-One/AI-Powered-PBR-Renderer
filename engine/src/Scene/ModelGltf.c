#include "ModelInternal.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// Everything the recursive scene walk needs, bundled so the helpers below
// don't each take seven parameters.
typedef struct gltf_context
{
    model*       result;
    cgltf_data*  data;
    const char*  directory;         // folder of the .gltf/.glb, for relative image URIs

    // One GPU texture per (glTF texture, colorspace). The same image used
    // as both albedo and data would need two uploads (sRGB vs linear).
    rhi_texture* texture_cache;     // [texture_index * 2 + is_srgb]

    // glTF material index -> index in result->materials, or -1 = not converted yet.
    int32*       material_of;

    // Each glTF mesh is a list of primitives; every primitive becomes one
    // GPU mesh, built the first time a node instances it and reused after.
    // Flat index = primitive_base[mesh_index] + primitive_index.
    // Values: >= 0 mesh index in result, -1 = not built yet, -2 = unusable.
    uint32*      primitive_base;
    int32*       primitive_mesh;
} gltf_context;

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

static image
decode_gltf_image(const gltf_context* ctx, const cgltf_image* img)
{
    image none = { 0 };

    // 1) Embedded in a buffer (the normal case for .glb).
    if (img->buffer_view)
    {
        const uint8* base = (const uint8*)img->buffer_view->buffer->data;
        if (!base)
            return none;

        return image_load_from_memory(base + img->buffer_view->offset, img->buffer_view->size, 4);
    }

    if (!img->uri)
        return none;

    // 2) Base64 data URI inside a .gltf.
    if (strncmp(img->uri, "data:", 5) == 0)
    {
        const char* comma = strchr(img->uri, ',');
        if (!comma || comma - img->uri < 7 || strncmp(comma - 7, ";base64", 7) != 0)
            return none;

        const char* b64 = comma + 1;
        size_t len = strlen(b64);
        size_t padding = 0;
        if (len > 0 && b64[len - 1] == '=') padding++;
        if (len > 1 && b64[len - 2] == '=') padding++;
        size_t size = len / 4 * 3 - padding;

        cgltf_options options = { 0 };
        void* bytes = NULL;
        if (cgltf_load_buffer_base64(&options, size, b64, &bytes) != cgltf_result_success)
            return none;

        image decoded = image_load_from_memory((const uint8*)bytes, size, 4);
        free(bytes);
        return decoded;
    }

    // 3) External file next to the model, possibly percent-encoded ("my%20tex.png").
    char* relative = model_duplicate_string(img->uri);
    cgltf_decode_uri(relative);

    size_t dir_len = strlen(ctx->directory);
    char*  full    = malloc(dir_len + strlen(relative) + 1);
    if (!full)
        FATAL("model_load_gltf: out of memory");

    memcpy(full, ctx->directory, dir_len);
    strcpy(full + dir_len, relative);

    image decoded = image_load(full, 4);

    free(full);
    free(relative);
    return decoded;
}

static rhi_texture_wrap
convert_wrap(cgltf_wrap_mode mode)
{
    switch (mode)
    {
        case cgltf_wrap_mode_clamp_to_edge:    return RHI_WRAP_CLAMP_TO_EDGE;
        case cgltf_wrap_mode_mirrored_repeat:  return RHI_WRAP_MIRRORED_REPEAT;
        default:                               return RHI_WRAP_REPEAT;
    }
}

// Returns the GPU texture for a material's texture reference, uploading it
// on first use. NULL = no texture (unset, undecodable, or an image format
// stb can't read such as KTX2/WebP -- the material then falls back to its
// factors).
static rhi_texture
get_texture(gltf_context* ctx, const cgltf_texture_view* view, b8 srgb)
{
    if (!view->texture || !view->texture->image)
        return NULL;

    if (view->texcoord != 0)
        LOG_WARN("model_load_gltf: texture uses UV set %d; only set 0 is supported\n", (int)view->texcoord);

    if (view->has_transform)
        LOG_WARN("model_load_gltf: KHR_texture_transform is not supported; texture drawn untransformed\n");

    cgltf_size index = cgltf_texture_index(ctx->data, view->texture);
    rhi_texture* cached = &ctx->texture_cache[index * 2 + (srgb ? 1 : 0)];

    if (*cached)
        return *cached;

    image img = decode_gltf_image(ctx, view->texture->image);
    if (!img.pixels)
    {
        LOG_WARN("model_load_gltf: could not decode image '%s'; material will render without it\n",
                 view->texture->image->uri ? view->texture->image->uri : "(embedded)");
        return NULL;
    }

    rhi_texture_wrap   wrap   = RHI_WRAP_REPEAT;
    rhi_texture_filter filter = RHI_FILTER_LINEAR;

    if (view->texture->sampler)
    {
        // The RHI has one wrap mode for both axes; S is used for both.
        wrap = convert_wrap(view->texture->sampler->wrap_s);

        if (view->texture->sampler->mag_filter == cgltf_filter_type_nearest)
            filter = RHI_FILTER_NEAREST;
    }

    *cached = model_upload_image(&img, srgb, filter, wrap);
    image_free(&img);

    model_add_texture(ctx->result, *cached);
    return *cached;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

// Everything about a glTF material except its textures: no GPU work, so
// model_read_gltf_materials() can use it headless too.
static void
convert_material_factors(const cgltf_material* src, material* out)
{
    material_init_default(out);
    out->name = model_duplicate_string(src->name ? src->name : "unnamed");

    if (src->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness* pbr = &src->pbr_metallic_roughness;

        memcpy(out->base_color, pbr->base_color_factor, sizeof(out->base_color));
        out->metallic  = pbr->metallic_factor;
        out->roughness = pbr->roughness_factor;
    }
    else if (src->has_pbr_specular_glossiness)
    {
        // KHR_materials_pbrSpecularGlossiness is deprecated. Approximate it as
        // a dielectric: diffuse color -> base color, glossiness -> roughness.
        const cgltf_pbr_specular_glossiness* sg = &src->pbr_specular_glossiness;

        LOG_WARN("model_load_gltf: material '%s' uses specular-glossiness; approximated as metallic-roughness\n", out->name);

        memcpy(out->base_color, sg->diffuse_factor, sizeof(out->base_color));
        out->metallic  = 0.0f;
        out->roughness = 1.0f - sg->glossiness_factor;
    }
    else
    {
        // No pbrMetallicRoughness block at all: glTF's defaults are 1/1/1.
        out->metallic  = 1.0f;
        out->roughness = 1.0f;
    }

    f32 emissive_strength = src->has_emissive_strength ? src->emissive_strength.emissive_strength : 1.0f;
    for (int i = 0; i < 3; ++i)
        out->emissive[i] = src->emissive_factor[i] * emissive_strength;

    switch (src->alpha_mode)
    {
        case cgltf_alpha_mode_mask:  out->alpha_mode = MATERIAL_ALPHA_MASK;  break;
        case cgltf_alpha_mode_blend: out->alpha_mode = MATERIAL_ALPHA_BLEND; break;
        default:                     out->alpha_mode = MATERIAL_ALPHA_OPAQUE; break;
    }

    out->alpha_cutoff = src->alpha_cutoff;
    out->double_sided = src->double_sided ? true : false;

    // Glass: KHR_materials_transmission, _ior and _volume. Factors only; the
    // transmission and thickness textures aren't read yet.
    if (src->has_transmission)
        out->transmission = src->transmission.transmission_factor;
    if (src->has_ior)
        out->ior = src->ior.ior;
    if (src->has_volume)
    {
        // In the mesh's own units (glTF scales it with the node), taken as meters.
        out->thickness = src->volume.thickness_factor;
        memcpy(out->attenuation_color, src->volume.attenuation_color, sizeof(out->attenuation_color));

        // glTF's default is +infinity, "no absorption"; ours is 0.
        f32 distance = src->volume.attenuation_distance;
        out->attenuation_distance = (isfinite(distance) && distance < FLT_MAX) ? distance : 0.0f;
    }

    if (src->transmission.transmission_texture.texture || src->volume.thickness_texture.texture)
        LOG_WARN("model_load_gltf: material '%s' has transmission or thickness textures; only the factors are used\n",
                 out->name);
}

static int32
get_material(gltf_context* ctx, const cgltf_material* src)
{
    if (!src)
        return -1;

    cgltf_size src_index = (cgltf_size)(src - ctx->data->materials);
    if (ctx->material_of[src_index] >= 0)
        return ctx->material_of[src_index];

    material out;
    convert_material_factors(src, &out);

    if (src->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness* pbr = &src->pbr_metallic_roughness;

        out.textures[MATERIAL_SLOT_BASE_COLOR]         = get_texture(ctx, &pbr->base_color_texture, true);
        out.textures[MATERIAL_SLOT_METALLIC_ROUGHNESS] = get_texture(ctx, &pbr->metallic_roughness_texture, false);
    }
    else if (src->has_pbr_specular_glossiness)
    {
        out.textures[MATERIAL_SLOT_BASE_COLOR] = get_texture(ctx, &src->pbr_specular_glossiness.diffuse_texture, true);
    }

    out.textures[MATERIAL_SLOT_NORMAL]    = get_texture(ctx, &src->normal_texture, false);
    out.textures[MATERIAL_SLOT_OCCLUSION] = get_texture(ctx, &src->occlusion_texture, false);
    out.textures[MATERIAL_SLOT_EMISSIVE]  = get_texture(ctx, &src->emissive_texture, true);

    out.normal_scale       = out.textures[MATERIAL_SLOT_NORMAL]    ? src->normal_texture.scale    : 1.0f;
    out.occlusion_strength = out.textures[MATERIAL_SLOT_OCCLUSION] ? src->occlusion_texture.scale : 1.0f;

    int32 index = (int32)model_add_material(ctx->result, &out);
    ctx->material_of[src_index] = index;
    return index;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

// Accessors backed by a compressed buffer view hold garbage until decoded
// by an extension we don't implement.
static b8
accessor_usable(const cgltf_accessor* accessor)
{
    return accessor && !(accessor->buffer_view && accessor->buffer_view->has_meshopt_compression);
}

// Reads `accessor` as tightly packed floats (`components` per element)
// into a fresh array. Handles normalized integer formats and sparse
// accessors. NULL on failure; caller frees.
static f32*
unpack_attribute(const cgltf_accessor* accessor, uint32 components)
{
    if (!accessor_usable(accessor) || cgltf_num_components(accessor->type) != components)
        return NULL;

    cgltf_size float_count = accessor->count * components;
    f32* out = malloc((uint64)float_count * sizeof(f32));
    if (!out)
        FATAL("model_load_gltf: out of memory unpacking accessor");

    if (cgltf_accessor_unpack_floats(accessor, out, float_count) < float_count)
    {
        free(out);
        return NULL;
    }

    return out;
}

// Rewrites strips/fans as a plain triangle list. Returns a new index array
// (caller frees) and updates *count, or NULL if the topology is degenerate.
static uint32*
to_triangle_list(cgltf_primitive_type type, const uint32* indices, uint32* count)
{
    if (*count < 3)
        return NULL;

    uint32  tris = *count - 2;
    uint32* out  = malloc((uint64)tris * 3 * sizeof(uint32));
    if (!out)
        FATAL("model_load_gltf: out of memory");

    for (uint32 i = 0; i < tris; ++i)
    {
        if (type == cgltf_primitive_type_triangle_strip)
        {
            // Every other triangle in a strip has flipped winding; swap two
            // corners on odd triangles to keep them all facing the same way.
            out[3 * i + 0] = indices[(i & 1) ? i + 1 : i];
            out[3 * i + 1] = indices[(i & 1) ? i     : i + 1];
            out[3 * i + 2] = indices[i + 2];
        }
        else   // triangle fan
        {
            out[3 * i + 0] = indices[0];
            out[3 * i + 1] = indices[i + 1];
            out[3 * i + 2] = indices[i + 2];
        }
    }

    *count = tris * 3;
    return out;
}

// glTF says a primitive without normals must be flat-shaded. That needs a
// vertex per triangle corner (each face gets its own normal), so expand the
// indexed mesh into an unindexed one.
static void
expand_to_flat_shaded(mesh_vertex** vertices, uint32* vertex_count, uint32** indices, uint32 index_count)
{
    mesh_vertex* expanded = malloc((uint64)index_count * sizeof(mesh_vertex));
    if (!expanded)
        FATAL("model_load_gltf: out of memory");

    for (uint32 i = 0; i < index_count; ++i)
        expanded[i] = (*vertices)[(*indices)[i]];

    for (uint32 i = 0; i + 2 < index_count; i += 3)
    {
        vec3 e1, e2, n;
        glm_vec3_sub(expanded[i + 1].position, expanded[i].position, e1);
        glm_vec3_sub(expanded[i + 2].position, expanded[i].position, e2);
        glm_vec3_cross(e1, e2, n);

        if (glm_vec3_norm2(n) > 1e-20f)
            glm_vec3_normalize(n);
        else
            glm_vec3_copy((vec3){ 0.0f, 1.0f, 0.0f }, n);

        for (int c = 0; c < 3; ++c)
            glm_vec3_copy(n, expanded[i + c].normal);
    }

    for (uint32 i = 0; i < index_count; ++i)
        (*indices)[i] = i;

    free(*vertices);
    *vertices     = expanded;
    *vertex_count = index_count;
}

// Builds one GPU mesh from a glTF primitive. Returns its index in
// ctx->result, or -1 (with a logged reason) if the primitive is unusable.
static int32
build_primitive(gltf_context* ctx, const cgltf_primitive* prim)
{
    if (prim->has_draco_mesh_compression)
    {
        LOG_WARN("model_load_gltf: primitive uses Draco compression (unsupported); skipped\n");
        return -1;
    }

    if (prim->type != cgltf_primitive_type_triangles &&
        prim->type != cgltf_primitive_type_triangle_strip &&
        prim->type != cgltf_primitive_type_triangle_fan)
    {
        LOG_WARN("model_load_gltf: point/line primitive skipped (only triangles are rendered)\n");
        return -1;
    }

    const cgltf_accessor *position = NULL, *normal = NULL, *tangent = NULL, *uv0 = NULL;

    for (cgltf_size a = 0; a < prim->attributes_count; ++a)
    {
        const cgltf_attribute* attr = &prim->attributes[a];

        if      (attr->type == cgltf_attribute_type_position)                    position = attr->data;
        else if (attr->type == cgltf_attribute_type_normal)                      normal   = attr->data;
        else if (attr->type == cgltf_attribute_type_tangent)                     tangent  = attr->data;
        else if (attr->type == cgltf_attribute_type_texcoord && attr->index == 0) uv0     = attr->data;
    }

    if (!position)
    {
        LOG_WARN("model_load_gltf: primitive without POSITION skipped\n");
        return -1;
    }

    uint32 vertex_count = (uint32)position->count;

    f32* positions = unpack_attribute(position, 3);
    f32* normals   = normal  ? unpack_attribute(normal, 3)  : NULL;
    f32* tangents  = tangent ? unpack_attribute(tangent, 4) : NULL;
    f32* uvs       = uv0     ? unpack_attribute(uv0, 2)     : NULL;

    // Optional attributes that fail to unpack (wrong size, unsupported
    // compression) are treated as absent rather than aborting the load.
    if (!positions || vertex_count == 0 ||
        (normal && normals && normal->count != vertex_count) ||
        (tangent && tangents && tangent->count != vertex_count) ||
        (uv0 && uvs && uv0->count != vertex_count))
    {
        LOG_WARN("model_load_gltf: primitive with unreadable or mismatched attributes skipped\n");
        free(positions); free(normals); free(tangents); free(uvs);
        return -1;
    }

    mesh_vertex* vertices = calloc(vertex_count, sizeof(mesh_vertex));
    if (!vertices)
        FATAL("model_load_gltf: out of memory");

    for (uint32 i = 0; i < vertex_count; ++i)
    {
        memcpy(vertices[i].position, &positions[3 * i], sizeof(vec3));

        if (normals)  memcpy(vertices[i].normal,  &normals[3 * i],  sizeof(vec3));
        if (tangents) memcpy(vertices[i].tangent, &tangents[4 * i], 4 * sizeof(f32));

        if (uvs)
        {
            vertices[i].uv[0] = uvs[2 * i + 0];
            // glTF's UV origin is top-left; textures are flipped to a
            // bottom-left origin on load (Core/Image.c), so flip V to match.
            vertices[i].uv[1] = 1.0f - uvs[2 * i + 1];
        }
    }

    free(positions); free(normals); free(tangents); free(uvs);

    // Indices (or 0..n-1 for a non-indexed primitive).
    uint32  index_count = prim->indices ? (uint32)prim->indices->count : vertex_count;
    uint32* indices     = malloc((uint64)index_count * sizeof(uint32));
    if (!indices)
        FATAL("model_load_gltf: out of memory");

    if (prim->indices)
    {
        if (!accessor_usable(prim->indices) ||
            cgltf_accessor_unpack_indices(prim->indices, indices, sizeof(uint32), index_count) < index_count)
        {
            LOG_WARN("model_load_gltf: primitive with unreadable indices skipped\n");
            free(vertices); free(indices);
            return -1;
        }
    }
    else
    {
        for (uint32 i = 0; i < index_count; ++i)
            indices[i] = i;
    }

    if (prim->type != cgltf_primitive_type_triangles)
    {
        uint32* list = to_triangle_list(prim->type, indices, &index_count);
        free(indices);
        indices = list;

        if (!indices)
        {
            free(vertices);
            return -1;
        }
    }

    index_count -= index_count % 3;   // drop a dangling partial triangle

    // An out-of-range index would make the GPU read past the vertex buffer.
    for (uint32 i = 0; i < index_count; ++i)
    {
        if (indices[i] >= vertex_count)
        {
            LOG_WARN("model_load_gltf: primitive with out-of-range index skipped\n");
            free(vertices); free(indices);
            return -1;
        }
    }

    if (index_count == 0)
    {
        free(vertices); free(indices);
        return -1;
    }

    if (!normals)
    {
        expand_to_flat_shaded(&vertices, &vertex_count, &indices, index_count);
        mesh_compute_tangents(vertices, vertex_count, indices, index_count);
    }
    else if (!tangents)
    {
        mesh_compute_tangents(vertices, vertex_count, indices, index_count);
    }

    mesh gpu = mesh_create(vertices, vertex_count, indices, index_count);

    free(vertices);
    free(indices);

    return (int32)model_add_mesh(ctx->result, gpu);
}

// ---------------------------------------------------------------------------
// Scene walk
// ---------------------------------------------------------------------------

static void
emit_mesh(gltf_context* ctx, const cgltf_mesh* gltf_mesh, const mat4 world)
{
    cgltf_size mesh_index = cgltf_mesh_index(ctx->data, gltf_mesh);

    for (cgltf_size p = 0; p < gltf_mesh->primitives_count; ++p)
    {
        const cgltf_primitive* prim = &gltf_mesh->primitives[p];
        int32* slot = &ctx->primitive_mesh[ctx->primitive_base[mesh_index] + p];

        if (*slot == -1)
        {
            int32 built = build_primitive(ctx, prim);
            *slot = built >= 0 ? built : -2;
        }

        if (*slot < 0)
            continue;

        model_add_submesh(ctx->result, (uint32)*slot, get_material(ctx, prim->material), world);
    }
}

static void
visit_node(gltf_context* ctx, const cgltf_node* node)
{
    if (node->mesh)
    {
        mat4 world;
        cgltf_node_transform_world(node, (cgltf_float*)world);
        emit_mesh(ctx, node->mesh, world);
    }

    for (cgltf_size c = 0; c < node->children_count; ++c)
        visit_node(ctx, node->children[c]);
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

model model_load_gltf(const char* path)
{
    model result = { 0 };

    cgltf_options options = { 0 };
    cgltf_data*   data    = NULL;

    cgltf_result status = cgltf_parse_file(&options, path, &data);
    if (status != cgltf_result_success)
    {
        LOG_ERROR("model_load_gltf: failed to parse '%s' (cgltf error %d)\n", path, (int)status);
        return result;
    }

    // Loads the .bin files / GLB payload / base64 buffers into memory.
    status = cgltf_load_buffers(&options, data, path);
    if (status != cgltf_result_success)
    {
        LOG_ERROR("model_load_gltf: failed to load buffers for '%s' (cgltf error %d)\n", path, (int)status);
        cgltf_free(data);
        return result;
    }

    // Structural checks (index ranges, accessor bounds, node cycles) so the
    // code below can trust the file.
    status = cgltf_validate(data);
    if (status != cgltf_result_success)
    {
        LOG_ERROR("model_load_gltf: '%s' failed validation (cgltf error %d)\n", path, (int)status);
        cgltf_free(data);
        return result;
    }

    material_init_default(&result.default_material);

    char* directory = model_directory_of(path);

    gltf_context ctx = { 0 };
    ctx.result    = &result;
    ctx.data      = data;
    ctx.directory = directory;

    ctx.texture_cache = calloc(data->textures_count * 2 + 1, sizeof(rhi_texture));
    ctx.material_of   = malloc((data->materials_count + 1) * sizeof(int32));
    ctx.primitive_base = malloc((data->meshes_count + 1) * sizeof(uint32));

    uint32 total_primitives = 0;
    for (cgltf_size i = 0; i < data->meshes_count; ++i)
    {
        ctx.primitive_base[i] = total_primitives;
        total_primitives += (uint32)data->meshes[i].primitives_count;
    }

    ctx.primitive_mesh = malloc(((uint64)total_primitives + 1) * sizeof(int32));

    if (!ctx.texture_cache || !ctx.material_of || !ctx.primitive_base || !ctx.primitive_mesh)
        FATAL("model_load_gltf: out of memory");

    for (cgltf_size i = 0; i < data->materials_count; ++i)
        ctx.material_of[i] = -1;

    for (uint32 i = 0; i < total_primitives; ++i)
        ctx.primitive_mesh[i] = -1;

    if (data->nodes_count == 0)
    {
        // Legal but unusual: meshes with no scene graph. Place each at the origin.
        mat4 identity;
        glm_mat4_identity(identity);

        for (cgltf_size i = 0; i < data->meshes_count; ++i)
            emit_mesh(&ctx, &data->meshes[i], identity);
    }
    else if (data->scene || data->scenes_count > 0)
    {
        const cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0];

        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            visit_node(&ctx, scene->nodes[i]);
    }
    else
    {
        // Nodes but no scene: treat every parentless node as a root.
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent)
                visit_node(&ctx, &data->nodes[i]);
    }

    free(ctx.primitive_mesh);
    free(ctx.primitive_base);
    free(ctx.material_of);
    free(ctx.texture_cache);
    free(directory);
    cgltf_free(data);

    if (result.submesh_count == 0)
    {
        LOG_ERROR("model_load_gltf: '%s' contains no drawable triangle geometry\n", path);
        model_destroy(&result);
        return result;
    }

    model_finalize(&result);

    LOG_INFO("model_load_gltf: loaded '%s' (%u submeshes, %u meshes, %u materials, %u textures, %u vertices, %u triangles)\n",
             path, result.submesh_count, result.mesh_count, result.material_count, result.texture_count,
             result.vertex_count, result.triangle_count);

    return result;
}

uint32 model_read_gltf_materials(const char* path, material* out, uint32 max_count)
{
    cgltf_options options = { 0 };
    cgltf_data*   data    = NULL;

    // Materials live in the JSON; no buffers needed.
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success)
    {
        LOG_ERROR("model_read_gltf_materials: failed to parse '%s'\n", path);
        return 0;
    }

    uint32 count = (uint32)data->materials_count;
    for (uint32 i = 0; i < count && i < max_count; ++i)
        convert_material_factors(&data->materials[i], &out[i]);

    cgltf_free(data);
    return count;
}
