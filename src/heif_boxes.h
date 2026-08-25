#ifndef CSHARP_HEIF_BOXES_H
#define CSHARP_HEIF_BOXES_H

#include <stdint.h>

struct heif_box_summary {
    unsigned box_count;
    int has_ftyp;
    int has_meta;
    int has_mdat;
    uint64_t mdat_offset;
    uint64_t mdat_size;
};

int heif_scan_top_level_boxes(const char *path, struct heif_box_summary *summary);

#endif
