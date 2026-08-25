#include <libheif/heif.h>

#include "input_classify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *colorspace_name(enum heif_colorspace value)
{
    switch (value) {
        case heif_colorspace_YCbCr: return "ycbcr";
        case heif_colorspace_RGB: return "rgb";
        case heif_colorspace_monochrome: return "mono";
        case heif_colorspace_undefined: return "undefined";
        default: return "other";
    }
}

static const char *chroma_name(enum heif_chroma value)
{
    switch (value) {
        case heif_chroma_monochrome: return "mono";
        case heif_chroma_420: return "420";
        case heif_chroma_422: return "422";
        case heif_chroma_444: return "444";
        case heif_chroma_interleaved_RGB: return "rgb";
        case heif_chroma_interleaved_RGBA: return "rgba";
        case heif_chroma_undefined: return "undefined";
        default: return "other";
    }
}

static int inspect_file(const char *path, int tsv, int header)
{
    struct heif_context *ctx = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_error err;
    struct csharp_input_classification classification;
    int width, height, luma_bits, chroma_bits, alpha, metadata, top_level, eligible;
    size_t icc_size;
    int result = 1;

    ctx = heif_context_alloc();
    if (!ctx) {
        fprintf(stderr, "csharp-inspect: cannot allocate context for %s\n", path);
        return 1;
    }
    err = heif_context_read_from_file(ctx, path, NULL);
    if (err.code != heif_error_Ok) {
        fprintf(stderr, "csharp-inspect: %s: %s\n", path,
                err.message ? err.message : "cannot read HEIF");
        goto cleanup;
    }
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        fprintf(stderr, "csharp-inspect: %s: %s\n", path,
                err.message ? err.message : "no primary image");
        goto cleanup;
    }

    width = heif_image_handle_get_width(handle);
    height = heif_image_handle_get_height(handle);
    luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);
    alpha = heif_image_handle_has_alpha_channel(handle) ? 1 : 0;
    metadata = heif_image_handle_get_number_of_metadata_blocks(handle, NULL);
    top_level = heif_context_get_number_of_top_level_images(ctx);
    icc_size = heif_image_handle_get_raw_color_profile_size(handle);
    csharp_classify_input(handle, &classification);
    eligible = classification.direct_eligible;

    if (tsv) {
        if (header) {
            puts("path\twidth\theight\tluma_bits\tchroma_bits\talpha\tcolorspace\tchroma\tmetadata_blocks\ticc_bytes\ttop_level_images\tdirect_eligible\tfallback_reason\tpredicted_path");
        }
        printf("%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%zu\t%d\t%d\t%s\t%s\n",
            path, width, height, luma_bits, chroma_bits, alpha,
               colorspace_name(classification.colorspace), chroma_name(classification.chroma), metadata, icc_size,
               top_level, eligible, csharp_fallback_reason_name(classification.reason),
               eligible ? "direct" : "cpu-fallback");
    } else {
        printf("%s\n", path);
        printf("  dimensions: %dx%d\n", width, height);
        printf("  bit depth: luma=%d chroma=%d\n", luma_bits, chroma_bits);
        printf("  alpha: %s\n", alpha ? "yes" : "no");
        printf("  preferred decode: %s / %s\n",
               colorspace_name(classification.colorspace), chroma_name(classification.chroma));
        printf("  metadata blocks: %d\n", metadata);
        printf("  ICC bytes: %zu\n", icc_size);
        printf("  top-level images: %d\n", top_level);
        printf("  direct eligible: %s\n", eligible ? "yes" : "no");
        printf("  fallback reason: %s\n", csharp_fallback_reason_name(classification.reason));
        printf("  predicted path: %s\n", eligible ? "direct" : "cpu-fallback");
    }
    result = 0;

cleanup:
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return result;
}

int main(int argc, char **argv)
{
    int tsv = 0;
    int argi = 1;
    int status = 0;

    if (argi < argc && strcmp(argv[argi], "--tsv") == 0) {
        tsv = 1;
        ++argi;
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [--tsv] FILE.heic [FILE.heic ...]\n", argv[0]);
        return 2;
    }
    for (int i = argi; i < argc; ++i) {
        if (inspect_file(argv[i], tsv, tsv && i == argi) != 0) status = 1;
    }
    return status;
}
