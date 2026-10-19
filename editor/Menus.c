// editor/Menus.c
//
// The main menu (File, Edit, Add, View, Play, Help), the status bar along
// the window's bottom, and every dialog. A menu item that needs a dialog
// sets open_popup; draw_popups() opens it the next frame.

#include "Editor.h"

#include "Core/FileIO.h"
#include "Core/Paths.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POPUP_SHORTCUTS "Keyboard shortcuts"

void
open_import_dialog(editor_state* state)
{
    state->import_path[0] = '\0';
    state->popup_error[0] = '\0';
    state->open_popup     = POPUP_IMPORT;
}

void
open_export_dialog(editor_state* state)
{
    if (!state->export_dir[0])
        default_export_location(state, state->export_dir, sizeof(state->export_dir));
    state->open_popup = POPUP_EXPORT;
}

// --- Main menu ----------------------------------------------------------------------

static void
draw_scene_menu_items(editor_state* state)
{
    cJSON*       list   = run_command(state, "scene.list", NULL);
    const cJSON* scenes = cJSON_GetObjectItem(list, "scenes");
    const cJSON* name;
    cJSON_ArrayForEach(name, scenes)
    {
        if (!cJSON_IsString(name))
            continue;
        b8 current = strcmp(name->valuestring, state->doc.name) == 0;
        if (igMenuItem_Bool(name->valuestring, NULL, current, true))
            request(state, ACTION_OPEN_SCENE, name->valuestring);
    }
    if (cJSON_GetArraySize(scenes) == 0)
        igTextDisabled("(no saved scenes)");
    cJSON_Delete(list);
}

static void
draw_file_menu(editor_state* state)
{
    if (igMenuItem_Bool("New Project...", NULL, false, true))
    {
        snprintf(state->new_project_name, sizeof(state->new_project_name), "My Game");
        if (!state->new_project_location[0])
            default_projects_location(state->new_project_location, sizeof(state->new_project_location));
        state->popup_error[0] = '\0';
        state->open_popup     = POPUP_NEW_PROJECT;
    }
    if (igMenuItem_Bool("Open Project...", NULL, false, true))
    {
        state->open_project_path[0] = '\0';
        state->popup_error[0]       = '\0';
        state->open_popup           = POPUP_OPEN_PROJECT;
    }
    if (igBeginMenu("Recent Projects", state->recent.count > 0))
    {
        for (uint32 i = 0; i < state->recent.count; ++i)
        {
            if (igMenuItem_Bool(state->recent.paths[i], NULL, false, true))
            {
                char directory[PROJECT_PATH_MAX];   // the list changes when it opens
                snprintf(directory, sizeof(directory), "%s", state->recent.paths[i]);
                request(state, ACTION_OPEN_PROJECT, directory);
            }
        }
        igEndMenu();
    }
    if (igMenuItem_Bool("Show Project Folder", NULL, false, true))
        open_folder(paths_project_root());
    igSeparator();

    if (igMenuItem_Bool("New Scene", NULL, false, true))
        request(state, ACTION_NEW_SCENE, NULL);
    if (igBeginMenu("Open Scene", true))
    {
        draw_scene_menu_items(state);
        igEndMenu();
    }
    if (igMenuItem_Bool("Save", "Ctrl+S", false, !play_active(&state->play)))
        save_scene(state);
    if (igMenuItem_Bool("Save As...", NULL, false, !play_active(&state->play)))
    {
        snprintf(state->save_as_name, sizeof(state->save_as_name), "%s", state->doc.name);
        state->popup_error[0] = '\0';
        state->open_popup     = POPUP_SAVE_AS;
    }
    igSeparator();

    if (igMenuItem_Bool("Import Asset...", NULL, false, true))
        open_import_dialog(state);
    if (igMenuItem_Bool("Export Game...", NULL, false, true))
        open_export_dialog(state);
    if (igMenuItem_Bool("Project Settings...", NULL, false, true))
    {
        state->settings_edit  = state->project;
        state->input_capture  = -1;
        memset(state->input_binding_text, 0, sizeof(state->input_binding_text));
        state->input_new_action[0] = '\0';
        state->popup_error[0] = '\0';
        state->open_popup     = POPUP_SETTINGS;
    }
    igSeparator();

    if (igMenuItem_Bool("Quit", "Esc", false, true))
        request(state, ACTION_QUIT, NULL);
}

