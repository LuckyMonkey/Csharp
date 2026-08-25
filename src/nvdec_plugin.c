#define _POSIX_C_SOURCE 200809L

#include "nvdec_plugin.h"

#include <libheif/heif_plugin.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CSHARP_NVDEC_MAX_INPUT (256U * 1024U * 1024U)
#define CSHARP_NVDEC_CODEC_POOL_SIZE 32

struct nvdec_decoder {
    AVCodecContext *codec;
    AVBufferRef *device_ctx;
    uint8_t *input;
    size_t input_size;
    size_t input_capacity;
    int strict;
    int pooled_codec;
    int pooled_slot;
    enum csharp_nvdec_backend backend;
};

struct pooled_codec {
    AVCodecContext *codec;
    uint64_t parameter_key;
    enum csharp_nvdec_backend backend;
    int state; /* 0 empty, 1 constructing, 2 ready/in-use, 3 ready/free */
};

static AVBufferRef *shared_cuda_device;
static struct pooled_codec codec_pool[CSHARP_NVDEC_CODEC_POOL_SIZE];
static atomic_flag shared_lock = ATOMIC_FLAG_INIT;
static atomic_ulong cuda_device_initializations;
static atomic_ulong decoder_initializations;
static atomic_ulong decoder_reuses;
static atomic_ulong decode_count;
static atomic_ullong acquire_ns;
static atomic_ullong annexb_ns;
static atomic_ullong decode_ns;
static atomic_ullong transfer_ns;
static atomic_ullong output_ns;
static atomic_ullong total_ns;
static atomic_int selected_backend = CSHARP_NVDEC_BACKEND_CUVID;
static int verbose_logging = 1;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void lock_shared_state(void)
{
    while (atomic_flag_test_and_set_explicit(&shared_lock, memory_order_acquire)) {
    }
}

static void unlock_shared_state(void)
{
    atomic_flag_clear_explicit(&shared_lock, memory_order_release);
}

static const char *plugin_name(void)
{
    return "Csharp NVDEC HEVC decoder";
}

static int supports_format(enum heif_compression_format format)
{
    return format == heif_compression_HEVC ? 1000 : 0;
}

static struct heif_error ok_error(void)
{
    return (struct heif_error){heif_error_Ok, heif_suberror_Unspecified, "ok"};
}

static struct heif_error plugin_error(enum heif_error_code code,
                                      enum heif_suberror_code subcode,
                                      const char *message)
{
    return (struct heif_error){code, subcode, message};
}

const char *csharp_nvdec_backend_name(enum csharp_nvdec_backend backend)
{
    switch (backend) {
    case CSHARP_NVDEC_BACKEND_NATIVE: return "native";
    case CSHARP_NVDEC_BACKEND_AUTO: return "auto";
    case CSHARP_NVDEC_BACKEND_CUVID:
    default: return "cuvid";
    }
}

void csharp_nvdec_set_backend(enum csharp_nvdec_backend backend)
{
    atomic_store_explicit(&selected_backend, (int)backend, memory_order_relaxed);
}

enum csharp_nvdec_backend csharp_nvdec_get_backend(void)
{
    return (enum csharp_nvdec_backend)atomic_load_explicit(&selected_backend, memory_order_relaxed);
}

static enum AVPixelFormat choose_cuda_format(AVCodecContext *codec,
                                             const enum AVPixelFormat *formats)
{
    (void)codec;
    for (const enum AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA) {
            if (verbose_logging) fprintf(stderr, "csharp-nvdec: selected CUDA frame format\n");
            return *p;
        }
    }
    if (verbose_logging) fprintf(stderr, "csharp-nvdec: CUDA frame format unavailable\n");
    return AV_PIX_FMT_NONE;
}

static const AVCodec *find_backend_codec(enum csharp_nvdec_backend backend)
{
    if (backend == CSHARP_NVDEC_BACKEND_CUVID) {
        return avcodec_find_decoder_by_name("hevc_cuvid");
    }
    return avcodec_find_decoder(AV_CODEC_ID_HEVC);
}

