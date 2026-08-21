#include "nvdec_plugin.h"

#include <libheif/heif_plugin.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSHARP_NVDEC_MAX_INPUT (256U * 1024U * 1024U)

struct nvdec_decoder {
    AVCodecContext *codec;
    AVBufferRef *device_ctx;
    uint8_t *input;
    size_t input_size;
    size_t input_capacity;
    int strict;
    int pooled_codec;
};

static AVBufferRef *shared_cuda_device;
static AVCodecContext *shared_codec;
static int shared_codec_in_use;
static atomic_flag shared_lock = ATOMIC_FLAG_INIT;
static unsigned long cuda_device_initializations;
static unsigned long decoder_initializations;
static unsigned long decoder_reuses;
static int verbose_logging = 1;

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

static AVCodecContext *create_codec_context(AVBufferRef *device_ctx)
{
    const AVCodec *codec = avcodec_find_decoder_by_name("hevc_cuvid");
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
    ++decoder_initializations;
    return context;
}

static struct heif_error new_decoder(void **out_decoder)
{
    struct nvdec_decoder *decoder = calloc(1, sizeof(*decoder));
    AVCodecContext *codec = NULL;

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
        ++cuda_device_initializations;
    }

    decoder->device_ctx = av_buffer_ref(shared_cuda_device);
    if (!decoder->device_ctx) {
        unlock_shared_state();
        free(decoder);
        return plugin_error(heif_error_Memory_allocation_error,
                            heif_suberror_Unspecified, "CUDA device reference failed");
    }

    if (shared_codec && !shared_codec_in_use) {
        shared_codec_in_use = 1;
        decoder->pooled_codec = 1;
        codec = shared_codec;
        avcodec_flush_buffers(codec);
        ++decoder_reuses;
    } else if (!shared_codec) {
        shared_codec = create_codec_context(decoder->device_ctx);
        if (!shared_codec) {
            av_buffer_unref(&decoder->device_ctx);
            unlock_shared_state();
            free(decoder);
            return plugin_error(heif_error_Unsupported_feature,
                                heif_suberror_Unsupported_codec,
                                "cannot open hevc_cuvid decoder");
        }
        shared_codec_in_use = 1;
        decoder->pooled_codec = 1;
        codec = shared_codec;
    }
    unlock_shared_state();

    if (!codec) {
        codec = create_codec_context(decoder->device_ctx);
        if (!codec) {
            av_buffer_unref(&decoder->device_ctx);
            free(decoder);
            return plugin_error(heif_error_Unsupported_feature,
                                heif_suberror_Unsupported_codec,
                                "cannot open temporary hevc_cuvid decoder");
        }
    }

    decoder->codec = codec;
    *out_decoder = decoder;
    if (verbose_logging) {
        fprintf(stderr, "csharp-nvdec: %s hevc_cuvid/NVDEC context\n",
                decoder->pooled_codec ? "acquired pooled" : "initialized temporary");
    }
    return ok_error();
}

