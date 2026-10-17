#include "RdpGraphics.h"
#include "OpenH264.h"
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QImage>
#include <QTest>
#include <freerdp/freerdp.h>
#include <freerdp/codecs.h>
#include <freerdp/codec/planar.h>
#include <freerdp/codec/h264.h>
#include <freerdp/config.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/gdi/bitmap.h>
#include <freerdp/gdi/dc.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <cstdlib>
#include <memory>

namespace {
struct TestContext {
    RdpGraphicsContext graphicsContext;
    quint64 geometry;
    quint64 flushedGeometry;
    int flushedWidth;
    bool failFlush;
};
BOOL endPaint(rdpContext* context) {
    auto* self = reinterpret_cast<TestContext*>(context);
    self->flushedGeometry = self->geometry;
    self->flushedWidth = context->gdi->width;
    if (self->graphicsContext.pipeline) self->graphicsContext.pipeline->collectPaint();
    return !self->failFlush;
}
struct Desktop {
    freerdp* instance = freerdp_new();
    Desktop() {
        if (!instance) return;
        instance->ContextSize = sizeof(TestContext);
        if (!freerdp_context_new(instance)) return;
        auto* c = instance->context;
        reinterpret_cast<TestContext*>(c)->geometry = 1;
        c->codecs = freerdp_client_codecs_new(0);
        if (!c->codecs || !freerdp_client_codecs_prepare(c->codecs, FREERDP_CODEC_PLANAR, 1024, 768)) return;
        // No transport/capability cache exists here. Seed a primary using public GDI APIs;
        // all replacement/flush behavior below is the actual pinned gdi_resize implementation.
        c->gdi = static_cast<rdpGdi*>(std::calloc(1, sizeof(rdpGdi)));
        if (!c->gdi) return;
        auto* g = c->gdi; g->context = c; g->width = 1024; g->height = 768;
        g->dstFormat = PIXEL_FORMAT_BGRA32; g->hdc = gdi_GetDC();
        if (!g->hdc) return;
        g->hdc->format = g->dstFormat;
        g->primary = static_cast<gdiBitmap*>(std::calloc(1, sizeof(gdiBitmap)));
        if (!g->primary) return;
        g->primary->hdc = gdi_CreateDC(g->dstFormat);
        g->primary->bitmap = gdi_CreateCompatibleBitmap(g->hdc, 1024, 768);
        if (!g->primary->hdc || !g->primary->bitmap) return;
        gdi_SelectObject(g->primary->hdc, reinterpret_cast<HGDIOBJECT>(g->primary->bitmap));
        g->primary_buffer = g->primary->bitmap->data; g->stride = g->primary->bitmap->scanline;
        g->drawing = g->primary;
        c->update->BeginPaint = [](rdpContext* context) -> BOOL {
            context->gdi->primary->hdc->hwnd->invalid->null = TRUE;
            context->gdi->primary->hdc->hwnd->ninvalid = 0;
            return TRUE;
        };
        c->update->EndPaint = endPaint;
        freerdp_settings_set_uint32(c->settings, FreeRDP_DesktopWidth, 1024);
        freerdp_settings_set_uint32(c->settings, FreeRDP_DesktopHeight, 768);
    }
    ~Desktop() {
        if (instance) { gdi_free(instance); freerdp_context_free(instance); freerdp_free(instance); }
    }
    bool ready() const {
        return instance && instance->context && instance->context->codecs && instance->context->gdi &&
            instance->context->gdi->primary_buffer;
    }
    TestContext* context() const { return reinterpret_cast<TestContext*>(instance->context); }
    bool resize(UINT32 width, UINT32 height) {
        auto* c = context();
        if (!freerdp_settings_set_uint32(c->graphicsContext.base.settings, FreeRDP_DesktopWidth, width) ||
            !freerdp_settings_set_uint32(c->graphicsContext.base.settings, FreeRDP_DesktopHeight, height)) return false;
        return resizeRdpDesktop(&c->graphicsContext.base, c->geometry);
    }
};
struct GfxChannel {
    RdpgfxClientContext gfx{};
    std::map<UINT16, void*> surfaces;
    pcRdpgfxDeleteSurface nativeDelete = nullptr;
    int deletes = 0;
    GfxChannel() {
        gfx.handle = this;
        gfx.GetSurfaceData = [](RdpgfxClientContext* gfx, UINT16 id) -> void* {
            const auto& surfaces = static_cast<GfxChannel*>(gfx->handle)->surfaces;
            const auto it = surfaces.find(id);
            return it == surfaces.end() ? nullptr : it->second;
        };
        gfx.SetSurfaceData = [](RdpgfxClientContext* gfx, UINT16 id, void* data) -> UINT {
            auto& surfaces = static_cast<GfxChannel*>(gfx->handle)->surfaces;
            if (data) surfaces[id] = data; else surfaces.erase(id);
            return CHANNEL_RC_OK;
        };
        gfx.GetSurfaceIds = [](RdpgfxClientContext* gfx, UINT16** ids, UINT16* count) -> UINT {
            const auto& surfaces = static_cast<GfxChannel*>(gfx->handle)->surfaces;
            *count = UINT16(surfaces.size()); *ids = nullptr;
            if (surfaces.empty()) return CHANNEL_RC_OK;
            *ids = static_cast<UINT16*>(std::calloc(surfaces.size(), sizeof(UINT16)));
            if (!*ids) return ERROR_NOT_ENOUGH_MEMORY;
            size_t index = 0;
            for (const auto& entry : surfaces) (*ids)[index++] = entry.first;
            return CHANNEL_RC_OK;
        };
    }
    UINT create(UINT16 id, UINT16 width = 64, UINT16 height = 64) {
        const RDPGFX_CREATE_SURFACE_PDU pdu{id, width, height, GFX_PIXEL_FORMAT_XRGB_8888};
        return gfx.CreateSurface(&gfx, &pdu);
    }
    UINT map(UINT16 id, UINT32 x, UINT32 y) {
        const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU pdu{id, 0, x, y};
        return gfx.MapSurfaceToOutput(&gfx, &pdu);
    }
    UINT fill(UINT16 id, QRgb color) {
        RECTANGLE_16 rect{0, 0, 64, 64};
        RDPGFX_SOLID_FILL_PDU pdu{}; pdu.surfaceId = id;
        pdu.fillPixel.B = qBlue(color); pdu.fillPixel.G = qGreen(color); pdu.fillPixel.R = qRed(color);
        pdu.fillRectCount = 1; pdu.fillRects = &rect;
        return gfx.SolidFill(&gfx, &pdu);
    }
};
struct Publications {
    Desktop* desktop;
    std::vector<QImage> frames;
    std::vector<QRegion> damage;
    RdpGfxPipeline::Hooks hooks() {
        return {this, [](void* user, const QRegion& damage) {
            auto* self = static_cast<Publications*>(user);
            self->damage.push_back(damage);
            const auto* g = self->desktop->instance->context->gdi;
            self->frames.push_back(QImage(g->primary_buffer, g->width, g->height, g->stride,
                QImage::Format_RGB32).copy());
            return true;
        }, nullptr};
    }
};
}

