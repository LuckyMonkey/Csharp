#define _POSIX_C_SOURCE 200809L

#include "direct_nvdec_plugin.h"
#include "direct_nvdec_memory.h"
#include "direct_nvdec_timing.h"

#include <cuda.h>
#include <nvcuvid.h>
#include <libheif/heif_plugin.h>

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <string.h>

#define DIRECT_MAX_INPUT (256U * 1024U * 1024U)
#define DIRECT_MAX_WIDTH 8192U
#define DIRECT_MAX_HEIGHT 8192U
#define DIRECT_LANE_COUNT 32

struct direct_decoder {
    CUcontext context;
    CUvideoparser parser;
    CUvideodecoder decoder;
    int decoder_created;
    int sequence_seen;
    int display_index;
    int display_ready;
    int width;
    int height;
    int display_left;
    int display_top;
    int display_width;
    int display_height;
    uint8_t *input;
    size_t input_size;
    size_t input_capacity;
    uint8_t *uv_staging;
    size_t uv_staging_capacity;
    int busy;
    int initialized;
    int has_decoded;
    int lane_id;
    CUvideotimestamp next_timestamp;
    unsigned int configured_max_width;
    unsigned int configured_max_height;
    unsigned int configured_surfaces;
};

static struct direct_decoder direct_lanes[DIRECT_LANE_COUNT];
static pthread_mutex_t direct_lane_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t direct_lane_available = PTHREAD_COND_INITIALIZER;
static unsigned int direct_active_lanes;
static atomic_ulong direct_lane_creates;
static atomic_ulong direct_lane_reuses;
static atomic_ulong direct_decoder_creates;
static atomic_ulong direct_decoder_reconfigures;
static atomic_ulong direct_decoder_bucket_grows;
static atomic_ulong direct_decodes;

static struct heif_error direct_error(enum heif_error_code code,
                                      enum heif_suberror_code subcode,
                                      const char *message)
{
    return (struct heif_error){code, subcode, message};
}

static struct heif_error direct_ok(void)
{
    return (struct heif_error){heif_error_Ok, heif_suberror_Unspecified, "ok"};
}

static int cuda_ok(CUresult result)
{
    return result == CUDA_SUCCESS;
}

static const char *cuda_name(CUresult result)
{
    const char *name = NULL;
    if (cuGetErrorName(result, &name) != CUDA_SUCCESS || !name) return "CUDA_ERROR_UNKNOWN";
    return name;
}

static int direct_trace_enabled(void)
{
    const char *value = getenv("CSHARP_DIRECT_TRACE");
    return value && *value && *value != '0';
}

static int direct_memory_trace_enabled(void)
{
    const char *trace = getenv("CSHARP_DIRECT_TRACE");
    const char *memory = getenv("CSHARP_DIRECT_MEMORY_TRACE");
    return (trace && *trace && *trace != '0') ||
           (memory && *memory && *memory != '0');
}

static void trace_memory(struct direct_decoder *decoder, const char *operation,
                         const struct csharp_nvdec_memory_snapshot *before,
                         const struct csharp_nvdec_memory_snapshot *after,
                         unsigned int max_width, unsigned int max_height,
                         unsigned int surfaces, unsigned long output_surfaces)
{
    long long delta = 0;
    char before_text[32];
    char after_text[32];

    if (!direct_memory_trace_enabled() || !before || !after) return;
    delta = (long long)after->free_bytes - (long long)before->free_bytes;
    fprintf(stderr,
            "direct-memory: lane=%d op=%s coded=%ux%u max=%ux%u surfaces=%u output=%lu "
            "free_before=%s free_after=%s delta=%lld bytes\n",
            decoder ? decoder->lane_id : -1, operation,
            decoder ? (unsigned int)decoder->width : 0U,
            decoder ? (unsigned int)decoder->height : 0U,
            max_width, max_height, surfaces, output_surfaces,
            csharp_nvdec_format_bytes(before->free_bytes, before_text, sizeof(before_text)),
            csharp_nvdec_format_bytes(after->free_bytes, after_text, sizeof(after_text)),
            delta);
}