static AVCodecContext *create_codec_context(AVBufferRef *device_ctx,
                                            enum csharp_nvdec_backend backend)
{
    const AVCodec *codec = find_backend_codec(backend);
    AVCodecContext *context;

    if (!codec) return NULL;
    context = avcodec_alloc_context3(codec);
    if (!context) return NULL;
    context->get_format = choose_cuda_format;
    context->pkt_timebase = (AVRational){1, 90000};
    context->hw_device_ctx = av_buffer_ref(device_ctx);
    if (!context->hw_device_ctx || avcodec_open2(context, codec, NULL) < 0) {
        avcodec_free_context(&context);
        return NULL;
    }
    atomic_fetch_add_explicit(&decoder_initializations, 1, memory_order_relaxed);
    return context;
}

static uint64_t parameter_set_key(const uint8_t *input, size_t input_size)
{
    const uint64_t offset = UINT64_C(14695981039346656037);
    const uint64_t prime = UINT64_C(1099511628211);
    uint64_t hash = offset;
    size_t pos = 0;
    unsigned int parameter_sets = 0;

    while (pos + 4 <= input_size) {
        uint32_t nal_size = ((uint32_t)input[pos] << 24) |
                            ((uint32_t)input[pos + 1] << 16) |
                            ((uint32_t)input[pos + 2] << 8) |
                            (uint32_t)input[pos + 3];
        const uint8_t *nal;
        pos += 4;
        if (!nal_size || nal_size > input_size - pos) break;
        nal = input + pos;
        if (nal_size >= 2) {
            unsigned int nal_type = (nal[0] >> 1) & 0x3fU;
            if (nal_type >= 32 && nal_type <= 34) {
                for (size_t i = 0; i < nal_size; ++i) {
                    hash ^= nal[i];
                    hash *= prime;
                }
                ++parameter_sets;
            }
        }
        pos += nal_size;
    }

    if (!parameter_sets) {
        size_t fallback_size = input_size < 1024 ? input_size : 1024;
        for (size_t i = 0; i < fallback_size; ++i) {
            hash ^= input[i];
            hash *= prime;
        }
    }
    return hash ^ (uint64_t)parameter_sets;
}

static int acquire_codec(struct nvdec_decoder *decoder)
{
    uint64_t key;
    int free_slot = -1;
    enum csharp_nvdec_backend backend;
    AVCodecContext *created = NULL;

    if (!decoder || !decoder->input_size) return -1;
    key = parameter_set_key(decoder->input, decoder->input_size);
    backend = csharp_nvdec_get_backend();
    if (backend == CSHARP_NVDEC_BACKEND_AUTO) backend = CSHARP_NVDEC_BACKEND_NATIVE;

retry:
    lock_shared_state();
    free_slot = -1;
    for (int i = 0; i < CSHARP_NVDEC_CODEC_POOL_SIZE; ++i) {
        if (codec_pool[i].state == 3 && codec_pool[i].codec &&
            codec_pool[i].parameter_key == key && codec_pool[i].backend == backend) {
            codec_pool[i].state = 2;
            decoder->codec = codec_pool[i].codec;
            decoder->pooled_codec = 1;
            decoder->pooled_slot = i;
            decoder->backend = backend;
            avcodec_flush_buffers(decoder->codec);
            atomic_fetch_add_explicit(&decoder_reuses, 1, memory_order_relaxed);
            unlock_shared_state();
            return 0;
        }
        if (codec_pool[i].state == 0 && free_slot < 0) free_slot = i;
    }

    if (free_slot >= 0) {
        codec_pool[free_slot].state = 1;
        codec_pool[free_slot].parameter_key = key;
        codec_pool[free_slot].backend = backend;
        unlock_shared_state();

        created = create_codec_context(decoder->device_ctx, backend);
        if (!created && csharp_nvdec_get_backend() == CSHARP_NVDEC_BACKEND_AUTO &&
            backend == CSHARP_NVDEC_BACKEND_NATIVE) {
            lock_shared_state();
            codec_pool[free_slot].state = 0;
            unlock_shared_state();
            backend = CSHARP_NVDEC_BACKEND_CUVID;
            goto retry;
        }

        lock_shared_state();
        if (!created) {
            codec_pool[free_slot].state = 0;
            unlock_shared_state();
            return -1;
        }
        codec_pool[free_slot].codec = created;
        codec_pool[free_slot].state = 2;
        decoder->codec = created;
        decoder->pooled_codec = 1;
        decoder->pooled_slot = free_slot;
        decoder->backend = backend;
        unlock_shared_state();
        return 0;
    }
    unlock_shared_state();

    created = create_codec_context(decoder->device_ctx, backend);
    if (!created && csharp_nvdec_get_backend() == CSHARP_NVDEC_BACKEND_AUTO &&
        backend == CSHARP_NVDEC_BACKEND_NATIVE) {
        backend = CSHARP_NVDEC_BACKEND_CUVID;
        created = create_codec_context(decoder->device_ctx, backend);
    }
    if (!created) return -1;
    decoder->codec = created;
    decoder->pooled_codec = 0;
    decoder->pooled_slot = -1;
    decoder->backend = backend;
    return 0;
}

