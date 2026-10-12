#pragma once

#include "Defines.h"

// Starting and watching a child process -- the app's Python sidecars (the
// chat agent, agent/pbr_agent/agent.py). No pipes: the child's stdout and
// stderr go to a log file.

typedef struct process process;

// Starts argv[0] with the arguments argv[1..] (argv ends with NULL), its
// output appended to `log_path` (NULL = discarded). NULL (logged) if it
// couldn't be started -- e.g. argv[0] doesn't exist.
process* process_spawn(const char* const* argv, const char* log_path);

// True while it runs. Once it has exited, *out_exit_code (may be NULL) gets
// its exit code.
b8 process_running(process* p, int* out_exit_code);

// Ends it if it still runs, and frees `p`. NULL is a no-op.
void process_destroy(process* p);
