#ifndef CSHARP_DIRECT_NVDEC_TIMING_H
#define CSHARP_DIRECT_NVDEC_TIMING_H

#include <stdint.h>

enum csharp_direct_stage {
    CSHARP_DIRECT_STAGE_ANNEXB = 0,
    CSHARP_DIRECT_STAGE_PARSE,
    CSHARP_DIRECT_STAGE_FLUSH_WAIT,
    CSHARP_DIRECT_STAGE_MAP,
    CSHARP_DIRECT_STAGE_COPY_Y,
    CSHARP_DIRECT_STAGE_COPY_UV,
    CSHARP_DIRECT_STAGE_DEINTERLEAVE,
    CSHARP_DIRECT_STAGE_OUTPUT,
    CSHARP_DIRECT_STAGE_UNMAP,
    CSHARP_DIRECT_STAGE_TOTAL,
    CSHARP_DIRECT_STAGE_COUNT
};

struct csharp_direct_timing_snapshot {
    uint64_t nanoseconds[CSHARP_DIRECT_STAGE_COUNT];
    uint64_t samples[CSHARP_DIRECT_STAGE_COUNT];
};

uint64_t csharp_direct_now_ns(void);
void csharp_direct_timing_reset(void);
void csharp_direct_timing_add(enum csharp_direct_stage stage, uint64_t nanoseconds);
void csharp_direct_timing_get(struct csharp_direct_timing_snapshot *snapshot);
const char *csharp_direct_stage_name(enum csharp_direct_stage stage);

double csharp_direct_stage_ms_per_sample(const struct csharp_direct_timing_snapshot *snapshot,
                                         enum csharp_direct_stage stage);

#endif
