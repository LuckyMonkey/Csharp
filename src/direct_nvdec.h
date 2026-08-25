#ifndef CSHARP_DIRECT_NVDEC_H
#define CSHARP_DIRECT_NVDEC_H

struct csharp_nvdecode_caps {
    int hevc_supported;
    int max_width;
    int max_height;
    int max_mb_count;
    unsigned int output_format_mask;
};

/*
 * Probe NVIDIA's NVDECODE API directly, without FFmpeg/libavcodec.
 * Returns 0 on success and fills caps; non-zero means the direct backend
 * could not be initialized on this system.
 */
int csharp_direct_nvdecode_probe(int device_index, struct csharp_nvdecode_caps *caps);

#endif