static struct heif_error new_decoder(void **out_decoder)
{
    struct nvdec_decoder *decoder = calloc(1, sizeof(*decoder));

    if (!decoder) {
        return plugin_error(heif_error_Memory_allocation_error,
                            heif_suberror_Unspecified, "decoder allocation failed");
    }

    lock_shared_state();
    if (!shared_cuda_device) {
        if (av_hwdevice_ctx_create(&shared_cuda_device, AV_HWDEVICE_TYPE_CUDA,
                                   NULL, NULL, 0) < 0) {
            unlock_shared_state();
            free(decoder);
            return plugin_error(heif_error_Unsupported_feature,
                                heif_suberror_Unsupported_codec,
                                "CUDA device initialization failed");
        }
        atomic_fetch_add_explicit(&cuda_device_initializations, 1, memory_order_relaxed);
    }
    decoder->device_ctx = av_buffer_ref(shared_cuda_device);
    unlock_shared_state();

    if (!decoder->device_ctx) {
        free(decoder);
        return plugin_error(heif_error_Memory_allocation_error,
                            heif_suberror_Unspecified, "CUDA device reference failed");
    }

    decoder->pooled_slot = -1;
    *out_decoder = decoder;
    return ok_error();
}

static void free_decoder(void *raw_decoder)
{
    struct nvdec_decoder *decoder = raw_decoder;
    if (!decoder) return;

    if (decoder->pooled_codec && decoder->pooled_slot >= 0) {
        avcodec_flush_buffers(decoder->codec);
        lock_shared_state();
        codec_pool[decoder->pooled_slot].state = 3;
        unlock_shared_state();
    } else {
        avcodec_free_context(&decoder->codec);
    }

    av_buffer_unref(&decoder->device_ctx);
    free(decoder->input);
    free(decoder);
}

static struct heif_error push_data(void *raw_decoder, const void *data, size_t size)
{
    struct nvdec_decoder *decoder = raw_decoder;
    size_t needed;

    if (!decoder || (!data && size)) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_Unspecified,
                            "invalid decoder input");
    }
    if (size > CSHARP_NVDEC_MAX_INPUT - decoder->input_size) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "HEVC input exceeds safety limit");
    }

    needed = decoder->input_size + size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (needed > decoder->input_capacity) {
        size_t capacity = decoder->input_capacity ? decoder->input_capacity : 4096;
        while (capacity < needed) {
            if (capacity > (CSHARP_NVDEC_MAX_INPUT + AV_INPUT_BUFFER_PADDING_SIZE) / 2) {
                capacity = CSHARP_NVDEC_MAX_INPUT + AV_INPUT_BUFFER_PADDING_SIZE;
                break;
            }
            capacity *= 2;
        }
        uint8_t *grown = realloc(decoder->input, capacity);
        if (!grown) {
            return plugin_error(heif_error_Memory_allocation_error,
                                heif_suberror_Unspecified, "HEVC input allocation failed");
        }
        decoder->input = grown;
        decoder->input_capacity = capacity;
    }
    memcpy(decoder->input + decoder->input_size, data, size);
    decoder->input_size += size;
    memset(decoder->input + decoder->input_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return ok_error();
}

