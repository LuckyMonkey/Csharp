#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#include <libheif/heif.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef CSHARP_HAVE_DIRECT_NVDEC
#include "direct_nvdec_plugin.h"
#endif

#define CSHARP_VERSION "0.1.0"
#define DEFAULT_QUALITY 90
#define MAX_JPEG_MARKER 65533U

enum backend { BACKEND_DIRECT, BACKEND_CPU };

struct jpeg_failure {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

struct metadata_blob {
    uint8_t *data;
    size_t size;
};

static void jpeg_error_exit(j_common_ptr common)
{
    struct jpeg_failure *failure = (struct jpeg_failure *)common->err;
    (*common->err->format_message)(common, failure->message);
    longjmp(failure->jump, 1);
}

static void print_heif_error(const char *what, struct heif_error error)
{
    fprintf(stderr, "csharp: %s: %s (%d/%d)\n", what,
            error.message ? error.message : "libheif error",
            (int)error.code, (int)error.subcode);
}

static uint16_t read16(const uint8_t *p, bool little)
{
    return little ? (uint16_t)p[0] | ((uint16_t)p[1] << 8)
                  : ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read32(const uint8_t *p, bool little)
{
    return little ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)
                  : ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                        ((uint32_t)p[2] << 8) | p[3];
}

static void write16(uint8_t *p, bool little, uint16_t value)
{
    if (little) {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
    } else {
        p[0] = (uint8_t)(value >> 8);
        p[1] = (uint8_t)value;
    }
}

static void write32(uint8_t *p, bool little, uint32_t value)
{
    if (little) {
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16);
        p[3] = (uint8_t)(value >> 24);
    } else {
        p[0] = (uint8_t)(value >> 24);
        p[1] = (uint8_t)(value >> 16);
        p[2] = (uint8_t)(value >> 8);
        p[3] = (uint8_t)value;
    }
}

/* HEIF EXIF payloads conventionally begin with a four-byte TIFF offset. */
static void normalize_exif_orientation(uint8_t *data, size_t size)
{
    size_t base = 0;
    const uint8_t *tiff;
    bool little;
    uint32_t ifd_offset;
    uint16_t entries;

    if (!data || size < 14) return;
    if (size >= 6 && memcmp(data, "Exif\0\0", 6) == 0) base = 6;
    else if (size >= 4) base = 4;
    if (base > size || size - base < 8) return;
    tiff = data + base;
    if (memcmp(tiff, "II", 2) == 0) little = true;
    else if (memcmp(tiff, "MM", 2) == 0) little = false;
    else return;
    if (read16(tiff + 2, little) != 42) return;
    ifd_offset = read32(tiff + 4, little);
    if (ifd_offset > size - base || size - base - ifd_offset < 2) return;
    entries = read16(tiff + ifd_offset, little);
    if ((size_t)entries > (size - base - ifd_offset - 2) / 12) return;
    for (uint16_t i = 0; i < entries; ++i) {
        uint8_t *entry = (uint8_t *)tiff + ifd_offset + 2U + (size_t)i * 12U;
        uint16_t tag = read16(entry, little);
        uint16_t type = read16(entry + 2, little);
        uint32_t count = read32(entry + 4, little);
        if (tag != 0x0112 || count != 1) continue;
        if (type == 3) write16(entry + 8, little, 1);
        else if (type == 4) write32(entry + 8, little, 1);
        break;
    }
}

static int write_marker(struct jpeg_compress_struct *jpeg, int marker,
                        const uint8_t *data, size_t size)
{
    if (!data || size > MAX_JPEG_MARKER) return -1;
    jpeg_write_marker(jpeg, marker, data, (unsigned int)size);
    return 0;
}

