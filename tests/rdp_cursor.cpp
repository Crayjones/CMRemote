#include "RdpCursor.h"
#include <QTest>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/settings.h>
#include <memory>

struct CursorContext { rdpContext base; RemoteCursorMailbox* mailbox; };
struct CursorInstance {
    freerdp* instance = freerdp_new();
    RemoteCursorMailbox mailbox;
    CursorInstance() {
        instance->ContextSize = sizeof(CursorContext);
        if (!freerdp_context_new(instance)) return;
        auto* context = instance->context;
        reinterpret_cast<CursorContext*>(context)->mailbox = &mailbox;
        context->gdi = new rdpGdi{};
        const auto prototype = rdpCursorPrototype([](rdpContext* c, rdpPointer* p) -> BOOL {
            reinterpret_cast<CursorContext*>(c)->mailbox->publish(rdpCursorImage(p)); return TRUE;
        }, [](rdpContext* c) -> BOOL {
            reinterpret_cast<CursorContext*>(c)->mailbox->publish({RemoteCursor::Kind::Hidden}); return TRUE;
        }, [](rdpContext* c) -> BOOL {
            reinterpret_cast<CursorContext*>(c)->mailbox->publish({}); return TRUE;
        });
        graphics_register_pointer(context->graphics, &prototype);
    }
    void close() {
        delete instance->context->gdi; instance->context->gdi = nullptr;
        freerdp_context_free(instance);
    }
    bool select(const POINTER_NEW_UPDATE& update) {
        auto* c = instance->context; auto* p = Pointer_Alloc(c);
        if (!p) return false;
        p->width = update.colorPtrAttr.width; p->height = update.colorPtrAttr.height;
        p->xPos = update.colorPtrAttr.hotSpotX; p->yPos = update.colorPtrAttr.hotSpotY; p->xorBpp = update.xorBpp;
        p->xorMaskData = update.colorPtrAttr.xorMaskData; p->lengthXorMask = update.colorPtrAttr.lengthXorMask;
        p->andMaskData = update.colorPtrAttr.andMaskData; p->lengthAndMask = update.colorPtrAttr.lengthAndMask;
        const bool ok = p->New(c, p) && p->Set(c, p);
        p->Free(c, p); free(p); return ok;
    }
    ~CursorInstance() {
        if (instance->context) close(); freerdp_free(instance);
    }
};
class CursorTest : public QObject {
    Q_OBJECT
private slots:
    void remoteCursorDecodesMasksAndHotspot_data() {
        QTest::addColumn<int>("depth");
        for (int depth : {1, 8, 16, 24, 32}) QTest::newRow(qPrintable(QString::number(depth))) << depth;
    }
    void remoteCursorDecodesMasksAndHotspot() {
        QFETCH(int, depth); CursorInstance client; auto* c = client.instance->context; QVERIFY(c && c->gdi);
        const int stride = depth == 1 ? 2 : ((3 * (depth / 8) + 1) & ~1);
        QByteArray pixels(stride * 2, '\0'), mask(4, '\0');
        if (depth == 1) { pixels[0] = char(0x40); pixels[2] = char(0x80); }
        else if (depth == 8) { c->gdi->palette.format = PIXEL_FORMAT_BGRA32; c->gdi->palette.palette[1] = FreeRDPGetColor(PIXEL_FORMAT_BGRA32, 255, 0, 0, 255); pixels.fill(1); }
        else if (depth == 16) { for (int row = 0; row < 2; ++row) for (int x = 0; x < 3; ++x) { pixels[row * stride + x * 2] = 0; pixels[row * stride + x * 2 + 1] = char(0x7c); } }
        else for (int row = 0; row < 2; ++row) for (int x = 0; x < 3; ++x) {
            const int offset = row * stride + x * (depth / 8); pixels[offset + 2] = char(0xff);
            if (depth == 32) pixels[offset + 3] = char(row ? 0xff : 0x80);
        }
        POINTER_NEW_UPDATE update{}; update.xorBpp = depth;
        update.colorPtrAttr.width = 3; update.colorPtrAttr.height = 2; update.colorPtrAttr.hotSpotX = 1; update.colorPtrAttr.hotSpotY = 1;
        update.colorPtrAttr.lengthXorMask = pixels.size(); update.colorPtrAttr.xorMaskData = reinterpret_cast<BYTE*>(pixels.data());
        update.colorPtrAttr.lengthAndMask = mask.size(); update.colorPtrAttr.andMaskData = reinterpret_cast<BYTE*>(mask.data());
        QVERIFY(client.select(update));
        auto cursor = client.mailbox.take(); QVERIFY(cursor); QCOMPARE(cursor->kind, RemoteCursor::Kind::Bitmap);
        QCOMPARE(cursor->hotspot, QPoint(1, 1)); QCOMPARE(cursor->image.size(), QSize(3, 2));
        if (depth == 1) { QCOMPARE(cursor->image.pixelColor(1, 0), QColor(Qt::white)); QCOMPARE(cursor->image.pixelColor(0, 1), QColor(Qt::white)); }
        else { QCOMPARE(cursor->image.pixelColor(0, 0), QColor(Qt::red)); if (depth == 32) QCOMPARE(cursor->image.pixelColor(0, 1), QColor(255, 0, 0, 128)); }
        pixels.fill(0); mask.fill(char(0xff));
        QVERIFY(cursor->image.pixelColor(depth == 1 ? 1 : 0, 0).alpha() > 0);
    }
    void remoteCursorRejectsMalformedMasks() {
        CursorInstance client; auto* c = client.instance->context;
        QByteArray pixels(16, char(0xff)), mask(4, '\0');
        auto* pointer = Pointer_Alloc(c); QVERIFY(pointer);
        pointer->width = 2; pointer->height = 2; pointer->xorBpp = 32;
        pointer->xorMaskData = reinterpret_cast<BYTE*>(pixels.data()); pointer->lengthXorMask = pixels.size();
        pointer->andMaskData = reinterpret_cast<BYTE*>(mask.data()); pointer->lengthAndMask = mask.size();
        QVERIFY(pointer->New(c, pointer)); pointer->Free(c, pointer);
        pointer->width = 385; QVERIFY(!pointer->New(c, pointer)); pointer->width = 2;
        pointer->xPos = 2; QVERIFY(!pointer->New(c, pointer)); pointer->xPos = 0;
        pointer->lengthXorMask = 1; QVERIFY(!pointer->New(c, pointer)); pointer->lengthXorMask = pixels.size();
        pointer->lengthAndMask = 1; QVERIFY(!pointer->New(c, pointer)); pointer->lengthAndMask = mask.size();
        pointer->xorBpp = 4; QVERIFY(!pointer->New(c, pointer));
        free(pointer); QVERIFY(!client.mailbox.take());
    }
    void remoteCursorCacheSurvivesEviction() {
        CursorInstance client; auto* c = client.instance->context;
        QByteArray pixels(16, char(0xff));
        POINTER_NEW_UPDATE update{}; update.xorBpp = 32;
        update.colorPtrAttr.width = 2; update.colorPtrAttr.height = 2;
        update.colorPtrAttr.lengthXorMask = pixels.size(); update.colorPtrAttr.xorMaskData = reinterpret_cast<BYTE*>(pixels.data());
        QVERIFY(client.select(update)); auto held = client.mailbox.take(); QVERIFY(held);
        const auto image = held->image;
        pixels.fill('\0'); QVERIFY(client.select(update));
        auto newest = client.mailbox.take(); QVERIFY(newest); QCOMPARE(newest->image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
        client.close();
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
    }
    void remoteCursorHandlesDefaultAndHidden() {
        CursorInstance client; auto* c = client.instance->context;
        auto* pointer = Pointer_Alloc(c); QVERIFY(pointer);
        QVERIFY(pointer->SetNull(c)); QCOMPARE(client.mailbox.take()->kind, RemoteCursor::Kind::Hidden);
        QVERIFY(pointer->SetDefault(c)); QCOMPARE(client.mailbox.take()->kind, RemoteCursor::Kind::Default);
        QVERIFY(pointer->SetPosition(c, 100, 200)); free(pointer);
    }
    void remoteCursorScaledHotspotStaysInsideSmallImage() {
        QImage image(16, 16, QImage::Format_ARGB32); image.fill(Qt::red);
        RemoteCursor cursor{RemoteCursor::Kind::Bitmap, image, {15, 15}};
        const auto scaled = scaleRemoteCursor(cursor, {1000, 1000}, {40, 40}, 1);
        QCOMPARE(scaled.image.size(), QSize(1, 1)); QCOMPARE(scaled.hotspot, QPoint(0, 0));
    }
    void remoteCursorMailboxIsBounded() {
        RemoteCursorMailbox mailbox; QImage image(32, 32, QImage::Format_ARGB32); image.fill(Qt::red);
        QVERIFY(mailbox.publish({RemoteCursor::Kind::Bitmap, image, {3, 4}}));
        for (int i = 0; i < 10000; ++i) QVERIFY(!mailbox.publish({RemoteCursor::Kind::Hidden}));
        QCOMPARE(mailbox.take()->kind, RemoteCursor::Kind::Hidden); QVERIFY(!mailbox.take());
        QVERIFY(!mailbox.publish({RemoteCursor::Kind::Bitmap, image, {32, 0}})); QVERIFY(!mailbox.take());
        QVERIFY(mailbox.publish({})); mailbox.clear(); QVERIFY(!mailbox.take());
    }
};
QTEST_GUILESS_MAIN(CursorTest)
#include "rdp_cursor.moc"
