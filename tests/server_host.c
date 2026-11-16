// server_host.c
//
// A headless process running a World behind the command server, for
// server_test.py and the agent's tests: no window, no GPU. It has the chat
// commands too (Chat/ChatSession.h), so a chat agent can be tested against it. Prints the server's URL on its first
// line of output, then serves until a client calls "test.shutdown" (or 60 s
// pass, so a failed test run can't leave it behind).
//
//   ServerHost <discovery file path> [scenes folder]
//
// The built-in shapes are registered as bounds-only models (no GPU here),
// so placement commands work exactly as in the app.

#include "Chat/ChatSession.h"
#include "Commands/Commands.h"
#include "Net/CommandServer.h"

#include <stdio.h>
#include <time.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
    #include <unistd.h>
    static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
#endif

static b8 g_quit = false;

static b8
host_shutdown(void* user_data, command_context* ctx, const cJSON* args, cJSON** out_result, char* error, size_t error_size)
{
    (void)user_data; (void)ctx; (void)args; (void)out_result; (void)error; (void)error_size;
    g_quit = true;
    return true;
}

static const cmd_host_command HOST_COMMANDS[] = {
    { "test.shutdown", "Stops the test host", NULL, 0, false, host_shutdown },
};

int
main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: ServerHost <discovery file path> [scenes folder]\n");
        return 2;
    }

    asset_cache     assets;
    world           w;
    command_history history;
    assets_init(&assets);

    // Same keys and 1 m bounds as the real built-ins, but no meshes: a
    // later assets_builtin() finds these instead of uploading to a GPU.
    static const char* const BUILTINS[] = { "builtin:cube", "builtin:sphere", "builtin:plane" };
    for (int i = 0; i < 3; ++i)
    {
        model m = { 0 };
        glm_vec3_copy((vec3){ -0.5f, i == 2 ? 0.0f : -0.5f, -0.5f }, m.bounds_min);
        glm_vec3_copy((vec3){ 0.5f, i == 2 ? 0.0f : 0.5f, 0.5f }, m.bounds_max);
        assets_add_model(&assets, BUILTINS[i], m);
    }

    world_init(&w, &assets);
    history_init(&history);

    command_context ctx = {
        .w                  = &w,
        .history            = &history,
        .host_commands      = HOST_COMMANDS,
        .host_command_count = 1,
        .scenes_dir         = argc > 2 ? argv[2] : NULL,
    };

    server_config   config = { .port = 0, .discovery_path = argv[1] };
    command_server* server = server_start(&config);
    if (!server)
        return 1;

    ctx.on_change      = server_on_change;
    ctx.on_change_user = server;

    chat_session chat;
    chat_init(&chat, server);
    ctx.module_commands = chat_commands(&ctx.module_command_count);
    ctx.module_user     = &chat;

    printf("%s\n", server_url(server));
    fflush(stdout);

    time_t start = time(NULL);   // wall clock: clock() is CPU time on POSIX, and this mostly sleeps
    while (!g_quit && time(NULL) - start < 60)
    {
        server_poll(server, &ctx);
        chat_poll(&chat);
        sleep_ms(2);
    }

    // One more poll so the shutdown request gets its reply.
    server_poll(server, &ctx);
    chat_shutdown(&chat);
    server_stop(server);

    history_shutdown(&history, &w);
    world_shutdown(&w);
    assets_shutdown(&assets);
    return 0;
}