static void
draw_edit_menu(editor_state* state)
{
    const command_history* h        = &state->doc.history;
    b8                     editable = !play_active(&state->play) && !history_in_transaction(h);

    char undo_label[HISTORY_LABEL_MAX + 16], redo_label[HISTORY_LABEL_MAX + 16];
    snprintf(undo_label, sizeof(undo_label), "Undo %s", history_can_undo(h) ? h->steps[h->cursor - 1].label : "");
    snprintf(redo_label, sizeof(redo_label), "Redo %s", history_can_redo(h) ? h->steps[h->cursor].label : "");
    if (igMenuItem_Bool(undo_label, "Ctrl+Z", false, editable && history_can_undo(h)))
        undo(state);
    if (igMenuItem_Bool(redo_label, "Ctrl+Y", false, editable && history_can_redo(h)))
        redo(state);
    igSeparator();

    validate_selection(state);
    world*    w        = active_world(state);
    entity_id selected = state->selected;
    if (igMenuItem_Bool("Duplicate", "Ctrl+D", false, selected != ENTITY_NULL))
        duplicate_selection(state);
    if (igMenuItem_Bool("Delete", "Del", false, selected != ENTITY_NULL))
        delete_selection(state);
    if (igMenuItem_Bool("Unparent", NULL, false, selected && world_parent(w, selected)))
        reparent(state, selected, ENTITY_NULL);
    if (igMenuItem_Bool("Select Parent", NULL, false, selected && world_parent(w, selected)))
        select_entity(state, world_parent(w, selected));
    if (igMenuItem_Bool("Deselect", "Esc", false, selected != ENTITY_NULL))
        state->selected = ENTITY_NULL;
    igSeparator();

    if (igMenuItem_Bool("Move", "W", state->gizmo_op == GIZMO_TRANSLATE, true)) state->gizmo_op = GIZMO_TRANSLATE;
    if (igMenuItem_Bool("Rotate", "E", state->gizmo_op == GIZMO_ROTATE, true))  state->gizmo_op = GIZMO_ROTATE;
    if (igMenuItem_Bool("Scale", "R", state->gizmo_op == GIZMO_SCALE, true))    state->gizmo_op = GIZMO_SCALE;
    igMenuItem_BoolPtr("Snap", NULL, &state->gizmo_snap, true);
    if (igMenuItem_Bool("Local axes", NULL, state->gizmo_space == GIZMO_LOCAL, true))
        state->gizmo_space = state->gizmo_space == GIZMO_LOCAL ? GIZMO_WORLD : GIZMO_LOCAL;
}

static void
draw_add_menu(editor_state* state)
{
    if (igBeginMenu("Model", true))
    {
        cJSON*       list   = run_command(state, "assets.list", NULL);
        const cJSON* models = cJSON_GetObjectItem(list, "models");
        const cJSON* key;
        cJSON_ArrayForEach(key, models)
        {
            if (cJSON_IsString(key) && igMenuItem_Bool(key->valuestring, NULL, false, true))
                add_model(state, key->valuestring);
        }
        if (cJSON_GetArraySize(models) == 0)
            igTextDisabled("(none yet -- File > Import Asset)");
        cJSON_Delete(list);
        igEndMenu();
    }
    if (igMenuItem_Bool("Cube", NULL, false, true))   add_model(state, "builtin:cube");
    if (igMenuItem_Bool("Sphere", NULL, false, true)) add_model(state, "builtin:sphere");
    if (igMenuItem_Bool("Plane", NULL, false, true))  add_model(state, "builtin:plane");
    igSeparator();
    if (igMenuItem_Bool("Point Light", NULL, false, true)) add_light(state, "point");
    if (igMenuItem_Bool("Spot Light", NULL, false, true))  add_light(state, "spot");
    if (igMenuItem_Bool("Sun", NULL, false, true))         add_light(state, "directional");
    igSeparator();
    if (igMenuItem_Bool("Camera (this view)", NULL, false, true)) add_camera(state);
    if (igMenuItem_Bool("Reflection Probe", NULL, false, true))   add_reflection_probe(state);
    if (igMenuItem_Bool("Scene Trigger", NULL, false, true))      add_scene_trigger(state);
    if (igMenuItem_Bool("Empty", NULL, false, true))              add_empty(state);
    igSeparator();
    igTextDisabled("At the view's centre, %.2f m (Scene panel)", (double)state->add_size);
}

