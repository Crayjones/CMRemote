#include "RdpResize.h"
#include <QImage>
#include <QTest>
#include <freerdp/freerdp.h>
#include <freerdp/codec/planar.h>
#include <freerdp/gdi/bitmap.h>
#include <freerdp/gdi/dc.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>
#include <cstdlib>
#include <memory>

namespace {
struct TestContext {
    rdpContext base;
    int flushedWidth;
    QImage* flushedImage;
};
struct Desktop {
    freerdp* instance = freerdp_new();
    QImage flushedImage;
    Desktop() {
        if (!instance) return;
        instance->ContextSize = sizeof(TestContext);
        if (!freerdp_context_new(instance)) return;
        auto* c = instance->context;
        reinterpret_cast<TestContext*>(c)->flushedImage = &flushedImage;
        c->codecs = freerdp_client_codecs_new(0);
        if (!c->codecs || !freerdp_client_codecs_prepare(c->codecs, FREERDP_CODEC_PLANAR, 1024, 768)) return;
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
        c->update->BeginPaint = [](rdpContext* c) -> BOOL {
            c->gdi->primary->hdc->hwnd->invalid->null = TRUE;
            c->gdi->primary->hdc->hwnd->ninvalid = 0;
            return TRUE;
        };
        c->update->EndPaint = [](rdpContext* c) -> BOOL {
            auto* self = reinterpret_cast<TestContext*>(c);
            auto* g = c->gdi;
            self->flushedWidth = g->width;
            *self->flushedImage = QImage(g->primary_buffer, g->width, g->height, g->stride, QImage::Format_RGB32).copy();
            return !self->flushedImage->isNull();
        };
        setSize(1024, 768);
    }
    ~Desktop() {
        if (instance) { gdi_free(instance); freerdp_context_free(instance); freerdp_free(instance); }
    }
    rdpContext* context() const { return instance ? instance->context : nullptr; }
    bool ready() const { return context() && context()->codecs && context()->gdi && context()->gdi->primary_buffer; }
    bool setSize(UINT32 width, UINT32 height) {
        return freerdp_settings_set_uint32(context()->settings, FreeRDP_DesktopWidth, width) &&
            freerdp_settings_set_uint32(context()->settings, FreeRDP_DesktopHeight, height);
    }
};
int resets = 0, resizes = 0;
BOOL countReset(rdpCodecs*, UINT32, UINT32, UINT32) { ++resets; return TRUE; }
BOOL countResize(rdpGdi*, UINT32, UINT32) { ++resizes; return TRUE; }
}

