#ifndef CSHARP_NVDEC_PLUGIN_H
#define CSHARP_NVDEC_PLUGIN_H

#include <libheif/heif.h>

#ifdef __cplusplus
extern "C" {
#endif

enum csharp_nvdec_backend {
    CSHARP_NVDEC_BACKEND_CUVID = 0,
    CSHARP_NVDEC_BACKEND_NATIVE = 1,
    CSHARP_NVDEC_BACKEND_AUTO = 2
};

struct csharp_nvdec_stats {
    unsigned long cuda_device_initializations;
    unsigned long decoder_initializations;
    unsigned long decoder_reuses;
    unsigned long decodes;
    double acquire_ms;
    double annexb_ms;
    double decode_ms;
    double transfer_ms;
    double output_ms;
    double total_ms;
};

struct heif_error csharp_register_nvdec_plugin(void);
void csharp_nvdec_set_verbose(int enabled);
void csharp_nvdec_set_backend(enum csharp_nvdec_backend backend);
enum csharp_nvdec_backend csharp_nvdec_get_backend(void);
const char *csharp_nvdec_backend_name(enum csharp_nvdec_backend backend);
void csharp_nvdec_reset_stats(void);
void csharp_nvdec_get_stats(struct csharp_nvdec_stats *stats);
unsigned long csharp_nvdec_cuda_device_initializations(void);
unsigned long csharp_nvdec_decoder_initializations(void);
unsigned long csharp_nvdec_decoder_reuses(void);

#ifdef __cplusplus
}
#endif

#endif
