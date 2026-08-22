#ifndef CSHARP_INPUT_CLASSIFY_H
#define CSHARP_INPUT_CLASSIFY_H

#include <libheif/heif.h>

enum csharp_fallback_reason {
    CSHARP_REASON_NONE = 0,
    CSHARP_REASON_NOT_8_BIT,
    CSHARP_REASON_ALPHA,
    CSHARP_REASON_NON_420,
    CSHARP_REASON_UNSUPPORTED_COLORSPACE,
    CSHARP_REASON_CPU_REQUESTED,
    CSHARP_REASON_DIRECT_DECODE_FAILED,
    CSHARP_REASON_NO_IMAGE
};

struct csharp_input_classification {
    int direct_eligible;
    enum csharp_fallback_reason reason;
    int luma_bits;
    int chroma_bits;
    int has_alpha;
    enum heif_colorspace colorspace;
    enum heif_chroma chroma;
};

void csharp_classify_input(const struct heif_image_handle *handle,
                           struct csharp_input_classification *out);
const char *csharp_fallback_reason_name(enum csharp_fallback_reason reason);

#endif
