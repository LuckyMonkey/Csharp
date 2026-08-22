#define _GNU_SOURCE

#include <cuda.h>
#include <nvcuvid.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int trace_enabled(void)
{
    const char *value = getenv("CSHARP_NVCUVID_TRACE");
    return value && *value && *value != '0';
}

static unsigned long trace_thread(void)
{
    return (unsigned long)(uintptr_t)pthread_self();
}

static CUcontext trace_context(void)
{
    CUcontext context = NULL;
    (void)cuCtxGetCurrent(&context);
    return context;
}

static const char *cuda_result_name(CUresult result)
{
    const char *name = NULL;
    if (cuGetErrorName(result, &name) != CUDA_SUCCESS || !name) return "CUDA_ERROR_UNKNOWN";
    return name;
}

#define TRACE_CALL(label, handle, result) \
    do { \
        if (trace_enabled()) { \
            fprintf(stderr, \
                    "csharp-nvcuvid-trace: tid=%lu ctx=%p %s handle=%p result=%d(%s)\n", \
                    trace_thread(), (void *)trace_context(), (label), (void *)(handle), \
                    (int)(result), cuda_result_name(result)); \
        } \
    } while (0)

typedef CUresult (CUDAAPI *create_parser_fn)(CUvideoparser *, CUVIDPARSERPARAMS *);
typedef CUresult (CUDAAPI *destroy_parser_fn)(CUvideoparser);
typedef CUresult (CUDAAPI *parse_video_fn)(CUvideoparser, CUVIDSOURCEDATAPACKET *);
typedef CUresult (CUDAAPI *create_decoder_fn)(CUvideodecoder *, CUVIDDECODECREATEINFO *);
typedef CUresult (CUDAAPI *destroy_decoder_fn)(CUvideodecoder);
typedef CUresult (CUDAAPI *reconfigure_decoder_fn)(CUvideodecoder, CUVIDRECONFIGUREDECODERINFO *);
typedef CUresult (CUDAAPI *decode_picture_fn)(CUvideodecoder, CUVIDPICPARAMS *);
typedef CUresult (CUDAAPI *map_frame_fn)(CUvideodecoder, int, CUdeviceptr *, unsigned int *, CUVIDPROCPARAMS *);
typedef CUresult (CUDAAPI *unmap_frame_fn)(CUvideodecoder, CUdeviceptr);

static void *next_symbol(const char *name)
{
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        fprintf(stderr, "csharp-nvcuvid-trace: cannot resolve %s: %s\n", name, dlerror());
        abort();
    }
    return symbol;
}

#define LOAD_SYMBOL(function_pointer, symbol_name) \
    do { \
        void *symbol_value = next_symbol(symbol_name); \
        memcpy(&(function_pointer), &symbol_value, sizeof(function_pointer)); \
    } while (0)

CUresult CUDAAPI cuvidCreateVideoParser(CUvideoparser *parser, CUVIDPARSERPARAMS *params)
{
    static create_parser_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidCreateVideoParser");
    result = real_fn(parser, params);
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p create-parser parser=%p codec=%d surfaces=%u delay=%u result=%d(%s)\n",
                trace_thread(), (void *)trace_context(), parser ? (void *)*parser : NULL,
                params ? (int)params->CodecType : -1,
                params ? params->ulMaxNumDecodeSurfaces : 0,
                params ? params->ulMaxDisplayDelay : 0,
                (int)result, cuda_result_name(result));
    }
    return result;
}

CUresult CUDAAPI cuvidDestroyVideoParser(CUvideoparser parser)
{
    static destroy_parser_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidDestroyVideoParser");
    result = real_fn(parser);
    TRACE_CALL("destroy-parser", parser, result);
    return result;
}

CUresult CUDAAPI cuvidParseVideoData(CUvideoparser parser, CUVIDSOURCEDATAPACKET *packet)
{
    static parse_video_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidParseVideoData");
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p parse-enter parser=%p flags=0x%lx bytes=%lu ts=%lld\n",
                trace_thread(), (void *)trace_context(), (void *)parser,
                packet ? packet->flags : 0UL,
                packet ? packet->payload_size : 0UL,
                packet ? (long long)packet->timestamp : -1LL);
    }
    result = real_fn(parser, packet);
    TRACE_CALL("parse-exit", parser, result);
    return result;
}