static void direct_trace(struct direct_decoder *decoder, const char *event, ...)
{
    CUcontext current = NULL;
    va_list args;
    if (!direct_trace_enabled()) return;
    (void)cuCtxGetCurrent(&current);
    fprintf(stderr, "direct-trace: lane=%d tid=%lu lane_ctx=%p current_ctx=%p parser=%p decoder=%p ",
            decoder ? decoder->lane_id : -1, (unsigned long)pthread_self(),
            decoder ? (void *)decoder->context : NULL, (void *)current,
            decoder ? (void *)decoder->parser : NULL,
            decoder ? (void *)decoder->decoder : NULL);
    va_start(args, event);
    vfprintf(stderr, event, args);
    va_end(args);
    fputc('\n', stderr);
}

static void direct_timing_record(enum csharp_direct_stage stage, uint64_t start)
{
    uint64_t end = csharp_direct_now_ns();
    if (end >= start) csharp_direct_timing_add(stage, end - start);
}

static void lock_lanes(void)
{
    (void)pthread_mutex_lock(&direct_lane_lock);
}

static void unlock_lanes(void)
{
    (void)pthread_mutex_unlock(&direct_lane_lock);
}

static unsigned int active_lane_limit(void)
{
    const char *value = getenv("CSHARP_DIRECT_MAX_ACTIVE_LANES");
    char *end = NULL;
    unsigned long parsed;
    if (!value || !*value) return 4;
    parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed < 1) return 4;
    if (parsed > DIRECT_LANE_COUNT) parsed = DIRECT_LANE_COUNT;
    return (unsigned int)parsed;
}

static unsigned long output_surface_count(void)
{
    const char *value = getenv("CSHARP_DIRECT_OUTPUT_SURFACES");
    return value && strcmp(value, "1") == 0 ? 1UL : 2UL;
}

static void destroy_lane(struct direct_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->parser) cuvidDestroyVideoParser(decoder->parser);
    if (decoder->decoder_created) cuvidDestroyDecoder(decoder->decoder);
    if (decoder->context) cuCtxDestroy(decoder->context);
    free(decoder->input);
    free(decoder->uv_staging);
    memset(decoder, 0, sizeof(*decoder));
    decoder->display_index = -1;
}

