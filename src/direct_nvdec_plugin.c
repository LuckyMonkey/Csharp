#define _POSIX_C_SOURCE 200809L

#include "direct_nvdec_plugin.h"

#include <cuda.h>
#include <nvcuvid.h>
#include <libheif/heif_plugin.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DIRECT_MAX_INPUT (256U * 1024U * 1024U)
#define DIRECT_MAX_WIDTH 8192U
#define DIRECT_MAX_HEIGHT 8192U

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
};

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
    info.display_area.left = (short)format->display_area.left;
    info.display_area.top = (short)format->display_area.top;
    info.display_area.right = (short)format->display_area.right;
    info.display_area.bottom = (short)format->display_area.bottom;
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
        reconfigure.target_rect.left = info.target_rect.left;
        reconfigure.target_rect.top = info.target_rect.top;
        reconfigure.target_rect.right = info.target_rect.right;
        reconfigure.target_rect.bottom = info.target_rect.bottom;
        if (!cuda_ok(cuvidReconfigureDecoder(decoder->decoder, &reconfigure))) return 0;
    } else {
        if (!cuda_ok(cuvidCreateDecoder(&decoder->decoder, &info))) return 0;
        decoder->decoder_created = 1;
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
    CUVIDPARSERPARAMS params;

    if (!out_decoder) {
        return direct_error(heif_error_Invalid_input, heif_suberror_Invalid_parameter_value,
                            "missing decoder output");
    }
    decoder = calloc(1, sizeof(*decoder));
    if (!decoder) {
        return direct_error(heif_error_Memory_allocation_error, heif_suberror_Unspecified,
                            "direct decoder allocation failed");
    }
    decoder->display_index = -1;
    if (!cuda_ok(cuInit(0))) {
        free(decoder);
        return direct_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec,
                            "CUDA device initialization failed");
    }
    {
        CUdevice device;
        if (!cuda_ok(cuDeviceGet(&device, 0)) ||
            !cuda_ok(cuCtxCreate(&decoder->context, NULL, CU_CTX_SCHED_AUTO, device))) {
            free(decoder);
            return direct_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec,
                                "CUDA context creation failed");
        }
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
        free(decoder);
        return direct_error(heif_error_Unsupported_feature, heif_suberror_Unsupported_codec,
                            "CUVID parser creation failed");
    }
    *out_decoder = decoder;
    return direct_ok();
}

static void free_decoder(void *opaque)
{
    struct direct_decoder *decoder = opaque;
    if (!decoder) return;
    if (decoder->parser) cuvidDestroyVideoParser(decoder->parser);
    if (decoder->decoder_created) cuvidDestroyDecoder(decoder->decoder);
    if (decoder->context) cuCtxDestroy(decoder->context);
    free(decoder->input);
    free(decoder);
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
    if (!input || input_size < 4 || !out_data || !out_size) return -1;
    output = malloc(input_size + 4 * 64);
    if (!output) return -1;
    while (pos < input_size) {
        uint32_t nal_size;
        if (input_size - pos < 4) { free(output); return -1; }
        nal_size = ((uint32_t)input[pos] << 24) | ((uint32_t)input[pos + 1] << 16) |
                   ((uint32_t)input[pos + 2] << 8) | input[pos + 3];
        pos += 4;
        if (!nal_size || nal_size > input_size - pos) { free(output); return -1; }
        if (written > input_size + 4 * 64 - (size_t)nal_size - 4) {
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

static struct heif_error make_image(const struct direct_decoder *decoder,
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
    CUVIDPROCPARAMS proc;
    CUdeviceptr source;

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
    memset(&proc, 0, sizeof(proc));
    proc.progressive_frame = 1;
    source = mapped + (CUdeviceptr)decoder->display_top * pitch + decoder->display_left;
    for (int row = 0; row < decoder->display_height; ++row) {
        CUDA_MEMCPY2D copy = {0};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source + (CUdeviceptr)row * pitch;
        copy.srcPitch = pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = y + (size_t)row * (size_t)y_stride;
        copy.dstPitch = (size_t)y_stride;
        copy.WidthInBytes = (size_t)decoder->display_width;
        copy.Height = 1;
        if (!cuda_ok(cuMemcpy2D(&copy))) return direct_error(heif_error_Decoder_plugin_error,
                                                              heif_suberror_Unspecified,
                                                              "Y surface copy failed");
    }
    source = mapped + (CUdeviceptr)decoder->height * pitch +
             (CUdeviceptr)(decoder->display_top / 2) * pitch + decoder->display_left;
    for (int row = 0; row < (decoder->display_height + 1) / 2; ++row) {
        for (int col = 0; col < (decoder->display_width + 1) / 2; ++col) {
            uint8_t uv[2];
            CUDA_MEMCPY2D copy = {0};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = source + (CUdeviceptr)row * pitch + (CUdeviceptr)col * 2;
            copy.srcPitch = pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = uv;
            copy.dstPitch = 2;
            copy.WidthInBytes = 2;
            copy.Height = 1;
            if (!cuda_ok(cuMemcpy2D(&copy))) return direct_error(heif_error_Decoder_plugin_error,
                                                                  heif_suberror_Unspecified,
                                                                  "UV surface copy failed");
            cb[(size_t)row * (size_t)cb_stride + col] = uv[0];
            cr[(size_t)row * (size_t)cr_stride + col] = uv[1];
        }
    }
    (void)proc;
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
    struct heif_error err = direct_ok();

    if (!decoder || !out_image || !decoder->input_size) {
        return direct_error(heif_error_Invalid_input, heif_suberror_No_item_data,
                            "empty direct HEVC input");
    }
    *out_image = NULL;
    if (length_prefixed_to_annexb(decoder->input, decoder->input_size,
                                  &annexb, &annexb_size) != 0) {
        return direct_error(heif_error_Invalid_input, heif_suberror_End_of_data,
                            "invalid length-prefixed HEVC stream");
    }
    memset(&packet, 0, sizeof(packet));
    packet.flags = CUVID_PKT_TIMESTAMP | CUVID_PKT_ENDOFPICTURE;
    packet.payload_size = (unsigned long)annexb_size;
    packet.payload = annexb;
    packet.timestamp = 1;
    decoder->display_ready = 0;
    if (!cuda_ok(cuvidParseVideoData(decoder->parser, &packet))) {
        err = direct_error(heif_error_Decoder_plugin_error, heif_suberror_Unspecified,
                           "CUVID parser rejected HEVC stream");
        goto cleanup;
    }
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
    free(annexb);
    if (err.code != heif_error_Ok && out_image && *out_image) {
        heif_image_release(*out_image);
        *out_image = NULL;
    }
    return err;
}

static const struct heif_decoder_plugin plugin = {
    .plugin_api_version = 3,
    .get_plugin_name = plugin_name,
    .init_plugin = NULL,
    .deinit_plugin = NULL,
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
