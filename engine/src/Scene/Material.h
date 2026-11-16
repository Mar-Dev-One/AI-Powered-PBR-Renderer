#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"

// A metallic-roughness PBR material -- the model glTF 2.0 uses, and what
// the pbr.* shaders (engine/shaders/pbr.frag) consume. Loaders for other
// formats (Wavefront .obj) convert whatever they have into this shape, so
// there is exactly one material representation downstream.
//
// Texture channel conventions (identical to glTF):
//   base color         RGB = albedo (sRGB-encoded), A = opacity
//   metallic-roughness G = roughness, B = metallic   (linear)
//   normal             tangent-space normal, +Y up   (linear)
//   occlusion          R = ambient occlusion         (linear)
//   emissive           RGB = emitted light           (sRGB-encoded)
// The factor fields below multiply the matching texture (or stand in for
// it when there is no texture).

typedef enum material_alpha_mode
{
    MATERIAL_ALPHA_OPAQUE,   // alpha ignored
    MATERIAL_ALPHA_MASK,     // fragments with alpha < alpha_cutoff are discarded (foliage, fences)
    MATERIAL_ALPHA_BLEND     // true transparency; drawn after all opaque geometry
} material_alpha_mode;

// Fixed texture units. material_bind() always uses these, and pbr.frag's
// samplers are wired to the same numbers, so callers never juggle units.
typedef enum material_texture_slot
{
    MATERIAL_SLOT_BASE_COLOR = 0,
    MATERIAL_SLOT_METALLIC_ROUGHNESS,
    MATERIAL_SLOT_NORMAL,
    MATERIAL_SLOT_OCCLUSION,
    MATERIAL_SLOT_EMISSIVE,
    MATERIAL_SLOT_COUNT
} material_texture_slot;

typedef struct material
{
    char* name;                       // heap-owned, may be NULL

    f32   base_color[4];              // linear RGBA factor
    f32   metallic;                   // 0 = dielectric, 1 = metal
    f32   roughness;                  // 0 = mirror, 1 = fully rough
    f32   emissive[3];                // linear RGB factor
    f32   normal_scale;               // strength of the normal map (1 = as authored)
    f32   occlusion_strength;         // 0 = ignore occlusion map, 1 = full effect

    material_alpha_mode alpha_mode;
    f32   alpha_cutoff;               // only used by MATERIAL_ALPHA_MASK
    b8    double_sided;               // true = no back-face culling, back faces get a flipped normal

    // Glass and clear liquids (glTF KHR_materials_transmission, _volume, _ior).
    // transmission 0 = an ordinary surface; every material made before these
    // fields existed renders exactly as it did.
    f32   transmission;               // 0..1: share of the diffuse light replaced by light passing through
    f32   ior;                        // index of refraction (1.5 = glass, 1.33 = water)
    f32   thickness;                  // how far light goes through the volume, in the model's own units
                                      // (scaled with it, as glTF says); 0 = a thin sheet, no refraction
    f32   attenuation_color[3];       // linear RGB light keeps after attenuation_distance inside the volume
    f32   attenuation_distance;       // meters; 0 = no absorption (glTF's "infinite")

    // A clear coat (glTF KHR_materials_clearcoat): a thin glossy layer over
    // the surface -- lacquer, car paint, coated plastic. 0 = none.
    f32   clearcoat;                  // 0..1: how much of the coat there is
    f32   clearcoat_roughness;        // the coat's own roughness, 0 = mirror-smooth

    // How strongly a dielectric reflects (glTF KHR_materials_specular): its
    // reflectance at normal incidence is the one `ior` gives (0.04 at 1.5),
    // times specular_color, times specular. Metals ignore both.
    f32   specular;                   // 0..1; 1 = as ior says
    f32   specular_color[3];          // linear RGB tint of that reflectance

    // Borrowed from the owning model's texture pool (Model.h) -- the
    // material never frees these. NULL = "no texture, use the factor alone".
    rhi_texture textures[MATERIAL_SLOT_COUNT];
} material;

// Per-instance tweaks layered over a model's own materials at draw time
// (World's MeshRenderer component): two placements of one model can differ
// in color or finish without copying the model. Only the fields whose
// MATERIAL_OVERRIDE_* bit is set in `fields` apply; base_color replaces
// RGB and keeps the material's alpha. Textures still multiply as usual.
typedef enum material_override_field
{
    MATERIAL_OVERRIDE_BASE_COLOR           = 1 << 0,
    MATERIAL_OVERRIDE_METALLIC             = 1 << 1,
    MATERIAL_OVERRIDE_ROUGHNESS            = 1 << 2,
    MATERIAL_OVERRIDE_EMISSIVE             = 1 << 3,
    MATERIAL_OVERRIDE_TRANSMISSION         = 1 << 4,
    MATERIAL_OVERRIDE_IOR                  = 1 << 5,
    MATERIAL_OVERRIDE_THICKNESS            = 1 << 6,
    MATERIAL_OVERRIDE_ATTENUATION_COLOR    = 1 << 7,
    MATERIAL_OVERRIDE_ATTENUATION_DISTANCE = 1 << 8,
    MATERIAL_OVERRIDE_CLEARCOAT            = 1 << 9,
    MATERIAL_OVERRIDE_CLEARCOAT_ROUGHNESS  = 1 << 10,
    MATERIAL_OVERRIDE_SPECULAR             = 1 << 11,
    MATERIAL_OVERRIDE_SPECULAR_COLOR       = 1 << 12
} material_override_field;

typedef struct material_override
{
    uint32 fields;          // MATERIAL_OVERRIDE_* bits
    f32    base_color[3];   // linear RGB
    f32    metallic;
    f32    roughness;
    f32    emissive[3];     // linear RGB
    f32    transmission;
    f32    ior;
    f32    thickness;
    f32    attenuation_color[3];   // linear RGB
    f32    attenuation_distance;
    f32    clearcoat;
    f32    clearcoat_roughness;
    f32    specular;
    f32    specular_color[3];      // linear RGB
} material_override;

// Writes the fields `o` enables into `m`. NULL or fields == 0 is a no-op.
void material_apply_override(material* m, const material_override* o);

// White dielectric of roughness 0.5, not transmissive, no clear coat, glTF's
// default specular, no textures. What a mesh with no material at all gets
// drawn with.
void material_init_default(material* m);

// Uploads this material's factors as uniforms, binds its textures to
// MATERIAL_SLOT_* units, and applies its cull/blend render state.
// `flip_winding` is for instances whose transform mirrors the mesh
// (negative determinant), which reverses triangle winding.
//
// Uniform contract (see engine/shaders/pbr.frag): u_base_color, u_metallic,
// u_roughness, u_emissive, u_normal_scale, u_occlusion_strength,
// u_alpha_mode, u_alpha_cutoff, u_transmission, u_ior, u_thickness,
// u_attenuation_color, u_attenuation_distance, u_clearcoat,
// u_clearcoat_roughness, u_specular, u_specular_color, u_texture_mask (bit N set = slot N has a
// texture) and the five sampler2Ds u_base_color_tex ... u_emissive_tex.
void material_bind(const material* m, rhi_shader shader, b8 flip_winding);

// Frees the material's own heap data (name). Not the textures.
void material_destroy(material* m);
