#ifndef CSHARP_DIRECT_NVDEC_MEMORY_H
#define CSHARP_DIRECT_NVDEC_MEMORY_H

#include <stddef.h>

struct csharp_nvdec_memory_snapshot {
    size_t free_bytes;
    size_t total_bytes;
};

struct csharp_nvdec_geometry_bucket {
    unsigned int max_width;
    unsigned int max_height;
};

int csharp_nvdec_memory_snapshot(struct csharp_nvdec_memory_snapshot *snapshot);
struct csharp_nvdec_geometry_bucket csharp_nvdec_geometry_bucket(unsigned int width,
                                                                  unsigned int height);
const char *csharp_nvdec_format_bytes(size_t bytes, char *buffer, size_t buffer_size);

#endif