static void
draw_view_menu(editor_state* state)
{
    panel_visibility* show = &state->show;
    igMenuItem_BoolPtr(WINDOW_SCENE, NULL, &show->scene, true);
    igMenuItem_BoolPtr(WINDOW_ENTITIES, NULL, &show->entities, true);
    igMenuItem_BoolPtr(WINDOW_INSPECTOR, NULL, &show->inspector, true);
    igMenuItem_BoolPtr(WINDOW_RENDERER, NULL, &show->renderer, true);
    igMenuItem_BoolPtr(WINDOW_ASSETS, NULL, &show->assets, true);
    igMenuItem_BoolPtr(WINDOW_HISTORY, NULL, &show->history, true);
    igMenuItem_BoolPtr("Console", "F1", &show->console, true);
    igMenuItem_BoolPtr(WINDOW_CHAT, NULL, &show->chat, true);
    igMenuItem_BoolPtr(WINDOW_LOG, NULL, &show->log, true);
    igMenuItem_BoolPtr(WINDOW_TIMINGS, NULL, &show->gpu_timings, true);
    igSeparator();

    if (igBeginMenu("Camera", true))
    {
        world* w = active_world(state);
        if (igMenuItem_Bool("Editor camera", NULL, state->view_cam_entity == ENTITY_NULL, true))
            use_editor_camera(state);
        const uint32 mask = COMPONENT_BIT(COMPONENT_CAMERA);
        for (entity_id e = world_next(w, ENTITY_NULL, mask); e; e = world_next(w, e, mask))
        {
            if (igMenuItem_Bool(world_name(w, e), NULL, state->view_cam_entity == e, true))
            {
                state->look_through     = e;
                state->play_game_camera = false;
            }
        }
        igSeparator();
        igMenuItem_BoolPtr("Play through the game camera", NULL, &state->play_game_camera, true);
        igEndMenu();
    }
    if (igBeginMenu("Overlays", true))
    {
        igMenuItem_BoolPtr("Lights", NULL, &state->overlays.lights, true);
        igMenuItem_BoolPtr("Cameras", NULL, &state->overlays.cameras, true);
        igMenuItem_BoolPtr("Reflection probes", NULL, &state->overlays.probes, true);
        igMenuItem_BoolPtr("Scene triggers", NULL, &state->overlays.triggers, true);
        igMenuItem_BoolPtr("Ground grid", NULL, &state->overlays.grid, true);
        igEndMenu();
    }
    if (igBeginMenu("Anti-aliasing", true))
    {
        static const char* const labels[SCENE_AA_COUNT] = { "Off", "FXAA", "TAA" };
        for (int a = 0; a < SCENE_AA_COUNT; ++a)
            if (igMenuItem_Bool(labels[a], NULL, state->antialiasing == (scene_antialiasing)a, true))
                state->antialiasing = (scene_antialiasing)a;
        igEndMenu();
    }
    if (igBeginMenu("Debug view", true))
    {
        for (int v = 0; v < SCENE_VIEW_COUNT; ++v)
            if (igMenuItem_Bool(DEBUG_VIEW_LABELS[v], NULL, state->debug_view == (scene_debug_view)v, true))
                state->debug_view = (scene_debug_view)v;
        igSeparator();
        igMenuItem_BoolPtr("Shadow cascades", NULL, &state->debug_show_cascades, true);
        igMenuItem_BoolPtr("Unclustered lighting", NULL, &state->debug_unclustered, true);
        igEndMenu();
    }
    igMenuItem_BoolPtr("Shadows", NULL, &state->shadows, true);
    igSeparator();
    if (igMenuItem_Bool("Frame selection", "F", false, state->selected != ENTITY_NULL))
        frame_selection(state);
    if (igMenuItem_Bool("Frame everything", NULL, false, true))
        frame_view(state, NULL);
}

static void
draw_play_menu(editor_state* state)
{
    b8 playing = play_active(&state->play);
    if (igMenuItem_Bool(playing ? "Stop" : "Play", "Ctrl+P", false, playing || !chat_busy(&state->chat)))
    {
        if (playing)
            play_stop(state);
        else
            play_start(state);
    }
    if (igMenuItem_Bool(state->play.mode == PLAY_PAUSED ? "Resume" : "Pause", "Ctrl+Shift+P", false, playing))
        play_toggle_pause(&state->play);
    if (igMenuItem_Bool("Step", "F10", false, state->play.mode == PLAY_PAUSED))
        state->play.step = true;
    igSeparator();
    igMenuItem_BoolPtr("Through the game camera", NULL, &state->play_game_camera, true);
}

void
draw_main_menu(editor_state* state)
{
    if (!igBeginMainMenuBar())
        return;

    if (igBeginMenu("File", true))
    {
        draw_file_menu(state);
        igEndMenu();
    }
    if (igBeginMenu("Edit", true))
    {
        draw_edit_menu(state);
        igEndMenu();
    }
    if (igBeginMenu("Add", true))
    {
        draw_add_menu(state);
        igEndMenu();
    }
    if (igBeginMenu("View", true))
    {
        draw_view_menu(state);
        igEndMenu();
    }
    if (igBeginMenu("Play", true))
    {
        draw_play_menu(state);
        igEndMenu();
    }
    if (igBeginMenu("Help", true))
    {
        if (igMenuItem_Bool("Keyboard shortcuts", NULL, false, true))
            state->open_popup = POPUP_SHORTCUTS;
        if (igMenuItem_Bool("Commands (in the console)", NULL, false, true))
        {
            state->show.console = true;
            console_info(&state->console, "{\"cmd\": \"commands.list\"} lists every command; "
                                          "{\"cmd\": \"components.describe\"} every component field.");
        }
        igEndMenu();
    }

    igEndMainMenuBar();
}

