#pragma once
#include <freerdp/codecs.h>
#include <freerdp/gdi/gdi.h>

struct RdpResizeOperations {
    decltype(&freerdp_client_codecs_reset) reset = freerdp_client_codecs_reset;
    decltype(&gdi_resize) resize = gdi_resize;
};

bool resizeRdpLegacyDesktop(rdpContext* context, RdpResizeOperations operations = {});
