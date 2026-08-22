#ifndef CSHARP_DIRECT_NVDEC_PLUGIN_H
#define CSHARP_DIRECT_NVDEC_PLUGIN_H

#include <libheif/heif.h>

struct heif_error csharp_register_direct_nvdec_plugin(void);

struct csharp_direct_nvdec_stats {
    unsigned long lane_creates;
    unsigned long lane_reuses;
    unsigned long decoder_creates;
    unsigned long decoder_reconfigures;
    unsigned long decodes;
};

void csharp_direct_nvdec_reset_stats(void);
void csharp_direct_nvdec_get_stats(struct csharp_direct_nvdec_stats *stats);

#endif
