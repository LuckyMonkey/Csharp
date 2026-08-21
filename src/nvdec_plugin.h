#ifndef CSHARP_NVDEC_PLUGIN_H
#define CSHARP_NVDEC_PLUGIN_H

#include <libheif/heif.h>

struct heif_error csharp_register_nvdec_plugin(void);
void csharp_nvdec_set_verbose(int enabled);
void csharp_nvdec_reset_stats(void);
unsigned long csharp_nvdec_cuda_device_initializations(void);
unsigned long csharp_nvdec_decoder_initializations(void);
unsigned long csharp_nvdec_decoder_reuses(void);

#endif