static void free_decoder(void *raw_decoder)
{
    struct nvdec_decoder *decoder = raw_decoder;
    if (!decoder) return;

    if (decoder->pooled_codec) {
        avcodec_flush_buffers(decoder->codec);
        lock_shared_state();
        shared_codec_in_use = 0;
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
    if (!decoder || (!data && size)) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_Unspecified,
                            "invalid decoder input");
    }
    if (size > CSHARP_NVDEC_MAX_INPUT - decoder->input_size) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "HEVC input exceeds safety limit");
    }
    if (decoder->input_size + size > decoder->input_capacity) {
        size_t capacity = decoder->input_capacity ? decoder->input_capacity : 4096;
        while (capacity < decoder->input_size + size) {
            if (capacity > CSHARP_NVDEC_MAX_INPUT / 2) {
                capacity = CSHARP_NVDEC_MAX_INPUT;
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
    return ok_error();
}

static int make_annexb(const uint8_t *input, size_t input_size,
                       uint8_t **out_data, size_t *out_size)
{
    size_t pos = 0;
    size_t capacity = input_size + 4;
    size_t size = 0;
    uint8_t *output;
    if (!input || input_size < 4 || !out_data || !out_size) return -1;
    output = malloc(capacity);
    if (!output) return -1;
    while (pos < input_size) {
        uint32_t nal_size;
        if (input_size - pos < 4) { free(output); return -1; }
        nal_size = ((uint32_t)input[pos] << 24) |
                   ((uint32_t)input[pos + 1] << 16) |
                   ((uint32_t)input[pos + 2] << 8) |
                   (uint32_t)input[pos + 3];
        pos += 4;
        if (!nal_size || nal_size > input_size - pos) { free(output); return -1; }
        if (size > SIZE_MAX - (size_t)nal_size - 4) { free(output); return -1; }
        if (size + nal_size + 4 > capacity) {
            size_t needed = size + nal_size + 4;
            uint8_t *grown;
            capacity = needed;
            grown = realloc(output, capacity);
            if (!grown) { free(output); return -1; }
            output = grown;
        }
        output[size++] = 0; output[size++] = 0; output[size++] = 0; output[size++] = 1;
        memcpy(output + size, input + pos, nal_size);
        size += nal_size;
        pos += nal_size;
    }
    *out_data = output;
    *out_size = size;
    return 0;
}

static struct heif_error add_plane_copy(struct heif_image *image,
                                        enum heif_channel channel,
                                        const uint8_t *source, int source_stride,
                                        int width, int height)
{
    int destination_stride;
    uint8_t *destination;
    struct heif_error err = heif_image_add_plane(image, channel, width, height, 8);
    if (err.code != heif_error_Ok) return err;
    destination = heif_image_get_plane(image, channel, &destination_stride);
    if (!destination) {
        return plugin_error(heif_error_Decoder_plugin_error,
                            heif_suberror_Unspecified, "cannot access HEIF output plane");
    }
    for (int y = 0; y < height; ++y) {
        memcpy(destination + y * destination_stride, source + y * source_stride,
               (size_t)width);
    }
    return ok_error();
}

static struct heif_error decode_image(void *raw_decoder, struct heif_image **out_image)
{
    struct nvdec_decoder *decoder = raw_decoder;
    AVPacket *packet = NULL;
    AVFrame *decoded = NULL;
    AVFrame *downloaded = NULL;
    struct SwsContext *scaler = NULL;
    uint8_t *annexb = NULL;
    size_t annexb_size = 0;
    uint8_t *converted[4] = {0};
    int converted_stride[4] = {0};
    int converted_buffer_size = 0;
    int ret;
    struct heif_error err = ok_error();

    if (!decoder || !out_image || !decoder->input_size) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_No_item_data,
                            "empty HEVC image data");
    }
    *out_image = NULL;
    if (make_annexb(decoder->input, decoder->input_size, &annexb, &annexb_size) < 0) {
        return plugin_error(heif_error_Invalid_input, heif_suberror_End_of_data,
                            "invalid length-prefixed HEVC stream");
    }
    if (annexb_size > INT_MAX) {
        err = plugin_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                           "HEVC packet is too large");
        goto cleanup;
    }
    packet = av_packet_alloc(); decoded = av_frame_alloc(); downloaded = av_frame_alloc();
    if (!packet || !decoded || !downloaded) {
        err = plugin_error(heif_error_Memory_allocation_error,
                           heif_suberror_Unspecified, "FFmpeg frame allocation failed");
        goto cleanup;
    }
    packet->data = annexb; packet->size = (int)annexb_size;
    ret = avcodec_send_packet(decoder->codec, packet);
    if (ret < 0) {
        err = plugin_error(heif_error_Decoder_plugin_error,
                           heif_suberror_Unspecified, "NVDEC rejected HEVC packet");
        goto cleanup;
    }
    ret = avcodec_receive_frame(decoder->codec, decoded);
    if (ret == AVERROR(EAGAIN)) {
        ret = avcodec_send_packet(decoder->codec, NULL);
        if (ret >= 0) ret = avcodec_receive_frame(decoder->codec, decoded);
    }
    if (ret < 0) {
        err = plugin_error(heif_error_Decoder_plugin_error,
                           heif_suberror_Unspecified, "NVDEC produced no decoded frame");
        goto cleanup;
    }
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
    if (verbose_logging) {
        fprintf(stderr, "csharp-nvdec: downloaded format=%s size=%dx%d linesizes=%d,%d,%d data=%p,%p\n",
                av_get_pix_fmt_name(downloaded->format), downloaded->width, downloaded->height,
                downloaded->linesize[0], downloaded->linesize[1], downloaded->linesize[2],
                (void *)downloaded->data[0], (void *)downloaded->data[1]);
    }
    converted_buffer_size = av_image_alloc(converted, converted_stride,
                                            downloaded->width, downloaded->height,
                                            AV_PIX_FMT_YUV420P, 1);
    if (converted_buffer_size < 0) {
        err = plugin_error(heif_error_Memory_allocation_error,
                           heif_suberror_Unspecified, "YUV output allocation failed");
        goto cleanup;
    }
    if (downloaded->format == AV_PIX_FMT_NV12) {
        int chroma_width = (downloaded->width + 1) / 2;
        int chroma_height = (downloaded->height + 1) / 2;
        for (int y = 0; y < downloaded->height; ++y) {
            memcpy(converted[0] + y * converted_stride[0],
                   downloaded->data[0] + y * downloaded->linesize[0],
                   (size_t)downloaded->width);
        }
        for (int y = 0; y < chroma_height; ++y) {
            const uint8_t *src = downloaded->data[1] + y * downloaded->linesize[1];
            uint8_t *cb = converted[1] + y * converted_stride[1];
            uint8_t *cr = converted[2] + y * converted_stride[2];
            for (int x = 0; x < chroma_width; ++x) {
                cb[x] = src[2 * x];
                cr[x] = src[2 * x + 1];
            }
        }
    } else {
        scaler = sws_getContext(downloaded->width, downloaded->height, downloaded->format,
                                downloaded->width, downloaded->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, NULL, NULL, NULL);
        if (!scaler || sws_scale(scaler, (const uint8_t *const *)downloaded->data,
                                 downloaded->linesize, 0, downloaded->height,
                                 converted, converted_stride) <= 0) {
            err = plugin_error(heif_error_Decoder_plugin_error,
                               heif_suberror_Unsupported_color_conversion,
                               "YUV conversion failed");
            goto cleanup;
        }
    }
    err = heif_image_create(downloaded->width, downloaded->height,
                            heif_colorspace_YCbCr, heif_chroma_420, out_image);
    if (err.code != heif_error_Ok) goto cleanup;
    err = add_plane_copy(*out_image, heif_channel_Y, converted[0], converted_stride[0],
                         downloaded->width, downloaded->height);
    if (err.code != heif_error_Ok) goto cleanup;
    err = add_plane_copy(*out_image, heif_channel_Cb, converted[1], converted_stride[1],
                         (downloaded->width + 1) / 2, (downloaded->height + 1) / 2);
    if (err.code != heif_error_Ok) goto cleanup;
    err = add_plane_copy(*out_image, heif_channel_Cr, converted[2], converted_stride[2],
                         (downloaded->width + 1) / 2, (downloaded->height + 1) / 2);
    if (err.code != heif_error_Ok) goto cleanup;
    if (verbose_logging) {
        fprintf(stderr, "csharp-nvdec: decoded %dx%d through CUDA\n",
                downloaded->width, downloaded->height);
    }

cleanup:
    if (err.code != heif_error_Ok && out_image && *out_image) {
        heif_image_release(*out_image); *out_image = NULL;
    }
    if (converted_buffer_size > 0) av_freep(&converted[0]);
    sws_freeContext(scaler); av_frame_free(&downloaded); av_frame_free(&decoded);
    av_packet_free(&packet); free(annexb);
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
    cuda_device_initializations = 0;
    decoder_initializations = 0;
    decoder_reuses = 0;
}

unsigned long csharp_nvdec_cuda_device_initializations(void)
{
    return cuda_device_initializations;
}

unsigned long csharp_nvdec_decoder_initializations(void)
{
    return decoder_initializations;
}

unsigned long csharp_nvdec_decoder_reuses(void)
{
    return decoder_reuses;
}
