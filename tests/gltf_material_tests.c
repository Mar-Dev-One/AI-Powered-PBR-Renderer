// gltf_material_tests.c
//
// Headless tests for the glTF material reader (model_read_gltf_materials):
// glass from KHR_materials_transmission, _ior and _volume, glTF's defaults
// when they're absent, and its "infinite" attenuation distance read as 0.
// If Khronos's TransmissionTest.glb or DragonAttenuation.glb are in
// assets/models/, they're checked too; otherwise those checks are skipped.

#include "Scene/Model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        ++g_checks;                                                             \
        if (!(condition))                                                       \
        {                                                                       \
            ++g_failures;                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

#define MAX_MATERIALS 64

static b8
near(f32 a, f32 b)
{
    return fabsf(a - b) < 1e-5f;
}

static const char* const GLASS_GLTF =
    "{\n"
    "  \"asset\": { \"version\": \"2.0\" },\n"
    "  \"extensionsUsed\": [ \"KHR_materials_transmission\", \"KHR_materials_ior\", \"KHR_materials_volume\" ],\n"
    "  \"materials\": [\n"
    "    { \"name\": \"plain\", \"pbrMetallicRoughness\": { \"roughnessFactor\": 0.3 } },\n"
    "    { \"name\": \"tinted_water\",\n"
    "      \"pbrMetallicRoughness\": { \"metallicFactor\": 0, \"roughnessFactor\": 0.05 },\n"
    "      \"extensions\": {\n"
    "        \"KHR_materials_transmission\": { \"transmissionFactor\": 1.0 },\n"
    "        \"KHR_materials_ior\": { \"ior\": 1.33 },\n"
    "        \"KHR_materials_volume\": { \"thicknessFactor\": 0.1,\n"
    "                                  \"attenuationColor\": [ 0.9, 0.5, 0.2 ],\n"
    "                                  \"attenuationDistance\": 0.25 } } },\n"
    "    { \"name\": \"clear_glass\",\n"
    "      \"extensions\": {\n"
    "        \"KHR_materials_transmission\": { \"transmissionFactor\": 0.5 },\n"
    "        \"KHR_materials_volume\": { \"thicknessFactor\": 2.0 } } }\n"
    "  ]\n"
    "}\n";

static void
test_glass_extensions(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/glass_materials.gltf", TEST_SCRATCH_DIR);

    FILE* file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file)
        return;
    fputs(GLASS_GLTF, file);
    fclose(file);

    material m[MAX_MATERIALS];
    uint32 count = model_read_gltf_materials(path, m, MAX_MATERIALS);
    CHECK(count == 3);
    if (count != 3)
        return;

    // No extensions: not glass.
    CHECK(strcmp(m[0].name, "plain") == 0);
    CHECK(m[0].transmission == 0.0f && m[0].ior == 1.5f && m[0].thickness == 0.0f);
    CHECK(m[0].attenuation_distance == 0.0f && m[0].attenuation_color[1] == 1.0f);

    // All three extensions, every value given.
    CHECK(m[1].transmission == 1.0f);
    CHECK(near(m[1].ior, 1.33f));
    CHECK(near(m[1].thickness, 0.1f));
    CHECK(near(m[1].attenuation_color[0], 0.9f) && near(m[1].attenuation_color[1], 0.5f)
          && near(m[1].attenuation_color[2], 0.2f));
    CHECK(near(m[1].attenuation_distance, 0.25f));
    CHECK(near(m[1].roughness, 0.05f) && m[1].metallic == 0.0f);

    // Defaults inside the extensions: no ior extension = 1.5, no
    // attenuation = white at glTF's infinite distance, which reads as 0.
    CHECK(m[2].transmission == 0.5f && m[2].ior == 1.5f && near(m[2].thickness, 2.0f));
    CHECK(m[2].attenuation_color[0] == 1.0f && m[2].attenuation_color[2] == 1.0f);
    CHECK(m[2].attenuation_distance == 0.0f);

    for (uint32 i = 0; i < count; ++i)
        material_destroy(&m[i]);

    remove(path);
}

// Khronos's sample models, when present. Values are checked loosely (some
// material is glass, absorbs, has volume) so the test doesn't depend on
// which revision of the samples is in the folder.
static void
test_khronos_samples(void)
{
    const char* names[] = { "TransmissionTest.glb", "DragonAttenuation.glb" };

    for (int n = 0; n < 2; ++n)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ASSETS_MODELS_DIR, names[n]);

        FILE* file = fopen(path, "rb");
        if (!file)
        {
            printf("gltf_material_tests: %s not in assets/models, skipped\n", names[n]);
            continue;
        }
        fclose(file);

        material m[MAX_MATERIALS];
        uint32 count  = model_read_gltf_materials(path, m, MAX_MATERIALS);
        uint32 filled = count < MAX_MATERIALS ? count : MAX_MATERIALS;
        CHECK(count > 0);

        b8 glass = false, absorbs = false, volume = false;
        for (uint32 i = 0; i < filled; ++i)
        {
            glass   |= m[i].transmission > 0.0f;
            absorbs |= m[i].attenuation_distance > 0.0f;
            volume  |= m[i].thickness > 0.0f;
            printf("  %s / %s: transmission %.2f ior %.2f thickness %.3f attenuation [%.2f %.2f %.2f] at %.3f\n",
                   names[n], m[i].name, m[i].transmission, m[i].ior, m[i].thickness, m[i].attenuation_color[0],
                   m[i].attenuation_color[1], m[i].attenuation_color[2], m[i].attenuation_distance);
            material_destroy(&m[i]);
        }

        CHECK(glass);
        if (n == 1)
            CHECK(absorbs && volume);
    }
}

int
main(void)
{
    test_glass_extensions();
    test_khronos_samples();

    printf("gltf_material_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
