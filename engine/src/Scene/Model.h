#pragma once

#include "../Core/Defines.h"
#include "Mesh.h"
#include "Material.h"

#include <cglm/cglm.h>

// A loaded 3D model: GPU meshes, the materials and textures they use, and
// a list of "submeshes" saying which mesh is drawn with which material at
// which transform.
//
// The mesh/submesh split matters for glTF, where one mesh (a wheel, a
// chair leg) is commonly placed many times by different scene nodes: the
// geometry is uploaded once, and each placement is one lightweight
// submesh entry. For .obj there is simply one mesh + one identity-transform
// submesh per material.

typedef struct model_submesh
{
    uint32 mesh_index;       // index into model::meshes
    int32  material_index;   // index into model::materials, or -1 = model::default_material
    mat4   transform;        // this placement's transform in model space (glTF node hierarchy, baked)
} model_submesh;

typedef struct model
{
    mesh*          meshes;          // owned GPU meshes
    uint32         mesh_count;

    model_submesh* submeshes;       // draw list
    uint32         submesh_count;

    material*      materials;       // owned
    uint32         material_count;

    // Every texture any material uses, owned by the model and destroyed
    // with it. Materials only borrow pointers into this pool.
    rhi_texture*   textures;
    uint32         texture_count;

    // Used by submeshes with material_index == -1 (meshes the file gave no
    // material to, e.g. a bare .obj). Public on purpose: an app can tweak
    // it live (color, metallic, roughness) to preview a model.
    material       default_material;

    // Model-space bounds of everything drawn (transforms applied), plus
    // totals for on-screen stats. Filled in by the loaders.
    vec3   bounds_min;
    vec3   bounds_max;
    uint32 vertex_count;      // unique GPU vertices
    uint32 triangle_count;    // triangles drawn per frame (instances counted every time)
} model;

// Loads a model, picking the loader from the file extension:
//   .obj          Wavefront OBJ (+ .mtl and its textures)
//   .gltf / .glb  glTF 2.0 (external, embedded or binary buffers/textures)
// Returns a zero-initialized model (submesh_count == 0) on failure -- log
// output says why -- so callers check `submesh_count` before drawing.
model model_load(const char* path);

// Wavefront .obj (via the vendored fast_obj parser). Faces are
// fan-triangulated; identical (position, uv, normal) corners are welded into
// shared vertices; one submesh per material group. Kd/Ke/Ns/d and the
// map_Kd / map_Ke textures become a metallic-roughness material (metallic 0,
// roughness derived from Ns). Bump maps (map_bump) are not used -- .obj
// doesn't say whether one is a height map or a normal map, so guessing
// wrong would look worse than skipping. Use glTF for normal-mapped assets.
model model_load_obj(const char* path);

// glTF 2.0 (via the vendored cgltf parser), .gltf or .glb. Supports scene
// hierarchies (node transforms are baked into submeshes), multiple
// primitives per mesh, shared meshes, all five standard material textures,
// embedded and external images, sampler wrap/filter modes, alpha
// mask/blend, double-sided materials and emissive strength.
// Not supported (loaded as static geometry / ignored): skinning and
// animation (models appear in their bind pose), morph targets, sparse
// accessors beyond what cgltf unpacks, Draco/meshopt compression,
// KHR_texture_transform, and second UV sets.
model model_load_gltf(const char* path);

// Uniform-scale + translation that centres the model's bounds on the
// origin and scales its largest dimension to `target_size`. Real-world
// assets come in wildly different units (an avocado is 0.08 m wide, a
// building 80 m), so apps that just want to *see* a model apply this as
// the world matrix. `out` is a column-major mat4.
void model_get_fit_transform(const model* m, f32 target_size, mat4 out);

// Wraps one already-uploaded mesh as a single-submesh model with no
// materials of its own (it draws with default_material). Takes ownership of
// `msh`. `transform` places the mesh in model space; NULL = identity. Used
// for built-in primitives (Assets/AssetCache.h).
model model_from_mesh(mesh msh, const mat4 transform);

// Draws every submesh through `shader`, which must follow the uniform
// contract in Material.h plus u_model / u_normal_matrix (the world matrix
// and its inverse-transpose, set here per submesh). `world` is the whole
// model's transform. The caller sets view-projection and lighting
// uniforms and binds the shader first. Opaque and alpha-masked submeshes
// draw first, blended ones after (not depth-sorted). Restores the default
// render state when done.
void model_draw(const model* m, rhi_shader shader, const mat4 world);

// Which submeshes model_draw_ex() draws. A renderer drawing many models
// draws every model's MODEL_PASS_OPAQUE first, then every MODEL_PASS_BLEND,
// so no model's transparent parts are drawn before another's opaque ones.
typedef enum model_draw_pass
{
    MODEL_PASS_OPAQUE = 1 << 0,   // opaque + alpha-masked
    MODEL_PASS_BLEND  = 1 << 1,
    MODEL_PASS_ALL    = MODEL_PASS_OPAQUE | MODEL_PASS_BLEND
} model_draw_pass;

// model_draw() with a per-instance material override (see Material.h;
// NULL = none) applied to every submesh, limited to `passes`.
void model_draw_ex(const model* m, rhi_shader shader, const mat4 world,
                   const material_override* override, model_draw_pass passes);

// Depth-only pass for shadow mapping: draws every opaque/alpha-masked
// submesh (blended ones are skipped, same as model_draw()'s first pass --
// casting a shadow from something translucent would need its alpha, which
// this simple pass doesn't sample) into whatever depth-only framebuffer is
// currently bound, from the light's point of view. `shader` only needs to
// follow shadow_depth.vert's tiny uniform contract: u_model and
// u_light_view_projection; no material binding happens here; the caller
// binds `shader` and the target framebuffer first. Restores the default
// render state when done.
void model_draw_depth(const model* m, rhi_shader shader, const mat4 world, const mat4 light_view_projection);

// Frees every GPU resource and all CPU-side bookkeeping. Safe to call on a
// zero-initialized/failed-load model.
void model_destroy(model* m);
