#define _POSIX_C_SOURCE 200809L

#include <libheif/heif.h>

#include "nvdec_plugin.h"
#ifdef CSHARP_HAVE_DIRECT_NVDEC
#include "direct_nvdec_plugin.h"
#include "direct_nvdec_timing.h"
#endif

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

struct start_gate {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t ready;
    size_t expected;
    int go;
};

struct worker_args {
    const struct benchmark_input *inputs;
    size_t input_count;
    size_t start_index;
    size_t iterations;
    size_t stride;
    struct start_gate *gate;
    int direct_backend;
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

static int decode_input(const struct benchmark_input *input, int direct_backend)
{
    struct heif_context *ctx = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_image *image = NULL;
    struct heif_decoding_options *options = NULL;
    struct heif_error err = {heif_error_Ok, heif_suberror_Unspecified, "ok"};
    int result = -1;

    ctx = heif_context_alloc();
    if (!ctx) goto cleanup;
    err = heif_context_read_from_memory_without_copy(ctx, input->data, input->size, NULL);
    if (err.code != heif_error_Ok) goto cleanup;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) goto cleanup;
    options = heif_decoding_options_alloc();
    if (!options) goto cleanup;
    options->decoder_id = direct_backend ? "csharp-direct-nvdec" : "csharp-nvdec";
    err = heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_420, options);
    if (err.code == heif_error_Ok) result = 0;

cleanup:
    if (result != 0 && direct_backend) {
        fprintf(stderr, "direct decode failed for %s: %s (%d/%d)\n",
                input->path, err.message ? err.message : "unknown error",
                (int)err.code, (int)err.subcode);
    }
    heif_decoding_options_free(options);
    heif_image_release(image);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return result;
}

static void gate_wait(struct start_gate *gate)
{
    if (!gate) return;
    pthread_mutex_lock(&gate->mutex);
    ++gate->ready;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->go) pthread_cond_wait(&gate->cond, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
}

static void *worker_main(void *opaque)
{
    struct worker_args *args = opaque;
    gate_wait(args->gate);

    for (size_t i = 0; i < args->iterations; ++i) {
        size_t global = args->start_index + i * args->stride;
        if (decode_input(&args->inputs[global % args->input_count], args->direct_backend) != 0) {
            args->failed = 1;
            break;
        }
    }
    return NULL;
}

static int run_parallel(const struct benchmark_input *inputs, size_t input_count,
                        size_t workers, size_t iterations,
                        int direct_backend, double *wall_out, double *cpu_out)
{
    struct worker_args *args = calloc(workers, sizeof(*args));
    pthread_t *threads = calloc(workers, sizeof(*threads));
    struct start_gate gate;
    double wall_start, cpu_start;
    size_t created = 0;
    int failed = 0;

    if (!args || !threads) { free(args); free(threads); return -1; }
    memset(&gate, 0, sizeof(gate));
    gate.expected = workers;
    pthread_mutex_init(&gate.mutex, NULL);
    pthread_cond_init(&gate.cond, NULL);

    for (size_t w = 0; w < workers; ++w) {
        args[w].inputs = inputs;
        args[w].input_count = input_count;
        args[w].start_index = w;
        args[w].stride = workers;
        args[w].iterations = iterations / workers + (w < iterations % workers ? 1 : 0);
        args[w].gate = &gate;
        args[w].direct_backend = direct_backend;
        if (pthread_create(&threads[w], NULL, worker_main, &args[w]) != 0) {
            failed = 1;
            break;
        }
        ++created;
    }

    pthread_mutex_lock(&gate.mutex);
    while (!failed && gate.ready < created) pthread_cond_wait(&gate.cond, &gate.mutex);
    wall_start = monotonic_seconds();
    cpu_start = process_cpu_seconds();
    gate.go = 1;
    pthread_cond_broadcast(&gate.cond);
    pthread_mutex_unlock(&gate.mutex);

    for (size_t w = 0; w < created; ++w) pthread_join(threads[w], NULL);
    if (wall_out) *wall_out = monotonic_seconds() - wall_start;
    if (cpu_out) *cpu_out = process_cpu_seconds() - cpu_start;

    for (size_t w = 0; w < created; ++w) if (args[w].failed) failed = 1;
    pthread_cond_destroy(&gate.cond);
    pthread_mutex_destroy(&gate.mutex);
    free(threads);
    free(args);
    return failed ? -1 : 0;
}

