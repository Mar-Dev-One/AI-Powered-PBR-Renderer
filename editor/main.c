// editor/main.c
//
// The editor: opens a project (Project/Project.h) and edits one of its
// scenes at a time, with the outliner and inspector, the gizmo, the command
// console, the command server other processes (MCP server, AI agent,
// scripts) drive it through, and the in-app chat. The game runtime -- the
// same engine with none of this -- is game/main.c. Editor.h says which file
// holds which part.
//
// Projects: File > New Project makes a folder with project.json, scenes/,
// models/ and hdri/ (and a starter scene); File > Open Project / Recent
// switches to another. The open project is the project root (Core/Paths.h):
// every "models/..." and "hdri/..." in a scene, every scene.save / scene.load
// / assets.list -- the agent's too -- works inside it. Without --project the
// editor opens the sample project, the source tree's assets/.
//
// Scenes: File > New Scene / Open Scene / Save (Ctrl+S) / Save As. The
// title bar shows "*" while the scene has changes that aren't saved; opening
// something else, or quitting, asks first. Opening a scene starts a fresh
// undo history.
//
// Play (the Viewport toolbar) runs a copy of the scene in place; Export
// makes the game a player runs (game/main.c, Project/Project.h).

#include "Editor.h"

#include "Core/Paths.h"
#include "Platform/Window.h"
#include "Renderer/Renderer.h"
#include "UI/ImGuiLayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

scene_render_settings
build_render_settings(const editor_state* state)
{
    scene_render_settings settings = scene_render_settings_default();

    scene_render_settings_apply_environment(&settings, &state->doc.env, ibl_cache_maps(&state->ibl));
    settings.shadows              = state->shadows;
    settings.cascade_split_lambda = state->cascade_split_lambda;
    settings.debug_show_cascades  = state->debug_show_cascades;
    settings.debug_view           = state->debug_view;
    settings.debug_unclustered    = state->debug_unclustered;
    settings.antialiasing         = state->antialiasing;

    // Where nothing (not even the skybox) is drawn -- a scene without an
    // HDRI: a neutral dark grey, so an empty background never reads as an
    // error.
    glm_vec3_copy((vec3){ 0.045f, 0.05f, 0.06f }, settings.clear_color);

    return settings;
}

// The panels' first-run arrangement, when the dockspace has never been
// split (no imgui.ini yet): Scene and Renderer on the left, Entities over the
// Inspector on the right, Console, Chat, Assets and History along the
// bottom, the Viewport in the middle. After that, ImGui remembers whatever
// the user makes of it.
static void
default_layout(ImGuiID dockspace)
{
    const ImGuiDockNode* node = igDockBuilderGetNode(dockspace);
    if (node && (node->ChildNodes[0] || node->Windows.Size > 0))
        return;

    igDockBuilderRemoveNode(dockspace);
    igDockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    igDockBuilderSetNodeSize(dockspace, igGetMainViewport()->WorkSize);

    ImGuiID center = dockspace, left, right, right_bottom, bottom;
    left   = igDockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, NULL, &center);
    right  = igDockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, NULL, &center);
    bottom = igDockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, NULL, &center);
    right  = igDockBuilderSplitNode(right, ImGuiDir_Up, 0.40f, NULL, &right_bottom);

    igDockBuilderDockWindow(WINDOW_SCENE, left);
    igDockBuilderDockWindow(WINDOW_RENDERER, left);
    igDockBuilderDockWindow(WINDOW_ENTITIES, right);
    igDockBuilderDockWindow(WINDOW_INSPECTOR, right_bottom);
    igDockBuilderDockWindow(WINDOW_CONSOLE, bottom);
    igDockBuilderDockWindow(WINDOW_CHAT, bottom);
    igDockBuilderDockWindow(WINDOW_ASSETS, bottom);
    igDockBuilderDockWindow(WINDOW_HISTORY, bottom);
    igDockBuilderDockWindow(WINDOW_LOG, bottom);
    igDockBuilderDockWindow(WINDOW_VIEWPORT, center);
    igDockBuilderFinish(dockspace);
}

// A window that has never been shown -- one added to the editor after the
// user's imgui.ini was written -- docks as a tab beside `host` the first
// time, instead of floating over the layout. (In a fresh layout,
// default_layout() has placed it already, and this does nothing.)
static void
dock_beside(const char* host)
{
    ImGuiWindow* window = igFindWindowByName(host);
    if (window && window->DockId)
        igSetNextWindowDockID(window->DockId, ImGuiCond_FirstUseEver);
}

// --- App hooks -------------------------------------------------------------------------