static int write_metadata(struct jpeg_compress_struct *jpeg,
                          const struct heif_image_handle *handle)
{
    int count = heif_image_handle_get_number_of_metadata_blocks(handle, NULL);
    heif_item_id *ids = NULL;
    int written = 0;

    if (count <= 0) return 0;
    ids = calloc((size_t)count, sizeof(*ids));
    if (!ids) return -1;
    count = heif_image_handle_get_list_of_metadata_block_IDs(handle, NULL, ids, count);
    for (int i = 0; i < count; ++i) {
        const char *type = heif_image_handle_get_metadata_type(handle, ids[i]);
        const char *content = heif_image_handle_get_metadata_content_type(handle, ids[i]);
        char type_copy[32];
        char content_copy[64];
        size_t size = heif_image_handle_get_metadata_size(handle, ids[i]);
        struct metadata_blob blob = {0};
        if (!type || size == 0) continue;
        snprintf(type_copy, sizeof(type_copy), "%s", type);
        snprintf(content_copy, sizeof(content_copy), "%s", content ? content : "");
        blob.data = malloc(size);
        if (!blob.data) { free(ids); return -1; }
        blob.size = size;
        if (heif_image_handle_get_metadata(handle, ids[i], blob.data).code != heif_error_Ok) {
            free(blob.data);
            continue;
        }
        if (strcmp(type_copy, "Exif") == 0) {
            size_t offset = size >= 4 ? 4 : 0;
            if (size - offset > SIZE_MAX - 6) {
                free(blob.data); free(ids); return -1;
            }
            size_t payload_size = size - offset + 6;
            uint8_t *payload = malloc(payload_size);
            if (!payload || payload_size > MAX_JPEG_MARKER) {
                free(payload); free(blob.data); free(ids); return -1;
            }
            normalize_exif_orientation(blob.data, size);
            memcpy(payload, "Exif\0\0", 6);
            memcpy(payload + 6, blob.data + offset, size - offset);
            if (write_marker(jpeg, JPEG_APP0 + 1, payload, payload_size) != 0) {
                free(payload); free(blob.data); free(ids); return -1;
            }
            free(payload);
            ++written;
        } else if (strcmp(type_copy, "XMP") == 0 ||
                   strcmp(content_copy, "application/rdf+xml") == 0) {
            static const char xmp_header[] = "http://ns.adobe.com/xap/1.0/\0";
            size_t header_size = sizeof(xmp_header) - 1;
            if (size > SIZE_MAX - header_size) {
                free(blob.data); free(ids); return -1;
            }
            size_t payload_size = header_size + size;
            uint8_t *payload = malloc(payload_size);
            if (!payload || payload_size > MAX_JPEG_MARKER) {
                free(payload); free(blob.data); free(ids); return -1;
            }
            memcpy(payload, xmp_header, header_size);
            memcpy(payload + header_size, blob.data, size);
            if (write_marker(jpeg, JPEG_APP0 + 1, payload, payload_size) != 0) {
                free(payload); free(blob.data); free(ids); return -1;
            }
            free(payload);
            ++written;
        }
        free(blob.data);
    }
    free(ids);

    {
        size_t icc_size = heif_image_handle_get_raw_color_profile_size(handle);
        if (icc_size > UINT_MAX) return -1;
        if (icc_size > 0) {
            uint8_t *icc = malloc(icc_size);
            if (!icc) return -1;
            if (heif_image_handle_get_raw_color_profile(handle, icc).code == heif_error_Ok) {
                jpeg_write_icc_profile(jpeg, icc, (unsigned int)icc_size);
                ++written;
            }
            free(icc);
        }
    }
    return written;
}

