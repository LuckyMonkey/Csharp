#include "direct_nvdec_memory.h"

#include <cuda.h>
#include <stdio.h>
#include <stdlib.h>

static int init_cuda(void)
{
    CUdevice device;
    CUcontext context = NULL;

    if (cuInit(0) != CUDA_SUCCESS) return -1;
    if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) return -1;
    if (cuCtxCreate(&context, NULL, CU_CTX_SCHED_AUTO, device) != CUDA_SUCCESS) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct csharp_nvdec_memory_snapshot snapshot;
    struct csharp_nvdec_geometry_bucket bucket;
    unsigned long width = 1536;
    unsigned long height = 2048;
    char free_text[32];
    char total_text[32];

    if (argc == 3) {
        char *end = NULL;
        width = strtoul(argv[1], &end, 10);
        if (!end || *end != '\0' || width < 1 || width > 8192) return EXIT_FAILURE;
        end = NULL;
        height = strtoul(argv[2], &end, 10);
        if (!end || *end != '\0' || height < 1 || height > 8192) return EXIT_FAILURE;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [coded-width coded-height]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (init_cuda() != 0 || csharp_nvdec_memory_snapshot(&snapshot) != 0) {
        fputs("cannot initialize CUDA or query memory\n", stderr);
        return EXIT_FAILURE;
    }

    bucket = csharp_nvdec_geometry_bucket((unsigned int)width, (unsigned int)height);
    printf("CUDA memory: free=%s total=%s\n",
           csharp_nvdec_format_bytes(snapshot.free_bytes, free_text, sizeof(free_text)),
           csharp_nvdec_format_bytes(snapshot.total_bytes, total_text, sizeof(total_text)));
    printf("coded=%lux%lu -> decoder bucket=%ux%u\n",
           width, height, bucket.max_width, bucket.max_height);
    return EXIT_SUCCESS;
}