// Runs once from App.c's run(), after the GL context / RHI / ImGui exist.
static void
on_init(App* app)
{
    editor_state* state = (editor_state*)app->user_data;
    state->app = app;
    window_size framebuffer_size = window_get_size(get_renderer()->drawing_window);

    if (!scene_renderer_create(&state->renderer, framebuffer_size.width, framebuffer_size.height,
                               CASCADE_COUNT, SHADOW_MAP_SIZE))
        FATAL("editor: failed to create the scene renderer");

    assets_init(&state->assets);

    console_init(&state->console);
    inspector_init(&state->inspector);

    scene_render_settings defaults = scene_render_settings_default();
    state->shadows              = defaults.shadows;
    state->cascade_split_lambda = defaults.cascade_split_lambda;
    state->antialiasing         = SCENE_AA_FXAA;
    state->overlays             = (overlay_settings){ .grid = false, .lights = true, .cameras = true, .probes = true };
    state->show                 = (panel_visibility){ .scene = true, .entities = true, .inspector = true,
                                                      .renderer = true, .assets = true, .history = true,
                                                      .console = true, .chat = true };

    state->gizmo_op    = GIZMO_TRANSLATE;
    state->gizmo_space = GIZMO_WORLD;
    state->snap_move   = 0.25f;
    state->snap_rotate = 15.0f;
    state->snap_scale  = 0.1f;
    state->add_size    = 1.0f;

    state->server = server_start(NULL);
    chat_init(&state->chat, state->server);
    chat_panel_init(&state->chat_panel);

    uint32                  host_command_count, chat_command_count;
    const cmd_host_command* host_command_table = editor_host_commands(&host_command_count);
    const cmd_host_command* chat_command_table = chat_commands(&chat_command_count);

    doc_init(&state->doc, &state->assets, (command_context){
        .on_change            = on_world_change,
        .on_scene_file        = on_scene_file,
        .on_change_user       = state,
        .host_commands        = host_command_table,
        .host_command_count   = host_command_count,
        .host_user            = state,
        .module_commands      = chat_command_table,
        .module_command_count = chat_command_count,
        .module_user          = &state->chat,
    });

    if (state->server)
    {
        char line[256];
        snprintf(line, sizeof(line), "Command server: %s (also in ~/.pbr-renderer/server.json)", server_url(state->server));
        console_info(&state->console, line);
    }
    else
    {
        console_info(&state->console, "Command server could not start; only this console can run commands.");
    }

#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    snprintf(state->agent_log, sizeof(state->agent_log), "%s/.pbr-renderer/agent.log", home ? home : ".");

    // The chat agent connects through the server, so without one there's no agent.
    const char* autostart = getenv("PBR_AGENT_AUTOSTART");
    if (!state->server)
        snprintf(state->agent_note, sizeof(state->agent_note), "The command server didn't start, so no agent can connect.");
    else if (autostart && strcmp(autostart, "0") == 0)
        snprintf(state->agent_note, sizeof(state->agent_note), "Not started automatically (PBR_AGENT_AUTOSTART=0).");
    else
        start_agent(state);

    f32 aspect = (f32)framebuffer_size.width / (f32)framebuffer_size.height;
    camera_init(&state->cam, (vec3){ 4.0f, 3.0f, 6.0f }, (vec3){ 0.0f, 0.0f, 0.0f }, 45.0f, aspect, 0.1f, 500.0f);
    orbit_from_camera(&state->orbit, &state->cam);
    state->view_cam = state->cam;

    // The project main() checked, and its startup scene.
    char error[640];
    if (!project_read(paths_project_root(), &state->project, error, sizeof(error)))
        FATAL("editor: %s", error);
    project_recent_load(&state->recent, NULL);
    project_recent_add(&state->recent, paths_project_root(), NULL);

    char line[PROJECT_PATH_MAX * 2 + PROJECT_NAME_MAX + 64];
    snprintf(line, sizeof(line), "Project %s: %s (engine data: %s)", state->project.name, paths_project_root(),
             paths_engine_root());
    console_info(&state->console, line);

    if (!open_scene(state, state->project.startup_scene))
        new_scene(state);
}

// The window's close button: ask about unsaved changes first.
static b8
on_close_request(App* app)
{
    editor_state* state = (editor_state*)app->user_data;
    if (!doc_dirty(&state->doc))
        return true;
    request(state, ACTION_QUIT, NULL);
    return false;
}