static int convert_annexb_in_place(uint8_t *input, size_t input_size)
{
    size_t pos = 0;
    if (!input || input_size < 4) return -1;
    while (pos < input_size) {
        uint32_t nal_size;
        if (input_size - pos < 4) return -1;
        nal_size = ((uint32_t)input[pos] << 24) |
                   ((uint32_t)input[pos + 1] << 16) |
                   ((uint32_t)input[pos + 2] << 8) |
                   (uint32_t)input[pos + 3];
        if (!nal_size || nal_size > input_size - pos - 4) return -1;
        input[pos] = 0;
        input[pos + 1] = 0;
        input[pos + 2] = 0;
        input[pos + 3] = 1;
        pos += 4 + (size_t)nal_size;
    }
    return pos == input_size ? 0 : -1;
}

static struct heif_error create_output_image(const AVFrame *frame,
                                             struct heif_image **out_image,
                                             uint8_t **planes,
                                             int *strides)
{
    struct heif_error err;
    const enum heif_channel channels[3] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
    int widths[3] = {frame->width, (frame->width + 1) / 2, (frame->width + 1) / 2};
    int heights[3] = {frame->height, (frame->height + 1) / 2, (frame->height + 1) / 2};

    err = heif_image_create(frame->width, frame->height,
                            heif_colorspace_YCbCr, heif_chroma_420, out_image);
    if (err.code != heif_error_Ok) return err;
    for (int i = 0; i < 3; ++i) {
        err = heif_image_add_plane(*out_image, channels[i], widths[i], heights[i], 8);
        if (err.code != heif_error_Ok) return err;
        planes[i] = heif_image_get_plane(*out_image, channels[i], &strides[i]);
        if (!planes[i]) {
            return plugin_error(heif_error_Decoder_plugin_error,
                                heif_suberror_Unspecified, "cannot access HEIF output plane");
        }
    }
    return ok_error();
}

static struct heif_error output_frame(const AVFrame *frame, struct heif_image **out_image)
{
    struct SwsContext *scaler = NULL;
    uint8_t *dst[4] = {0};
    int dst_stride[4] = {0};
    struct heif_error err = create_output_image(frame, out_image, dst, dst_stride);
    if (err.code != heif_error_Ok) return err;

    if (frame->format == AV_PIX_FMT_NV12) {
        int cw = (frame->width + 1) / 2;
        int ch = (frame->height + 1) / 2;
        for (int y = 0; y < frame->height; ++y) {
            memcpy(dst[0] + (size_t)y * (size_t)dst_stride[0],
                   frame->data[0] + (size_t)y * (size_t)frame->linesize[0],
                   (size_t)frame->width);
        }
        for (int y = 0; y < ch; ++y) {
            const uint8_t *src = frame->data[1] + (size_t)y * (size_t)frame->linesize[1];
            uint8_t *cb = dst[1] + (size_t)y * (size_t)dst_stride[1];
            uint8_t *cr = dst[2] + (size_t)y * (size_t)dst_stride[2];
            for (int x = 0; x < cw; ++x) {
                cb[x] = src[2 * x];
                cr[x] = src[2 * x + 1];
            }
        }
        return ok_error();
    }

