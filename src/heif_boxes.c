#include "heif_boxes.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const unsigned char *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

int heif_scan_top_level_boxes(const char *path, struct heif_box_summary *summary)
{
    FILE *fp;
    uint64_t offset = 0;

    if (!path || !summary) return -1;
    memset(summary, 0, sizeof(*summary));

    fp = fopen(path, "rb");
    if (!fp) return -1;

    for (;;) {
        unsigned char header[16];
        uint64_t size;
        size_t header_size = 8;
        char type[5] = {0};

        if (fseek(fp, (long)offset, SEEK_SET) != 0) break;
        if (fread(header, 1, 8, fp) != 8) break;

        size = be32(header);
        memcpy(type, header + 4, 4);

        if (size == 1) {
            if (fread(header + 8, 1, 8, fp) != 8) {
                fclose(fp);
                return -1;
            }
            size = be64(header + 8);
            header_size = 16;
        } else if (size == 0) {
            if (fseek(fp, 0, SEEK_END) != 0) {
                fclose(fp);
                return -1;
            }
            long end = ftell(fp);
            if (end < 0 || (uint64_t)end < offset) {
                fclose(fp);
                return -1;
            }
            size = (uint64_t)end - offset;
        }

        if (size < header_size || UINT64_MAX - offset < size) {
            fclose(fp);
            return -1;
        }

        summary->box_count++;
        if (strcmp(type, "ftyp") == 0) summary->has_ftyp = 1;
        if (strcmp(type, "meta") == 0) summary->has_meta = 1;
        if (strcmp(type, "mdat") == 0) {
            summary->has_mdat = 1;
            summary->mdat_offset = offset + header_size;
            summary->mdat_size = size - header_size;
        }

        offset += size;
    }

    fclose(fp);
    return summary->box_count ? 0 : -1;
}