static int sequence_callback(void *opaque, CUVIDEOFORMAT *format)
{
    struct direct_decoder *decoder = opaque;
    CUVIDDECODECREATEINFO info;
    struct csharp_nvdec_geometry_bucket bucket;
    struct csharp_nvdec_memory_snapshot memory_before;
    struct csharp_nvdec_memory_snapshot memory_after;
    unsigned int surfaces;
    unsigned long output_surfaces;
    int needs_create;

    if (!decoder || !format || format->codec != cudaVideoCodec_HEVC ||
        format->chroma_format != cudaVideoChromaFormat_420 ||
        format->bit_depth_luma_minus8 != 0 || format->bit_depth_chroma_minus8 != 0 ||
        format->coded_width == 0 || format->coded_height == 0 ||
        format->coded_width > DIRECT_MAX_WIDTH || format->coded_height > DIRECT_MAX_HEIGHT) {
        return 0;
    }

    decoder->width = (int)format->coded_width;
    decoder->height = (int)format->coded_height;
    decoder->display_left = format->display_area.left;
    decoder->display_top = format->display_area.top;
    decoder->display_width = format->display_area.right - format->display_area.left;
    decoder->display_height = format->display_area.bottom - format->display_area.top;
    direct_trace(decoder, "sequence coded=%ux%u display=%d,%d-%d,%d surfaces_min=%u",
                 format->coded_width, format->coded_height,
                 format->display_area.left, format->display_area.top,
                 format->display_area.right, format->display_area.bottom,
                 format->min_num_decode_surfaces);
    if (getenv("CSHARP_DIRECT_GEOMETRY_DEBUG")) {
        fprintf(stderr, "direct geometry coded=%ux%u display_area=%d,%d-%d,%d visible=%dx%d\n",
                format->coded_width, format->coded_height,
                format->display_area.left, format->display_area.top,
                format->display_area.right, format->display_area.bottom,
                decoder->display_width, decoder->display_height);
    }
    if (decoder->display_width <= 0 || decoder->display_height <= 0 ||
        decoder->display_left < 0 || decoder->display_top < 0 ||
        decoder->display_left + decoder->display_width > decoder->width ||
        decoder->display_top + decoder->display_height > decoder->height) {
        return 0;
    }

    surfaces = format->min_num_decode_surfaces;
    if (surfaces < 2) surfaces = 2;
    if (surfaces > 16) surfaces = 16;
    output_surfaces = output_surface_count();

    bucket = csharp_nvdec_geometry_bucket(format->coded_width, format->coded_height);
    needs_create = !decoder->decoder_created ||
                   format->coded_width > decoder->configured_max_width ||
                   format->coded_height > decoder->configured_max_height;
    if (decoder->decoder_created && needs_create) {
        CUresult result;
        direct_trace(decoder, "bucket-grow old-max=%ux%u new-max=%ux%u",
                     decoder->configured_max_width, decoder->configured_max_height,
                     bucket.max_width, bucket.max_height);
        result = cuvidDestroyDecoder(decoder->decoder);
        if (!cuda_ok(result)) {
            direct_trace(decoder, "destroy-for-bucket-grow result=%d(%s)",
                         (int)result, cuda_name(result));
            return 0;
        }
        decoder->decoder = NULL;
        decoder->decoder_created = 0;
        decoder->configured_max_width = 0;
        decoder->configured_max_height = 0;
        decoder->configured_surfaces = 0;
        atomic_fetch_add_explicit(&direct_decoder_bucket_grows, 1, memory_order_relaxed);
    }

    memset(&info, 0, sizeof(info));
    info.ulWidth = format->coded_width;
    info.ulHeight = format->coded_height;
    info.ulNumDecodeSurfaces = surfaces;
    info.CodecType = cudaVideoCodec_HEVC;
    info.ChromaFormat = cudaVideoChromaFormat_420;
    info.ulCreationFlags = cudaVideoCreate_Default;
    info.bitDepthMinus8 = 0;
    info.ulIntraDecodeOnly = 0;
    info.ulMaxWidth = bucket.max_width;
    info.ulMaxHeight = bucket.max_height;
    info.display_area.left = 0;
    info.display_area.top = 0;
    info.display_area.right = (short)format->coded_width;
    info.display_area.bottom = (short)format->coded_height;
    info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = format->coded_width;
    info.ulTargetHeight = format->coded_height;
    info.ulNumOutputSurfaces = output_surfaces;

    if (decoder->decoder_created) {
        CUVIDRECONFIGUREDECODERINFO reconfigure;
        int have_memory_before = direct_memory_trace_enabled() &&
                                 csharp_nvdec_memory_snapshot(&memory_before) == 0;
        memset(&reconfigure, 0, sizeof(reconfigure));
        reconfigure.ulWidth = format->coded_width;
        reconfigure.ulHeight = format->coded_height;
        reconfigure.ulTargetWidth = format->coded_width;
        reconfigure.ulTargetHeight = format->coded_height;
        reconfigure.ulNumDecodeSurfaces = surfaces;
        reconfigure.display_area.left = info.display_area.left;
        reconfigure.display_area.top = info.display_area.top;
        reconfigure.display_area.right = info.display_area.right;
        reconfigure.display_area.bottom = info.display_area.bottom;
        CUresult result = cuvidReconfigureDecoder(decoder->decoder, &reconfigure);
        direct_trace(decoder, "reconfigure result=%d(%s) surfaces=%u",
                     (int)result, cuda_name(result), surfaces);
        if (!cuda_ok(result)) return 0;
        atomic_fetch_add_explicit(&direct_decoder_reconfigures, 1, memory_order_relaxed);
        if (have_memory_before && csharp_nvdec_memory_snapshot(&memory_after) == 0) {
            trace_memory(decoder, "reconfigure", &memory_before, &memory_after,
                         decoder->configured_max_width, decoder->configured_max_height,
                         surfaces, info.ulNumOutputSurfaces);
        }
    } else {
        int have_memory_before = direct_memory_trace_enabled() &&
                                 csharp_nvdec_memory_snapshot(&memory_before) == 0;
        CUresult result = cuvidCreateDecoder(&decoder->decoder, &info);
        direct_trace(decoder, "create-decoder result=%d(%s) surfaces=%u output=%lu",
                     (int)result, cuda_name(result), surfaces, info.ulNumOutputSurfaces);
        if (!cuda_ok(result)) return 0;
        decoder->decoder_created = 1;
        decoder->configured_max_width = bucket.max_width;
        decoder->configured_max_height = bucket.max_height;
        decoder->configured_surfaces = surfaces;
        atomic_fetch_add_explicit(&direct_decoder_creates, 1, memory_order_relaxed);
        if (have_memory_before && csharp_nvdec_memory_snapshot(&memory_after) == 0) {
            trace_memory(decoder, "create", &memory_before, &memory_after,
                         bucket.max_width, bucket.max_height, surfaces,
                         info.ulNumOutputSurfaces);
        }
    }
    decoder->sequence_seen = 1;
    return (int)surfaces;
}

