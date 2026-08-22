#define _POSIX_C_SOURCE 200809L

#include "direct_nvdec_plugin.h"

#include <cuda.h>
#include <nvcuvid.h>
#include <libheif/heif_plugin.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
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
    CUvideotimestamp next_timestamp;
};

static struct direct_decoder direct_lanes[DIRECT_LANE_COUNT];
static atomic_flag direct_lane_lock = ATOMIC_FLAG_INIT;
static atomic_ulong direct_lane_creates;
static atomic_ulong direct_lane_reuses;
static atomic_ulong direct_decoder_creates;
static atomic_ulong direct_decoder_reconfigures;
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

static void lock_lanes(void)
{
    while (atomic_flag_test_and_set_explicit(&direct_lane_lock, memory_order_acquire)) {
    }
}

static void unlock_lanes(void)
{
    atomic_flag_clear_explicit(&direct_lane_lock, memory_order_release);
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
    unsigned int surfaces;

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

    memset(&info, 0, sizeof(info));
    info.ulWidth = format->coded_width;
    info.ulHeight = format->coded_height;
    info.ulNumDecodeSurfaces = surfaces;
    info.CodecType = cudaVideoCodec_HEVC;
    info.ChromaFormat = cudaVideoChromaFormat_420;
    info.ulCreationFlags = cudaVideoCreate_Default;
    info.bitDepthMinus8 = 0;
    info.ulIntraDecodeOnly = 0;
    info.ulMaxWidth = DIRECT_MAX_WIDTH;
    info.ulMaxHeight = DIRECT_MAX_HEIGHT;
    info.display_area.left = 0;
    info.display_area.top = 0;
    info.display_area.right = (short)format->coded_width;
    info.display_area.bottom = (short)format->coded_height;
    info.OutputFormat = cudaVideoSurfaceFormat_NV12;
    info.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    info.ulTargetWidth = format->coded_width;
    info.ulTargetHeight = format->coded_height;
    info.ulNumOutputSurfaces = 2;

    if (decoder->decoder_created) {
        CUVIDRECONFIGUREDECODERINFO reconfigure;
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
        if (!cuda_ok(cuvidReconfigureDecoder(decoder->decoder, &reconfigure))) return 0;
        atomic_fetch_add_explicit(&direct_decoder_reconfigures, 1, memory_order_relaxed);
    } else {
        if (!cuda_ok(cuvidCreateDecoder(&decoder->decoder, &info))) return 0;
        decoder->decoder_created = 1;
        atomic_fetch_add_explicit(&direct_decoder_creates, 1, memory_order_relaxed);
    }
    decoder->sequence_seen = 1;
    return (int)surfaces;
}

static int decode_callback(void *opaque, CUVIDPICPARAMS *picture)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder || !decoder->decoder_created || !picture) return 0;
    return cuda_ok(cuvidDecodePicture(decoder->decoder, picture)) ? 1 : 0;
}

static int display_callback(void *opaque, CUVIDPARSERDISPINFO *display)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder || !display) return 1;
    decoder->display_index = display->picture_index;
    decoder->display_ready = 1;
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

    if (!out_decoder) {
        return direct_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "missing decoder output");
    }
    for (int attempt = 0; attempt < DIRECT_LANE_COUNT; ++attempt) {
        decoder = NULL;
        lock_lanes();
        for (int i = 0; i < DIRECT_LANE_COUNT; ++i) {
            if (!direct_lanes[i].busy) {
                decoder = &direct_lanes[i];
                was_initialized = decoder->initialized;
                decoder->busy = 1;
                decoder->input_size = 0;
                decoder->display_ready = 0;
                decoder->has_decoded = 0;
                break;
            }
        }
        unlock_lanes();
        if (!decoder) break;
        if (was_initialized || initialize_lane(decoder) == 0) {
            if (was_initialized) {
                atomic_fetch_add_explicit(&direct_lane_reuses, 1, memory_order_relaxed);
            }
            *out_decoder = decoder;
            return direct_ok();
        }
        lock_lanes();
        decoder->busy = 0;
        unlock_lanes();
    }
    return direct_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec,
                        "direct NVDECODE lane initialization failed or pool exhausted");
}

