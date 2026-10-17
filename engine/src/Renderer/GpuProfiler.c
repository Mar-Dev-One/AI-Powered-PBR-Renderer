#include "GpuProfiler.h"

#include <string.h>

#define NO_SCOPE ((uint32)-1)

// How much of each new reading goes into the shown value: about the last
// 20 frames, steady enough to read while still following a change.
#define SMOOTHING 0.1f

b8
gpu_profiler_create(gpu_profiler* p)
{
    memset(p, 0, sizeof(*p));

    for (uint32 f = 0; f < GPU_PROFILER_LATENCY; ++f)
    {
        for (uint32 s = 0; s < GPU_PROFILER_MAX_SCOPES; ++s)
        {
            p->frames[f].start[s] = rhi_timestamp_create();
            p->frames[f].end[s]   = rhi_timestamp_create();
            if (!p->frames[f].start[s] || !p->frames[f].end[s])
                return false;
        }
    }
    return true;
}

static f32
smooth(f32 previous, f32 reading, b8 had_previous)
{
    return had_previous ? previous + (reading - previous) * SMOOTHING : reading;
}

// Folds a finished frame into p->timings. False if the GPU isn't done with
// it yet.
static b8
read_frame(gpu_profiler* p, gpu_profiler_frame* frame)
{
    uint64 start[GPU_PROFILER_MAX_SCOPES];
    uint64 end[GPU_PROFILER_MAX_SCOPES];

    for (uint32 s = 0; s < frame->count; ++s)
        if (!rhi_timestamp_read(frame->start[s], &start[s]) || !rhi_timestamp_read(frame->end[s], &end[s]))
            return false;

    // This frame's scopes, in its order, each smoothed with what the same
    // name had before: a pass that stopped running (shadows turned off)
    // drops out of the list.
    gpu_timing timings[GPU_PROFILER_MAX_SCOPES];
    for (uint32 s = 0; s < frame->count; ++s)
    {
        f32 ms = end[s] > start[s] ? (f32)(end[s] - start[s]) / 1.0e6f : 0.0f;

        const gpu_timing* before = NULL;
        for (uint32 i = 0; i < p->timing_count && !before; ++i)
            if (strcmp(p->timings[i].name, frame->names[s]) == 0)
                before = &p->timings[i];

        timings[s] = (gpu_timing){ frame->names[s], smooth(before ? before->ms : 0.0f, ms, before != NULL) };
    }

    if (frame->count)
    {
        uint64 first = start[0], last = end[0];
        for (uint32 s = 1; s < frame->count; ++s)
        {
            first = start[s] < first ? start[s] : first;
            last  = end[s] > last ? end[s] : last;
        }
        p->frame_ms = smooth(p->frame_ms, (f32)(last - first) / 1.0e6f, p->timing_count > 0);
    }

    memcpy(p->timings, timings, sizeof(timings[0]) * frame->count);
    p->timing_count = frame->count;
    frame->pending  = false;
    return true;
}

void
gpu_profiler_begin_frame(gpu_profiler* p)
{
    if (!p->frames[0].start[0])
        return;   // not created

    // Oldest first, stopping at the first the GPU hasn't finished: the
    // ones after it can't be done either.
    for (uint32 i = 1; i <= GPU_PROFILER_LATENCY; ++i)
    {
        gpu_profiler_frame* frame = &p->frames[(p->current + i) % GPU_PROFILER_LATENCY];
        if (frame->pending && !read_frame(p, frame))
            break;
    }

    // The slot to record into is the oldest: if the GPU still hasn't
    // finished it, its times are dropped rather than waited for.
    p->current = (p->current + 1) % GPU_PROFILER_LATENCY;
    p->frames[p->current].count   = 0;
    p->frames[p->current].pending = false;
}

uint32
gpu_profiler_begin(gpu_profiler* p, const char* name)
{
    gpu_profiler_frame* frame = &p->frames[p->current];
    if (!frame->start[0] || frame->count >= GPU_PROFILER_MAX_SCOPES)
        return NO_SCOPE;

    uint32 scope = frame->count++;
    frame->names[scope] = name;
    frame->pending      = true;
    rhi_timestamp_write(frame->start[scope]);
    return scope;
}

void
gpu_profiler_end(gpu_profiler* p, uint32 scope)
{
    if (scope == NO_SCOPE)
        return;
    rhi_timestamp_write(p->frames[p->current].end[scope]);
}

void
gpu_profiler_destroy(gpu_profiler* p)
{
    for (uint32 f = 0; f < GPU_PROFILER_LATENCY; ++f)
    {
        for (uint32 s = 0; s < GPU_PROFILER_MAX_SCOPES; ++s)
        {
            if (p->frames[f].start[s])
                rhi_timestamp_destroy(p->frames[f].start[s]);
            if (p->frames[f].end[s])
                rhi_timestamp_destroy(p->frames[f].end[s]);
        }
    }
    memset(p, 0, sizeof(*p));
}
