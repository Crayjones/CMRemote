#include "RdpResize.h"
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>

bool resizeRdpLegacyDesktop(rdpContext* context, RdpResizeOperations operations) {
    if (!context || !context->settings || !context->gdi || !context->codecs || !context->update) return false;
    auto* gdi = context->gdi;
    if (gdi->context != context || !gdi->primary || !gdi->primary_buffer) return false;
    const auto width = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    const auto height = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    if (!width || !height || width > 8192 || height > 8192 || UINT64(width) * height > 16 * 1024 * 1024) return false;
    if (gdi->width == INT32(width) && gdi->height == INT32(height)) return true;
    if (!operations.reset || !operations.resize ||
        !operations.reset(context->codecs, FREERDP_CODEC_PLANAR, width, height)) return false;
    // gdi_resize flushes the old primary before replacing it. Failure ends the session.
    return operations.resize(gdi, width, height);
}