static int encode_jpeg(FILE *file, const struct heif_image *image,
                       const struct heif_image_handle *handle, int quality)
{
    struct jpeg_compress_struct jpeg;
    struct jpeg_failure failure;
    JSAMPROW y_rows[16];
    JSAMPROW cb_rows[8];
    JSAMPROW cr_rows[8];
    JSAMPARRAY planes[3] = {y_rows, cb_rows, cr_rows};
    const uint8_t *y;
    const uint8_t *cb;
    const uint8_t *cr;
    int y_stride = 0, cb_stride = 0, cr_stride = 0;
    int width = heif_image_get_primary_width(image);
    int height = heif_image_get_primary_height(image);
    int chroma_width = (width + 1) / 2;
    int chroma_height = (height + 1) / 2;
    int result = -1;

    y = heif_image_get_plane_readonly(image, heif_channel_Y, &y_stride);
    cb = heif_image_get_plane_readonly(image, heif_channel_Cb, &cb_stride);
    cr = heif_image_get_plane_readonly(image, heif_channel_Cr, &cr_stride);
    if (!y || !cb || !cr || width <= 0 || height <= 0 ||
        y_stride < width || cb_stride < chroma_width || cr_stride < chroma_width) {
        fprintf(stderr, "csharp: decoded image has no usable 8-bit YCbCr planes\n");
        return -1;
    }
    memset(&jpeg, 0, sizeof(jpeg));
    memset(&failure, 0, sizeof(failure));
    jpeg.err = jpeg_std_error(&failure.base);
    failure.base.error_exit = jpeg_error_exit;
    if (setjmp(failure.jump) != 0) {
        fprintf(stderr, "csharp: JPEG encoder: %s\n", failure.message);
        jpeg_destroy_compress(&jpeg);
        return -1;
    }
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, file);
    jpeg.image_width = (JDIMENSION)width;
    jpeg.image_height = (JDIMENSION)height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&jpeg);
    jpeg_set_colorspace(&jpeg, JCS_YCbCr);
    jpeg.raw_data_in = TRUE;
    jpeg_set_quality(&jpeg, quality, TRUE);
    jpeg_start_compress(&jpeg, TRUE);
    if (write_metadata(&jpeg, handle) < 0) goto done;
    for (int row = 0; row < height; row += 16) {
        for (int i = 0; i < 16; ++i) {
            int source = row + i < height ? row + i : height - 1;
            y_rows[i] = (JSAMPROW)(y + (size_t)source * (size_t)y_stride);
        }
        for (int i = 0; i < 8; ++i) {
            int source = row / 2 + i < chroma_height ? row / 2 + i : chroma_height - 1;
            cb_rows[i] = (JSAMPROW)(cb + (size_t)source * (size_t)cb_stride);
            cr_rows[i] = (JSAMPROW)(cr + (size_t)source * (size_t)cr_stride);
        }
        if (jpeg_write_raw_data(&jpeg, planes, 16) != 16) goto done;
    }
    result = 0;
done:
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    return result;
}

static int supported_input(struct heif_context *ctx, struct heif_image_handle *handle,
                           enum backend backend)
{
    enum heif_colorspace colorspace = heif_colorspace_undefined;
    enum heif_chroma chroma = heif_chroma_undefined;
    int bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    int chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);
    if (bits != 8 || chroma_bits != 8) {
        fprintf(stderr, "csharp: unsupported bit depth (luma=%d chroma=%d; v0.1 requires 8-bit)\n",
                bits, chroma_bits);
        return -1;
    }
    if (heif_image_handle_has_alpha_channel(handle)) {
        fputs("csharp: unsupported primary alpha channel (JPEG v0.1 is opaque)\n", stderr);
        return -1;
    }
    if (heif_context_get_number_of_top_level_images(ctx) < 1) {
        fputs("csharp: input contains no top-level image\n", stderr);
        return -1;
    }
    if (heif_image_handle_get_preferred_decoding_colorspace(handle, &colorspace, &chroma).code != heif_error_Ok ||
        (colorspace != heif_colorspace_YCbCr && colorspace != heif_colorspace_undefined) ||
        (chroma != heif_chroma_420 && chroma != heif_chroma_undefined)) {
        fputs("csharp: unsupported source colorspace/chroma for v0.1\n", stderr);
        return -1;
    }
#ifndef CSHARP_HAVE_DIRECT_NVDEC
    if (backend == BACKEND_DIRECT) {
        fputs("csharp: direct backend was not built; use --backend cpu or rebuild with CUDA\n", stderr);
        return -1;
    }
#else
    (void)backend;
#endif
    return 0;
}

static int make_temp_path(const char *output, char *temp, size_t capacity, int *fd)
{
    int written = snprintf(temp, capacity, "%s.csharp-tmp-XXXXXX", output);
    if (written < 0 || (size_t)written >= capacity) return -1;
    *fd = mkstemp(temp);
    return *fd >= 0 ? 0 : -1;
}

static int install_output(const char *temp, const char *output, int overwrite)
{
    if (overwrite) return rename(temp, output);
    return (int)syscall(SYS_renameat2, AT_FDCWD, temp, AT_FDCWD, output,
                        RENAME_NOREPLACE);
}

static int convert_file(const char *input, const char *output, int overwrite,
                        enum backend backend, int quality, int verbose)
{
    struct heif_context *ctx = NULL;
    struct heif_image_handle *handle = NULL;
    struct heif_image *image = NULL;
    struct heif_decoding_options *options = NULL;
    struct heif_error error;
    char temp[PATH_MAX];
    FILE *file = NULL;
    int fd = -1;
    int result = -1;

    temp[0] = '\0';