// --- Status bar --------------------------------------------------------------------------

// Along the main window's bottom: the scene, Play, the selection, the
// command server and agent, and the frame rate.
void
draw_status_bar(editor_state* state)
{
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (!igBeginViewportSideBar("##status", igGetMainViewport(), ImGuiDir_Down, igGetFrameHeight(), flags))
    {
        igEnd();
        return;
    }
    if (igBeginMenuBar())
    {
        igText("%s / %s%s", state->project.name, state->doc.name[0] ? state->doc.name : "untitled",
               doc_dirty(&state->doc) ? "*" : "");
        igSameLine(0.0f, 20.0f);

        if (play_active(&state->play))
            igTextColored(state->play.mode == PLAY_PAUSED ? COLOR_WARNING : COLOR_OK, "%s %.1f s",
                          state->play.mode == PLAY_PAUSED ? "Paused" : "Playing", (double)state->play.time);
        else
            igTextDisabled("Editing");
        igSameLine(0.0f, 20.0f);

        world* w = active_world(state);
        igTextDisabled("%u entities", world_count(w));
        if (world_is_alive(w, state->selected))
        {
            igSameLine(0.0f, 20.0f);
            igText("Selected: %s", world_name(w, state->selected));
        }
        igSameLine(0.0f, 20.0f);

        if (state->server)
            igTextDisabled("Server :%u, %u client(s)", (unsigned)server_port(state->server), server_client_count(state->server));
        else
            igTextColored(COLOR_WARNING, "No command server");
        igSameLine(0.0f, 20.0f);
        if (chat_busy(&state->chat))
        {
            igTextColored(COLOR_OK, "Agent working...");
            igSameLine(0.0f, 20.0f);
        }

        igTextDisabled("%.1f ms (%.0f fps)", (double)state->cpu_frame_ms,
                       state->cpu_frame_ms > 0.0f ? 1000.0 / (double)state->cpu_frame_ms : 0.0);
        igEndMenuBar();
    }
    igEnd();
}

// --- Dialogs ---------------------------------------------------------------------------

// The open dialog's error line, if any.
static void
draw_popup_error(editor_state* state)
{
    if (state->popup_error[0])
    {
        igPushTextWrapPos(480.0f);
        igTextColored(COLOR_ERROR, "%s", state->popup_error);
        igPopTextWrapPos();
    }
}

static b8
dialog_button(const char* label)
{
    return igButton(label, (ImVec2_c){ 120.0f, 0.0f });
}

