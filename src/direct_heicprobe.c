#define _POSIX_C_SOURCE 200809L

#include "direct_nvdec_plugin.h"

#include <libheif/heif.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t hash_plane(uint64_t hash, const uint8_t *plane, int stride,
                           int width, int height)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            hash ^= plane[(size_t)y * (size_t)stride + (size_t)x];
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

static int image_hash(const struct heif_image *image, uint64_t *out_hash)
{
    const int width = heif_image_get_primary_width(image);
    const int height = heif_image_get_primary_height(image);
    const enum heif_channel channels[3] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
    uint64_t hash = UINT64_C(14695981039346656037);
    if (!image || !out_hash || width <= 0 || height <= 0) return -1;
    for (int c = 0; c < 3; ++c) {
        int stride = 0;
        const int plane_width = c == 0 ? width : (width + 1) / 2;
        const int plane_height = c == 0 ? height : (height + 1) / 2;
        const uint8_t *plane = heif_image_get_plane_readonly(image, channels[c], &stride);
        if (!plane || stride < plane_width) return -1;
        hash = hash_plane(hash, plane, stride, plane_width, plane_height);
    }
    *out_hash = hash;
    return 0;
}

static int decode(const char *path, struct heif_image **out_cpu,
                  struct heif_image **out_direct)
{
    struct heif_context *ctx = heif_context_alloc();
    struct heif_image_handle *handle = NULL;
    struct heif_decoding_options *options = NULL;
    struct heif_error err;
    if (!ctx) return -1;
    err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code == heif_error_Ok) err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code == heif_error_Ok)
        err = heif_decode_image(handle, out_cpu, heif_colorspace_YCbCr, heif_chroma_420, NULL);
    if (err.code == heif_error_Ok) err = csharp_register_direct_nvdec_plugin();
    if (err.code == heif_error_Ok) {
        options = heif_decoding_options_alloc();
        if (!options) err.code = heif_error_Memory_allocation_error;
    }
    if (err.code == heif_error_Ok) {
        options->decoder_id = "csharp-direct-nvdec";
        err = heif_decode_image(handle, out_direct, heif_colorspace_YCbCr,
                                heif_chroma_420, options);
    }
    if (err.code != heif_error_Ok) {
        fprintf(stderr, "direct-heicprobe: %s (%d/%d)\n", err.message ? err.message : "decode failed",
                (int)err.code, (int)err.subcode);
    }
    heif_decoding_options_free(options);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return err.code == heif_error_Ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct heif_image *cpu = NULL;
    struct heif_image *direct = NULL;
    uint64_t cpu_hash = 0;
    uint64_t direct_hash = 0;
    int result;
    if (argc != 2) {
        fprintf(stderr, "usage: %s image.heic\n", argv[0]);
        return EXIT_FAILURE;
    }
    result = decode(argv[1], &cpu, &direct);
    if (result == 0) result = image_hash(cpu, &cpu_hash) || image_hash(direct, &direct_hash);
    if (result == 0) {
        printf("CPU: %dx%d fnv1a=%016" PRIx64 "\n", heif_image_get_primary_width(cpu),
               heif_image_get_primary_height(cpu), cpu_hash);
        printf("Direct NVDECODE: %dx%d fnv1a=%016" PRIx64 "\n",
               heif_image_get_primary_width(direct), heif_image_get_primary_height(direct),
               direct_hash);
        result = cpu_hash == direct_hash ? 0 : -1;
        puts(result == 0 ? "Pixel comparison: exact YCbCr plane match" :
             "Pixel comparison: decoded bytes differ");
    }
    heif_image_release(direct);
    heif_image_release(cpu);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
