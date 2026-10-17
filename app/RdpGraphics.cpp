#include "RdpGraphics.h"
#include "DesktopUpdate.h"
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/settings.h>
#include <freerdp/codecs.h>
#include <limits>
#include <freerdp/config.h>
#include <freerdp/codec/h264.h>
#include <freerdp/gdi/gfx.h>

bool rdpDesktopSizeValid(quint32 width, quint32 height) {
    return width <= 8192 && height <= 8192 && desktopSizeValid(QSize(int(width), int(height)));
}
QRegion rdpDesktopDamage(const rdpGdi* gdi) {
    if (!gdi || !gdi->primary || !gdi->primary->hdc || !gdi->primary->hdc->hwnd ||
        !rdpDesktopSizeValid(gdi->width, gdi->height)) return {};
    const auto* window = gdi->primary->hdc->hwnd;
    if (!window->invalid || window->invalid->null) return {};
    const QRect bounds(0, 0, gdi->width, gdi->height);
    if (window->ninvalid <= 0 || window->ninvalid > DesktopUpdateAccumulator::MaxRegions ||
        UINT32(window->ninvalid) > window->count || !window->cinvalid) return QRegion(bounds);
    QRegion damage;
    for (int i = 0; i < window->ninvalid; ++i) {
        const auto& region = window->cinvalid[i];
        if (region.null) continue;
        if (region.x < 0 || region.y < 0 || region.w <= 0 || region.h <= 0 ||
            qint64(region.x) + region.w > gdi->width || qint64(region.y) + region.h > gdi->height) return QRegion(bounds);
        damage |= QRect(region.x, region.y, region.w, region.h);
        if (damage.rectCount() > DesktopUpdateAccumulator::MaxRegions) return QRegion(bounds);
    }
    return damage;
}
bool resizeRdpDesktop(rdpContext* context, quint64& geometry) {
    if (!context || !context->settings || !context->update || !context->gdi ||
        !context->gdi->primary || !context->codecs) return false;
    const auto width = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth);
    const auto height = freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight);
    if (!rdpDesktopSizeValid(width, height)) return false;
    if (context->gdi->width == int(width) && context->gdi->height == int(height)) return true;
    if (geometry == std::numeric_limits<quint64>::max()) return false;
    if (!freerdp_client_codecs_reset(context->codecs, FREERDP_CODEC_PLANAR, width, height)) return false;
    // gdi_resize flushes the OLD primary through EndPaint before replacing it.
    // Keep its geometry stamp unchanged until the replacement has succeeded.
    if (!gdi_resize(context->gdi, width, height)) return false;
    ++geometry;
    return true;
}

bool rdpSoftwareAvcAvailable() {
// With runtime loading this succeeds only once the app has supplied the verified Cisco binary.
#if defined(WITH_OPENH264) && defined(WITH_GFX_H264)
    auto* decoder = h264_context_new(FALSE);
    if (!decoder) return false;
    h264_context_free(decoder);
    return true;
#else
    return false;
#endif
}
bool applyRdpGraphicsPolicy(rdpSettings* settings, RdpGraphicsPolicy policy, bool softwareAvc) {
    if (!settings) return false;
    const bool gfx = policy == RdpGraphicsPolicy::Gfx &&
        freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth) == 32;
    const bool avc = gfx && softwareAvc;
    return freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, gfx) &&
        freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, gfx) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxH264, avc) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, avc) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, avc) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxCodecAV1, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_AsyncChannels, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_SynchronousDynamicChannels, TRUE);
}

