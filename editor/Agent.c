// editor/Agent.c
//
// The chat agent's sidecar: "python -m pbr_agent.agent --exit-with-app" with
// the Python from the agent's virtual environment (agent/README.md), or the
// one PBR_AGENT_PYTHON names. It reads its API key from the environment it
// inherits from the app, and logs to ~/.pbr-renderer/agent.log. AGENT_DIR
// (set by CMake) is agent/ in the source tree -- a development tool, not
// part of any project.

#include "Editor.h"

#include "Core/FileIO.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// "<directory>/<relative>", heap-allocated; caller frees.
static char*
join_path(const char* directory, const char* relative)
{
    size_t length = strlen(directory) + 1 + strlen(relative) + 1;
    char*  path   = malloc(length);
    if (!path)
        FATAL("editor: out of memory");
    snprintf(path, length, "%s/%s", directory, relative);
    return path;
}

void
start_agent(editor_state* state)
{
    if (state->agent_process && process_running(state->agent_process, NULL))
        return;
    process_destroy(state->agent_process);
    state->agent_process = NULL;

    char*       python;
    const char* configured = getenv("PBR_AGENT_PYTHON");
    if (configured && configured[0])
    {
        size_t length = strlen(configured) + 1;
        python = malloc(length);
        if (!python)
            FATAL("editor: out of memory");
        memcpy(python, configured, length);
    }
    else
    {
#ifdef _WIN32
        python = join_path(AGENT_DIR, ".venv/Scripts/python.exe");
#else
        python = join_path(AGENT_DIR, ".venv/bin/python");
#endif
    }

    if (!file_exists(python))
    {
        snprintf(state->agent_note, sizeof(state->agent_note),
                 "No agent environment at %s. Set it up as agent/README.md describes, or set PBR_AGENT_PYTHON.", python);
        free(python);
        return;
    }

    const char* argv[] = { python, "-m", "pbr_agent.agent", "--exit-with-app", NULL };
    state->agent_process = process_spawn(argv, state->agent_log);
    if (state->agent_process)
        snprintf(state->agent_note, sizeof(state->agent_note), "Starting the agent...");
    else
        snprintf(state->agent_note, sizeof(state->agent_note), "Could not start %s.", python);
    free(python);
}

void
watch_agent(editor_state* state)
{
    int exit_code;
    if (!state->agent_process || process_running(state->agent_process, &exit_code))
        return;

    snprintf(state->agent_note, sizeof(state->agent_note), "The agent exited (code %d); its log is %s.",
             exit_code, state->agent_log);
    process_destroy(state->agent_process);
    state->agent_process = NULL;
}
