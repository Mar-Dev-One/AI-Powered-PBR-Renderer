#pragma once

#include "../Core/Defines.h"
#include "../RHI/RHI.h"

// How long each pass of a frame takes on the GPU, in milliseconds: named
// scopes between gpu_profiler_begin() / gpu_profiler_end(), timed with GPU
// timestamps (RHI.h). The GPU runs a few frames behind, so a frame's times
// are read GPU_PROFILER_LATENCY frames later, without ever waiting for the
// GPU; `timings` then holds the latest frame's scopes, in their order,
// smoothed over the last frames so the numbers can be read.
//
// SceneRenderer owns one and times its own passes (shadows, sky, scene,
// post); see scene_renderer.profiler.

#define GPU_PROFILER_MAX_SCOPES 8
#define GPU_PROFILER_LATENCY    4   // frames in flight before a frame's times are read back

typedef struct gpu_timing
{
    const char* name;   // as given to gpu_profiler_begin() (a string literal)
    f32         ms;     // smoothed
} gpu_timing;

typedef struct gpu_profiler_frame
{
    rhi_query   start[GPU_PROFILER_MAX_SCOPES];
    rhi_query   end[GPU_PROFILER_MAX_SCOPES];
    const char* names[GPU_PROFILER_MAX_SCOPES];
    uint32      count;
    b8          pending;   // has scopes not read back yet
} gpu_profiler_frame;

typedef struct gpu_profiler
{
    gpu_profiler_frame frames[GPU_PROFILER_LATENCY];
    uint32             current;   // the frame being recorded

    gpu_timing timings[GPU_PROFILER_MAX_SCOPES];
    uint32     timing_count;      // 0 until the first frame comes back
    f32        frame_ms;          // smoothed: first scope's start to last scope's end
} gpu_profiler;

// Creates the timestamp queries. False (and `p` safe to destroy) on failure.
b8 gpu_profiler_create(gpu_profiler* p);

// Reads back every frame the GPU has finished, then starts recording a new
// one. Once per frame, before its first scope.
void gpu_profiler_begin_frame(gpu_profiler* p);

// Starts timing `name` (a string literal: it's kept, not copied). Returns
// the scope for gpu_profiler_end(); past GPU_PROFILER_MAX_SCOPES in a frame,
// one that end() ignores.
uint32 gpu_profiler_begin(gpu_profiler* p, const char* name);
void   gpu_profiler_end(gpu_profiler* p, uint32 scope);

// Safe on a zeroed / failed / already-destroyed gpu_profiler.
void gpu_profiler_destroy(gpu_profiler* p);
