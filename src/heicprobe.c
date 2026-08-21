#include <libheif/heif.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void print_heif_error(const char *what, struct heif_error err)
{
    fprintf(stderr, "%s: %s", what, err.message ? err.message : "unknown libheif error");
    if (err.subcode != heif_suberror_Unspecified) {
        fprintf(stderr, " (subcode %d)", (int)err.subcode);
    }
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image.heic|image.heif>\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct heif_context *ctx = heif_context_alloc();
    if (!ctx) {
        fputs("heicprobe: failed to allocate libheif context\n", stderr);
        return EXIT_FAILURE;
    }

    struct heif_error err = heif_context_read_from_file(ctx, argv[1], NULL);
    if (err.code != heif_error_Ok) {
        print_heif_error("heicprobe: cannot read input", err);
        heif_context_free(ctx);
        return EXIT_FAILURE;
    }

    struct heif_image_handle *handle = NULL;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        print_heif_error("heicprobe: cannot obtain primary image", err);
        heif_context_free(ctx);
        return EXIT_FAILURE;
    }

    const int width = heif_image_handle_get_width(handle);
    const int height = heif_image_handle_get_height(handle);
    const int luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    const int chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);
    const bool has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;

    const int exif_blocks = heif_image_handle_get_number_of_metadata_blocks(handle, "Exif");
    const int xmp_blocks = heif_image_handle_get_number_of_metadata_blocks(handle, "mime");

    printf("File: %s\n", argv[1]);
    printf("Primary image: %dx%d\n", width, height);
    printf("Luma bit depth: %d\n", luma_bits);
    printf("Chroma bit depth: %d\n", chroma_bits);
    printf("Alpha: %s\n", has_alpha ? "yes" : "no");
    printf("EXIF blocks: %d\n", exif_blocks);
    printf("MIME metadata blocks: %d\n", xmp_blocks);

    /*
     * Phase-1 fast-path policy is deliberately conservative. This is not yet
     * proof that NVDEC can consume the coded item; it merely identifies sane
     * candidates for the next stage.
     */
    const bool plausible_fast_path =
        width > 0 && height > 0 &&
        width <= 16384 && height <= 16384 &&
        (luma_bits == 8 || luma_bits == 10) &&
        !has_alpha;

    printf("NVDEC candidate: %s\n", plausible_fast_path ? "yes" : "no/fallback");

    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return EXIT_SUCCESS;
}