static int decode_callback(void *opaque, CUVIDPICPARAMS *picture)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder || !decoder->decoder_created || !picture) return 0;
    CUresult result = cuvidDecodePicture(decoder->decoder, picture);
    direct_trace(decoder, "decode-picture curr=%d result=%d(%s)",
                 picture->CurrPicIdx, (int)result, cuda_name(result));
    return cuda_ok(result) ? 1 : 0;
}

static int display_callback(void *opaque, CUVIDPARSERDISPINFO *display)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder || !display) return 1;
    decoder->display_index = display->picture_index;
    decoder->display_ready = 1;
    direct_trace(decoder, "display picture=%d", display->picture_index);
    return 1;
}

static int initialize_lane(struct direct_decoder *decoder)
{
    CUVIDPARSERPARAMS params;
    CUdevice device;
    CUcontext previous = NULL;

    decoder->display_index = -1;
    decoder->next_timestamp = 1;
    if (!cuda_ok(cuInit(0)) || !cuda_ok(cuDeviceGet(&device, 0)) ||
        !cuda_ok(cuCtxCreate(&decoder->context, NULL, CU_CTX_SCHED_AUTO, device))) {
        return -1;
    }
    memset(&params, 0, sizeof(params));
    params.CodecType = cudaVideoCodec_HEVC;
    params.ulMaxNumDecodeSurfaces = 16;
    params.ulMaxDisplayDelay = 0;
    params.pUserData = decoder;
    params.pfnSequenceCallback = sequence_callback;
    params.pfnDecodePicture = decode_callback;
    params.pfnDisplayPicture = display_callback;
    if (!cuda_ok(cuvidCreateVideoParser(&decoder->parser, &params))) {
        cuCtxDestroy(decoder->context);
        decoder->context = NULL;
        return -1;
    }
    if (!cuda_ok(cuCtxPopCurrent(&previous))) {
        destroy_lane(decoder);
        return -1;
    }
    decoder->initialized = 1;
    atomic_fetch_add_explicit(&direct_lane_creates, 1, memory_order_relaxed);
    return 0;
}

static const char *plugin_name(void)
{
    return "Csharp direct NVDECODE HEVC decoder";
}

static int supports_format(enum heif_compression_format format)
{
    return format == heif_compression_HEVC ? 1001 : 0;
}

static struct heif_error new_decoder(void **out_decoder)
{
    struct direct_decoder *decoder;
    int was_initialized;
    unsigned int limit = active_lane_limit();

    if (!out_decoder) {
        return direct_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "missing decoder output");
    }
    lock_lanes();
    for (;;) {
        decoder = NULL;
        while (direct_active_lanes >= limit) {
            (void)pthread_cond_wait(&direct_lane_available, &direct_lane_lock);
        }
        for (int i = 0; i < DIRECT_LANE_COUNT; ++i) {
            if (!direct_lanes[i].busy) {
                decoder = &direct_lanes[i];
                decoder->lane_id = i;
                was_initialized = decoder->initialized;
                decoder->busy = 1;
                decoder->input_size = 0;
                decoder->display_ready = 0;
                decoder->has_decoded = 0;
                ++direct_active_lanes;
                break;
            }
        }
        if (decoder) break;
        (void)pthread_cond_wait(&direct_lane_available, &direct_lane_lock);
    }
    unlock_lanes();
    if (!was_initialized && initialize_lane(decoder) != 0) {
        lock_lanes();
        decoder->busy = 0;
        --direct_active_lanes;
        (void)pthread_cond_broadcast(&direct_lane_available);
        unlock_lanes();
        return direct_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec,
                            "direct NVDECODE lane initialization failed");
    }
    if (was_initialized) {
        atomic_fetch_add_explicit(&direct_lane_reuses, 1, memory_order_relaxed);
    }
    *out_decoder = decoder;
    return direct_ok();
}