static void print_stage_stats(const struct csharp_nvdec_stats *s)
{
    double n = s->decodes ? (double)s->decodes : 1.0;
    printf("Stages: decodes=%lu acquire=%.3f annexb=%.3f decode=%.3f transfer=%.3f output=%.3f total=%.3f ms/image\n",
           s->decodes, s->acquire_ms / n, s->annexb_ms / n,
           s->decode_ms / n, s->transfer_ms / n,
           s->output_ms / n, s->total_ms / n);
    printf("NVDEC counts: CUDA=%lu AVCodecContext=%lu reuses=%lu\n",
           s->cuda_device_initializations, s->decoder_initializations, s->decoder_reuses);
}

static int parse_backend(const char *name, enum csharp_nvdec_backend *backend)
{
    if (strcmp(name, "cuvid") == 0) *backend = CSHARP_NVDEC_BACKEND_CUVID;
    else if (strcmp(name, "native") == 0) *backend = CSHARP_NVDEC_BACKEND_NATIVE;
    else if (strcmp(name, "auto") == 0) *backend = CSHARP_NVDEC_BACKEND_AUTO;
    else return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct benchmark_input *inputs = NULL;
    enum csharp_nvdec_backend backend = CSHARP_NVDEC_BACKEND_CUVID;
    int direct_backend = 0;
    unsigned repeats = 1;
    unsigned long requested_workers;
    unsigned long long requested_iterations;
    size_t workers, iterations, input_count;
    struct heif_error err;
    int argi = 1;
    int result = EXIT_FAILURE;
    char *end;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--backend") == 0 && argi + 1 < argc) {
            if (strcmp(argv[argi + 1], "direct") == 0) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
                direct_backend = 1;
#else
                fputs("direct backend was not built; configure with CSHARP_ENABLE_DIRECT_NVDEC=ON\n", stderr);
                return EXIT_FAILURE;
#endif
            } else if (parse_backend(argv[argi + 1], &backend) != 0) {
                fputs("backend must be cuvid, native, auto, or direct\n", stderr);
                return EXIT_FAILURE;
            }
            argi += 2;
        } else if (strcmp(argv[argi], "--repeats") == 0 && argi + 1 < argc) {
            unsigned long value;
            end = NULL;
            value = strtoul(argv[argi + 1], &end, 10);
            if (!end || *end != '\0' || value < 1 || value > 20) {
                fputs("repeats must be in range 1..20\n", stderr);
                return EXIT_FAILURE;
            }
            repeats = (unsigned)value;
            argi += 2;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            return EXIT_FAILURE;
        }
    }

    if (argc - argi < 3) {
        fprintf(stderr, "usage: %s [--backend cuvid|native|auto|direct] [--repeats N] <workers> <iterations> <file...>\n", argv[0]);
        return EXIT_FAILURE;
    }

    end = NULL;
    requested_workers = strtoul(argv[argi++], &end, 10);
    if (!end || *end != '\0' || requested_workers < 1 || requested_workers > 64) {
        fputs("workers must be in range 1..64\n", stderr);
        return EXIT_FAILURE;
    }
    end = NULL;
    requested_iterations = strtoull(argv[argi++], &end, 10);
    if (!end || *end != '\0' || requested_iterations < 1) {
        fputs("iterations must be >= 1\n", stderr);
        return EXIT_FAILURE;
    }

    workers = (size_t)requested_workers;
    iterations = (size_t)requested_iterations;
    input_count = (size_t)(argc - argi);
    inputs = calloc(input_count, sizeof(*inputs));
    if (!inputs) return EXIT_FAILURE;

    for (size_t i = 0; i < input_count; ++i) {
        if (load_input(argv[argi + (int)i], &inputs[i]) != 0) {
            fprintf(stderr, "cannot load input: %s\n", argv[argi + (int)i]);
            goto cleanup;
        }
    }

    csharp_nvdec_set_verbose(0);
    csharp_nvdec_set_backend(backend);
    err = direct_backend ? csharp_register_direct_nvdec_plugin() : csharp_register_nvdec_plugin();
    if (err.code != heif_error_Ok) {
        fprintf(stderr, "cannot register NVDEC plugin: %s\n", err.message ? err.message : "unknown error");
        goto cleanup;
    }

    printf("Backend=%s workers=%zu iterations=%zu inputs=%zu repeats=%u\n",
           direct_backend ? "direct" : csharp_nvdec_backend_name(backend), workers, iterations,
           input_count, repeats);

    /* Prewarm all lanes concurrently, not merely input zero. This intentionally
     * preserves worker/input affinity from the measured run and populates enough
     * decoder contexts before timing begins. */
    {
        size_t warm_iterations = workers;
        if (warm_iterations < input_count) warm_iterations = input_count;
        if (run_parallel(inputs, input_count, workers, warm_iterations, direct_backend, NULL, NULL) != 0) {
            fputs("NVDEC multi-lane prewarm failed\n", stderr);
            goto cleanup;
        }
    }

    for (unsigned r = 0; r < repeats; ++r) {
        double wall = 0.0, cpu = 0.0;
        struct csharp_nvdec_stats stats;
#ifdef CSHARP_HAVE_DIRECT_NVDEC
        struct csharp_direct_nvdec_stats direct_stats;
#endif
        if (direct_backend) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
            csharp_direct_nvdec_reset_stats();
            csharp_direct_timing_reset();
#endif
        } else {
            csharp_nvdec_reset_stats();
        }
        if (run_parallel(inputs, input_count, workers, iterations, direct_backend, &wall, &cpu) != 0) {
            fputs("parallel decode failed\n", stderr);
            goto cleanup;
        }
        printf("Run %u: wall=%.6f sec cpu=%.6f sec images/sec=%.2f cpu-ms/image=%.3f avg-cpu-cores=%.2f\n",
               r + 1, wall, cpu, (double)iterations / wall,
               cpu * 1000.0 / (double)iterations, wall > 0.0 ? cpu / wall : 0.0);
        if (direct_backend) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
            struct csharp_direct_timing_snapshot timing;
            csharp_direct_nvdec_get_stats(&direct_stats);
            printf("Direct counts: decodes=%lu lane_creates=%lu lane_reuses=%lu decoder_creates=%lu reconfigures=%lu bucket_grows=%lu\n",
                   direct_stats.decodes, direct_stats.lane_creates, direct_stats.lane_reuses,
                   direct_stats.decoder_creates, direct_stats.decoder_reconfigures,
                   direct_stats.decoder_bucket_grows);
            csharp_direct_timing_get(&timing);
            printf("Direct stages: annexb=%.4f parse=%.4f flush_wait=%.4f map=%.4f "
                   "copy_y=%.4f copy_uv=%.4f deinterleave=%.4f output=%.4f "
                   "unmap=%.4f total=%.4f ms/image samples=%llu\n",
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_ANNEXB),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_PARSE),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_FLUSH_WAIT),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_MAP),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_COPY_Y),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_COPY_UV),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_DEINTERLEAVE),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_OUTPUT),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_UNMAP),
                   csharp_direct_stage_ms_per_sample(&timing, CSHARP_DIRECT_STAGE_TOTAL),
                   (unsigned long long)timing.samples[CSHARP_DIRECT_STAGE_TOTAL]);
#endif
        } else {
            csharp_nvdec_get_stats(&stats);
            print_stage_stats(&stats);
        }
    }

    result = EXIT_SUCCESS;

cleanup:
    if (inputs) for (size_t i = 0; i < input_count; ++i) free(inputs[i].data);
    free(inputs);
    return result;
}