CUresult CUDAAPI cuvidCreateDecoder(CUvideodecoder *decoder, CUVIDDECODECREATEINFO *info)
{
    static create_decoder_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidCreateDecoder");
    result = real_fn(decoder, info);
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p create-decoder decoder=%p coded=%lux%lu target=%lux%lu surfaces=%lu output=%lu result=%d(%s)\n",
                trace_thread(), (void *)trace_context(), decoder ? (void *)*decoder : NULL,
                info ? info->ulWidth : 0UL, info ? info->ulHeight : 0UL,
                info ? info->ulTargetWidth : 0UL, info ? info->ulTargetHeight : 0UL,
                info ? info->ulNumDecodeSurfaces : 0UL,
                info ? info->ulNumOutputSurfaces : 0UL,
                (int)result, cuda_result_name(result));
    }
    return result;
}

CUresult CUDAAPI cuvidDestroyDecoder(CUvideodecoder decoder)
{
    static destroy_decoder_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidDestroyDecoder");
    result = real_fn(decoder);
    TRACE_CALL("destroy-decoder", decoder, result);
    return result;
}

CUresult CUDAAPI cuvidReconfigureDecoder(CUvideodecoder decoder, CUVIDRECONFIGUREDECODERINFO *info)
{
    static reconfigure_decoder_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidReconfigureDecoder");
    result = real_fn(decoder, info);
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p reconfigure decoder=%p coded=%lux%lu target=%lux%lu surfaces=%lu result=%d(%s)\n",
                trace_thread(), (void *)trace_context(), (void *)decoder,
                info ? info->ulWidth : 0UL, info ? info->ulHeight : 0UL,
                info ? info->ulTargetWidth : 0UL, info ? info->ulTargetHeight : 0UL,
                info ? info->ulNumDecodeSurfaces : 0UL,
                (int)result, cuda_result_name(result));
    }
    return result;
}

CUresult CUDAAPI cuvidDecodePicture(CUvideodecoder decoder, CUVIDPICPARAMS *picture)
{
    static decode_picture_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidDecodePicture");
    result = real_fn(decoder, picture);
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p decode decoder=%p curr_pic=%d field_pic=%u bottom=%u result=%d(%s)\n",
                trace_thread(), (void *)trace_context(), (void *)decoder,
                picture ? picture->CurrPicIdx : -1,
                picture ? (unsigned int)picture->field_pic_flag : 0U,
                picture ? (unsigned int)picture->bottom_field_flag : 0U,
                (int)result, cuda_result_name(result));
    }
    return result;
}

CUresult CUDAAPI cuvidMapVideoFrame(CUvideodecoder decoder, int picture_index,
                                    CUdeviceptr *device_ptr, unsigned int *pitch,
                                    CUVIDPROCPARAMS *params)
{
    static map_frame_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidMapVideoFrame64");
    result = real_fn(decoder, picture_index, device_ptr, pitch, params);
    if (trace_enabled()) {
        fprintf(stderr,
                "csharp-nvcuvid-trace: tid=%lu ctx=%p map decoder=%p pic=%d ptr=0x%llx pitch=%u result=%d(%s)\n",
                trace_thread(), (void *)trace_context(), (void *)decoder, picture_index,
                device_ptr ? (unsigned long long)*device_ptr : 0ULL,
                pitch ? *pitch : 0U, (int)result, cuda_result_name(result));
    }
    return result;
}

CUresult CUDAAPI cuvidUnmapVideoFrame(CUvideodecoder decoder, CUdeviceptr device_ptr)
{
    static unmap_frame_fn real_fn;
    CUresult result;
    if (!real_fn) LOAD_SYMBOL(real_fn, "cuvidUnmapVideoFrame64");
    result = real_fn(decoder, device_ptr);
    TRACE_CALL("unmap", decoder, result);
    return result;
}
