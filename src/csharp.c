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

#include "input_classify.h"

#ifdef CSHARP_HAVE_DIRECT_NVDEC
#include "direct_nvdec_plugin.h"
#endif

#define CSHARP_VERSION "0.1.0"
#define DEFAULT_QUALITY 90
#define MAX_JPEG_MARKER 65533U

enum backend { BACKEND_DIRECT, BACKEND_CPU };

struct conversion_report {
    const char *input;
    const char *output;
    const char *requested_backend;
    const char *selected_decode_path;
    const char *error_stage;
    struct csharp_input_classification classification;
    int quality;
    int width;
    int height;
    int metadata_exif_count;
    int metadata_xmp_count;
    int icc_present;
    int status;
};

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

static int encode_rgb(FILE *file, const struct heif_image *image,
                      const struct heif_image_handle *handle, int quality)
{
    struct jpeg_compress_struct jpeg;
    struct jpeg_failure failure;
    const uint8_t *pixels;
    uint8_t *row_buffer = NULL;
    int stride = 0;
    int width = heif_image_get_primary_width(image);
    int height = heif_image_get_primary_height(image);
    enum heif_chroma chroma = heif_image_get_chroma_format(image);
    int channels = chroma == heif_chroma_interleaved_RGBA ? 4 : 3;
    int result = -1;

    pixels = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
    if (!pixels || width <= 0 || height <= 0 || stride < width * channels ||
        (channels != 3 && channels != 4)) {
        fputs("csharp: decoded image has no usable RGB/RGBA plane\n", stderr);
        return -1;
    }
    if (channels == 4) {
        if ((size_t)width > SIZE_MAX / 3U) return -1;
        row_buffer = malloc((size_t)width * 3U);
        if (!row_buffer) return -1;
    }
    memset(&jpeg, 0, sizeof(jpeg));
    memset(&failure, 0, sizeof(failure));
    jpeg.err = jpeg_std_error(&failure.base);
    failure.base.error_exit = jpeg_error_exit;
    if (setjmp(failure.jump) != 0) {
        fprintf(stderr, "csharp: JPEG encoder: %s\n", failure.message);
        free(row_buffer);
        jpeg_destroy_compress(&jpeg);
        return -1;
    }
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, file);
    jpeg.image_width = (JDIMENSION)width;
    jpeg.image_height = (JDIMENSION)height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, quality, TRUE);
    jpeg_start_compress(&jpeg, TRUE);
    if (write_metadata(&jpeg, handle) < 0) goto done;
    for (int row = 0; row < height; ++row) {
        JSAMPROW scanline;
        if (channels == 4) {
            const uint8_t *src = pixels + (size_t)row * (size_t)stride;
            for (int x = 0; x < width; ++x) {
                unsigned int alpha = src[(size_t)x * 4U + 3U];
                for (int c = 0; c < 3; ++c) {
                    unsigned int value = src[(size_t)x * 4U + (size_t)c];
                    row_buffer[(size_t)x * 3U + (size_t)c] =
                        (uint8_t)((value * alpha + 255U * (255U - alpha) + 127U) / 255U);
                }
            }
            scanline = row_buffer;
        } else {
            scanline = (JSAMPROW)(pixels + (size_t)row * (size_t)stride);
        }
        if (jpeg_write_scanlines(&jpeg, &scanline, 1) != 1) goto done;
    }
    result = 0;
done:
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    free(row_buffer);
    return result;
}