static void
draw_unsaved_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_UNSAVED, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Save the changes to %s first?", state->doc.name[0] ? state->doc.name : "this untitled scene");
    igSpacing();

    if (dialog_button("Save"))
    {
        igCloseCurrentPopup();
        if (state->doc.name[0])
        {
            if (save_scene_as(state, state->doc.name))
                perform(state, state->pending, state->pending_arg);
            state->pending = ACTION_NONE;
        }
        else
        {
            // No file yet: ask for a name, then carry on.
            state->save_as_name[0]        = '\0';
            state->popup_error[0]         = '\0';
            state->continue_after_save_as = true;
            state->open_popup             = POPUP_SAVE_AS;
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Don't save"))
    {
        igCloseCurrentPopup();
        perform(state, state->pending, state->pending_arg);
        state->pending = ACTION_NONE;
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
    {
        igCloseCurrentPopup();
        state->pending = ACTION_NONE;
    }
    igEndPopup();
}

static void
draw_save_as_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_SAVE_AS, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Scene name, saved as scenes/<name>.json in %s", state->project.name);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##scene_name", state->save_as_name, sizeof(state->save_as_name),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

    char relative[PROJECT_SCENE_MAX + 16];
    snprintf(relative, sizeof(relative), "scenes/%s.json", state->save_as_name);
    char* full = project_path(relative);
    if (state->save_as_name[0] && strcmp(state->save_as_name, state->doc.name) != 0 && file_exists(full))
        igTextDisabled("Replaces the existing %s.", state->save_as_name);
    free(full);

    draw_popup_error(state);

    if (dialog_button("Save") || enter)
    {
        if (save_scene_as(state, state->save_as_name))
        {
            igCloseCurrentPopup();
            if (state->continue_after_save_as)
                perform(state, state->pending, state->pending_arg);
            state->continue_after_save_as = false;
            state->pending                = ACTION_NONE;
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
    {
        igCloseCurrentPopup();
        state->continue_after_save_as = false;
        state->pending                = ACTION_NONE;
    }
    igEndPopup();
}

static void
draw_new_project_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_NEW_PROJECT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igSetNextItemWidth(420.0f);
    igInputText("Name", state->new_project_name, sizeof(state->new_project_name), 0, NULL, NULL);
    igSetNextItemWidth(420.0f);
    igInputText("Location", state->new_project_location, sizeof(state->new_project_location), 0, NULL, NULL);

    char directory[PROJECT_PATH_MAX];
    snprintf(directory, sizeof(directory), "%s/%s", state->new_project_location, state->new_project_name);
    igTextDisabled("Creates %s", directory);
    draw_popup_error(state);

    if (dialog_button("Create"))
    {
        // Made now, then opened like any other project (asking about
        // unsaved changes first).
        if (project_create(directory, state->new_project_name, state->popup_error, sizeof(state->popup_error)))
        {
            igCloseCurrentPopup();
            request(state, ACTION_OPEN_PROJECT, directory);
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_open_project_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_OPEN_PROJECT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Project folder (the one with project.json)");
    igSetNextItemWidth(480.0f);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##project_path", state->open_project_path, sizeof(state->open_project_path),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);

    if (state->recent.count)
    {
        igTextDisabled("Recent:");
        for (uint32 i = 0; i < state->recent.count; ++i)
        {
            if (igSelectable_Bool(state->recent.paths[i], false, 0, (ImVec2_c){ 0, 0 }))
                snprintf(state->open_project_path, sizeof(state->open_project_path), "%s", state->recent.paths[i]);
        }
    }
    draw_popup_error(state);

    if (dialog_button("Open") || enter)
    {
        project_settings check;
        if (project_read(state->open_project_path, &check, state->popup_error, sizeof(state->popup_error)))
        {
            igCloseCurrentPopup();
            char directory[PROJECT_PATH_MAX];
            snprintf(directory, sizeof(directory), "%s", state->open_project_path);
            request(state, ACTION_OPEN_PROJECT, directory);
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_import_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_IMPORT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("File to copy into %s: .glb, .gltf, .obj or .hdr", state->project.name);
    igSetNextItemWidth(480.0f);
    if (igIsWindowAppearing())
        igSetKeyboardFocusHere(0);
    b8 enter = igInputText("##import_path", state->import_path, sizeof(state->import_path),
                           ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igTextDisabled(".gltf and .obj bring the files they name (buffers, .mtl, textures) along.");
    draw_popup_error(state);

    if (dialog_button("Import") || enter)
    {
        // Pasted paths often come quoted.
        char path[PROJECT_PATH_MAX];
        snprintf(path, sizeof(path), "%s", state->import_path[0] == '"' ? state->import_path + 1 : state->import_path);
        size_t length = strlen(path);
        if (length && path[length - 1] == '"')
            path[length - 1] = '\0';

        char key[PROJECT_PATH_MAX];
        if (project_import_asset(paths_project_root(), path, key, sizeof(key), state->popup_error, sizeof(state->popup_error)))
        {
            char line[PROJECT_PATH_MAX + 64];
            snprintf(line, sizeof(line), "Imported %s (the Assets panel, Add > Model, or the HDRI list)", key);
            console_info(&state->console, line);
            igCloseCurrentPopup();
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

// --- Project settings: input ---------------------------------------------------------

// A key or stick turns a delta axis (look) at this many units a second, as the
// default look_x / look_y do.
#define CAPTURED_DELTA_RATE 150.0f

// What "Press to bind" caught this frame: a key (Esc cancels), a mouse button
// other than the left one (that one clicks the dialog), a pad button, or a
// stick or trigger pushed most of the way -- signed, so pushing a stick up
// for "move_y" binds up as +1. False while nothing has been pressed;
// *out_cancel when Esc was.
static b8
capture_binding(input_action_kind kind, input_binding* out, b8* out_cancel)
{
    *out_cancel = false;
    f32 rate    = kind == INPUT_ACTION_AXIS_DELTA ? CAPTURED_DELTA_RATE : 1.0f;

    int key = input_raw_key_pressed();
    if (key == INPUT_KEY_ESCAPE)
    {
        *out_cancel = true;
        return false;
    }
    if (key >= 0 && input_key_name((input_key)key)[0])
    {
        *out       = input_binding_key((input_key)key);
        out->scale = rate;
        return true;
    }

    int button = input_raw_mouse_pressed();
    if (button > INPUT_MOUSE_LEFT)
    {
        *out       = input_binding_mouse_button((input_mouse_button)button);
        out->scale = rate;
        return true;
    }

    for (int b = 0; b < INPUT_PAD_BUTTON_COUNT; ++b)
        if (input_pad_pressed(INPUT_PAD_ANY, (input_pad_button)b))
        {
            *out       = input_binding_pad_button((input_pad_button)b);
            out->scale = rate;
            return true;
        }

    for (int a = 0; a < INPUT_PAD_AXIS_COUNT; ++a)
    {
        f32 value = input_pad_axis_value(INPUT_PAD_ANY, (input_pad_axis)a);
        if (fabsf(value) >= 0.6f)
        {
            *out = input_binding_pad_axis((input_pad_axis)a, value < 0.0f ? -rate : rate);
            return true;
        }
    }
    return false;
}

static void
draw_input_action(editor_state* state, input_action_map* map, int index)
{
    input_action_def* a = &map->actions[index];
    char error[160];

    igPushID_Int(index);
    b8 open = igTreeNodeEx_Str(a->name, ImGuiTreeNodeFlags_SpanAvailWidth);
    igSameLine(220.0f, -1.0f);
    igSetNextItemWidth(90.0f);
    static const char* const kinds[INPUT_ACTION_KIND_COUNT] = { "button", "axis", "delta" };
    int kind = (int)a->kind;
    if (igCombo_Str_arr("##kind", &kind, kinds, INPUT_ACTION_KIND_COUNT, -1))
    {
        a->kind     = (input_action_kind)kind;
        map->custom = true;
    }
    igSameLine(0.0f, -1.0f);
    b8 remove = igSmallButton("Remove");

    if (open)
    {
        for (int i = 0; i < a->binding_count; ++i)
        {
            char text[INPUT_BINDING_TEXT_MAX];
            input_binding_to_text(&a->bindings[i], text, sizeof(text));
            igPushID_Int(i);
            if (igSmallButton("x"))
            {
                memmove(&a->bindings[i], &a->bindings[i + 1], (size_t)(a->binding_count - i - 1) * sizeof(a->bindings[0]));
                --a->binding_count;
                map->custom = true;
                --i;
            }
            igSameLine(0.0f, -1.0f);
            igTextUnformatted(text, NULL);
            igPopID();
        }

        // Typed: "key:w", "pad_axis:left_y*-1", "mouse_motion:x*0.1".
        char* typed = state->input_binding_text[index];
        igSetNextItemWidth(180.0f);
        b8 entered = igInputText("##binding", typed, INPUT_BINDING_TEXT_MAX, ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
        igSetItemTooltip("key:w  keys:a/d  mouse:right  pad:a  pad_axis:left_y*-1  mouse_motion:x*0.1");
        igSameLine(0.0f, -1.0f);
        if ((igSmallButton("Add") || entered) && typed[0])
        {
            input_binding binding;
            char          reason[160];
            if (!input_binding_from_text(typed, &binding, reason, sizeof(reason))
                || !input_action_map_bind(map, index, binding, reason, sizeof(reason)))
                snprintf(state->popup_error, sizeof(state->popup_error), "%s", reason);
            else
            {
                typed[0]              = '\0';
                state->popup_error[0] = '\0';
                map->custom           = true;
            }
        }
        igSameLine(0.0f, -1.0f);

        if (state->input_capture == index)
        {
            igTextColored((ImVec4_c){ 1.0f, 0.8f, 0.3f, 1.0f }, "Press a key, button or stick (Esc: cancel)");

            input_binding binding;
            b8            cancel;
            if (capture_binding(a->kind, &binding, &cancel))
            {
                if (input_action_map_bind(map, index, binding, error, sizeof(error)))
                {
                    map->custom           = true;
                    state->popup_error[0] = '\0';
                }
                else
                    snprintf(state->popup_error, sizeof(state->popup_error), "%s", error);
                state->input_capture = -1;
            }
            else if (cancel)
                state->input_capture = -1;
        }
        else if (igSmallButton("Press to bind"))
            state->input_capture = index;

        igTreePop();
    }
    igPopID();

    if (remove)
    {
        input_action_map_remove(map, index);
        memmove(state->input_binding_text[index], state->input_binding_text[index + 1],
                (size_t)(INPUT_ACTION_MAX - index - 1) * sizeof(state->input_binding_text[0]));
        state->input_binding_text[INPUT_ACTION_MAX - 1][0] = '\0';
        state->input_capture = -1;
        map->custom          = true;
    }
}

// The game's actions (Platform/InputActions.h): what Play and the exported
// game read. Saved into project.json with the rest of the dialog.
static void
draw_input_settings(editor_state* state, input_action_map* map)
{
    if (!igCollapsingHeader_TreeNodeFlags("Input", 0))
    {
        state->input_capture = -1;
        return;
    }

    igTextDisabled(map->custom ? "This project's own actions" : "The default actions (not stored in project.json)");
    igSameLine(0.0f, -1.0f);
    if (map->custom && igSmallButton("Reset to defaults"))
    {
        input_action_map_defaults(map);
        memset(state->input_binding_text, 0, sizeof(state->input_binding_text));
        state->input_capture = -1;
    }

    igSetNextItemWidth(300.0f);
    if (igSliderFloat("Stick dead zone", &map->stick_dead_zone, 0.0f, 0.9f, "%.2f", 0))
        map->custom = true;

    igBeginChild_Str("##actions", (ImVec2_c){ 520.0f, 260.0f }, ImGuiChildFlags_Borders, 0);
    for (int i = 0; i < map->count; ++i)
        draw_input_action(state, map, i);
    igEndChild();

    // A new action.
    igSetNextItemWidth(180.0f);
    b8 entered = igInputText("##new_action", state->input_new_action, sizeof(state->input_new_action),
                             ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL);
    igSetItemTooltip("A name: letters, digits and '_'");
    igSameLine(0.0f, -1.0f);
    igSetNextItemWidth(90.0f);
    static const char* const kinds[INPUT_ACTION_KIND_COUNT] = { "button", "axis", "delta" };
    igCombo_Str_arr("##new_kind", &state->input_new_kind, kinds, INPUT_ACTION_KIND_COUNT, -1);
    igSameLine(0.0f, -1.0f);
    if ((igButton("Add action", (ImVec2_c){ 0, 0 }) || entered) && state->input_new_action[0])
    {
        if (input_action_map_find(map, state->input_new_action) >= 0)
            snprintf(state->popup_error, sizeof(state->popup_error), "There's an action called \"%s\" already",
                     state->input_new_action);
        else if (input_action_map_add(map, state->input_new_action, (input_action_kind)state->input_new_kind) < 0)
            snprintf(state->popup_error, sizeof(state->popup_error),
                     input_action_name_valid(state->input_new_action)
                         ? "There's no room for more actions (%d at most)"
                         : "An action's name is letters, digits and '_' (at most 31)",
                     INPUT_ACTION_MAX);
        else
        {
            state->input_new_action[0] = '\0';
            state->popup_error[0]      = '\0';
            map->custom                = true;
        }
    }
}

static void
draw_settings_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_SETTINGS, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    project_settings* s = &state->settings_edit;
    igTextDisabled("%s", paths_project_root());
    igSetNextItemWidth(300.0f);
    igInputText("Name", s->name, sizeof(s->name), 0, NULL, NULL);

    // The scene the game opens with.
    igSetNextItemWidth(300.0f);
    if (igBeginCombo("Startup scene", s->startup_scene, 0))
    {
        cJSON*       list = run_command(state, "scene.list", NULL);
        const cJSON* name;
        cJSON_ArrayForEach(name, cJSON_GetObjectItem(list, "scenes"))
        {
            if (cJSON_IsString(name)
                && igSelectable_Bool(name->valuestring, strcmp(name->valuestring, s->startup_scene) == 0, 0, (ImVec2_c){ 0, 0 }))
                snprintf(s->startup_scene, sizeof(s->startup_scene), "%s", name->valuestring);
        }
        cJSON_Delete(list);
        igEndCombo();
    }

    int size[2] = { s->window_width, s->window_height };
    igSetNextItemWidth(300.0f);
    if (igInputInt2("Window size", size, 0))
    {
        s->window_width  = (uint16)(size[0] < 64 ? 64 : (size[0] > 16384 ? 16384 : size[0]));
        s->window_height = (uint16)(size[1] < 64 ? 64 : (size[1] > 16384 ? 16384 : size[1]));
    }

    // The HDRI new scenes start with.
    igSetNextItemWidth(300.0f);
    if (igBeginCombo("Default HDRI", s->default_hdri[0] ? s->default_hdri : "(none)", 0))
    {
        hdri_list hdris;
        list_hdris(&hdris);
        if (igSelectable_Bool("(none)", !s->default_hdri[0], 0, (ImVec2_c){ 0, 0 }))
            s->default_hdri[0] = '\0';
        for (uint32 i = 0; i < hdris.count; ++i)
        {
            if (igSelectable_Bool(hdris.paths[i], strcmp(hdris.paths[i], s->default_hdri) == 0, 0, (ImVec2_c){ 0, 0 }))
                snprintf(s->default_hdri, sizeof(s->default_hdri), "%s", hdris.paths[i]);
        }
        igEndCombo();
    }

    draw_input_settings(state, &s->input);
    draw_popup_error(state);

    if (dialog_button("Save"))
    {
        if (project_write(paths_project_root(), s, state->popup_error, sizeof(state->popup_error)))
        {
            state->project = *s;
            input_actions_load(&state->project.input);
            igCloseCurrentPopup();
        }
    }
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Cancel"))
        igCloseCurrentPopup();
    igEndPopup();
}

static void
draw_export_popup(editor_state* state)
{
    if (!igBeginPopupModal(POPUP_EXPORT, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    igText("Export %s as a game a player can run", state->project.name);
    igSetNextItemWidth(480.0f);
    igInputText("Folder", state->export_dir, sizeof(state->export_dir), 0, NULL, NULL);
    igTextDisabled("A new or empty folder, or an earlier export (replaced).");

    // What it ships: a Release game runs anywhere; a Debug one needs Visual
    // Studio's debug runtime.
    b8 release, stale;
    game_executable(&release, &stale);
    if (release && stale)
    {
        igPushTextWrapPos(480.0f);
        igTextColored(COLOR_WARNING, "Game: Release build, but older than this build of the engine, so it may miss "
                                     "recent changes. Rebuild it: cmake --build build --config Release --target Game");
        igPopTextWrapPos();
    }
    else if (release)
        igTextDisabled("Game: Release build, with the C++ runtime DLLs.");
    else
    {
        igPushTextWrapPos(480.0f);
        igTextColored(COLOR_WARNING, "Game: Debug build. It runs only on PCs with Visual Studio. For players, build "
                                     "the Release game first: cmake --build build --config Release --target Game");
        igPopTextWrapPos();
    }

    // Only saved scenes go out.
    if (doc_dirty(&state->doc))
    {
        igTextColored(COLOR_WARNING, "%s has unsaved changes, which won't be exported.",
                      state->doc.name[0] ? state->doc.name : "This scene");
        igSameLine(0.0f, -1.0f);
        if (igSmallButton("Save now"))
            save_scene(state);
    }

    igSeparator();
    igBeginDisabled(state->check_process != NULL);
    if (dialog_button("Export"))
        run_export(state, state->export_dir);
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    igBeginDisabled(!state->export_ok);
    if (dialog_button("Open folder"))
        open_folder(state->export_dir);
    igEndDisabled();
    igSameLine(0.0f, -1.0f);
    if (dialog_button("Close"))
        igCloseCurrentPopup();

    if (state->export_status[0])
    {
        igPushTextWrapPos(480.0f);
        if (state->export_ok)
            igTextWrapped("%s", state->export_status);
        else
            igTextColored(COLOR_ERROR, "%s", state->export_status);
        igPopTextWrapPos();
    }

    // The game's own check, and what it rendered.
    if (state->export_ok)
    {
        if (state->check == CHECK_RUNNING)
            igTextDisabled("Checking: the exported game is loading its startup scene...");
        else if (state->check == CHECK_PASSED)
            igTextColored(COLOR_OK, "Check passed: the game loads and renders %s.", state->project.startup_scene);
        else if (state->check == CHECK_FAILED)
            igTextColored(COLOR_ERROR, "Check FAILED (exit code %d): see log.txt in the export folder "
                                       "(~/.pbr-renderer/export_check.log for a Debug game).", state->check_exit_code);

        if (state->check_picture)
        {
            ImTextureRef_c picture = { NULL, (ImTextureID)rhi_texture_native_handle(state->check_picture) };
            igImage(picture, (ImVec2_c){ 480.0f, 270.0f }, (ImVec2_c){ 0.0f, 1.0f }, (ImVec2_c){ 1.0f, 0.0f });
        }
    }
    igEndPopup();
}

static void
draw_shortcuts_popup(void)
{
    if (!igBeginPopupModal(POPUP_SHORTCUTS, NULL, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    static const char* const SHORTCUTS[][2] = {
        { "Left drag (view)",   "Orbit the camera" },
        { "Mouse wheel (view)", "Zoom" },
        { "Click (view)",       "Select; empty space deselects" },
        { "W / E / R",          "Move / rotate / scale gizmo" },
        { "Ctrl (dragging)",    "Flip snapping" },
        { "F",                  "Frame the selection" },
        { "Esc",                "Deselect, or quit" },
        { "Delete",             "Delete the selection" },
        { "Ctrl+D",             "Duplicate the selection" },
        { "Ctrl+Z / Ctrl+Y",    "Undo / redo" },
        { "Ctrl+S",             "Save the scene" },
        { "Ctrl+P",             "Play / stop" },
        { "Ctrl+Shift+P / F10", "Pause or resume / one step" },
        { "F1",                 "Command console" },
        { "Drag in Entities",   "Parent onto another entity" },
    };
    if (igBeginTable("##shortcuts", 2, ImGuiTableFlags_RowBg, (ImVec2_c){ 0, 0 }, 0.0f))
    {
        for (size_t i = 0; i < sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]); ++i)
        {
            igTableNextRow(0, 0.0f);
            igTableSetColumnIndex(0);
            igTextUnformatted(SHORTCUTS[i][0], NULL);
            igTableSetColumnIndex(1);
            igTextUnformatted(SHORTCUTS[i][1], NULL);
        }
        igEndTable();
    }
    if (dialog_button("Close"))
        igCloseCurrentPopup();
    igEndPopup();
}

void
draw_popups(editor_state* state)
{
    if (state->open_popup)
    {
        igOpenPopup_Str(state->open_popup, 0);
        state->open_popup = NULL;
    }

    draw_unsaved_popup(state);
    draw_save_as_popup(state);
    draw_new_project_popup(state);
    draw_open_project_popup(state);
    draw_import_popup(state);
    draw_settings_popup(state);
    draw_export_popup(state);
    draw_shortcuts_popup();
}