// The keyboard shortcuts, read from ImGui every frame -- so they work
// whichever panel has focus (GLFW's key callback only gets keys no panel
// wants, which with keyboard navigation is almost never). Not while text is
// being typed (ImGui undoes inside a text field itself) or a menu, combo or
// dialog is open.
static void
handle_shortcuts(editor_state* state)
{
    const ImGuiIO* io = igGetIO_Nil();
    if (io->WantTextInput || igIsPopupOpen_Str(NULL, ImGuiPopupFlags_AnyPopup))
        return;

    const b8 ctrl  = io->KeyCtrl;
    const b8 shift = io->KeyShift;
#define PRESSED(key) igIsKeyPressed_Bool(key, false)
#define REPEATED(key) igIsKeyPressed_Bool(key, true)

    // Esc clears the selection first; with nothing selected it quits.
    if (PRESSED(ImGuiKey_Escape))
    {
        validate_selection(state);
        if (state->selected)
            state->selected = ENTITY_NULL;
        else
            request(state, ACTION_QUIT, NULL);
    }
    if (ctrl && PRESSED(ImGuiKey_S))
        save_scene(state);

    // Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z): undo / redo, like history.undo / history.redo.
    if (ctrl && REPEATED(ImGuiKey_Z))
    {
        if (shift)
            redo(state);
        else
            undo(state);
    }
    if (ctrl && REPEATED(ImGuiKey_Y))
        redo(state);

    if (ctrl && PRESSED(ImGuiKey_D))
        duplicate_selection(state);
    if (PRESSED(ImGuiKey_Delete) && !gizmo_is_using())
        delete_selection(state);
    if (!ctrl && PRESSED(ImGuiKey_F))
        frame_selection(state);

    // W / E / R: the gizmo's move / rotate / scale (not mid-drag).
    if (!ctrl && !gizmo_is_using())
    {
        if (PRESSED(ImGuiKey_W)) state->gizmo_op = GIZMO_TRANSLATE;
        if (PRESSED(ImGuiKey_E)) state->gizmo_op = GIZMO_ROTATE;
        if (PRESSED(ImGuiKey_R)) state->gizmo_op = GIZMO_SCALE;
    }
    if (PRESSED(ImGuiKey_F1))
        state->show.console = !state->show.console;

    // Ctrl+P: Play / Stop; Ctrl+Shift+P: pause / resume; F10: one step while paused.
    if (ctrl && PRESSED(ImGuiKey_P))
    {
        if (shift)
            play_toggle_pause(&state->play);
        else if (!play_active(&state->play))
            play_start(state);
        else
            play_stop(state);
    }
    if (REPEATED(ImGuiKey_F10) && state->play.mode == PLAY_PAUSED)
        state->play.step = true;
#undef PRESSED
#undef REPEATED
}

// The engine's own panels, and what the outliner asked for.
static void
draw_engine_panels(editor_state* state)
{
    validate_selection(state);   // commands this frame may have removed it
    command_context* active = active_commands(state);

    if (state->show.entities)
    {
        entity_panel_action action = entity_panel_draw(&state->entities, active->w, WINDOW_ENTITIES, &state->selected,
                                                       state->reveal_selection);
        switch (action.request)
        {
            case ENTITY_PANEL_REPARENT:  reparent(state, action.entity, action.parent); break;
            case ENTITY_PANEL_DUPLICATE: select_entity(state, action.entity); duplicate_selection(state); break;
            case ENTITY_PANEL_DELETE:    select_entity(state, action.entity); delete_selection(state); break;
            case ENTITY_PANEL_FRAME:     select_entity(state, action.entity); frame_selection(state); break;
            case ENTITY_PANEL_NONE:      break;
        }
    }
    if (state->show.inspector)
        inspector_draw(&state->inspector, active, &state->selected, WINDOW_INSPECTOR);
    state->reveal_selection = false;

    if (state->show.console)
        console_draw(&state->console, active, WINDOW_CONSOLE);

    // An error (a shader that won't compile on hot reload, a model that
    // won't load) brings the log up.
    if (log_panel_take_new_errors(&state->log))
        state->show.log = true;
    if (state->show.log)
    {
        dock_beside(WINDOW_CONSOLE);
        log_panel_draw(&state->log, WINDOW_LOG, &state->show.log);
    }

    if (state->show.chat
        && chat_panel_draw(&state->chat_panel, &state->chat, active, WINDOW_CHAT, state->agent_note) == CHAT_PANEL_START_AGENT
        && state->server)
        start_agent(state);
}

