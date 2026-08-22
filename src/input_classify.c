#include "input_classify.h"

#include <string.h>

void csharp_classify_input(const struct heif_image_handle *handle,
                           struct csharp_input_classification *out)
{
    struct csharp_input_classification value;
    memset(&value, 0, sizeof(value));
    value.reason = CSHARP_REASON_NO_IMAGE;
    value.colorspace = heif_colorspace_undefined;
    value.chroma = heif_chroma_undefined;
    if (!handle) {
        if (out) *out = value;
        return;
    }
    value.luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    value.chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);
    value.has_alpha = heif_image_handle_has_alpha_channel(handle) ? 1 : 0;
    (void)heif_image_handle_get_preferred_decoding_colorspace(
        handle, &value.colorspace, &value.chroma);
    value.direct_eligible = 1;
    value.reason = CSHARP_REASON_NONE;
    if (value.luma_bits != 8 || value.chroma_bits != 8) {
        value.direct_eligible = 0;
        value.reason = CSHARP_REASON_NOT_8_BIT;
    } else if (value.has_alpha) {
        value.direct_eligible = 0;
        value.reason = CSHARP_REASON_ALPHA;
    } else if (value.colorspace != heif_colorspace_YCbCr &&
               value.colorspace != heif_colorspace_undefined) {
        value.direct_eligible = 0;
        value.reason = CSHARP_REASON_UNSUPPORTED_COLORSPACE;
    } else if (value.chroma != heif_chroma_420 &&
               value.chroma != heif_chroma_undefined) {
        value.direct_eligible = 0;
        value.reason = CSHARP_REASON_NON_420;
    }
    if (out) *out = value;
}

const char *csharp_fallback_reason_name(enum csharp_fallback_reason reason)
{
    switch (reason) {
        case CSHARP_REASON_NONE: return "none";
        case CSHARP_REASON_NOT_8_BIT: return "not-8-bit";
        case CSHARP_REASON_ALPHA: return "alpha";
        case CSHARP_REASON_NON_420: return "non-420";
        case CSHARP_REASON_UNSUPPORTED_COLORSPACE: return "unsupported-colorspace";
        case CSHARP_REASON_CPU_REQUESTED: return "cpu-requested";
        case CSHARP_REASON_DIRECT_DECODE_FAILED: return "direct-decode-failed";
        case CSHARP_REASON_NO_IMAGE: return "no-image";
        default: return "unknown";
    }
}