class RdpResizeTest : public QObject {
    Q_OBJECT
private slots:
    void legacyPlanarResizeResetsAllocation() {
        Desktop desktop; QVERIFY(desktop.ready());
        QImage source(1124, 768, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgb((x * 7) % 256, (y * 3) % 256, (x + y) % 256));
        std::unique_ptr<BITMAP_PLANAR_CONTEXT, decltype(&freerdp_bitmap_planar_context_free)> encoder(
            freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_NA | PLANAR_FORMAT_HEADER_RLE, 1124, 768),
            freerdp_bitmap_planar_context_free);
        QVERIFY(encoder);
        UINT32 length = 0;
        std::unique_ptr<BYTE, decltype(&std::free)> compressed(freerdp_bitmap_compress_planar(encoder.get(),
            source.constBits(), PIXEL_FORMAT_BGRA32, 1124, 768, source.bytesPerLine(), nullptr, &length), std::free);
        QVERIFY(compressed); QVERIFY(length > 0);
        QImage decoded(source.size(), QImage::Format_RGB32); decoded.fill(Qt::black);
        auto decode = [&] {
            return freerdp_bitmap_decompress_planar(desktop.context()->codecs->planar,
                compressed.get(), length, 1124, 768, decoded.bits(), PIXEL_FORMAT_BGRA32,
                decoded.bytesPerLine(), 0, 0, 1124, 768, TRUE);
        };
        QVERIFY(!decode()); // Real B3 allocation failure, no server or decoder substitution.
        auto* g = desktop.context()->gdi;
        QImage old(g->primary_buffer, g->width, g->height, g->stride, QImage::Format_RGB32); old.fill(Qt::darkCyan);
        QVERIFY(desktop.setSize(1124, 768)); QVERIFY(resizeRdpLegacyDesktop(desktop.context()));
        QCOMPARE(reinterpret_cast<TestContext*>(desktop.context())->flushedWidth, 1024);
        QCOMPARE(desktop.flushedImage.size(), QSize(1024, 768));
        QCOMPARE(desktop.flushedImage.pixelColor(0, 0), QColor(Qt::darkCyan));
        QVERIFY(decode()); QCOMPARE(decoded, source);
        QVERIFY(freerdp_bitmap_decompress_planar(desktop.context()->codecs->planar, compressed.get(), length,
            1124, 768, g->primary_buffer, PIXEL_FORMAT_BGRA32, g->stride, 0, 0, 1124, 768, TRUE));
        QCOMPARE(QImage(g->primary_buffer, g->width, g->height, g->stride, QImage::Format_RGB32), source);
        QVERIFY(desktop.setSize(800, 600)); QVERIFY(resizeRdpLegacyDesktop(desktop.context()));
        QCOMPARE(reinterpret_cast<TestContext*>(desktop.context())->flushedWidth, 1124);
        QCOMPARE(desktop.flushedImage, source); QCOMPARE(g->width, 800); QCOMPARE(g->height, 600);
    }
    void legacyResizeRejectsBoundsBeforeAllocation() {
        Desktop desktop; QVERIFY(desktop.ready());
        auto* primary = desktop.context()->gdi->primary_buffer;
        const QSize rejected[] = {{0, 768}, {1024, 0}, {8193, 1}, {1, 8193}, {4096, 4097}, {8192, 8192}};
        for (const auto size : rejected) {
            resets = resizes = 0;
            QVERIFY(desktop.setSize(size.width(), size.height()));
            QVERIFY(!resizeRdpLegacyDesktop(desktop.context(), {countReset, countResize}));
            QCOMPARE(resets, 0); QCOMPARE(resizes, 0); QCOMPARE(desktop.context()->gdi->primary_buffer, primary);
        }
        for (const QSize size : {QSize(8192, 2048), QSize(4096, 4096)}) {
            resets = resizes = 0; QVERIFY(desktop.setSize(size.width(), size.height()));
            QVERIFY(resizeRdpLegacyDesktop(desktop.context(), {countReset, countResize}));
            QCOMPARE(resets, 1); QCOMPARE(resizes, 1);
        }
        resets = resizes = 0; QVERIFY(desktop.setSize(1024, 768));
        QVERIFY(resizeRdpLegacyDesktop(desktop.context(), {countReset, countResize}));
        QCOMPARE(resets, 0); QCOMPARE(resizes, 0);
        QVERIFY(!resizeRdpLegacyDesktop(nullptr));
    }
    void legacyResizeFailurePreservesOldPrimary() {
        Desktop desktop; QVERIFY(desktop.ready()); auto* g = desktop.context()->gdi;
        QImage old(g->primary_buffer, g->width, g->height, g->stride, QImage::Format_RGB32); old.fill(Qt::red);
        const auto copy = old.copy(); auto* primary = g->primary_buffer;
        QVERIFY(desktop.setSize(1124, 768)); resizes = 0;
        QVERIFY(!resizeRdpLegacyDesktop(desktop.context(), {
            [](rdpCodecs*, UINT32, UINT32, UINT32) -> BOOL { return FALSE; }, countResize}));
        QCOMPARE(resizes, 0); QCOMPARE(g->primary_buffer, primary); QCOMPARE(old, copy);
        resets = 0;
        QVERIFY(!resizeRdpLegacyDesktop(desktop.context(), {
            countReset, [](rdpGdi*, UINT32, UINT32) -> BOOL { return FALSE; }}));
        QCOMPARE(resets, 1); QCOMPARE(g->primary_buffer, primary); QCOMPARE(g->width, 1024);
        QCOMPARE(old, copy); QVERIFY(desktop.flushedImage.isNull());
    }
};
QTEST_GUILESS_MAIN(RdpResizeTest)
#include "rdp_resize.moc"
