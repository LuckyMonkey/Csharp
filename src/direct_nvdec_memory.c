#include "direct_nvdec_memory.h"

#include <cuda.h>
#include <stdio.h>

#define CSHARP_NVDEC_HARD_MAX 8192U

int csharp_nvdec_memory_snapshot(struct csharp_nvdec_memory_snapshot *snapshot)
{
    size_t free_bytes = 0;
    size_t total_bytes = 0;

    if (!snapshot) return -1;
    snapshot->free_bytes = 0;
    snapshot->total_bytes = 0;
    if (cuMemGetInfo(&free_bytes, &total_bytes) != CUDA_SUCCESS) return -1;
    snapshot->free_bytes = free_bytes;
    snapshot->total_bytes = total_bytes;
    return 0;
}

static unsigned int bucket_axis(unsigned int value)
{
    static const unsigned int buckets[] = {512U, 1024U, 2048U, 4096U, 8192U};
    size_t i;

    for (i = 0; i < sizeof(buckets) / sizeof(buckets[0]); ++i) {
        if (value <= buckets[i]) return buckets[i];
    }
    return CSHARP_NVDEC_HARD_MAX;
}

struct csharp_nvdec_geometry_bucket csharp_nvdec_geometry_bucket(unsigned int width,
                                                                  unsigned int height)
{
    struct csharp_nvdec_geometry_bucket bucket;

    bucket.max_width = bucket_axis(width);
    bucket.max_height = bucket_axis(height);
    return bucket;
}

const char *csharp_nvdec_format_bytes(size_t bytes, char *buffer, size_t buffer_size)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = (double)bytes;
    size_t unit = 0;

    if (!buffer || buffer_size == 0) return "";
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    (void)snprintf(buffer, buffer_size, "%.2f %s", value, units[unit]);
    return buffer;
}