    scaler = sws_getContext(frame->width, frame->height, frame->format,
                            frame->width, frame->height, AV_PIX_FMT_YUV420P,
                            SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!scaler || sws_scale(scaler, (const uint8_t *const *)frame->data,
                             frame->linesize, 0, frame->height,
                             dst, dst_stride) <= 0) {
        sws_freeContext(scaler);
        return plugin_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unsupported_color_conversion,
                            "YUV conversion failed");
    }
    sws_freeContext(scaler);
    return ok_error();
}

static struct heif_error decode_image(void *raw_decoder, struct heif_image **out_image)
{
    struct nvdec_decoder *decoder = raw_decoder;
    AVPacket packet = {0};
    AVFrame *decoded = NULL;
    AVFrame *downloaded = NULL;
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
    int ret;
    struct heif_error err = ok_error();

    if (!decoder || !out_image || !decoder->input_size) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_No_item_data,
                            "empty HEVC image data");
    }
    *out_image = NULL;
    t0 = now_ns();
    t1 = t0;

    t1 = now_ns();
    if (acquire_codec(decoder) < 0) {
        return plugin_error(heif_error_Unsupported_feature,
                            heif_suberror_Unsupported_codec,
                            "cannot acquire NVDEC decoder");
    }
    t2 = now_ns();

    if (convert_annexb_in_place(decoder->input, decoder->input_size) < 0) {
        err = plugin_error(heif_error_Invalid_input, heif_suberror_End_of_data,
                           "invalid length-prefixed HEVC stream");
        goto cleanup;
    }
    memset(decoder->input + decoder->input_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    t3 = now_ns();

    if (decoder->input_size > INT_MAX) {
        err = plugin_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                           "HEVC packet is too large");
        goto cleanup;
    }

    decoded = av_frame_alloc();
    downloaded = av_frame_alloc();
    if (!decoded || !downloaded) {
        err = plugin_error(heif_error_Memory_allocation_error,
                           heif_suberror_Unspecified, "FFmpeg frame allocation failed");
        goto cleanup;
    }

    packet.data = decoder->input;
    packet.size = (int)decoder->input_size;
    ret = avcodec_send_packet(decoder->codec, &packet);
    if (ret >= 0) {
        ret = avcodec_receive_frame(decoder->codec, decoded);
        if (ret == AVERROR(EAGAIN)) {
            ret = avcodec_send_packet(decoder->codec, NULL);
            if (ret >= 0) ret = avcodec_receive_frame(decoder->codec, decoded);
        }
    }
    if (ret < 0) {
        err = plugin_error(heif_error_Decoder_plugin_error,
                           heif_suberror_Unspecified, "NVDEC rejected HEVC packet");
        goto cleanup;
    }
    t4 = now_ns();

    if (decoded->format == AV_PIX_FMT_CUDA) {
        ret = av_hwframe_transfer_data(downloaded, decoded, 0);
        if (ret < 0) {
            err = plugin_error(heif_error_Decoder_plugin_error,
                               heif_suberror_Unspecified, "CUDA frame download failed");
            goto cleanup;
        }
    } else if ((ret = av_frame_ref(downloaded, decoded)) < 0) {
        err = plugin_error(heif_error_Decoder_plugin_error,
                           heif_suberror_Unspecified, "decoded frame reference failed");
        goto cleanup;
    }
    t5 = now_ns();

    err = output_frame(downloaded, out_image);
    if (err.code != heif_error_Ok) goto cleanup;

    if (verbose_logging) {
        fprintf(stderr, "csharp-nvdec: backend=%s format=%s size=%dx%d\n",
                csharp_nvdec_backend_name(decoder->backend),
                av_get_pix_fmt_name(downloaded->format),
                downloaded->width, downloaded->height);
    }

