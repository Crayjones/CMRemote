#include "RdpWorker.h"
#include "RdpGraphics.h"
#include "RdpCursor.h"
#include "RdpSecurity.h"
#include <QDir>
#include <QElapsedTimer>
#include <QtEndian>
#include <freerdp/freerdp.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <cstring>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/channels/disp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <freerdp/codec/color.h>
#include <freerdp/addin.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>
#include <winpr/string.h>
#include <algorithm>
#include <limits>

struct ChannelState {
    std::mutex mutex;
    CliprdrClientContext* clipboard = nullptr;
    DispClientContext* display = nullptr;
    bool displayReady = false;
    quint64 maxArea = 0;
    // cliprdr callbacks run on FreeRDP's channel thread, beside the worker loop: everything
    // below is touched only under this lock. Never take `mutex` while holding it.
    std::recursive_mutex clipboardLock;
    ClipboardProtocol clipboardProtocol;
    QString filesNotice;
    std::unique_ptr<RemoteFileDownload> download;
    quint64 downloadList = 0;
    std::optional<quint32> downloadLock;
    QElapsedTimer downloadProgress;
};

struct RdpWorker::Context {
    RdpGraphicsContext graphicsContext;
    RdpWorker* worker;
    ChannelState* channels;
    RdpGfxPipeline* gfxOwner;
    bool graphicsFailed;
    quint64 geometry = 1;
    pBitmapUpdate bitmapUpdate;
    pSurfaceBits surfaceBits;
    quint32 observedCodecs;
    static Context* from(rdpContext* c) { return reinterpret_cast<Context*>(c); }
    static Context* from(freerdp* i) { return from(i->context); }
    static Context* clip(CliprdrClientContext* c) { return static_cast<Context*>(c->custom); }
    static void observed(rdpContext* c, ReceivedCodec codec, quint32 width, quint32 height) {
        auto* self = from(c);
        const quint32 bit = 1u << quint32(codec);
        if (self->observedCodecs & bit) return;
        self->observedCodecs |= bit;
        const auto now = PerformanceTrace::now();
        PerformanceSample sample{PerformanceStage::ReceivedCodec, {self->geometry, 0, now, now, 0},
            now, now, 0, width, height};
        sample.codec = codec; self->worker->config_.performance->record(sample);
    }
    static BOOL tracedBitmapUpdate(rdpContext* c, const BITMAP_UPDATE* update) {
        auto* self = from(c);
        const BOOL ok = self->bitmapUpdate(c, update);
        if (ok) for (UINT32 i = 0; i < update->number; ++i) {
            const auto& rectangle = update->rectangles[i];
            // gdi_Bitmap_Decompress selects planar for compressed 32-bit legacy bitmaps.
            const auto codec = !rectangle.compressed ? ReceivedCodec::Uncompressed :
                rectangle.bitsPerPixel >= 32 ? ReceivedCodec::Planar : ReceivedCodec::Interleaved;
            observed(c, codec, rectangle.width, rectangle.height);
        }
        return ok;
    }
    static BOOL tracedSurfaceBits(rdpContext* c, const SURFACE_BITS_COMMAND* command) {
        auto* self = from(c);
        const BOOL ok = self->surfaceBits(c, command);
        if (ok) {
            auto codec = ReceivedCodec::Unknown;
            switch (command->bmp.codecID) {
            case RDP_CODEC_ID_NONE: codec = ReceivedCodec::Uncompressed; break;
            case RDP_CODEC_ID_REMOTEFX: case RDP_CODEC_ID_IMAGE_REMOTEFX: codec = ReceivedCodec::RemoteFx; break;
            case RDP_CODEC_ID_NSCODEC: codec = ReceivedCodec::NsCodec; break;
            }
            observed(c, codec, command->bmp.width, command->bmp.height);
        }
        return ok;
    }
    static BOOL beginPaint(rdpContext* c) {
        if (!c->gdi || !c->gdi->primary) return FALSE;
        c->gdi->primary->hdc->hwnd->invalid->null = TRUE;
        c->gdi->primary->hdc->hwnd->ninvalid = 0;
        return TRUE;
    }
    static BOOL endPaint(rdpContext* c) {
        auto* self = from(c);
        auto* g = c->gdi;
        if (!g) return FALSE;
        if (self->worker->stopping_ || g->primary->hdc->hwnd->invalid->null) return TRUE;
        if (self->graphicsContext.pipeline && self->graphicsContext.pipeline->collectPaint()) return TRUE;
        return publishDesktop(self, rdpDesktopDamage(g));
    }
    static bool publishDesktop(void* user, const QRegion& damage) {
        auto* self = static_cast<Context*>(user);
        if (self->worker->stopping_) return true;
        auto* g = self->graphicsContext.base.gdi;
        if (!g) return false;
        auto trace = self->worker->config_.performance;
        const auto begin = trace ? PerformanceTrace::now() : 0;
        FrameStamp stamp{self->geometry, ++self->worker->frameSequence_, begin, begin, 0};
        return self->worker->publish(g->primary_buffer, QSize(g->width, g->height), g->stride, damage, stamp);
    }
    static BOOL desktopResize(rdpContext* c) {
        if (resizeRdpDesktop(c, from(c)->geometry)) return TRUE;
        emit from(c)->worker->stateChanged("failed", "RDP desktop resize rejected; closing session");
        return FALSE;
    }
    static UINT displayCaps(DispClientContext* c, UINT32 monitors, UINT32 a, UINT32 b) {
        auto* self = static_cast<Context*>(c->custom);
        {
            std::lock_guard<std::mutex> lock(self->channels->mutex);
            self->channels->displayReady = monitors > 0;
            self->channels->maxArea = quint64(a) * b;
        }
        emit self->worker->resizeAvailable(monitors > 0 && self->worker->config_.options.dynamicResize());
        return CHANNEL_RC_OK;
    }
    static void clipboardBatch(Context* self, const RdpInputQueue::Batch& batch) {
        auto& protocol = self->channels->clipboardProtocol;
        if (batch.clipboardControl) protocol.control(*batch.clipboardControl);
        if (batch.clipboardDirty) protocol.local(batch.clipboard, batch.clipboardStamp);
        if (batch.clipboardFilesDirty && protocol.acceptsLocal(batch.clipboardFilesStamp)) {
            QString error; auto files = buildLocalFileList(batch.clipboardFiles, &error);
            // One notice per distinct problem, not one per focus change.
            if (error != self->channels->filesNotice && !error.isEmpty() && !self->worker->stopping_) emit self->worker->clipboardNotice(error);
            self->channels->filesNotice = error;
            protocol.localFiles(std::move(files), batch.clipboardFilesStamp);
        }
        if (!protocol.enabled()) cancelDownload(self, "Clipboard sharing was turned off, so the file copy stopped.");
    }
    static void refreshClipboard(Context* self) { clipboardBatch(self, self->worker->input_.takeClipboard()); }
    static void finishDownload(Context* self) {
        auto download = std::move(self->channels->download);
        if (download) { self->channels->clipboardProtocol.unpin(); self->channels->downloadLock.reset(); }
        if (download && !self->worker->stopping_) {
            emit self->worker->fileTransferProgress(download->done(), download->total());
            emit self->worker->fileTransferFinished(!download->failed(), download->summary());
        }
    }
    static void cancelDownload(Context* self, const QString& reason) {
        if (!self->channels->download) return;
        self->channels->download->cancel(reason); finishDownload(self);
    }
    static void startDownload(Context* self, const QString& directory, quint64 list) {
        auto* worker = self->worker;
        if (self->channels->download) { emit worker->fileTransferFinished(false, "A file copy is already running for this session."); return; }
        const auto files = self->channels->clipboard ? self->channels->clipboardProtocol.remoteFiles(list) : nullptr;
        if (!files) { emit worker->fileTransferFinished(false, "The remote clipboard no longer holds those files. Copy them again on the remote computer."); return; }
        self->channels->download = std::make_unique<RemoteFileDownload>(*files, directory);
        self->channels->downloadList = list; self->channels->downloadProgress.start();
        self->channels->downloadLock = self->channels->clipboardProtocol.remoteFilesLock(list);
        self->channels->clipboardProtocol.pin(self->channels->downloadLock);
        if (!self->channels->download->start()) { finishDownload(self); return; }
        emit worker->fileTransferProgress(0, self->channels->download->total());
    }
    static UINT pumpDownload(Context* self) {
        auto& download = self->channels->download; auto* c = self->channels->clipboard;
        if (!download) return CHANNEL_RC_OK;
        if (!c) { cancelDownload(self, "The clipboard channel closed, so the file copy stopped."); return CHANNEL_RC_OK; }
        if (const auto request = download->next()) {
            CLIPRDR_FILE_CONTENTS_REQUEST message{}; message.common.msgType = CB_FILECONTENTS_REQUEST;
            message.streamId = request->stream; message.listIndex = request->index; message.dwFlags = request->flags;
            message.nPositionLow = quint32(request->position); message.nPositionHigh = quint32(request->position >> 32);
            message.cbRequested = request->size;
            message.haveClipDataId = self->channels->downloadLock.has_value(); message.clipDataId = self->channels->downloadLock.value_or(0);
            return c->ClientFileContentsRequest(c, &message);
        }
        if (download->finished() || download->failed()) finishDownload(self);
        return CHANNEL_RC_OK;
    }
    static void publishRemoteFiles(Context* self) {
        auto update = self->channels->clipboardProtocol.takeRemoteFiles();
        if (!update) return;
        int files = 0; if (update->files) for (const auto& file : update->files->files) files += !file.directory;
        // Announce the new list before stopping the old copy so its outcome reads as superseded.
        if (!self->worker->stopping_)
            emit self->worker->remoteFilesChanged(update->id, update->files ? qMax(files, 1) : 0, update->files ? update->files->bytes : 0, update->stamp);
        if (self->channels->download && self->channels->downloadList != update->id)
            cancelDownload(self, "The remote clipboard changed, so the file copy stopped.");
    }
    static UINT unlockFiles(Context* self, CliprdrClientContext* c) {
        for (const auto id : self->channels->clipboardProtocol.takeUnlocks()) {
            CLIPRDR_UNLOCK_CLIPBOARD_DATA unlock{}; unlock.common.msgType = CB_UNLOCK_CLIPDATA; unlock.clipDataId = id;
            if (const UINT status = c->ClientUnlockClipboardData(c, &unlock); status != CHANNEL_RC_OK) return status;
        }
        return CHANNEL_RC_OK;
    }
    static UINT pumpClipboard(Context* self) {
        refreshClipboard(self);
        auto* c = self->channels->clipboard;
        if (!c || self->worker->stopping_) return CHANNEL_RC_OK;
        auto& protocol = self->channels->clipboardProtocol;
        if (const UINT status = unlockFiles(self, c); status != CHANNEL_RC_OK) return status;
        if (auto advertised = protocol.advertisement()) {
            CLIPRDR_FORMAT formats[2]{}; UINT32 count = 0;
            if (advertised->text) formats[count++].formatId = CF_UNICODETEXT;
            if (advertised->files) {
                formats[count].formatId = ClipboardFileListFormatId;
                formats[count++].formatName = const_cast<char*>(ClipboardFileListName);
            }
            CLIPRDR_FORMAT_LIST list{}; list.common.msgType = CB_FORMAT_LIST;
            list.numFormats = count; list.formats = count ? formats : nullptr;
            const UINT status = c->ClientFormatList(c, &list); if (status != CHANNEL_RC_OK) return status;
        }
        if (const auto lock = protocol.takeLock()) {
            CLIPRDR_LOCK_CLIPBOARD_DATA message{}; message.common.msgType = CB_LOCK_CLIPDATA; message.clipDataId = *lock;
            if (const UINT status = c->ClientLockClipboardData(c, &message); status != CHANNEL_RC_OK) return status;
        }
        if (const auto request = protocol.request()) {
            CLIPRDR_FORMAT_DATA_REQUEST message{}; message.common.msgType = CB_FORMAT_DATA_REQUEST; message.requestedFormatId = request->format;
            const UINT status = c->ClientFormatDataRequest(c, &message); if (status != CHANNEL_RC_OK) return status;
        }
        publishRemoteFiles(self);
        if (const UINT status = pumpDownload(self); status != CHANNEL_RC_OK) return status;
        return unlockFiles(self, c);
    }
    static UINT monitorReady(CliprdrClientContext* c, const CLIPRDR_MONITOR_READY*) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        CLIPRDR_GENERAL_CAPABILITY_SET general{};
        general.capabilitySetType = CB_CAPSTYPE_GENERAL; general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
        // The channel drops any flag the server did not also advertise.
        general.version = CB_CAPS_VERSION_2; general.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED |
            CB_FILECLIP_NO_FILE_PATHS | CB_CAN_LOCK_CLIPDATA | CB_HUGE_FILE_SUPPORT_ENABLED;
        CLIPRDR_CAPABILITIES caps{}; caps.common.msgType = CB_CLIP_CAPS; caps.cCapabilitiesSets = 1;
        caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
        const UINT status = c->ClientCapabilities(c, &caps);
        if (status != CHANNEL_RC_OK) return status;
        refreshClipboard(clip(c)); clip(c)->channels->clipboardProtocol.ready(); return pumpClipboard(clip(c));
    }
    static UINT serverCapabilities(CliprdrClientContext* c, const CLIPRDR_CAPABILITIES* caps) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        // The channel hands over exactly the server's general capability set.
        const auto* general = caps->cCapabilitiesSets ? reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(caps->capabilitySets) : nullptr;
        if (general && general->capabilitySetType == CB_CAPSTYPE_GENERAL) clip(c)->channels->clipboardProtocol.serverCapabilities(general->generalFlags);
        return CHANNEL_RC_OK;
    }
    static UINT formatResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE*) { return CHANNEL_RC_OK; }
    static UINT formats(CliprdrClientContext* c, const CLIPRDR_FORMAT_LIST* list) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        CLIPRDR_FORMAT_LIST_RESPONSE response{}; response.common.msgType = CB_FORMAT_LIST_RESPONSE; response.common.msgFlags = CB_RESPONSE_OK;
        const UINT status = c->ClientFormatListResponse(c, &response);
        if (status != CHANNEL_RC_OK) return status;
        refreshClipboard(clip(c)); bool unicode = false; std::optional<quint32> files;
        if (list->formats) for (UINT32 i = 0; i < list->numFormats; ++i) {
            const auto& format = list->formats[i];
            unicode |= format.formatId == CF_UNICODETEXT;
            if (format.formatName && !strcmp(format.formatName, ClipboardFileListName)) files = format.formatId;
        }
        clip(c)->channels->clipboardProtocol.offer(unicode, files); return pumpClipboard(clip(c));
    }
    static UINT dataResponse(CliprdrClientContext* c, const CLIPRDR_FORMAT_DATA_RESPONSE* response) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        auto* self = clip(c); refreshClipboard(self);
        auto delivery = self->channels->clipboardProtocol.response(response->requestedFormatData, response->common.dataLen,
            response->common.msgFlags & CB_RESPONSE_OK);
        if (delivery && !self->worker->stopping_) {
            emit self->worker->remoteClipboard(delivery->text);
            bool notify;
            {
                std::lock_guard<std::mutex> lock(self->worker->clipboardMutex_);
                if (self->worker->stopping_) return CHANNEL_RC_OK;
                notify = !self->worker->clipboardNotificationPending_;
                self->worker->clipboardNotificationPending_ = true;
                self->worker->pendingClipboard_ = std::move(delivery);
            }
            if (notify) emit self->worker->clipboardAvailable();
        }
        return pumpClipboard(self);
    }
    static UINT dataRequest(CliprdrClientContext* c, const CLIPRDR_FORMAT_DATA_REQUEST* request) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        auto* self = clip(c); refreshClipboard(self);
        CLIPRDR_FORMAT_DATA_RESPONSE response{}; response.common.msgType = CB_FORMAT_DATA_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_FAIL;
        const auto& protocol = self->channels->clipboardProtocol;
        const auto* bytes = request->requestedFormatId == CF_UNICODETEXT ? protocol.localReply() :
            request->requestedFormatId == ClipboardFileListFormatId ? protocol.localFileList() : nullptr;
        if (!self->worker->stopping_ && bytes) {
            response.common.msgFlags = CB_RESPONSE_OK; response.common.dataLen = bytes->size();
            response.requestedFormatData = reinterpret_cast<const BYTE*>(bytes->constData());
        }
        return c->ClientFormatDataResponse(c, &response);
    }
    static UINT fileContentsRequest(CliprdrClientContext* c, const CLIPRDR_FILE_CONTENTS_REQUEST* request) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        auto* self = clip(c); refreshClipboard(self);
        CLIPRDR_FILE_CONTENTS_RESPONSE response{}; response.common.msgType = CB_FILECONTENTS_RESPONSE;
        response.common.msgFlags = CB_RESPONSE_FAIL; response.streamId = request->streamId;
        std::optional<QByteArray> bytes;
        if (!self->worker->stopping_)
            bytes = self->channels->clipboardProtocol.fileContents(request->haveClipDataId ? std::optional<quint32>(request->clipDataId) : std::nullopt,
                request->listIndex, request->dwFlags, quint64(request->nPositionHigh) << 32 | request->nPositionLow, request->cbRequested);
        if (bytes) {
            response.common.msgFlags = CB_RESPONSE_OK; response.cbRequested = UINT32(bytes->size());
            response.requestedData = reinterpret_cast<const BYTE*>(bytes->constData());
        }
        return c->ClientFileContentsResponse(c, &response);
    }
    static UINT fileContentsResponse(CliprdrClientContext* c, const CLIPRDR_FILE_CONTENTS_RESPONSE* response) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        auto* self = clip(c); refreshClipboard(self);
        if (auto& download = self->channels->download) {
            download->response(response->streamId, response->requestedData, response->cbRequested, response->common.msgFlags & CB_RESPONSE_OK);
            if (download && !download->failed() && self->channels->downloadProgress.elapsed() >= 100 && !self->worker->stopping_) {
                self->channels->downloadProgress.restart(); emit self->worker->fileTransferProgress(download->done(), download->total());
            }
        }
        return pumpClipboard(self);
    }
    static UINT lockData(CliprdrClientContext* c, const CLIPRDR_LOCK_CLIPBOARD_DATA* lock) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        refreshClipboard(clip(c)); clip(c)->channels->clipboardProtocol.lock(lock->clipDataId); return CHANNEL_RC_OK;
    }
    static UINT unlockData(CliprdrClientContext* c, const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlock) {
        std::lock_guard<std::recursive_mutex> guard(clip(c)->channels->clipboardLock);
        clip(c)->channels->clipboardProtocol.unlock(unlock->clipDataId); return CHANNEL_RC_OK;
    }
    static void connected(void* c, const ChannelConnectedEventArgs* event) {
        auto* self = from(static_cast<rdpContext*>(c));
        if (!strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME)) {
            auto* gfx = static_cast<RdpgfxClientContext*>(event->pInterface);
            RdpGfxPipeline::Hooks hooks{self, publishDesktop, [](void* user, ReceivedCodec codec, quint32 width, quint32 height) {
                auto* context = static_cast<Context*>(user);
                if (context->worker->config_.performance) observed(&context->graphicsContext.base, codec, width, height);
            }};
            if (!self->gfxOwner->initialize(&self->graphicsContext, gfx, hooks)) {
                self->graphicsFailed = true;
                emit self->worker->stateChanged("failed", "RDP graphics channel initialization failed; closing session");
                setChannelError(&self->graphicsContext.base, ERROR_INTERNAL_ERROR, "RDP graphics channel initialization failed");
            }
            return;
        }
        std::lock_guard<std::mutex> lock(self->channels->mutex);
        if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME)) {
            self->channels->clipboard = static_cast<CliprdrClientContext*>(event->pInterface);
            auto* clip = self->channels->clipboard; clip->custom = self;
            clip->MonitorReady = monitorReady; clip->ServerCapabilities = serverCapabilities;
            clip->ServerFormatList = formats; clip->ServerFormatListResponse = formatResponse;
            clip->ServerFormatDataRequest = dataRequest; clip->ServerFormatDataResponse = dataResponse;
            clip->ServerFileContentsRequest = fileContentsRequest; clip->ServerFileContentsResponse = fileContentsResponse;
            clip->ServerLockClipboardData = lockData; clip->ServerUnlockClipboardData = unlockData;
        } else if (!strcmp(event->name, DISP_DVC_CHANNEL_NAME)) {
            self->channels->display = static_cast<DispClientContext*>(event->pInterface);
            self->channels->display->custom = self; self->channels->display->DisplayControlCaps = displayCaps;
        }
    }
    static void disconnected(void* c, const ChannelDisconnectedEventArgs* event) {
        auto* self = from(static_cast<rdpContext*>(c));
        if (!strcmp(event->name, RDPGFX_DVC_CHANNEL_NAME)) {
            if (self->gfxOwner->close() != CHANNEL_RC_OK) {
                self->graphicsFailed = true;
                emit self->worker->stateChanged("failed", "RDP graphics channel cleanup failed");
                setChannelError(&self->graphicsContext.base, ERROR_INTERNAL_ERROR, "RDP graphics channel cleanup failed");
            }
            return;
        }
        std::lock_guard<std::mutex> lock(self->channels->mutex);
        if (!strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME)) {
            std::lock_guard<std::recursive_mutex> guard(self->channels->clipboardLock);
            self->channels->clipboard = nullptr; self->channels->clipboardProtocol.reset();
            cancelDownload(self, "The clipboard channel closed, so the file copy stopped.");
        }
        if (!strcmp(event->name, DISP_DVC_CHANNEL_NAME)) {
            self->channels->display = nullptr; self->channels->displayReady = false; emit self->worker->resizeAvailable(false);
        }
    }
    static BOOL preConnect(freerdp* i) {
        return PubSub_SubscribeChannelConnected(i->context->pubSub, connected) >= 0 &&
               PubSub_SubscribeChannelDisconnected(i->context->pubSub, disconnected) >= 0;
    }
    static BOOL setCursor(rdpContext* c, rdpPointer* pointer) {
        auto cursor = rdpCursorImage(pointer);
        if (cursor.kind != RemoteCursor::Kind::Bitmap || !cursor.valid()) return FALSE;
        auto* worker = from(c)->worker;
        if (!worker->stopping_ && worker->pendingCursor_.publish(std::move(cursor))) emit worker->cursorAvailable();
        return TRUE;
    }
    static BOOL hideCursor(rdpContext* c) {
        auto* worker = from(c)->worker;
        if (!worker->stopping_ && worker->pendingCursor_.publish({RemoteCursor::Kind::Hidden})) emit worker->cursorAvailable();
        return TRUE;
    }
    static BOOL defaultCursor(rdpContext* c) {
        auto* worker = from(c)->worker;
        if (!worker->stopping_ && worker->pendingCursor_.publish({})) emit worker->cursorAvailable();
        return TRUE;
    }
    static BOOL cursorPosition(rdpContext* c, UINT32 x, UINT32 y) {
        auto* worker = from(c)->worker;
        if (!worker->stopping_ && x <= 0x7fff && y <= 0x7fff) emit worker->remotePointerMoved(QPoint(int(x), int(y)));
        return TRUE;
    }
    static BOOL postConnect(freerdp* i) {
        if (!rdpDesktopSizeValid(freerdp_settings_get_uint32(i->context->settings, FreeRDP_DesktopWidth),
                                freerdp_settings_get_uint32(i->context->settings, FreeRDP_DesktopHeight))) return FALSE;
        if (!gdi_init(i, PIXEL_FORMAT_BGRA32)) return FALSE;
        const auto pointer = rdpCursorPrototype(setCursor, hideCursor, defaultCursor, cursorPosition);
        graphics_register_pointer(i->context->graphics, &pointer);
        i->context->update->BeginPaint = beginPaint; i->context->update->EndPaint = endPaint;
        i->context->update->DesktopResize = desktopResize;
        if (from(i)->worker->config_.performance) {
            from(i)->bitmapUpdate = i->context->update->BitmapUpdate;
            from(i)->surfaceBits = i->context->update->SurfaceBits;
            if (!from(i)->bitmapUpdate || !from(i)->surfaceBits) return FALSE;
            i->context->update->BitmapUpdate = tracedBitmapUpdate;
            i->context->update->SurfaceBits = tracedSurfaceBits;
        }
        const quint64 width = freerdp_settings_get_uint32(i->context->settings, FreeRDP_DesktopWidth), height = freerdp_settings_get_uint32(i->context->settings, FreeRDP_DesktopHeight);
        const auto depth = freerdp_settings_get_uint32(i->context->settings, FreeRDP_ColorDepth);
        from(i)->worker->negotiatedDepth_ = depth; from(i)->worker->negotiated_ = width << 32 | height;
        emit from(i)->worker->desktopNegotiated(QSize(int(width), int(height)), depth);
        emit from(i)->worker->relativePointerAvailable(freerdp_settings_get_bool(i->context->settings, FreeRDP_HasRelativeMouseEvent));
        emit from(i)->worker->stateChanged("connected", "Embedded RDP connected");
        return TRUE;
    }
    static BOOL loadChannels(freerdp* i) {
        return freerdp_client_load_channels(i) && requestRdpMessageChannel(i->context->settings);
    }
    // Reached only for certificates FreeRDP could not verify by CA chain or its
    // known_hosts store; policy per Server Authentication (V100).
    static DWORD decide(freerdp* i) {
        auto* worker = from(i)->worker;
        if (rdpCertificateDecision(worker->config_.options.serverAuth) == RdpCertificateDecision::Reject) {
            worker->certificateRejected_ = true; return RdpCertificateReject;
        }
        return RdpCertificateSession;
    }
    static QString endpoint(const char* host, UINT16 port) {
        const auto name = QString::fromUtf8(host ? host : "");
        return (name.contains(':') ? "[" + name + "]" : name) + ":" + QString::number(port);
    }
    static DWORD verify(freerdp* i, const char*, UINT16, const char*, const char*, const char*, const char*, DWORD) {
        return decide(i);
    }
    static DWORD changed(freerdp* i, const char*, UINT16, const char*, const char*, const char*, const char*,
                         const char*, const char*, const char*, DWORD) {
        return decide(i);
    }
    static bool replace(char** field, const QByteArray& value) {
        free(*field); *field = nullptr; if (value.isEmpty()) return true;
        *field = _strdup(value.constData()); return *field != nullptr;
    }
    // FreeRDP asks only when a username or password is missing; the answer is
    // session-only and never saved (gateway passwords, V106).
    static BOOL authenticate(freerdp* i, char** username, char** password, char** domain, rdp_auth_reason reason) {
        auto* worker = from(i)->worker;
        const bool gateway = reason == GW_AUTH_HTTP || reason == GW_AUTH_RDG || reason == GW_AUTH_RPC;
        if (!gateway && reason != AUTH_NLA && reason != AUTH_TLS && reason != AUTH_RDP) return FALSE;
        const auto& o = worker->config_.options;
        const auto target = gateway ? endpoint(o.gatewayHost.toUtf8().constData(), o.gatewayPort) : endpoint(worker->config_.host.toUtf8().constData(), worker->config_.port);
        auto answer = worker->credentials(gateway, target, QString::fromUtf8(*username ? *username : ""), QString::fromUtf8(*domain ? *domain : ""));
        if (!answer) return FALSE;
        // TransientCredentials wipes its password copy on destruction.
        return replace(username, answer->username.toUtf8()) && replace(domain, answer->domain.toUtf8()) && replace(password, answer->password);
    }
};

