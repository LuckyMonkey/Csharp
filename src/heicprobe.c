#define _POSIX_C_SOURCE 200809L

#include <libheif/heif.h>

#include "nvdec_plugin.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_heif_error(const char *what, struct heif_error err)
{
    fprintf(stderr, "%s: %s", what, err.message ? err.message : "unknown libheif error");
    if (err.subcode != heif_suberror_Unspecified) {
        fprintf(stderr, " (subcode %d)", (int)err.subcode);
    }
    fputc('\n', stderr);
}

struct image_stats {
    int width;
    int height;
    uint64_t bytes;
    uint64_t hash;
};

static uint64_t fnv1a_update(uint64_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int collect_stats(const struct heif_image *image, struct image_stats *stats)
{
    const enum heif_channel channels[] = {heif_channel_Y, heif_channel_Cb, heif_channel_Cr};
    int width, height;

    if (!image || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    width = heif_image_get_primary_width(image);
    height = heif_image_get_primary_height(image);
    stats->width = width;
    stats->height = height;
    stats->hash = UINT64_C(14695981039346656037);
    for (size_t c = 0; c < 3; ++c) {
        int stride = 0;
        int plane_width = c == 0 ? width : (width + 1) / 2;
        int plane_height = c == 0 ? height : (height + 1) / 2;
        const uint8_t *plane = heif_image_get_plane_readonly(image, channels[c], &stride);
        if (!plane || stride < plane_width || plane_width <= 0 || plane_height <= 0) return -1;
        for (int y = 0; y < plane_height; ++y) {
            const uint8_t *row = plane + (size_t)y * (size_t)stride;
            stats->hash = fnv1a_update(stats->hash, row, (size_t)plane_width);
            stats->bytes += (uint64_t)plane_width;
        }
    }
    return 0;
}

static int compare_images(const struct heif_image *cpu, const struct heif_image *gpu)
{
    struct image_stats cpu_stats, gpu_stats;
    if (collect_stats(cpu, &cpu_stats) || collect_stats(gpu, &gpu_stats)) {
        fputs("heicprobe: cannot collect decoded image planes\n", stderr);
        return -1;
    }
    printf("CPU image: %dx%d bytes=%" PRIu64 " fnv1a=%016" PRIx64 "\n",
           cpu_stats.width, cpu_stats.height, cpu_stats.bytes, cpu_stats.hash);
    printf("NVDEC image: %dx%d bytes=%" PRIu64 " fnv1a=%016" PRIx64 "\n",
           gpu_stats.width, gpu_stats.height, gpu_stats.bytes, gpu_stats.hash);
    if (cpu_stats.width != gpu_stats.width || cpu_stats.height != gpu_stats.height ||
        cpu_stats.bytes != gpu_stats.bytes) {
        fputs("Pixel comparison: dimensions or plane sizes differ\n", stderr);
        return -1;
    }
    if (cpu_stats.hash != gpu_stats.hash) {
        fprintf(stderr,
                "Pixel comparison: decoded bytes differ (CPU=%016" PRIx64
                ", NVDEC=%016" PRIx64 ")\n",
                cpu_stats.hash, gpu_stats.hash);
        return -1;
    }
    puts("Pixel comparison: exact YCbCr plane match");
    return 0;
}

static int run_nvdec_comparison(struct heif_image_handle *handle)
{
    struct heif_image *cpu_image = NULL;
    struct heif_image *nvdec_image = NULL;
    struct heif_decoding_options *options = NULL;
    struct heif_error err;
    int result = EXIT_FAILURE;

    err = heif_decode_image(handle, &cpu_image, heif_colorspace_YCbCr,
                            heif_chroma_420, NULL);
    if (err.code != heif_error_Ok) {
        print_heif_error("heicprobe: CPU decode failed", err);
        goto cleanup;
    }
    err = csharp_register_nvdec_plugin();
    if (err.code != heif_error_Ok) {
        print_heif_error("heicprobe: NVDEC plugin registration failed", err);
        goto cleanup;
    }
    options = heif_decoding_options_alloc();
    if (!options) {
        fputs("heicprobe: cannot allocate decoding options\n", stderr);
        goto cleanup;
    }
    options->decoder_id = "csharp-nvdec";
    err = heif_decode_image(handle, &nvdec_image, heif_colorspace_YCbCr,
                            heif_chroma_420, options);
    if (err.code != heif_error_Ok) {
        print_heif_error("heicprobe: NVDEC decode failed", err);
        goto cleanup;
    }
    result = compare_images(cpu_image, nvdec_image) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

cleanup:
    heif_decoding_options_free(options);
    heif_image_release(nvdec_image);
    heif_image_release(cpu_image);
    return result;
}

struct benchmark_input {
    const char *path;
    uint8_t *data;
    size_t size;
};

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int load_benchmark_input(const char *path, struct benchmark_input *input)
{
    FILE *file;
    long length;

    memset(input, 0, sizeof(*input));
    file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -1; }
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return -1; }
    input->data = malloc((size_t)length);
    if (!input->data || fread(input->data, 1, (size_t)length, file) != (size_t)length) {
        free(input->data);
        input->data = NULL;
        fclose(file);
        return -1;
    }
    fclose(file);
    input->path = path;
    input->size = (size_t)length;
    return 0;
}