static void free_decoder(void *opaque)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder) return;
    decoder->input_size = 0;
    decoder->display_ready = 0;
    lock_lanes();
    decoder->busy = 0;
    if (direct_active_lanes > 0) --direct_active_lanes;
    (void)pthread_cond_signal(&direct_lane_available);
    unlock_lanes();
}

static void deinit_plugin(void)
{
    lock_lanes();
    for (int i = 0; i < DIRECT_LANE_COUNT; ++i) {
        if (!direct_lanes[i].busy) destroy_lane(&direct_lanes[i]);
    }
    unlock_lanes();
}

static struct heif_error push_data(void *opaque, const void *data, size_t size)
{
    struct direct_decoder *decoder = opaque;
    size_t needed;
    uint8_t *grown;

    if (!decoder || (!data && size) || size > DIRECT_MAX_INPUT - decoder->input_size) {
        return direct_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "invalid or oversized HEVC input");
    }
    needed = decoder->input_size + size;
    if (needed > decoder->input_capacity) {
        size_t capacity = decoder->input_capacity ? decoder->input_capacity : 4096;
        while (capacity < needed) {
            if (capacity > DIRECT_MAX_INPUT / 2) {
                capacity = DIRECT_MAX_INPUT;
                break;
            }
            capacity *= 2;
        }
        grown = realloc(decoder->input, capacity);
        if (!grown) {
            return direct_error(heif_error_Memory_allocation_error, heif_suberror_Unspecified,
                                "HEVC input allocation failed");
        }
        decoder->input = grown;
        decoder->input_capacity = capacity;
    }
    memcpy(decoder->input + decoder->input_size, data, size);
    decoder->input_size += size;
    return direct_ok();
}