RdpWorker::RdpWorker(RdpConfig config, QObject* parent) : QThread(parent), config_(std::move(config)) {
    if (config_.performance && !config_.performance->enabled()) config_.performance.reset();
    qRegisterMetaType<ClipboardDelivery>(); qRegisterMetaType<ClipboardStamp>();
    input_.clipboardControl({config_.clipboard, config_.clipboard, {1, 0, 0}});
}
RdpWorker::~RdpWorker() { stop(); wait(); config_.password.fill(0); }
void RdpWorker::stop() {
    stopping_ = true; input_.stop(); pendingCursor_.clear();
    { std::lock_guard<std::mutex> lock(clipboardMutex_); pendingClipboard_.reset(); }
    { std::lock_guard<std::mutex> lock(lifetimeMutex_); if (context_) freerdp_abort_connect_context(context_); }
    credentialsCv_.notify_all();
}
void RdpWorker::provideCredentials(std::optional<TransientCredentials> credentials) {
    { std::lock_guard<std::mutex> lock(credentialsMutex_); credentialAnswer_ = std::move(credentials); credentialsAnswered_ = true; }
    credentialsCv_.notify_all();
}
std::optional<TransientCredentials> RdpWorker::credentials(bool gateway, const QString& endpoint, const QString& username, const QString& domain) {
    std::unique_lock<std::mutex> lock(credentialsMutex_); credentialAnswer_.reset(); credentialsAnswered_ = false;
    emit credentialsRequired(gateway, endpoint, username, domain);
    credentialsCv_.wait(lock, [this] { return credentialsAnswered_ || stopping_; });
    auto answer = std::move(credentialAnswer_); credentialAnswer_.reset();
    if (stopping_) return std::nullopt;
    return answer;
}
QImage RdpWorker::copyFramebuffer(const uchar* bytes, int width, int height, int stride) {
    if (!bytes || width <= 0 || height <= 0 || !rdpDesktopSizeValid(width, height) || stride < width * 4) return {};
    return QImage(bytes, width, height, stride, QImage::Format_RGB32).copy();
}
bool RdpWorker::publish(const uchar* bytes, QSize size, int stride, const QRegion& damage, FrameStamp stamp) {
    DesktopUpdateAccumulator::Capture result;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        result = pendingUpdate_.capture(bytes, size, stride, damage, stamp);
        if (result.copiedBytes) pendingPublishedNs_ = config_.performance ? PerformanceTrace::now() : 0;
    }
    if (!result.accepted) {
        emit stateChanged("failed", "RDP framebuffer or damage batch rejected; closing session"); return false;
    }
    if (config_.performance && result.copiedBytes) config_.performance->record({PerformanceStage::Copy, stamp,
        stamp.latestNs, PerformanceTrace::now(), result.copiedBytes, quint32(size.width()), quint32(size.height())});
    if (result.notify) emit frameAvailable();
    return true;
}
DesktopUpdate RdpWorker::takeUpdate() {
    DesktopUpdate update;
    quint64 published;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        update = pendingUpdate_.take(); takenStamp_ = update.stamp; published = pendingPublishedNs_;
    }
    if (config_.performance && !update.patches.empty()) config_.performance->record({PerformanceStage::Mailbox,
        update.stamp, published, PerformanceTrace::now(), 0, quint32(update.size.width()), quint32(update.size.height())});
    return update;
}
QImage RdpWorker::takeFrame() {
    const auto update = takeUpdate();
    if (update.patches.empty()) return {};
    std::lock_guard<std::mutex> lock(frameMutex_);
    return applyDesktopUpdate(compatibilityFrame_, update) ? compatibilityFrame_ : QImage{};
}
FrameStamp RdpWorker::takenFrameStamp() {
    std::lock_guard<std::mutex> lock(frameMutex_); return takenStamp_;
}
void RdpWorker::checkInputResult(RdpInputQueue::Result result) {
    if (result == RdpInputQueue::Result::Overflow) {
        emit stateChanged("failed", "RDP input queue exceeded its bound; closing to avoid stuck keys"); stop();
    } else if (result == RdpInputQueue::Result::EventFailed) {
        emit stateChanged("failed", "RDP input event failed; closing to avoid stuck keys"); stop();
    }
}
void RdpWorker::enqueue(Command command) {
    if (stopping_) return;
    command.queuedNs = config_.performance ? PerformanceTrace::now() : 0;
    checkInputResult(input_.enqueue(std::move(command)));
}
void RdpWorker::key(quint32 scan, bool down, bool repeat) { Command c{0}; c.scan = scan; c.down = down; c.repeat = repeat; enqueue(std::move(c)); }
void RdpWorker::text(QString text) { if (text.size() > 4096) return; Command c{1}; c.text = std::move(text); enqueue(std::move(c)); }
void RdpWorker::pointer(quint16 flags, quint16 x, quint16 y) { Command c{2}; c.flags = flags; c.x = x; c.y = y; enqueue(std::move(c)); }
void RdpWorker::relativePointer(quint16 flags, qint16 dx, qint16 dy) { Command c{3}; c.flags = flags; c.x = quint16(dx); c.y = quint16(dy); enqueue(std::move(c)); }
void RdpWorker::resizeDesktop(QSize size) {
    if (stopping_ || !config_.options.dynamicResize()) return;
    checkInputResult(input_.resize(size));
}
void RdpWorker::localClipboard(QString text) {
    if (stopping_ || !config_.clipboard || text.size() > 512 * 1024 - 1) return;
    clipboardText(std::move(text), {1, 0, 0});
}
void RdpWorker::clipboardText(QString text, ClipboardStamp stamp) {
    if (stopping_ || text.size() > 512 * 1024 - 1) return;
    checkInputResult(input_.clipboard(std::move(text), stamp));
}
void RdpWorker::clipboardFiles(QStringList paths, ClipboardStamp stamp) {
    if (!stopping_) checkInputResult(input_.clipboardFiles(std::move(paths), stamp));
}
void RdpWorker::downloadFiles(QString directory, quint64 list) {
    if (!stopping_) checkInputResult(input_.download(std::move(directory), list));
}
void RdpWorker::cancelDownload() { if (!stopping_) checkInputResult(input_.cancelDownload()); }
void RdpWorker::configureClipboard(ClipboardControl control) {
    if (!control.enabled) { std::lock_guard<std::mutex> lock(clipboardMutex_); pendingClipboard_.reset(); }
    if (!stopping_) checkInputResult(input_.clipboardControl(control));
}
std::optional<ClipboardDelivery> RdpWorker::takeClipboard() {
    std::lock_guard<std::mutex> lock(clipboardMutex_);
    auto delivery = std::move(pendingClipboard_); pendingClipboard_.reset(); clipboardNotificationPending_ = false; return delivery;
}

