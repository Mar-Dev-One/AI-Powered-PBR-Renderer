// editor/Panels.c
//
// The editor's own panels (the Entities, Inspector, Console, Chat and Log
// windows are engine/src/UI's):
//
//   Scene        the project and scene, Add, and the scene's environment
//   Renderer     the editor's viewing choices (anti-aliasing, debug views,
//                shadows, clustering) and what the last frame drew
//   Assets       the project's models and HDRIs, and the models in memory
//   History      the undo steps; click one to go back (or forward) to it
//   GPU timings  each render pass's time

#include "Editor.h"

#include "Core/Paths.h"
#include "Platform/Window.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void
tooltip(const char* text)
{
    if (igIsItemHovered(0))
        igSetTooltip("%s", text);
}

// --- Scene ----------------------------------------------------------------------------

static void
draw_add_controls(editor_state* state)
{
    igSeparatorText("Add (at the view's centre)");
    igDragFloat("Size (m)", &state->add_size, 0.05f, 0.05f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    tooltip("Longest side of a model or shape added below");

    if (igBeginCombo("##add_model", "Model from the project...", 0))
    {
        cJSON*       list   = run_command(state, "assets.list", NULL);
        const cJSON* models = cJSON_GetObjectItem(list, "models");
        const cJSON* key;
        cJSON_ArrayForEach(key, models)
        {
            if (cJSON_IsString(key) && igSelectable_Bool(key->valuestring, false, 0, (ImVec2_c){ 0, 0 }))
                add_model(state, key->valuestring);
        }
        if (cJSON_GetArraySize(models) == 0)
            igTextDisabled("(none yet -- File > Import Asset)");
        cJSON_Delete(list);
        igEndCombo();
    }

    if (igButton("Cube", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:cube");
    igSameLine(0.0f, -1.0f);
    if (igButton("Sphere", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:sphere");
    igSameLine(0.0f, -1.0f);
    if (igButton("Plane", (ImVec2_c){ 0, 0 }))
        add_model(state, "builtin:plane");

    if (igButton("Point light", (ImVec2_c){ 0, 0 }))
        add_light(state, "point");
    igSameLine(0.0f, -1.0f);
    if (igButton("Spot light", (ImVec2_c){ 0, 0 }))
        add_light(state, "spot");
    igSameLine(0.0f, -1.0f);
    if (igButton("Sun", (ImVec2_c){ 0, 0 }))
        add_light(state, "directional");

    if (igButton("Camera (this view)", (ImVec2_c){ 0, 0 }))
        add_camera(state);
    igSameLine(0.0f, -1.0f);
    if (igButton("Reflection probe", (ImVec2_c){ 0, 0 }))
        add_reflection_probe(state);
    tooltip("A box whose shiny surfaces reflect the room around them instead of the distant sky\n"
            "(up to 8 per scene; resize it with its extents in the Inspector)");
    igSameLine(0.0f, -1.0f);
    if (igButton("Empty", (ImVec2_c){ 0, 0 }))
        add_empty(state);
}

// The HDRI picker: (none) or one of the project's .hdr files.
static void
draw_hdri_combo(char* hdri, size_t size, const char* label)
{
    if (!igBeginCombo(label, hdri[0] ? hdri : "(none)", 0))
        return;

    hdri_list hdris;
    list_hdris(&hdris);
    if (igSelectable_Bool("(none)", !hdri[0], 0, (ImVec2_c){ 0, 0 }))
        hdri[0] = '\0';
    for (uint32 i = 0; i < hdris.count; ++i)
    {
        if (igSelectable_Bool(hdris.paths[i], strcmp(hdris.paths[i], hdri) == 0, 0, (ImVec2_c){ 0, 0 }))
            snprintf(hdri, size, "%s", hdris.paths[i]);
    }
    if (hdris.count == 0)
        igTextDisabled("(no .hdr in the project's hdri/)");
    igEndCombo();
}

// Ambient, exposure, post-processing and image-based lighting: the scene's
// environment, not World entities (saved with the scene all the same).
static void
draw_environment_controls(editor_state* state)
{
    scene_environment* env = &state->doc.env;

    igSeparatorText("Environment (saved with the scene)");
    draw_hdri_combo(env->hdri, sizeof(env->hdri), "HDRI");
    if (ibl_cache_maps(&state->ibl))
    {
        igCheckbox("Use IBL", &env->use_ibl);
        tooltip("Light the scene from the HDRI (off: a simple sky/ground ambient)");
        igSameLine(0.0f, -1.0f);
        igCheckbox("Show skybox", &env->show_skybox);
        igSliderFloat("Sky blur", &env->sky_blur, 0.0f, 1.0f, "%.2f", 0);
    }
    else if (env->hdri[0])
    {
        igTextWrapped("Couldn't load %s; lit by the hemisphere ambient light.", env->hdri);
    }
    igSliderFloat("Ambient", &env->ambient_strength, 0.0f, 4.0f, "%.2f", 0);

    igSliderFloat("Exposure", &env->exposure, 0.1f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (env->auto_exposure)
        tooltip("With auto exposure: brighter or darker than the automatic level");
    igCheckbox("Auto exposure", &env->auto_exposure);
    tooltip("Adapt to the scene's brightness, as an eye does: dark scenes brighten, bright ones darken");

    static const char* const tonemaps[SCENE_TONEMAP_COUNT] = { "ACES", "AgX", "PBR Neutral" };
    igCombo_Str_arr("Tone mapping", (int*)&env->tonemap, tonemaps, SCENE_TONEMAP_COUNT, -1);
    tooltip("ACES: contrasty, bright colors turn yellow-white. AgX: softer, bright colors keep their hue.\n"
            "PBR Neutral: materials show their true color; for product shots.");

    igSliderFloat("Bloom", &env->bloom, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
    tooltip("How much bright light (the sun, lamps, emissive surfaces) spreads into its surroundings");

    igCheckbox("Ambient occlusion", &env->ssao);
    tooltip("Creases, corners and where objects meet the ground get less sky light (SSAO)");
    if (env->ssao)
    {
        igSliderFloat("AO radius", &env->ssao_radius, 0.05f, 5.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
        tooltip("How far, in meters, nearby geometry darkens a point");
    }
}

void
draw_scene_panel(editor_state* state)
{
    if (!igBegin(WINDOW_SCENE, &state->show.scene, 0))
    {
        igEnd();
        return;
    }

    igText("%s", state->project.name);
    igTextDisabled("%s", paths_project_root());
    igText("Scene: %s%s", state->doc.name[0] ? state->doc.name : "untitled", doc_dirty(&state->doc) ? " (unsaved)" : "");
    igBeginDisabled(play_active(&state->play));
    if (igButton("Save (Ctrl+S)", (ImVec2_c){ 0, 0 }))
        save_scene(state);
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (igButton("Frame all", (ImVec2_c){ 0, 0 }))
        frame_view(state, NULL);

    draw_add_controls(state);
    draw_environment_controls(state);
    igEnd();
}

// --- Renderer ------------------------------------------------------------------------

static void
stat_row(const char* name, const char* format, ...)
{
    igTableNextRow(0, 0.0f);
    igTableSetColumnIndex(0);
    igTextUnformatted(name, NULL);
    igTableSetColumnIndex(1);

    char    text[128];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    igTextUnformatted(text, NULL);
}

void
draw_renderer_panel(editor_state* state)
{
    if (!igBegin(WINDOW_RENDERER, &state->show.renderer, 0))
    {
        igEnd();
        return;
    }
    scene_renderer* r = &state->renderer;

    igSeparatorText("View (not saved with the scene)");
    static const char* const aa_labels[SCENE_AA_COUNT] = { "Off", "FXAA", "TAA" };
    igCombo_Str_arr("Anti-aliasing", (int*)&state->antialiasing, aa_labels, SCENE_AA_COUNT, -1);
    tooltip("FXAA: a cheap pass over the finished picture. TAA: steadier and closer to supersampling,\n"
            "a little softer; moving objects can smear slightly.");
    igCombo_Str_arr("Debug view", (int*)&state->debug_view, DEBUG_VIEW_LABELS, SCENE_VIEW_COUNT, -1);
    tooltip("The lit scene, or one input to its shading. Lights: how many lights each pixel shades.");

    igCheckbox("Shadows", &state->shadows);
    tooltip("The sun's cascades, and shadow maps for up to 4 spot and 4 point lights with cast_shadows");
    igBeginDisabled(!state->shadows);
    igSliderFloat("Cascade split", &state->cascade_split_lambda, 0.0f, 1.0f, "%.2f", 0);
    tooltip("Where the sun's shadow cascades split: 0 evenly, 1 crowded near the camera (sharper close up)");
    igCheckbox("Tint by cascade", &state->debug_show_cascades);
    igEndDisabled();
    igCheckbox("Unclustered lighting", &state->debug_unclustered);
    tooltip("Every light at every pixel: the reference the clustered lighting must match (slower)");

    if (igButton("Reload shaders", (ImVec2_c){ 0, 0 }))
    {
        cJSON_Delete(run_command(state, "render.reload_shaders", NULL));
    }
    tooltip("Rebuild every shader from its files now (saved edits also reload by themselves)");
    igSameLine(0.0f, -1.0f);
    igBeginDisabled(r->probes.count == 0);
    if (igButton("Bake reflections", (ImVec2_c){ 0, 0 }))
        scene_renderer_bake_probes(r);
    igEndDisabled();
    if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        igSetTooltip(r->probes.count ? "Capture the reflection probes again, after moving lights or objects"
                                     : "No ReflectionProbe in the scene (Scene > Add > Reflection probe)");

    igSeparatorText("Last frame");
    if (igBeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, (ImVec2_c){ 0, 0 }, 0.0f))
    {
        stat_row("View", "%u x %u", state->view_target_width, state->view_target_height);
        stat_row("Entities", "%u", world_count(active_world(state)));
        stat_row("Lights", "%u of %u (%u everywhere)", r->clusters.count, LIGHTING_MAX_LIGHTS, r->clusters.global_count);
        stat_row("Cluster lists", "%u of %u entries (%.0f%%)", r->clusters.index_count, CLUSTER_MAX_INDICES,
                 100.0 * r->clusters.index_count / CLUSTER_MAX_INDICES);
        stat_row("Sun shadows", state->shadows ? (r->shadowed ? "%u cascades, %u px" : "no shadowed sun")
                                               : "off", r->csm.cascade_count, r->csm.resolution);
        stat_row("Spot shadows", "%u of %u, %u redrawn", r->spot.count, SPOT_SHADOW_MAX, r->spot.rendered);
        stat_row("Point shadows", "%u of %u, %u redrawn", r->point.count, POINT_SHADOW_MAX, r->point.rendered);
        stat_row("Reflection probes", "%u of %u%s", r->probes.count, PROBE_MAX,
                 r->probes.count && !r->probes.baked_signature ? " (not baked yet)" : "");
        stat_row("Environment", ibl_cache_maps(&state->ibl) ? "%s" : "hemisphere ambient",
                 state->doc.env.hdri);
        stat_row("GPU frame", "%.2f ms", (double)r->profiler.frame_ms);
        igEndTable();
    }
    igEnd();
}

// --- Assets ---------------------------------------------------------------------------

void
draw_assets_panel(editor_state* state)
{
    if (!igBegin(WINDOW_ASSETS, &state->show.assets, 0))
    {
        igEnd();
        return;
    }

    if (igButton("Import...", (ImVec2_c){ 0, 0 }))
        open_import_dialog(state);
    tooltip("Copy a .glb, .gltf, .obj or .hdr into the project");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(120.0f);
    igDragFloat("Add size (m)", &state->add_size, 0.05f, 0.05f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

    // The folder listings are re-read a couple of times a second, not every
    // frame: a file copied in by hand shows up on its own.
    static cJSON*    models;
    static hdri_list hdris;
    static f64       listed_at = -100.0;
    f64              now       = window_get_time();
    if (now - listed_at > 2.0)
    {
        cJSON_Delete(models);
        models    = run_command(state, "assets.list", NULL);
        listed_at = now;
        list_hdris(&hdris);
    }

    if (igCollapsingHeader_TreeNodeFlags("Models", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const cJSON* list = cJSON_GetObjectItem(models, "models");
        const cJSON* key;
        int          row = 0;
        cJSON_ArrayForEach(key, list)
        {
            if (!cJSON_IsString(key))
                continue;
            igPushID_Int(row++);
            if (igSmallButton("Add"))
                add_model(state, key->valuestring);
            igSameLine(0.0f, -1.0f);
            igTextUnformatted(key->valuestring, NULL);
            igPopID();
        }
        if (cJSON_GetArraySize(list) == 0)
            igTextDisabled("None in models/ yet: Import... brings one in.");
    }

    if (igCollapsingHeader_TreeNodeFlags("HDRIs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (uint32 i = 0; i < hdris.count; ++i)
        {
            b8 current = strcmp(hdris.paths[i], state->doc.env.hdri) == 0;
            igPushID_Int((int)i);
            igBeginDisabled(current);
            if (igSmallButton(current ? "In use" : "Use"))
                snprintf(state->doc.env.hdri, sizeof(state->doc.env.hdri), "%s", hdris.paths[i]);
            igEndDisabled();
            igSameLine(0.0f, -1.0f);
            igTextUnformatted(hdris.paths[i], NULL);
            igPopID();
        }
        if (hdris.count == 0)
            igTextDisabled("None in hdri/ yet: Import... brings a .hdr in.");
    }

    // The asset cache: what's loaded, and who holds it. A model at 0
    // references (nothing uses it now; undo steps and Play count) stays
    // loaded until collected.
    if (igCollapsingHeader_TreeNodeFlags("Loaded models", 0))
    {
        const asset_cache* cache  = &state->assets;
        uint32             unused = 0;
        if (igBeginTable("##loaded", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Borders,
                         (ImVec2_c){ 0, 0 }, 0.0f))
        {
            igTableSetupColumn("Model", 0, 3.0f, 0);
            igTableSetupColumn("Refs", 0, 0.6f, 0);
            igTableSetupColumn("Triangles", 0, 1.0f, 0);
            igTableSetupColumn("Materials", 0, 0.8f, 0);
            igTableHeadersRow();
            for (uint32 i = 0; i < cache->capacity; ++i)
            {
                const asset_entry* entry = &cache->entries[i];
                if (!entry->key)
                    continue;
                unused += entry->ref_count == 0;
                igTableNextRow(0, 0.0f);
                igTableSetColumnIndex(0);
                igTextUnformatted(entry->key, NULL);
                igTableSetColumnIndex(1);
                igText("%u", entry->ref_count);
                igTableSetColumnIndex(2);
                igText("%u", entry->model.triangle_count);
                igTableSetColumnIndex(3);
                igText("%u", entry->model.material_count);
            }
            igEndTable();
        }
        igBeginDisabled(unused == 0);
        if (igButton("Unload unused", (ImVec2_c){ 0, 0 }))
        {
            char line[64];
            snprintf(line, sizeof(line), "Unloaded %u unused model(s).", assets_collect_unused(&state->assets));
            console_info(&state->console, line);
        }
        igEndDisabled();
    }
    igEnd();
}

// --- History ---------------------------------------------------------------------------

void
draw_history_panel(editor_state* state)
{
    if (!igBegin(WINDOW_HISTORY, &state->show.history, 0))
    {
        igEnd();
        return;
    }

    const command_history* h = &state->doc.history;
    if (play_active(&state->play))
        igTextColored(COLOR_WARNING, "Playing: no undo. Stop drops everything Play changed.");
    else if (history_in_transaction(h))
        igTextColored(COLOR_WARNING, "Recording \"%s\" (a chat turn or a drag)...", h->label);

    igBeginDisabled(play_active(&state->play) || history_in_transaction(h));
    if (igButton("Undo", (ImVec2_c){ 0, 0 }))
        undo(state);
    igSameLine(0.0f, -1.0f);
    if (igButton("Redo", (ImVec2_c){ 0, 0 }))
        redo(state);
    igSameLine(0.0f, -1.0f);
    igTextDisabled("%u of %u steps (last %u kept)", h->cursor, h->step_count, HISTORY_MAX_STEPS);
    igSeparator();

    igBeginChild_Str("##steps", (ImVec2_c){ 0, 0 }, 0, 0);
    uint32 jump = UINT32_MAX;
    if (igSelectable_Bool("(as opened)", h->cursor == 0, 0, (ImVec2_c){ 0, 0 }))
        jump = 0;
    for (uint32 i = 0; i < h->step_count; ++i)
    {
        b8 applied = i < h->cursor;
        igPushID_Int((int)i);
        if (!applied)
            igPushStyleColor_U32(ImGuiCol_Text, igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f));
        if (igSelectable_Bool(h->steps[i].label[0] ? h->steps[i].label : "(change)", i + 1 == h->cursor, 0,
                              (ImVec2_c){ 0, 0 }))
            jump = i + 1;
        if (!applied)
            igPopStyleColor(1);
        igPopID();
    }
    igEndChild();
    igEndDisabled();

    if (jump != UINT32_MAX)
        history_jump(state, jump);
    igEnd();
}

// --- GPU timings ------------------------------------------------------------------------

// Each pass of the Viewport's frame on the GPU, as a number and a bar
// against the whole GPU frame, then the CPU frame time.
void
draw_gpu_timings(editor_state* state)
{
    if (!igBegin(WINDOW_TIMINGS, &state->show.gpu_timings, 0))
    {
        igEnd();
        return;
    }

    const gpu_profiler* profiler = &state->renderer.profiler;
    if (profiler->timing_count == 0)
        igTextDisabled("No timings yet (or no GPU timestamp support).");

    for (uint32 i = 0; i < profiler->timing_count; ++i)
    {
        const gpu_timing* t = &profiler->timings[i];
        igText("%-13s %6.2f ms", t->name, (double)t->ms);
        igSameLine(0.0f, -1.0f);
        f32 fraction = profiler->frame_ms > 0.0f ? t->ms / profiler->frame_ms : 0.0f;
        igProgressBar(fraction, (ImVec2_c){ -1.0f, 0.0f }, "");
    }

    igSeparator();
    igText("GPU frame %6.2f ms", (double)profiler->frame_ms);
    igText("CPU frame %6.2f ms (%.0f fps)", (double)state->cpu_frame_ms,
           state->cpu_frame_ms > 0.0f ? 1000.0 / (double)state->cpu_frame_ms : 0.0);
    igTextDisabled("Averaged over about 20 frames. The Viewport only.");
    igEnd();
}