static int length_prefixed_to_annexb(const uint8_t *input, size_t input_size,
                                     uint8_t **out_data, size_t *out_size)
{
    uint8_t *output;
    size_t pos = 0;
    size_t written = 0;
    size_t nal_count = 0;
    size_t output_size;
    if (!out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;
    if (!input || input_size < 4) return -1;

    while (pos < input_size) {
        uint32_t nal_size;
        if (input_size - pos < 4) return -1;
        nal_size = ((uint32_t)input[pos] << 24) | ((uint32_t)input[pos + 1] << 16) |
                   ((uint32_t)input[pos + 2] << 8) | input[pos + 3];
        pos += 4;
        if (!nal_size || nal_size > input_size - pos) return -1;
        if (nal_count == SIZE_MAX / 4) return -1;
        ++nal_count;
        pos += nal_size;
    }
    if (nal_count > (SIZE_MAX - input_size) / 4) return -1;
    output_size = input_size + nal_count * 4;
    output = malloc(output_size);
    if (!output) return -1;
    pos = 0;
    while (pos < input_size) {
        uint32_t nal_size;
        if (input_size - pos < 4) { free(output); return -1; }
        nal_size = ((uint32_t)input[pos] << 24) | ((uint32_t)input[pos + 1] << 16) |
                   ((uint32_t)input[pos + 2] << 8) | input[pos + 3];
        pos += 4;
        if (!nal_size || nal_size > input_size - pos) { free(output); return -1; }
        if (written > output_size - (size_t)nal_size - 4) {
            free(output);
            return -1;
        }
        output[written++] = 0;
        output[written++] = 0;
        output[written++] = 0;
        output[written++] = 1;
        memcpy(output + written, input + pos, nal_size);
        written += nal_size;
        pos += nal_size;
    }
    *out_data = output;
    *out_size = written;
    return 0;
}

static int ensure_uv_staging(struct direct_decoder *decoder, size_t needed)
{
    uint8_t *grown;

    if (needed <= decoder->uv_staging_capacity) return 0;
    grown = realloc(decoder->uv_staging, needed);
    if (!grown) return -1;
    decoder->uv_staging = grown;
    decoder->uv_staging_capacity = needed;
    return 0;
}

static struct heif_error make_image(struct direct_decoder *decoder,
                                    CUdeviceptr mapped, unsigned int pitch,
                                    struct heif_image **out_image)
{
    struct heif_error err;
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
    int y_stride;
    int cb_stride;
    int cr_stride;
    CUdeviceptr source;
    CUDA_MEMCPY2D copy;
    int chroma_width;
    int chroma_height;
    size_t uv_row_bytes;
    size_t uv_bytes;
    uint64_t start;

    if (getenv("CSHARP_DIRECT_GEOMETRY_DEBUG")) {
        fprintf(stderr, "direct mapped pitch=%u output=%dx%d luma_crop=(%d,%d %dx%d) chroma_crop=(%d,%d %dx%d)\n",
                pitch, decoder->display_width, decoder->display_height,
                decoder->display_left, decoder->display_top,
                decoder->display_width, decoder->display_height,
                decoder->display_left / 2, decoder->display_top / 2,
                (decoder->display_width + 1) / 2, (decoder->display_height + 1) / 2);
    }

    start = csharp_direct_now_ns();
    err = heif_image_create(decoder->display_width, decoder->display_height,
                            heif_colorspace_YCbCr, heif_chroma_420, out_image);
    if (err.code != heif_error_Ok) return err;
    err = heif_image_add_plane(*out_image, heif_channel_Y, decoder->display_width,
                               decoder->display_height, 8);
    if (err.code != heif_error_Ok) return err;
    err = heif_image_add_plane(*out_image, heif_channel_Cb,
                               (decoder->display_width + 1) / 2,
                               (decoder->display_height + 1) / 2, 8);
    if (err.code != heif_error_Ok) return err;
    err = heif_image_add_plane(*out_image, heif_channel_Cr,
                               (decoder->display_width + 1) / 2,
                               (decoder->display_height + 1) / 2, 8);
    if (err.code != heif_error_Ok) return err;
    y = heif_image_get_plane(*out_image, heif_channel_Y, &y_stride);
    cb = heif_image_get_plane(*out_image, heif_channel_Cb, &cb_stride);
    cr = heif_image_get_plane(*out_image, heif_channel_Cr, &cr_stride);
    if (!y || !cb || !cr) return direct_error(heif_error_Decoder_plugin_error,
                                              heif_suberror_Unspecified,
                                              "cannot access direct output planes");
    direct_timing_record(CSHARP_DIRECT_STAGE_OUTPUT, start);
    if (getenv("CSHARP_DIRECT_GEOMETRY_DEBUG")) {
        fprintf(stderr, "direct heif strides Y=%d Cb=%d Cr=%d dimensions=%dx%d chroma=%dx%d\n",
                y_stride, cb_stride, cr_stride, decoder->display_width,
                decoder->display_height, (decoder->display_width + 1) / 2,
                (decoder->display_height + 1) / 2);
    }

    /* Copy the complete visible luma rectangle in one driver call. */
    source = mapped + (CUdeviceptr)decoder->display_top * pitch + decoder->display_left;
    memset(&copy, 0, sizeof(copy));
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = source;
    copy.srcPitch = pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = y;
    copy.dstPitch = (size_t)y_stride;
    copy.WidthInBytes = (size_t)decoder->display_width;
    copy.Height = (size_t)decoder->display_height;
    start = csharp_direct_now_ns();
    if (!cuda_ok(cuMemcpy2D(&copy))) {
        return direct_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unspecified,
                            "Y surface bulk copy failed");
    }
    direct_timing_record(CSHARP_DIRECT_STAGE_COPY_Y, start);

    /*
     * NV12 chroma is interleaved UV. Copy the complete visible chroma
     * rectangle once, then deinterleave it in host memory. This replaces one
     * CUDA driver call per chroma sample with a single 2D transfer.
     *
     * Keep display_left as a byte offset: this intentionally preserves the
     * exact odd-dimension crop semantics validated against libheif/FFmpeg.
     */
    chroma_width = (decoder->display_width + 1) / 2;
    chroma_height = (decoder->display_height + 1) / 2;
    uv_row_bytes = (size_t)chroma_width * 2U;
    if ((size_t)chroma_height > SIZE_MAX / uv_row_bytes) {
        return direct_error(heif_error_Memory_allocation_error,
                            heif_suberror_Unspecified,
                            "UV staging size overflow");
    }
    uv_bytes = uv_row_bytes * (size_t)chroma_height;
    if (ensure_uv_staging(decoder, uv_bytes) != 0) {
        return direct_error(heif_error_Memory_allocation_error,
                            heif_suberror_Unspecified,
                            "UV staging allocation failed");
    }

    source = mapped + (CUdeviceptr)decoder->height * pitch +
             (CUdeviceptr)(decoder->display_top / 2) * pitch + decoder->display_left;
    memset(&copy, 0, sizeof(copy));
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = source;
    copy.srcPitch = pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = decoder->uv_staging;
    copy.dstPitch = uv_row_bytes;
    copy.WidthInBytes = uv_row_bytes;
    copy.Height = (size_t)chroma_height;
    start = csharp_direct_now_ns();
    if (!cuda_ok(cuMemcpy2D(&copy))) {
        return direct_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unspecified,
                            "UV surface bulk copy failed");
    }
    direct_timing_record(CSHARP_DIRECT_STAGE_COPY_UV, start);

    start = csharp_direct_now_ns();
    start = csharp_direct_now_ns();
    for (int row = 0; row < chroma_height; ++row) {
        const uint8_t *restrict src = decoder->uv_staging + (size_t)row * uv_row_bytes;
        uint8_t *restrict dst_cb = cb + (size_t)row * (size_t)cb_stride;
        uint8_t *restrict dst_cr = cr + (size_t)row * (size_t)cr_stride;
        int col = 0;
        for (; col + 3 < chroma_width; col += 4) {
            dst_cb[0] = src[0];
            dst_cr[0] = src[1];
            dst_cb[1] = src[2];
            dst_cr[1] = src[3];
            dst_cb[2] = src[4];
            dst_cr[2] = src[5];
            dst_cb[3] = src[6];
            dst_cr[3] = src[7];
            src += 8;
            dst_cb += 4;
            dst_cr += 4;
        }
        for (; col < chroma_width; ++col) {
            *dst_cb++ = *src++;
            *dst_cr++ = *src++;
        }
    }
    direct_timing_record(CSHARP_DIRECT_STAGE_DEINTERLEAVE, start);

    return direct_ok();
}