bool RdpWorker::applyProfileSettings(rdp_settings* settings, const SessionTarget& target, QSize panelSize, QSize screenSize) {
    const auto normalized = normalizeRdpTarget(target); if (!settings || !normalized) return false;
    const auto& o = target.options; const auto size = o.initialSize(panelSize, screenSize);
    return freerdp_settings_set_string(settings, FreeRDP_ServerHostname, normalized->host.toUtf8().constData()) &&
        freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, target.port) &&
        freerdp_settings_set_string(settings, FreeRDP_Username, target.username.toUtf8().constData()) &&
        freerdp_settings_set_string(settings, FreeRDP_Domain, target.domain.toUtf8().constData()) &&
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, size.width()) &&
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, size.height()) &&
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, o.colorDepth) &&
        applyRdpSecurity(settings, o) && applyRdpAudio(settings, o.sound) &&
        freerdp_settings_set_bool(settings, FreeRDP_ConsoleSession, o.consoleSession) &&
        freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheEnabled, o.cacheBitmaps) &&
        freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, !o.wallpaper) &&
        freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, !o.themes) &&
        freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, o.fontSmoothing) &&
        freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, o.dynamicResize()) &&
        freerdp_settings_set_bool(settings, FreeRDP_DynamicResolutionUpdate, o.dynamicResize());
}
void RdpWorker::run() {
    if (!input_.healthy()) {
        config_.password.fill(0); config_.password.clear();
        emit stateChanged("failed", "Cannot create RDP input event"); return;
    }
    // Incompatible security settings fail before any network activity (V101).
    if (const auto conflict = rdpSecurityConflict(config_.options); !conflict.isEmpty()) {
        config_.password.fill(0); config_.password.clear();
        emit stateChanged("failed", "RDP settings conflict: " + conflict); return;
    }
    if (const auto problem = rdpSharedFolderProblem(config_.options.sharedFolder); !problem.isEmpty()) {
        config_.password.fill(0); config_.password.clear();
        emit stateChanged("failed", problem + ". Choose another folder or clear Shared folder."); return;
    }
    static std::once_flag init;
    std::call_once(init, [] {
        freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0);
        WLog_SetLogLevel(WLog_GetRoot(), WLOG_ERROR);
    });
    freerdp* instance = freerdp_new();
    if (!instance) { emit stateChanged("failed", "Cannot allocate RDP instance"); return; }
    instance->ContextSize = sizeof(Context); instance->PreConnect = Context::preConnect;
    instance->PostConnect = Context::postConnect; instance->LoadChannels = Context::loadChannels;
    instance->VerifyCertificateEx = Context::verify; instance->VerifyChangedCertificateEx = Context::changed;
    instance->AuthenticateEx = Context::authenticate;
    if (!freerdp_context_new(instance)) { freerdp_free(instance); emit stateChanged("failed", "Cannot allocate RDP context"); return; }
    auto* c = Context::from(instance); c->worker = this; c->geometry = 1;
    static_assert(offsetof(Context, graphicsContext) == 0);
    static_assert(offsetof(RdpGraphicsContext, base) == 0);
    ChannelState channels; c->channels = &channels;
    RdpGfxPipeline graphics; c->gfxOwner = &graphics;
    { std::lock_guard<std::mutex> lock(lifetimeMutex_); context_ = instance->context; }
    auto* settings = instance->context->settings;
    bool configured = QDir().mkpath(config_.configDirectory) &&
        freerdp_settings_set_string(settings, FreeRDP_ActionScript, nullptr) &&
        applyProfileSettings(settings, {config_.host, config_.port, config_.username, config_.domain, config_.options}, config_.panelSize, config_.screenSize) &&
        freerdp_settings_set_string(settings, FreeRDP_Password, config_.password.constData()) &&
        freerdp_settings_set_string(settings, FreeRDP_ConfigPath, config_.configDirectory.toUtf8().constData()) &&
        freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, 0x0409) &&
        freerdp_settings_set_uint32(settings, FreeRDP_TcpConnectTimeout, 5000) &&
        freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE) &&
        applyRdpSharedFolder(settings, config_.options.sharedFolder) &&
        freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE) &&
        freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE) &&
        applyRdpGraphicsPolicy(settings, config_.graphicsPolicy,
            config_.graphicsPolicy == RdpGraphicsPolicy::Gfx && rdpSoftwareAvcAvailable()) &&
        freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicChannels, TRUE) &&
        freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE) &&
        freerdp_settings_set_uint32(settings, FreeRDP_ClipboardFeatureMask, CLIPRDR_FLAG_LOCAL_TO_REMOTE | CLIPRDR_FLAG_REMOTE_TO_LOCAL |
            CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES | CLIPRDR_FLAG_REMOTE_TO_LOCAL_FILES);
    config_.password.fill(0); config_.password.clear();
    const bool connected = configured && !stopping_ && freerdp_connect(instance);
    if (!connected && !stopping_)
        emit stateChanged("failed", configured ? rdpFailureText(freerdp_get_last_error(instance->context), config_.options, certificateRejected_) :
            "RDP settings could not be applied; check Security Protocol, Load Balance Info and Gateway settings.");
    bool protocolFailure = false;
    while (connected && !stopping_ && !c->graphicsFailed && !freerdp_shall_disconnect_context(instance->context)) {
        const auto batch = input_.take();
        if (batch.eventFailed) { checkInputResult(RdpInputQueue::Result::EventFailed); break; }
        if (batch.stopping) break;
        for (const auto& cmd : batch.commands) {
            if (stopping_) break;
            if (config_.performance) config_.performance->record({PerformanceStage::InputQueue, {},
                cmd.queuedNs, PerformanceTrace::now()});
            if (cmd.type == 0) freerdp_input_send_keyboard_event_ex(instance->context->input, cmd.down, cmd.repeat, cmd.scan);
            else if (cmd.type == 1) for (QChar ch : cmd.text) {
                freerdp_input_send_unicode_keyboard_event(instance->context->input, 0, ch.unicode());
                freerdp_input_send_unicode_keyboard_event(instance->context->input, KBD_FLAGS_RELEASE, ch.unicode());
            } else if (cmd.type == 3) {
                if (freerdp_settings_get_bool(instance->context->settings, FreeRDP_HasRelativeMouseEvent))
                    freerdp_input_send_rel_mouse_event(instance->context->input, cmd.flags, qint16(cmd.x), qint16(cmd.y));
            } else freerdp_input_send_mouse_event(instance->context->input, cmd.flags, cmd.x, cmd.y);
        }
        if (stopping_) break;
        UINT clipboardStatus;
        {
            std::lock_guard<std::recursive_mutex> guard(channels.clipboardLock);
            Context::clipboardBatch(c, batch);
            if (batch.cancelDownload) Context::cancelDownload(c, "File copy canceled.");
            if (batch.download) Context::startDownload(c, batch.download->first, batch.download->second);
            clipboardStatus = Context::pumpClipboard(c);
        }
        if (clipboardStatus != CHANNEL_RC_OK) {
            emit stateChanged("failed", "RDP clipboard send failed; closing session"); break;
        }
        {
        std::lock_guard<std::mutex> lock(channels.mutex);
        const auto size = batch.size;
        if (config_.options.dynamicResize() && size.isValid() && channels.display && channels.displayReady) {
            DISPLAY_CONTROL_MONITOR_LAYOUT monitor{}; monitor.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
            monitor.Width = std::clamp(size.width(), 200, 8192) & ~1;
            monitor.Height = std::clamp(size.height(), 200, 8192);
            monitor.PhysicalWidth = 300; monitor.PhysicalHeight = 200; monitor.DesktopScaleFactor = 100; monitor.DeviceScaleFactor = 100;
            if (quint64(monitor.Width) * monitor.Height <= channels.maxArea && quint64(monitor.Width) * monitor.Height <= 16 * 1024 * 1024)
                channels.display->SendMonitorLayout(channels.display, 1, &monitor);
        }
        }
        if (stopping_) break;
        HANDLE events[64]{};
        const DWORD count = freerdp_get_event_handles(instance->context, events, 63);
        if (!count || count > 63) {
            emit stateChanged("failed", "RDP event handle count rejected"); break;
        }
        events[count] = input_.event();
        qint64 clipboardWait, downloadWait;
        {
            std::lock_guard<std::recursive_mutex> guard(channels.clipboardLock);
            clipboardWait = channels.clipboardProtocol.responseWait();
            downloadWait = channels.download ? channels.download->responseWait() : -1;
            // A slow file copy only stops itself; the session stays up.
            if (downloadWait == 0) Context::cancelDownload(c, "The remote computer stopped sending file data, so the file copy stopped.");
        }
        if (clipboardWait == 0) { emit stateChanged("failed", "RDP clipboard response timed out; reconnect the session"); break; }
        if (downloadWait == 0) continue;
        if (downloadWait > 0 && (clipboardWait < 0 || downloadWait < clipboardWait)) clipboardWait = downloadWait;
        const DWORD status = WaitForMultipleObjects(count + 1, events, FALSE, clipboardWait < 0 ? INFINITE : DWORD(clipboardWait));
        if (status == WAIT_TIMEOUT) continue;
        if (status == WAIT_FAILED || status >= WAIT_OBJECT_0 + count + 1) {
            emit stateChanged("failed", "RDP event wait failed"); break;
        }
        if (!stopping_) {
            const auto begin = config_.performance ? PerformanceTrace::now() : 0;
            const BOOL processed = freerdp_check_event_handles(instance->context);
            if (config_.performance) config_.performance->record({PerformanceStage::Processing, {}, begin, PerformanceTrace::now()});
            if (!processed) { protocolFailure = true; break; }
        }
    }
    const auto ended = connected && !stopping_ ? freerdp_get_last_error(instance->context) : 0;
    { std::lock_guard<std::mutex> lock(lifetimeMutex_); context_ = nullptr; }
    freerdp_disconnect(instance);
    { std::lock_guard<std::mutex> lock(channels.mutex); channels.clipboard = nullptr; channels.display = nullptr; }
    { std::lock_guard<std::recursive_mutex> guard(channels.clipboardLock); channels.download.reset(); }
    // Channel callbacks and codecs must be gone before GDI/context storage.
    if (graphics.close() != CHANNEL_RC_OK) emit stateChanged("failed", "RDP graphics channel cleanup failed");
    if (instance->context->gdi) gdi_free(instance);
    freerdp_context_free(instance); freerdp_free(instance);
    emit stateChanged("disconnected", stopping_ ? "RDP stopped" : ended ?
        "RDP session ended by the server (" + QString::fromUtf8(freerdp_get_last_error_name(ended)) + ")" :
        protocolFailure ? "RDP session closed: the client could not process data from the server (protocol error)" : "RDP disconnected");
}