static int encode_jpeg(FILE *file, const struct heif_image *image,
                       const struct heif_image_handle *handle, int quality)
{
    if (heif_image_get_colorspace(image) == heif_colorspace_RGB) {
        return encode_rgb(file, image, handle, quality);
    }
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

static void inspect_metadata(const struct heif_image_handle *handle,
                             struct conversion_report *report)
{
    int count = heif_image_handle_get_number_of_metadata_blocks(handle, NULL);
    heif_item_id *ids;
    if (!report || count <= 0) return;
    ids = calloc((size_t)count, sizeof(*ids));
    if (!ids) return;
    count = heif_image_handle_get_list_of_metadata_block_IDs(handle, NULL, ids, count);
    for (int i = 0; i < count; ++i) {
        const char *type = heif_image_handle_get_metadata_type(handle, ids[i]);
        const char *content = heif_image_handle_get_metadata_content_type(handle, ids[i]);
        if (type && strcmp(type, "Exif") == 0) ++report->metadata_exif_count;
        if ((type && strcmp(type, "XMP") == 0) ||
            (content && strcmp(content, "application/rdf+xml") == 0))
            ++report->metadata_xmp_count;
    }
    free(ids);
    report->icc_present = heif_image_handle_get_raw_color_profile_size(handle) > 0 ? 1 : 0;
}

static int write_report(const char *path, const struct conversion_report *report)
{
    FILE *file;
    if (!path || !report) return 0;
    file = fopen(path, "w");
    if (!file) return -1;
    fprintf(file, "input\toutput\trequested_backend\tselected_decode_path\tdirect_eligible\t"
                 "fallback_reason\twidth\theight\tluma_bits\tchroma_bits\thas_alpha\tquality\t"
                 "metadata_exif_count\tmetadata_xmp_count\ticc_present\tstatus\terror_stage\n");
    fprintf(file, "%s\t%s\t%s\t%s\t%d\t%s\t"
                 "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
            report->input ? report->input : "",
            report->output ? report->output : "",
            report->requested_backend ? report->requested_backend : "",
            report->selected_decode_path ? report->selected_decode_path : "unknown",
            report->classification.direct_eligible,
            csharp_fallback_reason_name(report->classification.reason),
            report->width, report->height,
            report->classification.luma_bits, report->classification.chroma_bits,
            report->classification.has_alpha, report->quality,
            report->metadata_exif_count, report->metadata_xmp_count,
            report->icc_present, report->status == 0 ? "success" : "failed",
            report->error_stage ? report->error_stage : "");
    return fclose(file) == 0 ? 0 : -1;
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
                        enum backend backend, int quality, int verbose,
                        const char *report_path, int no_fallback)
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
    int fast_path;
    int use_direct;
    struct conversion_report report;

    memset(&report, 0, sizeof(report));
    report.input = input;
    report.output = output;
    report.requested_backend = backend == BACKEND_DIRECT ? "direct" : "cpu";
    report.selected_decode_path = "unknown";
    report.error_stage = "none";
    report.quality = quality;

    temp[0] = '\0';

    if (!overwrite && access(output, F_OK) == 0) {
        report.error_stage = "output-exists";
        fprintf(stderr, "csharp: destination exists (use --overwrite): %s\n", output);
        goto cleanup;
    }
    ctx = heif_context_alloc();
    if (!ctx) { report.error_stage = "context"; fputs("csharp: cannot allocate libheif context\n", stderr); goto cleanup; }
    error = heif_context_read_from_file(ctx, input, NULL);
    if (error.code != heif_error_Ok) { report.error_stage = "read"; print_heif_error("read input", error); goto cleanup; }
    error = heif_context_get_primary_image_handle(ctx, &handle);
    if (error.code != heif_error_Ok) { report.error_stage = "primary"; print_heif_error("get primary image", error); goto cleanup; }
    csharp_classify_input(handle, &report.classification);
    report.width = heif_image_handle_get_width(handle);
    report.height = heif_image_handle_get_height(handle);
    inspect_metadata(handle, &report);
    fast_path = report.classification.direct_eligible;
    use_direct = backend == BACKEND_DIRECT && fast_path;
    if (backend == BACKEND_CPU) {
        report.classification.reason = CSHARP_REASON_CPU_REQUESTED;
        report.selected_decode_path = "cpu";
    } else if (!fast_path) {
        report.selected_decode_path = "cpu-fallback-feature";
    } else {
        report.selected_decode_path = "direct";
    }
    if (backend == BACKEND_DIRECT && !fast_path && verbose)
        fputs("csharp: using libheif CPU fallback for non-NVDEC HEIC features\n", stderr);
    if (backend == BACKEND_DIRECT && !fast_path && no_fallback) {
        report.error_stage = "classification";
        fputs("csharp: input is not eligible for direct NVDEC and --no-fallback was set\n", stderr);
        goto cleanup;
    }
    if (backend == BACKEND_DIRECT) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
        if (use_direct) {
            error = csharp_register_direct_nvdec_plugin();
            if (error.code != heif_error_Ok) { print_heif_error("register direct decoder", error); goto cleanup; }
        }
#else
        report.error_stage = "backend";
        fputs("csharp: direct backend was not built; use --backend cpu or rebuild with CUDA\n", stderr);
        goto cleanup;
#endif
    }
    options = heif_decoding_options_alloc();
    if (!options) { report.error_stage = "options"; fputs("csharp: cannot allocate decoding options\n", stderr); goto cleanup; }
    if (use_direct) {
#ifdef CSHARP_HAVE_DIRECT_NVDEC
        options->decoder_id = "csharp-direct-nvdec";
#endif
    }
    error = heif_decode_image(handle, &image,
                              fast_path ? heif_colorspace_YCbCr : heif_colorspace_RGB,
                              fast_path ? heif_chroma_420 :
                              (heif_image_handle_has_alpha_channel(handle) ?
                               heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB),
                              options);
    if (error.code != heif_error_Ok && use_direct) {
        report.classification.reason = CSHARP_REASON_DIRECT_DECODE_FAILED;
        report.selected_decode_path = "cpu-fallback-decode-failure";
        if (verbose) fputs("csharp: direct decode failed; retrying with libheif CPU decoder\n", stderr);
        heif_image_release(image);
        image = NULL;
        options->decoder_id = NULL;
        error = heif_decode_image(handle, &image, heif_colorspace_YCbCr,
                                  heif_chroma_420, options);
    }
    if (error.code != heif_error_Ok) { report.error_stage = "decode"; print_heif_error("decode image", error); goto cleanup; }
    if (make_temp_path(output, temp, sizeof(temp), &fd) != 0) {
        report.error_stage = "temp-output"; fprintf(stderr, "csharp: cannot create temporary output: %s\n", strerror(errno)); goto cleanup;
    }
    file = fdopen(fd, "wb");
    if (!file) { close(fd); fd = -1; goto cleanup; }
    fd = -1;
    if (verbose) fprintf(stderr, "csharp: encoding %dx%d -> %s\n",
                         heif_image_get_primary_width(image), heif_image_get_primary_height(image), output);
    if (encode_jpeg(file, image, handle, quality) != 0) { report.error_stage = "encode"; goto cleanup; }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        file = NULL; report.error_stage = "flush-output"; fputs("csharp: failed to flush JPEG output\n", stderr); goto cleanup;
    }
    file = NULL;
    if (overwrite) {
        if (rename(temp, output) != 0) { report.error_stage = "install"; goto cleanup; }
    } else if (install_output(temp, output, 0) != 0) {
        report.error_stage = "install"; fprintf(stderr, "csharp: atomic rename failed: %s\n", strerror(errno)); goto cleanup;
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
    report.status = result == 0 ? 0 : 1;
    if (write_report(report_path, &report) != 0)
        fprintf(stderr, "csharp: cannot write report: %s\n", strerror(errno));
    return result;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: csharp [options] INPUT.heic OUTPUT.jpg\n"
                   "  --quality N       JPEG quality 1-100 (default %d)\n"
                   "  --overwrite       replace an existing destination\n"
                   "  --backend direct|cpu\n"
                   "  --no-fallback     fail instead of using CPU fallback\n"
                   "  --report PATH     write one machine-readable TSV result\n"
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
    int no_fallback = 0;
    const char *report_path = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { usage(stdout); return EXIT_SUCCESS; }
        if (strcmp(argv[i], "--version") == 0) { puts(CSHARP_VERSION); return EXIT_SUCCESS; }
        if (strcmp(argv[i], "--overwrite") == 0) { overwrite = 1; continue; }
        if (strcmp(argv[i], "--verbose") == 0) { verbose = 1; continue; }
        if (strcmp(argv[i], "--no-fallback") == 0) { no_fallback = 1; continue; }
        if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) { report_path = argv[++i]; continue; }
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
    return convert_file(input, output, overwrite, backend, quality, verbose,
                        report_path, no_fallback) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