static int decode_benchmark_input(const struct benchmark_input *input, bool nvdec)
{
    struct heif_context *ctx = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_image *image = NULL;
    struct heif_decoding_options *options = NULL;
    struct heif_error err;
    int result = -1;

    ctx = heif_context_alloc();
    if (!ctx) goto cleanup;
    err = heif_context_read_from_memory_without_copy(ctx, input->data, input->size, NULL);
    if (err.code != heif_error_Ok) goto cleanup;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) goto cleanup;
    if (nvdec) {
        options = heif_decoding_options_alloc();
        if (!options) goto cleanup;
        options->decoder_id = "csharp-nvdec";
    }
    err = heif_decode_image(handle, &image, heif_colorspace_YCbCr,
                            heif_chroma_420, options);
    if (err.code == heif_error_Ok) result = 0;

cleanup:
    heif_decoding_options_free(options);
    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return result;
}

static double benchmark_decodes(const struct benchmark_input *inputs, size_t input_count,
                                size_t iterations, bool nvdec)
{
    double start = monotonic_seconds();
    for (size_t i = 0; i < iterations; ++i) {
        if (decode_benchmark_input(&inputs[i % input_count], nvdec) != 0) return -1.0;
    }
    return monotonic_seconds() - start;
}

static void print_timing(const char *label, double seconds, size_t iterations)
{
    printf("%s: total=%.6f seconds ms/image=%.3f images/sec=%.2f\n",
           label, seconds, seconds * 1000.0 / (double)iterations,
           (double)iterations / seconds);
}

static int run_benchmark(size_t iterations, int file_count, char **paths)
{
    struct benchmark_input *inputs = calloc((size_t)file_count, sizeof(*inputs));
    struct heif_error err;
    double seconds;
    int result = EXIT_FAILURE;

    if (!inputs) return EXIT_FAILURE;
    csharp_nvdec_set_verbose(0);
    for (int i = 0; i < file_count; ++i) {
        if (load_benchmark_input(paths[i], &inputs[i]) != 0) {
            fprintf(stderr, "heicprobe: cannot load benchmark input: %s\n", paths[i]);
            goto cleanup;
        }
    }
    printf("Benchmark: %zu iterations, %d distinct in-memory inputs (round-robin)\n",
           iterations, file_count);

    seconds = benchmark_decodes(inputs, (size_t)file_count, 1, false);
    if (seconds < 0.0) { fputs("CPU cold decode failed\n", stderr); goto cleanup; }
    print_timing("CPU cold", seconds, 1);
    if (decode_benchmark_input(&inputs[0], false) != 0) {
        fputs("CPU warm-up failed\n", stderr); goto cleanup;
    }
    seconds = benchmark_decodes(inputs, (size_t)file_count, iterations, false);
    if (seconds < 0.0) { fputs("CPU warm decode failed\n", stderr); goto cleanup; }
    print_timing("CPU warm", seconds, iterations);

    csharp_nvdec_reset_stats();
    seconds = monotonic_seconds();
    err = csharp_register_nvdec_plugin();
    if (err.code != heif_error_Ok) {
        print_heif_error("heicprobe: NVDEC plugin registration failed", err);
        goto cleanup;
    }
    if (decode_benchmark_input(&inputs[0], true) != 0) {
        fputs("NVDEC cold decode failed\n", stderr); goto cleanup;
    }
    seconds = monotonic_seconds() - seconds;
    print_timing("NVDEC cold (registration + CUDA + decode)", seconds, 1);
    if (decode_benchmark_input(&inputs[0], true) != 0) {
        fputs("NVDEC warm-up failed\n", stderr); goto cleanup;
    }
    seconds = benchmark_decodes(inputs, (size_t)file_count, iterations, true);
    if (seconds < 0.0) { fputs("NVDEC warm decode failed\n", stderr); goto cleanup; }
    print_timing("NVDEC warm", seconds, iterations);
    printf("NVDEC initialization counts: CUDA devices=%lu AVCodecContext/NVDEC=%lu\n",
           csharp_nvdec_cuda_device_initializations(),
           csharp_nvdec_decoder_initializations());
    result = EXIT_SUCCESS;

cleanup:
    for (int i = 0; i < file_count; ++i) free(inputs[i].data);
    free(inputs);
    return result;
}

int main(int argc, char **argv)
{
    const char *path;
    bool compare = false;

    if (argc >= 4 && strcmp(argv[1], "--benchmark") == 0) {
        char *end = NULL;
        unsigned long long requested = strtoull(argv[2], &end, 10);
        if (!end || *end != '\0' || requested == 0 || requested > SIZE_MAX) {
            fputs("usage: heicprobe --benchmark <iterations> <file...>\n", stderr);
            return EXIT_FAILURE;
        }
        return run_benchmark((size_t)requested, argc - 3, argv + 3);
    }

    if (argc == 2) {
        path = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--compare-nvdec") == 0) {
        compare = true;
        path = argv[2];
    } else {
        fprintf(stderr, "usage: %s [--compare-nvdec] <image.heic|image.heif>\n"
                        "       %s --benchmark <iterations> <file...>\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    struct heif_context *ctx = heif_context_alloc();
    if (!ctx) {
        fputs("heicprobe: failed to allocate libheif context\n", stderr);
        return EXIT_FAILURE;
    }

    struct heif_error err = heif_context_read_from_file(ctx, path, NULL);
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

    printf("File: %s\n", path);
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

    if (compare) {
        int result = run_nvdec_comparison(handle);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return result;
    }

    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return EXIT_SUCCESS;
}