RdpGfxPipeline::Operations RdpGfxPipeline::systemOperations() {
    return {gdi_graphics_pipeline_init, gdi_graphics_pipeline_uninit};
}
RdpGfxPipeline::RdpGfxPipeline(Operations operations) : operations_(operations) {}
RdpGfxPipeline::~RdpGfxPipeline() { close(); }
bool RdpGfxPipeline::initialize(RdpGraphicsContext* context, RdpgfxClientContext* gfx, Hooks hooks) {
    if (active() || !context || context->pipeline || !gfx || gfx->codecs || !operations_.init ||
        !operations_.uninit || !context->base.gdi || context->base.gdi->gfx || !context->base.settings ||
        freerdp_settings_get_bool(context->base.settings, FreeRDP_DeactivateClientDecoding) ||
        !rdpDesktopSizeValid(context->base.gdi->width, context->base.gdi->height)) return false;
    context_ = context; gfx_ = gfx; hooks_ = hooks; previous_ = *gfx; context_->pipeline = this;
    if (!operations_.init(context_->base.gdi, gfx_)) {
        // Pinned 3.32.1 can fail after callback/codec assignment, BEFORE mux initialization.
        // Its uninit is unsafe on that path. Free only the allocated codecs and restore callbacks.
        freerdp_client_codecs_free(gfx_->codecs); gfx_->codecs = nullptr;
        restore(); context_->pipeline = nullptr; context_->base.gdi->gfx = nullptr;
        context_ = nullptr; gfx_ = nullptr; hooks_ = {};
        return false;
    }
    installed_ = *gfx_;
    gfx_->StartFrame = startFrame; gfx_->EndFrame = endFrame; gfx_->UpdateSurfaces = updateSurfaces;
    gfx_->SurfaceCommand = surfaceCommand; gfx_->CreateSurface = createSurface;
    gfx_->DeleteSurface = deleteSurface; gfx_->ResetGraphics = resetGraphics;
    return true;
}
void RdpGfxPipeline::restore() {
    // Restore only fields assigned by the pinned GDI initializer; channel-owned fields stay intact.
#define RESTORE(name) gfx_->name = previous_.name
    RESTORE(custom); RESTORE(ResetGraphics); RESTORE(StartFrame); RESTORE(EndFrame);
    RESTORE(SurfaceCommand); RESTORE(DeleteEncodingContext); RESTORE(CreateSurface); RESTORE(DeleteSurface);
    RESTORE(SolidFill); RESTORE(SurfaceToSurface); RESTORE(SurfaceToCache); RESTORE(CacheToSurface);
    RESTORE(CacheImportReply); RESTORE(ImportCacheEntry); RESTORE(ExportCacheEntry); RESTORE(EvictCacheEntry);
    RESTORE(MapSurfaceToOutput); RESTORE(MapSurfaceToWindow); RESTORE(MapSurfaceToScaledOutput);
    RESTORE(MapSurfaceToScaledWindow); RESTORE(UpdateSurfaces); RESTORE(MapWindowForSurface);
    RESTORE(UnmapWindowForSurface); RESTORE(UpdateSurfaceArea);
#undef RESTORE
}
UINT RdpGfxPipeline::close() {
    if (!active()) return CHANNEL_RC_OK;
    UINT status = CHANNEL_RC_OK;
    // Normally the channel's OnClose already deleted these before its disconnect notification.
    // Also cover explicit unwind while the channel interface and GDI are still alive.
    for (const auto& [id, pixels] : surfacePixels_) {
        Q_UNUSED(pixels);
        const RDPGFX_DELETE_SURFACE_PDU pdu{id};
        const UINT result = installed_.DeleteSurface(gfx_, &pdu);
        if (status == CHANNEL_RC_OK && result != CHANNEL_RC_OK) status = result;
    }
    surfacePixels_.clear();
    operations_.uninit(context_->base.gdi, gfx_);
    restore(); context_->pipeline = nullptr;
    context_->base.gdi->inGfxFrame = FALSE;
    context_ = nullptr; gfx_ = nullptr; hooks_ = {};
    pending_ = collecting_ = inFrame_ = ending_ = false; frameId_ = 0; damage_ = {};
    return status;
}
RdpGfxPipeline* RdpGfxPipeline::from(RdpgfxClientContext* gfx) {
    // Once restored, custom belongs to the channel's previous owner, not GDI.
    // Reject saved wrappers before interpreting that pointer.
    if (!gfx || gfx->StartFrame != startFrame || gfx->EndFrame != endFrame || !gfx->custom) return nullptr;
    auto* gdi = static_cast<rdpGdi*>(gfx->custom);
    auto* context = reinterpret_cast<RdpGraphicsContext*>(gdi->context);
    if (!context || !context->pipeline || context->pipeline->gfx_ != gfx) return nullptr;
    return context->pipeline;
}
bool RdpGfxPipeline::collectPaint() {
    if (!active() || !collecting_) return false;
    damage_ |= rdpDesktopDamage(context_->base.gdi);
    if (damage_.rectCount() > DesktopUpdateAccumulator::MaxRegions)
        damage_ = QRegion(QRect(0, 0, context_->base.gdi->width, context_->base.gdi->height));
    pending_ = true;
    return true;
}
bool RdpGfxPipeline::publish() {
    if (!pending_) return true;
    pending_ = false;
    auto damage = std::move(damage_); damage_ = {};
    return !hooks_.publish || hooks_.publish(hooks_.user, damage);
}
UINT RdpGfxPipeline::startFrame(RdpgfxClientContext* gfx, const RDPGFX_START_FRAME_PDU* pdu) {
    auto* self = from(gfx);
    if (!self || !pdu || self->inFrame_) return ERROR_INVALID_DATA;
    const UINT status = self->installed_.StartFrame(gfx, pdu);
    if (status == CHANNEL_RC_OK) { self->inFrame_ = true; self->frameId_ = pdu->frameId; }
    return status;
}
UINT RdpGfxPipeline::endFrame(RdpgfxClientContext* gfx, const RDPGFX_END_FRAME_PDU* pdu) {
    auto* self = from(gfx);
    if (!self || !pdu || !self->inFrame_ || self->frameId_ != pdu->frameId) return ERROR_INVALID_DATA;
    self->ending_ = true;
    const UINT status = self->installed_.EndFrame(gfx, pdu);
    self->ending_ = false; self->inFrame_ = false;
    if (status != CHANNEL_RC_OK) { self->pending_ = false; self->damage_ = {}; return status; }
    return self->publish() ? CHANNEL_RC_OK : ERROR_INTERNAL_ERROR;
}
UINT RdpGfxPipeline::updateSurfaces(RdpgfxClientContext* gfx) {
    auto* self = from(gfx);
    if (!self || self->collecting_) return ERROR_INVALID_DATA;
    self->collecting_ = true;
    const UINT status = self->installed_.UpdateSurfaces(gfx);
    self->collecting_ = false;
    if (status != CHANNEL_RC_OK) { self->pending_ = false; self->damage_ = {}; return status; }
    if (!self->inFrame_ && !self->ending_ && !self->publish()) return ERROR_INTERNAL_ERROR;
    return CHANNEL_RC_OK;
}
UINT RdpGfxPipeline::surfaceCommand(RdpgfxClientContext* gfx, const RDPGFX_SURFACE_COMMAND* command) {
    auto* self = from(gfx);
    if (!self || !command) return ERROR_INVALID_DATA;
    const UINT status = self->installed_.SurfaceCommand(gfx, command);
    if (status == CHANNEL_RC_OK && self->hooks_.codec)
        self->hooks_.codec(self->hooks_.user, rdpGfxReceivedCodec(command->codecId), command->width, command->height);
    return status;
}
ReceivedCodec rdpGfxReceivedCodec(quint16 codecId) {
    switch (codecId) {
    case RDPGFX_CODECID_UNCOMPRESSED: return ReceivedCodec::Uncompressed;
    case RDPGFX_CODECID_CAVIDEO: return ReceivedCodec::RemoteFx;
    case RDPGFX_CODECID_PLANAR: return ReceivedCodec::Planar;
    case RDPGFX_CODECID_AVC420: return ReceivedCodec::Avc420;
    case RDPGFX_CODECID_AVC444: return ReceivedCodec::Avc444;
    case RDPGFX_CODECID_AVC444v2: return ReceivedCodec::Avc444v2;
    case RDPGFX_CODECID_CLEARCODEC: return ReceivedCodec::ClearCodec;
    case RDPGFX_CODECID_CAPROGRESSIVE: return ReceivedCodec::Progressive;
    case RDPGFX_CODECID_CAPROGRESSIVE_V2: return ReceivedCodec::ProgressiveV2;
    case RDPGFX_CODECID_ALPHA: return ReceivedCodec::Alpha;
    default: return ReceivedCodec::Unknown;
    }
}
UINT RdpGfxPipeline::createSurface(RdpgfxClientContext* gfx, const RDPGFX_CREATE_SURFACE_PDU* pdu) {
    auto* self = from(gfx);
    if (!self || !pdu || !rdpDesktopSizeValid(pdu->width, pdu->height) ||
        self->surfacePixels_.size() >= 64 || self->surfacePixels_.count(pdu->surfaceId)) return ERROR_INVALID_DATA;
    quint64 pixels = quint64(pdu->width) * pdu->height;
    for (const auto& entry : self->surfacePixels_) pixels += entry.second;
    if (pixels > 16 * 1024 * 1024) return ERROR_NOT_ENOUGH_MEMORY;
    const UINT status = self->installed_.CreateSurface(gfx, pdu);
    if (status == CHANNEL_RC_OK) self->surfacePixels_[pdu->surfaceId] = quint64(pdu->width) * pdu->height;
    return status;
}
UINT RdpGfxPipeline::deleteSurface(RdpgfxClientContext* gfx, const RDPGFX_DELETE_SURFACE_PDU* pdu) {
    auto* self = from(gfx);
    if (!self || !pdu) return ERROR_INVALID_DATA;
    const UINT status = self->installed_.DeleteSurface(gfx, pdu);
    // Pinned GDI frees the surface even when window unmapping reports an error.
    self->surfacePixels_.erase(pdu->surfaceId);
    return status;
}
UINT RdpGfxPipeline::resetGraphics(RdpgfxClientContext* gfx, const RDPGFX_RESET_GRAPHICS_PDU* pdu) {
    auto* self = from(gfx);
    if (!self || !pdu || self->inFrame_ || !rdpDesktopSizeValid(pdu->width, pdu->height)) return ERROR_INVALID_DATA;
    self->pending_ = false; self->damage_ = {};
    return self->installed_.ResetGraphics(gfx, pdu);
}
