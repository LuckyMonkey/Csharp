#include "direct_nvdec.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    struct csharp_nvdecode_caps caps;
    int device = 0;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [gpu-index]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);
        if (!end || *end != '\0' || value < 0 || value > 1024) {
            fprintf(stderr, "invalid gpu index: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        device = (int)value;
    }

    if (csharp_direct_nvdecode_probe(device, &caps) != 0) return EXIT_FAILURE;

    printf("Direct NVDECODE probe: gpu=%d\n", device);
    printf("HEVC 8-bit 4:2:0 supported: %s\n", caps.hevc_supported ? "yes" : "no");
    printf("Maximum coded dimensions: %dx%d\n", caps.max_width, caps.max_height);
    printf("Maximum macroblock count: %d\n", caps.max_mb_count);
    printf("Output format mask: 0x%x\n", caps.output_format_mask);

    return caps.hevc_supported ? EXIT_SUCCESS : EXIT_FAILURE;
}