static void
on_frame(App* app, f32 dt)
{
    editor_state* state = (editor_state*)app->user_data;

    // Smoothed like the GPU timings (GpuProfiler.c), to compare with them.
    f32 frame_ms        = dt * 1000.0f;
    state->cpu_frame_ms = state->cpu_frame_ms > 0.0f ? state->cpu_frame_ms + (frame_ms - state->cpu_frame_ms) * 0.1f : frame_ms;

    validate_selection(state);

    // Picks up edits to the engine's shaders saved while the app runs.
    scene_renderer_poll_hot_reload(&state->renderer);
    ibl_cache_poll_hot_reload(&state->ibl);   // re-bakes on the sync below

    // Requests from other processes -- before rendering, so their changes
    // show this same frame.
    server_poll(state->server, active_commands(state));
    chat_poll(&state->chat);
    doc_follow_history(&state->doc);   // an agent's or the console's undo may have gone back past a load
    watch_agent(state);
    watch_export_check(state);

    gizmo_begin_frame();
    ibl_cache_sync(&state->ibl, state->doc.env.hdri);

    // App/on_frame runs between imgui_layer_new_frame() and
    // imgui_layer_render() (see App.c), so any ig*() calls here just work.
    // The menu and status bars first, so the dockspace fits between them.
    // The scene lives in the Viewport panel, so the dockspace draws its own
    // background where no panel is.
    handle_shortcuts(state);
    draw_main_menu(state);
    draw_status_bar(state);
    ImGuiID dockspace = igDockSpaceOverViewport(0, NULL, 0, NULL);
    default_layout(dockspace);
    draw_viewport(state, dockspace, dt);

    if (state->show.scene)
        draw_scene_panel(state);
    if (state->show.renderer)
    {
        dock_beside(WINDOW_SCENE);
        draw_renderer_panel(state);
    }
    if (state->show.assets)
    {
        dock_beside(WINDOW_CONSOLE);
        draw_assets_panel(state);
    }
    if (state->show.history)
    {
        dock_beside(WINDOW_CONSOLE);
        draw_history_panel(state);
    }
    if (state->show.gpu_timings)
        draw_gpu_timings(state);
    draw_engine_panels(state);

    draw_popups(state);
    update_title(state);
}

// Runs from terminate(), before the GL context goes away.
static void
on_shutdown(App* app)
{
    editor_state* state = (editor_state*)app->user_data;

    play_stop(state);                        // the copy holds asset references too
    chat_shutdown(&state->chat);             // closes a running turn's transaction
    server_stop(state->server);              // the agent (--exit-with-app) leaves when its connection closes...
    process_destroy(state->agent_process);   // ...or is ended here
    finish_edits(state);                     // an edit still open becomes its own step
    inspector_shutdown(&state->inspector);
    console_shutdown(&state->console);
    doc_shutdown(&state->doc);               // releases the scene's assets...
    assets_shutdown(&state->assets);         // ...then the models themselves
    ibl_cache_destroy(&state->ibl);
    if (state->view_target)
        rhi_framebuffer_destroy(state->view_target);
    reset_export(state);                     // ends a running check, frees its picture
    scene_renderer_destroy(&state->renderer);
}

int
main(int argc, char** argv)
{
    // --project <dir>: the project to open (a folder with project.json).
    // Without it, the sample project: the source tree's assets/.
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--project") == 0 && i + 1 < argc)
        {
            if (!paths_set_project_root(argv[++i]))
            {
                fprintf(stderr, "--project: %s is not a folder\n", argv[i]);
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "usage: Editor [--project <dir>]\n");
            return 1;
        }
    }

    project_settings check;
    char             error[640];
    if (!project_read(paths_project_root(), &check, error, sizeof(error)))
    {
        fprintf(stderr, "%s\nPass --project with a folder that has a project.json.\n", error);
        return 1;
    }

    // GPU resources (renderer, IBL, models) can only be created after RHI
    // is initialized, which happens inside run() -- on_init creates them,
    // on_shutdown destroys them. Static: the state holds large buffers.
    static editor_state state;

    // Before anything else logs, so the Log window has the whole session.
    log_panel_init(&state.log);

    App app = {
        .name             = "PBR Editor",
        // Maximized: a fixed 1920 x 1080 window on a 1920 x 1080 screen ran
        // under the taskbar, hiding the bottom of every docked panel -- where
        // the Console's and the Chat's input boxes are. The size is what
        // un-maximizing restores to.
        .maximized        = true,
        .width            = 1600,
        .height           = 900,
        .on_init          = on_init,
        .on_frame         = on_frame,
        .on_shutdown      = on_shutdown,
        .on_close_request = on_close_request,
        .user_data        = &state,
        .should_close     = false,
        .ui               = imgui_layer()
    };

    run(&app);
    terminate(&app);
    log_panel_shutdown(&state.log);

    return 0;
}