static struct heif_error decode_image(void *opaque, struct heif_image **out_image)
{
    struct direct_decoder *decoder = opaque;
    uint8_t *annexb = NULL;
    size_t annexb_size = 0;
    CUVIDSOURCEDATAPACKET packet;
    CUVIDSOURCEDATAPACKET eos;
    CUdeviceptr mapped = 0;
    unsigned int pitch = 0;
    CUVIDPROCPARAMS proc;
    CUcontext previous = NULL;
    int context_pushed = 0;
    struct heif_error err = direct_ok();
    uint64_t total_start = csharp_direct_now_ns();
    uint64_t start;

    if (!decoder || !out_image || !decoder->input_size) {
        return direct_error(heif_error_Invalid_input, heif_suberror_No_item_data,
                            "empty direct HEVC input");
    }
    *out_image = NULL;
    if (!cuda_ok(cuCtxPushCurrent(decoder->context))) {
        return direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                            "CUDA context activation failed");
    }
    context_pushed = 1;
    start = csharp_direct_now_ns();
    if (length_prefixed_to_annexb(decoder->input, decoder->input_size,
                                  &annexb, &annexb_size) != 0) {
        err = direct_error(heif_error_Invalid_input, heif_suberror_End_of_data,
                           "invalid length-prefixed HEVC stream");
        goto cleanup;
    }
    direct_timing_record(CSHARP_DIRECT_STAGE_ANNEXB, start);
    memset(&packet, 0, sizeof(packet));
    packet.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    if (decoder->has_decoded) packet.flags |= CUVID_PKT_DISCONTINUITY;
    packet.payload_size = (unsigned long)annexb_size;
    packet.payload = annexb;
    packet.timestamp = decoder->next_timestamp++;
    decoder->display_ready = 0;
    direct_trace(decoder, "packet flags=0x%lx bytes=%lu timestamp=%lld",
                 packet.flags, packet.payload_size, (long long)packet.timestamp);
    {
        start = csharp_direct_now_ns();
        CUresult result = cuvidParseVideoData(decoder->parser, &packet);
        direct_timing_record(CSHARP_DIRECT_STAGE_PARSE, start);
        direct_trace(decoder, "parse packet result=%d(%s)", (int)result, cuda_name(result));
        if (!cuda_ok(result)) {
            err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                               "CUVID parser rejected HEVC stream");
            goto cleanup;
        }
    }
    decoder->has_decoded = 1;
    memset(&eos, 0, sizeof(eos));
    eos.flags = CUVID_PKT_ENDOFSTREAM | CUVID_PKT_NOTIFY_EOS;
    {
        start = csharp_direct_now_ns();
        CUresult result = cuvidParseVideoData(decoder->parser, &eos);
        direct_timing_record(CSHARP_DIRECT_STAGE_FLUSH_WAIT, start);
        direct_trace(decoder, "parse eos result=%d(%s) display_ready=%d",
                     (int)result, cuda_name(result), decoder->display_ready);
        if (!cuda_ok(result) || !decoder->display_ready) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID produced no decoded picture");
        goto cleanup;
        }
    }
    memset(&proc, 0, sizeof(proc));
    proc.progressive_frame = 1;
    {
        start = csharp_direct_now_ns();
        CUresult result = cuvidMapVideoFrame(decoder->decoder, decoder->display_index,
                                              &mapped, &pitch, &proc);
        direct_timing_record(CSHARP_DIRECT_STAGE_MAP, start);
        direct_trace(decoder, "map picture=%d result=%d(%s) pitch=%u",
                     decoder->display_index, (int)result, cuda_name(result), pitch);
        if (!cuda_ok(result)) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID surface mapping failed");
        goto cleanup;
        }
    }
    err = make_image(decoder, mapped, pitch, out_image);
    {
        start = csharp_direct_now_ns();
        CUresult result = cuvidUnmapVideoFrame(decoder->decoder, mapped);
        direct_timing_record(CSHARP_DIRECT_STAGE_UNMAP, start);
        direct_trace(decoder, "unmap result=%d(%s)", (int)result, cuda_name(result));
        if (!cuda_ok(result) && err.code == heif_error_Ok) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID surface unmapping failed");
        }
    }
