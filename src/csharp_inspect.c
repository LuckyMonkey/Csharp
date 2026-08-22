#include <libheif/heif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int direct_eligible(struct heif_image_handle *handle,
                           enum heif_colorspace *colorspace_out,
                           enum heif_chroma *chroma_out)
{
    enum heif_colorspace colorspace = heif_colorspace_undefined;
    enum heif_chroma chroma = heif_chroma_undefined;
    int bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    int chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);

    if (heif_image_handle_get_preferred_decoding_colorspace(handle, &colorspace, &chroma).code !=
        heif_error_Ok) {
        colorspace = heif_colorspace_undefined;
        chroma = heif_chroma_undefined;
    }
    if (colorspace_out) *colorspace_out = colorspace;
    if (chroma_out) *chroma_out = chroma;

    if (bits != 8 || chroma_bits != 8 || heif_image_handle_has_alpha_channel(handle)) return 0;
    if (colorspace != heif_colorspace_YCbCr && colorspace != heif_colorspace_undefined) return 0;
    if (chroma != heif_chroma_420 && chroma != heif_chroma_undefined) return 0;
    return 1;
}

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
    enum heif_colorspace colorspace = heif_colorspace_undefined;
    enum heif_chroma chroma = heif_chroma_undefined;
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
    eligible = direct_eligible(handle, &colorspace, &chroma);

    if (tsv) {
        if (header) {
            puts("path\twidth\theight\tluma_bits\tchroma_bits\talpha\tcolorspace\tchroma\tmetadata_blocks\ticc_bytes\ttop_level_images\tdirect_eligible\tpredicted_path");
        }
        printf("%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%zu\t%d\t%d\t%s\n",
               path, width, height, luma_bits, chroma_bits, alpha,
               colorspace_name(colorspace), chroma_name(chroma), metadata, icc_size,
               top_level, eligible, eligible ? "direct" : "cpu-fallback");
    } else {
        printf("%s\n", path);
        printf("  dimensions: %dx%d\n", width, height);
        printf("  bit depth: luma=%d chroma=%d\n", luma_bits, chroma_bits);
        printf("  alpha: %s\n", alpha ? "yes" : "no");
        printf("  preferred decode: %s / %s\n",
               colorspace_name(colorspace), chroma_name(chroma));
        printf("  metadata blocks: %d\n", metadata);
        printf("  ICC bytes: %zu\n", icc_size);
        printf("  top-level images: %d\n", top_level);
        printf("  direct eligible: %s\n", eligible ? "yes" : "no");
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
