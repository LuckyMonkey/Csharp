#include "direct_nvdec.h"

#include <cuda.h>
#include <nvcuvid.h>

#include <stdio.h>
#include <string.h>

static int cuda_ok(CUresult result, const char *what)
{
    const char *name = NULL;
    const char *message = NULL;
    if (result == CUDA_SUCCESS) return 1;
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &message);
    fprintf(stderr, "direct-nvdecode: %s failed: %s%s%s\n",
            what,
            name ? name : "CUDA_ERROR",
            message ? " - " : "",
            message ? message : "");
    return 0;
}

int csharp_direct_nvdecode_probe(int device_index, struct csharp_nvdecode_caps *caps)
{
    CUdevice device;
    CUcontext context = NULL;
    CUVIDDECODECAPS decode_caps;
    int device_count = 0;
    int result = -1;

    if (!caps || device_index < 0) return -1;
    memset(caps, 0, sizeof(*caps));
    memset(&decode_caps, 0, sizeof(decode_caps));

    if (!cuda_ok(cuInit(0), "cuInit")) return -1;
    if (!cuda_ok(cuDeviceGetCount(&device_count), "cuDeviceGetCount")) return -1;
    if (device_index >= device_count) {
        fprintf(stderr, "direct-nvdecode: GPU index %d unavailable (count=%d)\n",
                device_index, device_count);
        return -1;
    }
    if (!cuda_ok(cuDeviceGet(&device, device_index), "cuDeviceGet")) return -1;
    if (!cuda_ok(cuCtxCreate(&context, NULL, CU_CTX_SCHED_AUTO, device), "cuCtxCreate")) return -1;

    decode_caps.eCodecType = cudaVideoCodec_HEVC;
    decode_caps.eChromaFormat = cudaVideoChromaFormat_420;
    decode_caps.nBitDepthMinus8 = 0;

    if (!cuda_ok(cuvidGetDecoderCaps(&decode_caps), "cuvidGetDecoderCaps")) goto cleanup;

    caps->hevc_supported = decode_caps.bIsSupported != 0;
    caps->max_width = decode_caps.nMaxWidth;
    caps->max_height = decode_caps.nMaxHeight;
    caps->max_mb_count = decode_caps.nMaxMBCount;
    caps->output_format_mask = decode_caps.nOutputFormatMask;
    result = 0;

cleanup:
    if (context) cuCtxDestroy(context);
    return result;
}