static void free_decoder(void *opaque)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder) return;
    decoder->input_size = 0;
    decoder->display_ready = 0;
    lock_lanes();
    decoder->busy = 0;
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

    if (getenv("CSHARP_DIRECT_GEOMETRY_DEBUG")) {
        fprintf(stderr, "direct mapped pitch=%u output=%dx%d luma_crop=(%d,%d %dx%d) chroma_crop=(%d,%d %dx%d)\n",
                pitch, decoder->display_width, decoder->display_height,
                decoder->display_left, decoder->display_top,
                decoder->display_width, decoder->display_height,
                decoder->display_left / 2, decoder->display_top / 2,
                (decoder->display_width + 1) / 2, (decoder->display_height + 1) / 2);
    }

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
    if (!cuda_ok(cuMemcpy2D(&copy))) {
        return direct_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unspecified,
                            "Y surface bulk copy failed");
    }

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
    if (!cuda_ok(cuMemcpy2D(&copy))) {
        return direct_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unspecified,
                            "UV surface bulk copy failed");
    }

    for (int row = 0; row < chroma_height; ++row) {
        const uint8_t *src = decoder->uv_staging + (size_t)row * uv_row_bytes;
        uint8_t *dst_cb = cb + (size_t)row * (size_t)cb_stride;
        uint8_t *dst_cr = cr + (size_t)row * (size_t)cr_stride;
        for (int col = 0; col < chroma_width; ++col) {
            dst_cb[col] = src[(size_t)col * 2U];
            dst_cr[col] = src[(size_t)col * 2U + 1U];
        }
    }

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
    if (length_prefixed_to_annexb(decoder->input, decoder->input_size,
                                  &annexb, &annexb_size) != 0) {
        err = direct_error(heif_error_Invalid_input, heif_suberror_End_of_data,
                           "invalid length-prefixed HEVC stream");
        goto cleanup;
    }
    memset(&packet, 0, sizeof(packet));
    packet.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    if (decoder->has_decoded) packet.flags |= CUVID_PKT_DISCONTINUITY;
    packet.payload_size = (unsigned long)annexb_size;
    packet.payload = annexb;
    packet.timestamp = decoder->next_timestamp++;
    decoder->display_ready = 0;
    if (!cuda_ok(cuvidParseVideoData(decoder->parser, &packet))) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID parser rejected HEVC stream");
        goto cleanup;
    }
    decoder->has_decoded = 1;
    memset(&eos, 0, sizeof(eos));
    eos.flags = CUVID_PKT_ENDOFSTREAM | CUVID_PKT_NOTIFY_EOS;
    if (!cuda_ok(cuvidParseVideoData(decoder->parser, &eos)) || !decoder->display_ready) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID produced no decoded picture");
        goto cleanup;
    }
    memset(&proc, 0, sizeof(proc));
    proc.progressive_frame = 1;
    if (!cuda_ok(cuvidMapVideoFrame(decoder->decoder, decoder->display_index,
                                    &mapped, &pitch, &proc))) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID surface mapping failed");
        goto cleanup;
    }
    err = make_image(decoder, mapped, pitch, out_image);
    if (!cuda_ok(cuvidUnmapVideoFrame(decoder->decoder, mapped)) &&
        err.code == heif_error_Ok) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID surface unmapping failed");
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
    atomic_store_explicit(&direct_decodes, 0, memory_order_relaxed);
}

void csharp_direct_nvdec_get_stats(struct csharp_direct_nvdec_stats *stats)
{
    if (!stats) return;
    stats->lane_creates = atomic_load_explicit(&direct_lane_creates, memory_order_relaxed);
    stats->lane_reuses = atomic_load_explicit(&direct_lane_reuses, memory_order_relaxed);
    stats->decoder_creates = atomic_load_explicit(&direct_decoder_creates, memory_order_relaxed);
    stats->decoder_reconfigures = atomic_load_explicit(&direct_decoder_reconfigures, memory_order_relaxed);
    stats->decodes = atomic_load_explicit(&direct_decodes, memory_order_relaxed);
}