class GraphicsTests : public QObject {
    Q_OBJECT
    static bool useTestDecoder() {
#ifdef RM_TEST_OPENH264
        OpenH264::useLibrary(RM_TEST_OPENH264); return true;
#else
        return false;
#endif
    }
private slots:
    void gfxCodecIdsIdentifyProgressive() {
        QCOMPARE(rdpGfxReceivedCodec(RDPGFX_CODECID_CAPROGRESSIVE), ReceivedCodec::Progressive);
        QCOMPARE(rdpGfxReceivedCodec(RDPGFX_CODECID_CAPROGRESSIVE_V2), ReceivedCodec::ProgressiveV2);
        QCOMPARE(rdpGfxReceivedCodec(RDPGFX_CODECID_CLEARCODEC), ReceivedCodec::ClearCodec);
        QCOMPARE(rdpGfxReceivedCodec(RDPGFX_CODECID_ALPHA), ReceivedCodec::Alpha);
        QCOMPARE(rdpGfxReceivedCodec(0xffff), ReceivedCodec::Unknown);
    }
    void gfxSettingsMatchDecoderCapabilities() {
        Desktop desktop; QVERIFY(desktop.ready()); auto* settings = desktop.instance->context->settings;
        for (const auto policy : {RdpGraphicsPolicy::Legacy, RdpGraphicsPolicy::Gfx})
            for (const bool decoder : {false, true}) for (const UINT32 depth : {16u, 24u, 32u}) {
                QVERIFY(freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, depth));
                QVERIFY(applyRdpGraphicsPolicy(settings, policy, decoder));
                const bool gfx = policy == RdpGraphicsPolicy::Gfx && depth == 32;
                QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)), gfx);
                QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_GfxH264)), gfx && decoder);
                QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444)), gfx && decoder);
                QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2)), gfx && decoder);
                QCOMPARE(bool(freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec)), gfx);
                QVERIFY(freerdp_settings_get_bool(settings, FreeRDP_SynchronousDynamicChannels));
                QVERIFY(!freerdp_settings_get_bool(settings, FreeRDP_AsyncChannels));
            }
        // H.264 stays off until the app supplies a verified library, and turns off again when cleared.
        OpenH264::useLibrary({}); QVERIFY(!rdpSoftwareAvcAvailable());
        if (!useTestDecoder()) return;
        QVERIFY(rdpSoftwareAvcAvailable());
        OpenH264::useLibrary({}); QVERIFY(!rdpSoftwareAvcAvailable());
    }
    void codecDownloadsVerifiesAndEnablesDecoder() {
#ifndef RM_TEST_OPENH264
        QSKIP("No SDK OpenH264 library to stand in for Cisco's binary");
#else
        const auto compressor = QStandardPaths::findExecutable("bzip2");
        if (compressor.isEmpty()) QSKIP("bzip2 is not installed");
        QFile library(RM_TEST_OPENH264); QVERIFY(library.open(QIODevice::ReadOnly)); const auto bytes = library.readAll();
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QProcess bzip2; bzip2.start(compressor, {"-c", RM_TEST_OPENH264}); QVERIFY(bzip2.waitForFinished(30000));
        QFile download(dir.filePath("cisco.bz2")); QVERIFY(download.open(QIODevice::WriteOnly)); download.write(bzip2.readAllStandardOutput()); download.close();
        const OpenH264::Binary binary{QUrl::fromLocalFile(download.fileName()), QFileInfo(RM_TEST_OPENH264).fileName(),
                                      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()};
        OpenH264::useLibrary({});
        {
            OpenH264Codec wrong(dir.filePath("wrong"), nullptr, OpenH264::Binary{binary.url, binary.fileName, QByteArray(64, '0')});
            wrong.setEnabled(true); QTRY_COMPARE(wrong.state(), OpenH264Codec::State::Failed);
            QVERIFY(wrong.statusText().contains("does not match")); QVERIFY(!QFile::exists(wrong.libraryPath()));
            QVERIFY(!rdpSoftwareAvcAvailable());
        }
        OpenH264Codec codec(dir.filePath("data"), nullptr, binary);
        QCOMPARE(codec.state(), OpenH264Codec::State::Off);
        codec.setEnabled(true); QTRY_COMPARE(codec.state(), OpenH264Codec::State::Ready);
        QVERIFY(QFile::exists(codec.libraryPath())); QVERIFY(rdpSoftwareAvcAvailable());
        codec.setEnabled(false); QCOMPARE(codec.state(), OpenH264Codec::State::Off); QVERIFY(!rdpSoftwareAvcAvailable());
        // Re-enabling uses the verified file without downloading again.
        QVERIFY(QFile::remove(download.fileName()));
        codec.setEnabled(true); QCOMPARE(codec.state(), OpenH264Codec::State::Ready); QVERIFY(rdpSoftwareAvcAvailable());
        // A changed file is not loaded.
        codec.setEnabled(false);
        OpenH264Codec tampered(dir.filePath("data"), nullptr, OpenH264::Binary{binary.url, binary.fileName, QByteArray(64, 'f')});
        tampered.setEnabled(true); QTRY_COMPARE(tampered.state(), OpenH264Codec::State::Failed); QVERIFY(!rdpSoftwareAvcAvailable());
#endif
    }
    void ciscoBinaryIsPinnedForThisPlatform() {
        const auto binary = OpenH264::platformBinary();
#if (defined(Q_OS_MACOS) || defined(Q_OS_LINUX) || defined(Q_OS_WIN)) && (defined(Q_PROCESSOR_ARM_64) || defined(Q_PROCESSOR_X86_64))
#if !(defined(Q_OS_WIN) && defined(Q_PROCESSOR_ARM_64))
        QVERIFY(binary); QCOMPARE(binary->url.host(), QString("ciscobinary.openh264.org"));
        QVERIFY(binary->url.fileName().startsWith(QString("libopenh264-") + OpenH264::Version) || binary->url.fileName().startsWith(QString("openh264-") + OpenH264::Version));
        QCOMPARE(binary->sha256.size(), 64);
#endif
#endif
        Q_UNUSED(binary);
    }
    void ciscoBinaryDownloadsAndDecodes() {
        // Real network: RM_NETWORK_TESTS=1.
        if (qEnvironmentVariable("RM_NETWORK_TESTS") != "1") QSKIP("Set RM_NETWORK_TESTS=1 to download Cisco's OpenH264");
        const auto binary = OpenH264::platformBinary(); if (!binary) QSKIP("No Cisco binary for this platform");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        OpenH264::useLibrary({});
        OpenH264Codec codec(dir.path()); codec.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(codec.state() != OpenH264Codec::State::Downloading, 120000);
        QVERIFY2(codec.state() == OpenH264Codec::State::Ready, qPrintable(codec.statusText()));
        QVERIFY(rdpSoftwareAvcAvailable());
        std::unique_ptr<H264_CONTEXT, decltype(&h264_context_free)> decoder(h264_context_new(FALSE), h264_context_free);
        QVERIFY(decoder); QVERIFY(h264_context_reset(decoder.get(), 64, 64));
        codec.setEnabled(false); QVERIFY(!rdpSoftwareAvcAvailable());
    }
    void rdpgfxInitializesAndUninitializes() {
        Desktop desktop; QVERIFY(desktop.ready());
        RdpgfxClientContext gfx{}; int originalOwner = 0; gfx.custom = &originalOwner;
        auto* gdi = desktop.instance->context->gdi;
        RdpGfxPipeline pipeline;
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &gfx, {}));
        QVERIFY(pipeline.active()); QCOMPARE(gfx.custom, static_cast<void*>(gdi));
        QCOMPARE(gdi->gfx, &gfx); QVERIFY(gfx.codecs); QVERIFY(gfx.StartFrame); QVERIFY(gfx.EndFrame);
        QVERIFY(gfx.UpdateSurfaces); QVERIFY(gfx.SurfaceCommand);
        QVERIFY(!pipeline.initialize(&desktop.context()->graphicsContext, &gfx, {}));
        pipeline.close(); pipeline.close();
        QVERIFY(!pipeline.active()); QCOMPARE(gfx.custom, static_cast<void*>(&originalOwner));
        QVERIFY(!gfx.codecs); QVERIFY(!gdi->gfx); QVERIFY(!desktop.context()->graphicsContext.pipeline);
        QVERIFY(!gfx.StartFrame); QVERIFY(!gfx.EndFrame); QVERIFY(!gfx.UpdateSurfaces);
    }
    void gfxPartialInitializationRestoresCallbacks() {
        Desktop desktop; QVERIFY(desktop.ready()); RdpgfxClientContext gfx{};
        int originalOwner = 0; gfx.custom = &originalOwner;
        RdpGfxPipeline pipeline({[](rdpGdi* gdi, RdpgfxClientContext* gfx) -> BOOL {
            gdi->gfx = gfx; gfx->custom = gdi;
            gfx->codecs = freerdp_client_codecs_new(0);
            gfx->StartFrame = [](RdpgfxClientContext*, const RDPGFX_START_FRAME_PDU*) -> UINT { return 0; };
            return FALSE; // Pinned failure occurs before critical-section initialization.
        }, [](rdpGdi*, RdpgfxClientContext*) { QFAIL("Partial init must not uninit an uninitialized mutex"); }});
        QVERIFY(!pipeline.initialize(&desktop.context()->graphicsContext, &gfx, {}));
        QCOMPARE(gfx.custom, static_cast<void*>(&originalOwner)); QVERIFY(!gfx.codecs); QVERIFY(!gfx.StartFrame);
        QVERIFY(!desktop.instance->context->gdi->gfx); QVERIFY(!desktop.context()->graphicsContext.pipeline);
        QVERIFY(!pipeline.active()); pipeline.close();
    }
    void gfxFrameEndsPublishCoherentDesktop() {
        Desktop desktop; QVERIFY(desktop.ready()); GfxChannel channel;
        Publications publications{&desktop, {}}; RdpGfxPipeline pipeline;
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &channel.gfx, publications.hooks()));
        QCOMPARE(channel.create(1), UINT(CHANNEL_RC_OK)); QCOMPARE(channel.create(2), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.map(1, 0, 0), UINT(CHANNEL_RC_OK)); QCOMPARE(channel.map(2, 64, 0), UINT(CHANNEL_RC_OK));
        const RDPGFX_START_FRAME_PDU start{0, 9}; const RDPGFX_END_FRAME_PDU end{9}, wrong{8};
        QCOMPARE(channel.gfx.StartFrame(&channel.gfx, &start), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.gfx.StartFrame(&channel.gfx, &start), UINT(ERROR_INVALID_DATA));
        QCOMPARE(channel.fill(1, qRgb(255, 0, 0)), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.fill(2, qRgb(0, 255, 0)), UINT(CHANNEL_RC_OK));
        QVERIFY(publications.frames.empty());
        QCOMPARE(channel.gfx.EndFrame(&channel.gfx, &wrong), UINT(ERROR_INVALID_DATA));
        QVERIFY(publications.frames.empty());
        QCOMPARE(channel.gfx.EndFrame(&channel.gfx, &end), UINT(CHANNEL_RC_OK));
        QCOMPARE(publications.frames.size(), size_t(1));
        QCOMPARE(publications.damage.back(), QRegion(QRect(0, 0, 128, 64)));
        QCOMPARE(publications.frames.back().pixel(32, 32), qRgb(255, 0, 0));
        QCOMPARE(publications.frames.back().pixel(96, 32), qRgb(0, 255, 0));
        QCOMPARE(channel.gfx.EndFrame(&channel.gfx, &end), UINT(ERROR_INVALID_DATA));
        QCOMPARE(channel.fill(1, qRgb(0, 0, 255)), UINT(CHANNEL_RC_OK));
        QCOMPARE(publications.frames.size(), size_t(2)); // Successful inter-frame flush.
        QCOMPARE(publications.frames.back().pixel(32, 32), qRgb(0, 0, 255));
        QCOMPARE(publications.frames.back().pixel(96, 32), qRgb(0, 255, 0));
        QCOMPARE(channel.gfx.StartFrame(&channel.gfx, &start), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.fill(1, qRgb(255, 255, 0)), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.fill(2, qRgb(0, 255, 255)), UINT(CHANNEL_RC_OK));
        desktop.context()->failFlush = true;
        QVERIFY(channel.gfx.EndFrame(&channel.gfx, &end) != CHANNEL_RC_OK);
        QCOMPARE(publications.frames.size(), size_t(2));
        desktop.context()->failFlush = false;
        pipeline.close(); QVERIFY(channel.surfaces.empty());
    }
    void gfxReconnectRejectsOldFrames() {
        Desktop desktop; QVERIFY(desktop.ready()); GfxChannel oldChannel, newChannel;
        // This owner is not a GDI pointer. Saved wrappers must not inspect it after restoration.
        int owner = 1; oldChannel.gfx.custom = &owner;
        Publications publications{&desktop, {}}; RdpGfxPipeline pipeline;
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &oldChannel.gfx, publications.hooks()));
        const auto staleStart = oldChannel.gfx.StartFrame;
        const auto staleEnd = oldChannel.gfx.EndFrame;
        const auto staleUpdate = oldChannel.gfx.UpdateSurfaces;
        QCOMPARE(oldChannel.create(1), UINT(CHANNEL_RC_OK));
        const RDPGFX_START_FRAME_PDU start{0, 10}; const RDPGFX_END_FRAME_PDU end{10};
        QCOMPARE(oldChannel.gfx.StartFrame(&oldChannel.gfx, &start), UINT(CHANNEL_RC_OK));
        pipeline.close(); QVERIFY(oldChannel.surfaces.empty());
        QVERIFY(!desktop.instance->context->gdi->inGfxFrame);
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &newChannel.gfx, publications.hooks()));
        QCOMPARE(staleStart(&oldChannel.gfx, &start), UINT(ERROR_INVALID_DATA));
        QCOMPARE(staleEnd(&oldChannel.gfx, &end), UINT(ERROR_INVALID_DATA));
        QCOMPARE(staleUpdate(&oldChannel.gfx), UINT(ERROR_INVALID_DATA));
        QVERIFY(publications.frames.empty());
        QCOMPARE(newChannel.create(2), UINT(CHANNEL_RC_OK));
        QCOMPARE(newChannel.map(2, 0, 0), UINT(CHANNEL_RC_OK));
        QCOMPARE(newChannel.fill(2, qRgb(10, 20, 30)), UINT(CHANNEL_RC_OK));
        QCOMPARE(publications.frames.size(), size_t(1));
        QCOMPARE(publications.frames.back().pixel(32, 32), qRgb(10, 20, 30));
        pipeline.close(); QVERIFY(newChannel.surfaces.empty());
    }
    void gfxSurfaceBoundsRejectBeforeAllocation() {
        Desktop desktop; QVERIFY(desktop.ready()); GfxChannel channel; RdpGfxPipeline pipeline;
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &channel.gfx, {}));
        QCOMPARE(channel.create(1, 0, 64), UINT(ERROR_INVALID_DATA));
        QCOMPARE(channel.create(1, 8193, 1), UINT(ERROR_INVALID_DATA));
        QVERIFY(channel.surfaces.empty());
        for (UINT16 id = 0; id < 64; ++id) QCOMPARE(channel.create(id, 1, 1), UINT(CHANNEL_RC_OK));
        QCOMPARE(channel.create(64, 1, 1), UINT(ERROR_INVALID_DATA));
        QCOMPARE(channel.create(0, 1, 1), UINT(ERROR_INVALID_DATA));
        QCOMPARE(channel.surfaces.size(), size_t(64));
        pipeline.close(); QVERIFY(channel.surfaces.empty());
    }
    void gfxCleanupReportsFailureAndRestoresOwnership() {
        Desktop desktop; QVERIFY(desktop.ready()); GfxChannel channel;
        RdpGfxPipeline pipeline({[](rdpGdi* gdi, RdpgfxClientContext* gfx) -> BOOL {
            const auto operations = RdpGfxPipeline::systemOperations();
            if (!operations.init(gdi, gfx)) return FALSE;
            static_cast<GfxChannel*>(gfx->handle)->nativeDelete = gfx->DeleteSurface;
            gfx->DeleteSurface = [](RdpgfxClientContext* gfx, const RDPGFX_DELETE_SURFACE_PDU* pdu) -> UINT {
                auto* channel = static_cast<GfxChannel*>(gfx->handle); ++channel->deletes;
                const auto status = channel->nativeDelete(gfx, pdu);
                return status == CHANNEL_RC_OK ? ERROR_INTERNAL_ERROR : status;
            };
            return TRUE;
        }, RdpGfxPipeline::systemOperations().uninit});
        QVERIFY(pipeline.initialize(&desktop.context()->graphicsContext, &channel.gfx, {}));
        QCOMPARE(channel.create(1), UINT(CHANNEL_RC_OK)); QCOMPARE(channel.create(2), UINT(CHANNEL_RC_OK));
        const RDPGFX_DELETE_SURFACE_PDU pdu{1};
        QCOMPARE(channel.gfx.DeleteSurface(&channel.gfx, &pdu), UINT(ERROR_INTERNAL_ERROR));
        QCOMPARE(channel.deletes, 1);
        QCOMPARE(pipeline.close(), UINT(ERROR_INTERNAL_ERROR));
        QCOMPARE(channel.deletes, 2); QVERIFY(channel.surfaces.empty());
        QVERIFY(!pipeline.active()); QVERIFY(!channel.gfx.codecs); QVERIFY(!channel.gfx.custom);
        QCOMPARE(pipeline.close(), UINT(CHANNEL_RC_OK)); QCOMPARE(channel.deletes, 2);
    }
    void softwareDecoderCreatesCodecContexts() {
#if defined(WITH_OPENH264) && defined(WITH_GFX_H264)
        if (!useTestDecoder()) QSKIP("No SDK OpenH264 library");
        std::unique_ptr<H264_CONTEXT, decltype(&h264_context_free)> decoder(h264_context_new(FALSE), h264_context_free);
        std::unique_ptr<H264_CONTEXT, decltype(&h264_context_free)> encoder(h264_context_new(TRUE), h264_context_free);
        QVERIFY(decoder); QVERIFY(encoder);
        QVERIFY(h264_context_reset(decoder.get(), 64, 64));
        QVERIFY(h264_context_reset(encoder.get(), 64, 64));
#else
        QFAIL("The T39 project SDK must build and link the audited software codec");
#endif
    }
    void legacyPlanarResizeResetsAllocation() {
        Desktop desktop; QVERIFY(desktop.ready());
        QImage source(1124, 768, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgb((x * 7) % 256, (y * 3) % 256, (x + y) % 256));
        std::unique_ptr<BITMAP_PLANAR_CONTEXT, decltype(&freerdp_bitmap_planar_context_free)> encoder(
            freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_NA | PLANAR_FORMAT_HEADER_RLE, 1124, 768),
            freerdp_bitmap_planar_context_free);
        QVERIFY(encoder);
        UINT32 compressedSize = 0;
        std::unique_ptr<BYTE, decltype(&std::free)> compressed(freerdp_bitmap_compress_planar(encoder.get(),
            source.constBits(), PIXEL_FORMAT_BGRA32, 1124, 768, source.bytesPerLine(), nullptr, &compressedSize), std::free);
        QVERIFY(compressed); QVERIFY(compressedSize > 0);
        QImage decoded(source.size(), QImage::Format_RGB32); decoded.fill(Qt::black);
        auto decode = [&] {
            return freerdp_bitmap_decompress_planar(desktop.context()->graphicsContext.base.codecs->planar,
                compressed.get(), compressedSize, 1124, 768, decoded.bits(), PIXEL_FORMAT_BGRA32,
                decoded.bytesPerLine(), 0, 0, 1124, 768, TRUE);
        };
        QVERIFY(!decode()); // Reproduces B3 with a real owned compressed bitmap.
        QVERIFY(desktop.resize(1124, 768));
        QCOMPARE(desktop.context()->flushedGeometry, quint64(1));
        QCOMPARE(desktop.context()->flushedWidth, 1024);
        QCOMPARE(desktop.context()->geometry, quint64(2));
        QVERIFY(decode());
        QCOMPARE(decoded, source);
        auto* g = desktop.context()->graphicsContext.base.gdi;
        QCOMPARE(g->width, 1124);
        QVERIFY(freerdp_bitmap_decompress_planar(desktop.context()->graphicsContext.base.codecs->planar,
            compressed.get(), compressedSize, 1124, 768, g->primary_buffer, PIXEL_FORMAT_BGRA32,
            g->stride, 0, 0, 1124, 768, TRUE));
        QCOMPARE(QImage(g->primary_buffer, g->width, g->height, g->stride, QImage::Format_RGB32), source);
        QVERIFY(desktop.resize(800, 600));
        QCOMPARE(desktop.context()->flushedGeometry, quint64(2));
        QCOMPARE(desktop.context()->geometry, quint64(3));
    }
    void legacyResizeRejectsBoundsBeforeAllocation() {
        QVERIFY(rdpDesktopSizeValid(8192, 2048)); QVERIFY(rdpDesktopSizeValid(4096, 4096));
        const QSize rejected[] = {{0, 768}, {1024, 0}, {8193, 1}, {1, 8193}, {4096, 4097}, {8192, 8192}};
        Desktop desktop; QVERIFY(desktop.ready());
        auto* c = desktop.context(); auto* primary = c->graphicsContext.base.gdi->primary_buffer;
        for (const auto& size : rejected) {
            QVERIFY(!desktop.resize(size.width(), size.height()));
            QCOMPARE(c->geometry, quint64(1)); QCOMPARE(c->flushedGeometry, quint64(0));
            QCOMPARE(c->graphicsContext.base.gdi->primary_buffer, primary); QCOMPARE(c->graphicsContext.base.gdi->width, 1024);
        }
        QVERIFY(desktop.resize(1024, 768)); QCOMPARE(c->geometry, quint64(1));
        QVERIFY(!resizeRdpDesktop(nullptr, c->geometry));
        auto* codecs = c->graphicsContext.base.codecs; c->graphicsContext.base.codecs = nullptr;
        const bool resized = desktop.resize(1124, 768); c->graphicsContext.base.codecs = codecs;
        QVERIFY(!resized); QCOMPARE(c->geometry, quint64(1));
    }
    void legacyResizeFailureKeepsGeometry() {
        Desktop desktop; QVERIFY(desktop.ready());
        auto* c = desktop.context(); c->failFlush = true;
        auto* primary = c->graphicsContext.base.gdi->primary_buffer;
        QVERIFY(!desktop.resize(1124, 768));
        QCOMPARE(c->geometry, quint64(1)); QCOMPARE(c->graphicsContext.base.gdi->width, 1024);
        QCOMPARE(c->graphicsContext.base.gdi->primary_buffer, primary);
    }
};
QTEST_GUILESS_MAIN(GraphicsTests)
#include "rdp_graphics.moc"