cleanup:
    {
        uint64_t end = now_ns();
        if (t2 >= t1) atomic_fetch_add_explicit(&acquire_ns, t2 - t1, memory_order_relaxed);
        if (t3 >= t2) atomic_fetch_add_explicit(&annexb_ns, t3 - t2, memory_order_relaxed);
        if (t4 >= t3) atomic_fetch_add_explicit(&decode_ns, t4 - t3, memory_order_relaxed);
        if (t5 >= t4) atomic_fetch_add_explicit(&transfer_ns, t5 - t4, memory_order_relaxed);
        if (end >= t5) atomic_fetch_add_explicit(&output_ns, end - t5, memory_order_relaxed);
        atomic_fetch_add_explicit(&total_ns, end - t0, memory_order_relaxed);
        atomic_fetch_add_explicit(&decode_count, 1, memory_order_relaxed);
    }
    if (err.code != heif_error_Ok && out_image && *out_image) {
        heif_image_release(*out_image);
        *out_image = NULL;
    }
    av_frame_free(&downloaded);
    av_frame_free(&decoded);
    return err;
}

static void set_strict_decoding(void *raw_decoder, int strict)
{
    struct nvdec_decoder *decoder = raw_decoder;
    if (decoder) decoder->strict = strict;
}

static const struct heif_decoder_plugin nvdec_plugin = {
    .plugin_api_version = 3,
    .get_plugin_name = plugin_name,
    .init_plugin = NULL,
    .deinit_plugin = NULL,
    .does_support_format = supports_format,
    .new_decoder = new_decoder,
    .free_decoder = free_decoder,
    .push_data = push_data,
    .decode_image = decode_image,
    .set_strict_decoding = set_strict_decoding,
    .id_name = "csharp-nvdec"
};

struct heif_error csharp_register_nvdec_plugin(void)
{
    return heif_register_decoder_plugin(&nvdec_plugin);
}

void csharp_nvdec_set_verbose(int enabled)
{
    verbose_logging = enabled != 0;
}

void csharp_nvdec_reset_stats(void)
{
    atomic_store_explicit(&cuda_device_initializations, 0, memory_order_relaxed);
    atomic_store_explicit(&decoder_initializations, 0, memory_order_relaxed);
    atomic_store_explicit(&decoder_reuses, 0, memory_order_relaxed);
    atomic_store_explicit(&decode_count, 0, memory_order_relaxed);
    atomic_store_explicit(&acquire_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&annexb_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&decode_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&transfer_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&output_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&total_ns, 0, memory_order_relaxed);
}

void csharp_nvdec_get_stats(struct csharp_nvdec_stats *stats)
{
    if (!stats) return;
    stats->cuda_device_initializations = atomic_load_explicit(&cuda_device_initializations, memory_order_relaxed);
    stats->decoder_initializations = atomic_load_explicit(&decoder_initializations, memory_order_relaxed);
    stats->decoder_reuses = atomic_load_explicit(&decoder_reuses, memory_order_relaxed);
    stats->decodes = atomic_load_explicit(&decode_count, memory_order_relaxed);
    stats->acquire_ms = (double)atomic_load_explicit(&acquire_ns, memory_order_relaxed) / 1000000.0;
    stats->annexb_ms = (double)atomic_load_explicit(&annexb_ns, memory_order_relaxed) / 1000000.0;
    stats->decode_ms = (double)atomic_load_explicit(&decode_ns, memory_order_relaxed) / 1000000.0;
    stats->transfer_ms = (double)atomic_load_explicit(&transfer_ns, memory_order_relaxed) / 1000000.0;
    stats->output_ms = (double)atomic_load_explicit(&output_ns, memory_order_relaxed) / 1000000.0;
    stats->total_ms = (double)atomic_load_explicit(&total_ns, memory_order_relaxed) / 1000000.0;
}

unsigned long csharp_nvdec_cuda_device_initializations(void)
{
    return atomic_load_explicit(&cuda_device_initializations, memory_order_relaxed);
}

unsigned long csharp_nvdec_decoder_initializations(void)
{
    return atomic_load_explicit(&decoder_initializations, memory_order_relaxed);
}

unsigned long csharp_nvdec_decoder_reuses(void)
{
    return atomic_load_explicit(&decoder_reuses, memory_order_relaxed);
}