    if (!overwrite && access(output, F_OK) == 0) {
        fprintf(stderr, "csharp: destination exists (use --overwrite): %s\n", output);
        return -1;
    }
    ctx = heif_context_alloc();
    if (!ctx) { fputs("csharp: cannot allocate libheif context\n", stderr); goto cleanup; }
    error = heif_context_read_from_file(ctx, input, NULL);
    if (error.code != heif_error_Ok) { print_heif_error("read input", error); goto cleanup; }
    error = heif_context_get_primary_image_handle(ctx, &handle);
    if (error.code != heif_error_Ok) { print_heif_error("get primary image", error); goto cleanup; }
    if (supported_input(ctx, handle, backend) != 0) goto cleanup;
    if (backend == BACKEND_DIRECT) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
        error = csharp_register_direct_nvdec_plugin();
        if (error.code != heif_error_Ok) { print_heif_error("register direct decoder", error); goto cleanup; }
#endif
    }
    options = heif_decoding_options_alloc();
    if (!options) { fputs("csharp: cannot allocate decoding options\n", stderr); goto cleanup; }
#ifdef CSHARP_HAVE_DIRECT_NVDEC
    if (backend == BACKEND_DIRECT) options->decoder_id = "csharp-direct-nvdec";
#endif
    error = heif_decode_image(handle, &image, heif_colorspace_YCbCr, heif_chroma_420, options);
    if (error.code != heif_error_Ok) { print_heif_error("decode image", error); goto cleanup; }
    if (make_temp_path(output, temp, sizeof(temp), &fd) != 0) {
        fprintf(stderr, "csharp: cannot create temporary output: %s\n", strerror(errno)); goto cleanup;
    }
    file = fdopen(fd, "wb");
    if (!file) { close(fd); fd = -1; goto cleanup; }
    fd = -1;
    if (verbose) fprintf(stderr, "csharp: encoding %dx%d -> %s\n",
                         heif_image_get_primary_width(image), heif_image_get_primary_height(image), output);
    if (encode_jpeg(file, image, handle, quality) != 0) goto cleanup;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        file = NULL; fputs("csharp: failed to flush JPEG output\n", stderr); goto cleanup;
    }
    file = NULL;
    if (overwrite) {
        if (rename(temp, output) != 0) goto cleanup;
    } else if (install_output(temp, output, 0) != 0) {
        fprintf(stderr, "csharp: atomic rename failed: %s\n", strerror(errno)); goto cleanup;
    }
    result = 0;
cleanup:
    if (file) fclose(file);
    if (fd >= 0) close(fd);
    if (result != 0 && temp[0] != '\0') unlink(temp);
    heif_image_release(image);
    heif_decoding_options_free(options);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return result;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: csharp [options] INPUT.heic OUTPUT.jpg\n"
                   "  --quality N       JPEG quality 1-100 (default %d)\n"
                   "  --overwrite       replace an existing destination\n"
                   "  --backend direct|cpu\n"
                   "  --verbose         print conversion diagnostics\n"
                   "  --version         print version\n"
                   "  --help            print this help\n", DEFAULT_QUALITY);
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    enum backend backend = BACKEND_DIRECT;
    int quality = DEFAULT_QUALITY;
    int overwrite = 0;
    int verbose = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout); return EXIT_SUCCESS; }
        if (strcmp(argv[i], "--version") == 0) { puts(CSHARP_VERSION); return EXIT_SUCCESS; }
        if (strcmp(argv[i], "--overwrite") == 0) { overwrite = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
            char *end = NULL; long value = strtol(argv[++i], &end, 10);
            if (!end || *end != '\0' || value < 1 || value > 100) {
                fputs("csharp: quality must be an integer from 1 to 100\n", stderr); return EXIT_FAILURE;
            }
            quality = (int)value; continue;
        }
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "direct") == 0) backend = BACKEND_DIRECT;
            else if (strcmp(argv[i], "cpu") == 0) backend = BACKEND_CPU;
            else { fputs("csharp: backend must be direct or cpu\n", stderr); return EXIT_FAILURE; }
            continue;
        }
        if (argv[i][0] == '-') { fprintf(stderr, "csharp: unknown option: %s\n", argv[i]); return EXIT_FAILURE; }
        if (!input) input = argv[i];
        else if (!output) output = argv[i];
        else { fputs("csharp: expected one input and one output\n", stderr); return EXIT_FAILURE; }
    }
    if (!input || !output) { usage(stderr); return EXIT_FAILURE; }
    return convert_file(input, output, overwrite, backend, quality, verbose) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
