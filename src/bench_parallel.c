#define _POSIX_C_SOURCE 200809L

#include <libheif/heif.h>

#include "nvdec_plugin.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct benchmark_input {
    const char *path;
    uint8_t *data;
    size_t size;
};

struct worker_args {
    const struct benchmark_input *inputs;
    size_t input_count;
    size_t start_index;
    size_t iterations;
    size_t stride;
    int failed;
};

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double process_cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int load_input(const char *path, struct benchmark_input *input)
{
    FILE *file = fopen(path, "rb");
    long length;

    if (!file) return -1;
    memset(input, 0, sizeof(*input));
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
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

static int decode_input(const struct benchmark_input *input)
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
    options = heif_decoding_options_alloc();
    if (!options) goto cleanup;
    options->decoder_id = "csharp-nvdec";
    err = heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_420, options);
    if (err.code == heif_error_Ok) result = 0;

cleanup:
    heif_decoding_options_free(options);
    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return result;
}

static void *worker_main(void *opaque)
{
    struct worker_args *args = opaque;

    for (size_t i = 0; i < args->iterations; ++i) {
        size_t global = args->start_index + i * args->stride;
        if (decode_input(&args->inputs[global % args->input_count]) != 0) {
            args->failed = 1;
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    struct benchmark_input *inputs = NULL;
    struct worker_args *args = NULL;
    pthread_t *threads = NULL;
    char *end = NULL;
    unsigned long requested_workers;
    unsigned long long requested_iterations;
    size_t workers, iterations;
    double wall_start, cpu_start, wall, cpu;
    struct heif_error err;
    int result = EXIT_FAILURE;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <workers> <iterations> <file...>\n", argv[0]);
        return EXIT_FAILURE;
    }

    requested_workers = strtoul(argv[1], &end, 10);
    if (!end || *end != '\0' || requested_workers < 1 || requested_workers > 32) {
        fputs("workers must be in range 1..32\n", stderr);
        return EXIT_FAILURE;
    }
    end = NULL;
    requested_iterations = strtoull(argv[2], &end, 10);
    if (!end || *end != '\0' || requested_iterations < 1) {
        fputs("iterations must be >= 1\n", stderr);
        return EXIT_FAILURE;
    }

    workers = (size_t)requested_workers;
    iterations = (size_t)requested_iterations;
    inputs = calloc((size_t)(argc - 3), sizeof(*inputs));
    args = calloc(workers, sizeof(*args));
    threads = calloc(workers, sizeof(*threads));
    if (!inputs || !args || !threads) goto cleanup;

    for (int i = 3; i < argc; ++i) {
        if (load_input(argv[i], &inputs[i - 3]) != 0) {
            fprintf(stderr, "cannot load input: %s\n", argv[i]);
            goto cleanup;
        }
    }

    csharp_nvdec_set_verbose(0);
    csharp_nvdec_reset_stats();
    err = csharp_register_nvdec_plugin();
    if (err.code != heif_error_Ok) {
        fprintf(stderr, "cannot register NVDEC plugin: %s\n", err.message ? err.message : "unknown error");
        goto cleanup;
    }

    /* Pay cold-start once before the timed region. */
    if (decode_input(&inputs[0]) != 0) {
        fputs("NVDEC warm-up failed\n", stderr);
        goto cleanup;
    }

    for (size_t w = 0; w < workers; ++w) {
        args[w].inputs = inputs;
        args[w].input_count = (size_t)(argc - 3);
        args[w].start_index = w;
        args[w].stride = workers;
        args[w].iterations = iterations / workers + (w < iterations % workers ? 1 : 0);
    }

    wall_start = monotonic_seconds();
    cpu_start = process_cpu_seconds();
    for (size_t w = 0; w < workers; ++w) {
        if (pthread_create(&threads[w], NULL, worker_main, &args[w]) != 0) {
            fputs("pthread_create failed\n", stderr);
            goto cleanup;
        }
    }
    for (size_t w = 0; w < workers; ++w) pthread_join(threads[w], NULL);
    wall = monotonic_seconds() - wall_start;
    cpu = process_cpu_seconds() - cpu_start;

    for (size_t w = 0; w < workers; ++w) {
        if (args[w].failed) {
            fputs("parallel decode failed\n", stderr);
            goto cleanup;
        }
    }

    printf("NVDEC parallel: workers=%zu iterations=%zu wall=%.6f sec cpu=%.6f sec images/sec=%.2f cpu-ms/image=%.3f avg-cpu-cores=%.2f\n",
           workers, iterations, wall, cpu, (double)iterations / wall,
           cpu * 1000.0 / (double)iterations, wall > 0.0 ? cpu / wall : 0.0);
    printf("NVDEC counts: CUDA=%lu AVCodecContext=%lu reuses=%lu\n",
           csharp_nvdec_cuda_device_initializations(),
           csharp_nvdec_decoder_initializations(),
           csharp_nvdec_decoder_reuses());
    result = EXIT_SUCCESS;

cleanup:
    if (inputs) {
        for (int i = 3; i < argc; ++i) free(inputs[i - 3].data);
    }
    free(threads);
    free(args);
    free(inputs);
    return result;
}
