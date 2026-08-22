#define _POSIX_C_SOURCE 200809L

#include "direct_nvdec_timing.h"

#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

static atomic_ullong stage_nanoseconds[CSHARP_DIRECT_STAGE_COUNT];
static atomic_ullong stage_samples[CSHARP_DIRECT_STAGE_COUNT];

uint64_t csharp_direct_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void csharp_direct_timing_reset(void)
{
    for (int i = 0; i < CSHARP_DIRECT_STAGE_COUNT; ++i) {
        atomic_store_explicit(&stage_nanoseconds[i], 0, memory_order_relaxed);
        atomic_store_explicit(&stage_samples[i], 0, memory_order_relaxed);
    }
}

void csharp_direct_timing_add(enum csharp_direct_stage stage, uint64_t nanoseconds)
{
    if (stage < 0 || stage >= CSHARP_DIRECT_STAGE_COUNT) return;
    atomic_fetch_add_explicit(&stage_nanoseconds[stage], nanoseconds, memory_order_relaxed);
    atomic_fetch_add_explicit(&stage_samples[stage], 1, memory_order_relaxed);
}

void csharp_direct_timing_get(struct csharp_direct_timing_snapshot *snapshot)
{
    if (!snapshot) return;
    for (int i = 0; i < CSHARP_DIRECT_STAGE_COUNT; ++i) {
        snapshot->nanoseconds[i] =
            atomic_load_explicit(&stage_nanoseconds[i], memory_order_relaxed);
        snapshot->samples[i] =
            atomic_load_explicit(&stage_samples[i], memory_order_relaxed);
    }
}

const char *csharp_direct_stage_name(enum csharp_direct_stage stage)
{
    static const char *const names[CSHARP_DIRECT_STAGE_COUNT] = {
        "annexb",
        "parse",
        "flush_wait",
        "map",
        "copy_y",
        "copy_uv",
        "deinterleave",
        "output",
        "unmap",
        "total"
    };
    if (stage < 0 || stage >= CSHARP_DIRECT_STAGE_COUNT) return "unknown";
    return names[stage];
}

double csharp_direct_stage_ms_per_sample(const struct csharp_direct_timing_snapshot *snapshot,
                                         enum csharp_direct_stage stage)
{
    uint64_t samples;
    if (!snapshot || stage < 0 || stage >= CSHARP_DIRECT_STAGE_COUNT) return 0.0;
    samples = snapshot->samples[stage];
    if (!samples) return 0.0;
    return (double)snapshot->nanoseconds[stage] / (double)samples / 1000000.0;
}