cleanup:
    if (context_pushed) {
        (void)cuCtxPopCurrent(&previous);
    }
    free(annexb);
    if (err.code != heif_error_Ok && out_image && *out_image) {
        heif_image_release(*out_image);
        *out_image = NULL;
    }
    if (err.code == heif_error_Ok) {
        atomic_fetch_add_explicit(&direct_decodes, 1, memory_order_relaxed);
        direct_timing_record(CSHARP_DIRECT_STAGE_TOTAL, total_start);
    }
    return err;
}

static const struct heif_decoder_plugin plugin = {
    .plugin_api_version = 3,
    .get_plugin_name = plugin_name,
    .init_plugin = NULL,
    .deinit_plugin = deinit_plugin,
    .does_support_format = supports_format,
    .new_decoder = new_decoder,
    .free_decoder = free_decoder,
    .push_data = push_data,
    .decode_image = decode_image,
    .set_strict_decoding = NULL,
    .id_name = "csharp-direct-nvdec"
};

struct heif_error csharp_register_direct_nvdec_plugin(void)
{
    return heif_register_decoder_plugin(&plugin);
}

void csharp_direct_nvdec_reset_stats(void)
{
    atomic_store_explicit(&direct_lane_creates, 0, memory_order_relaxed);
    atomic_store_explicit(&direct_lane_reuses, 0, memory_order_relaxed);
    atomic_store_explicit(&direct_decoder_creates, 0, memory_order_relaxed);
    atomic_store_explicit(&direct_decoder_reconfigures, 0, memory_order_relaxed);
    atomic_store_explicit(&direct_decoder_bucket_grows, 0, memory_order_relaxed);
    atomic_store_explicit(&direct_decodes, 0, memory_order_relaxed);
}

void csharp_direct_nvdec_get_stats(struct csharp_direct_nvdec_stats *stats)
{
    if (!stats) return;
    stats->lane_creates = atomic_load_explicit(&direct_lane_creates, memory_order_relaxed);
    stats->lane_reuses = atomic_load_explicit(&direct_lane_reuses, memory_order_relaxed);
    stats->decoder_creates = atomic_load_explicit(&direct_decoder_creates, memory_order_relaxed);
    stats->decoder_reconfigures = atomic_load_explicit(&direct_decoder_reconfigures, memory_order_relaxed);
    stats->decoder_bucket_grows = atomic_load_explicit(&direct_decoder_bucket_grows, memory_order_relaxed);
    stats->decodes = atomic_load_explicit(&direct_decodes, memory_order_relaxed);
}
