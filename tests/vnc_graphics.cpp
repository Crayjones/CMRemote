#include "VncGraphics.h"
#include <QtTest>
#include <array>
#include <atomic>
#include <thread>

namespace {
QByteArray wireImage(int w, int h) {
    QByteArray bytes(w * h * 4, '\0');
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int offset = (y * w + x) * 4;
        bytes[offset] = char((x * 19 + y) & 255);
        bytes[offset + 1] = char((y * 23 + x) & 255);
        bytes[offset + 2] = char((x + y * 7) & 255);
        bytes[offset + 3] = char((x + y) & 255);
    }
    return bytes;
}
const uchar* data(const QByteArray& bytes) { return reinterpret_cast<const uchar*>(bytes.constData()); }
QRgb wirePixel(const QByteArray& bytes, int index) {
    const auto* pixel = data(bytes) + index * 4;
    return qRgb(pixel[2], pixel[1], pixel[0]);
}
// Rectangles publish only at the FramebufferUpdate boundary.
DesktopUpdate committed(VncGraphics& graphics) { graphics.commit(); return graphics.takeUpdate(); }
void referenceCopy(QImage& image, QRect source, QPoint destination) {
    const auto pixels = image.copy(source);
    for (int y = 0; y < pixels.height(); ++y) for (int x = 0; x < pixels.width(); ++x)
        image.setPixel(destination.x() + x, destination.y() + y, pixels.pixel(x, y));
}
}
class VncGraphicsTest final : public QObject {
    Q_OBJECT
private slots:
    void rawChannelOrderOpaqueAndOwned() {
        VncGraphics graphics;
        QVERIFY(graphics.geometry(3, 2));
        QVERIFY(!graphics.hasUpdate()); QVERIFY(graphics.commit()); QVERIFY(graphics.hasUpdate());
        auto initial = committed(graphics);
        QVERIFY(initial.full); QVERIFY(initial.stamp.geometry); QVERIFY(initial.stamp.revision);
        QVERIFY(!graphics.hasUpdate());
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, initial));
        QCOMPARE(desktop.format(), QImage::Format_RGB32); QCOMPARE(desktop.pixel(2, 1), qRgb(0, 0, 0));
        auto bytes = wireImage(3, 2);
        bytes[0] = char(0x12); bytes[1] = char(0x34); bytes[2] = char(0x56); bytes[3] = '\0';
        bytes[7] = char(0x80); bytes[11] = char(0xff);
        QVERIFY(graphics.raw(0, 0, 3, 2, data(bytes), size_t(bytes.size())));
        const auto update = committed(graphics);
        QVERIFY(update.stamp.revision > initial.stamp.revision);
        QCOMPARE(update.stamp.geometry, initial.stamp.geometry);
        const auto expected = wirePixel(bytes, 5);
        bytes.fill('\0'); bytes.clear();
        QVERIFY(applyDesktopUpdate(desktop, update));
        QCOMPARE(desktop.pixel(0, 0), qRgb(0x56, 0x34, 0x12));
        QCOMPARE(desktop.pixel(2, 1), expected);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) QCOMPARE(qAlpha(desktop.pixel(x, y)), 255);
        QCOMPARE(initial.patches.front().pixels.pixel(0, 0), qRgb(0, 0, 0));
        QVERIFY(committed(graphics).patches.empty());
    }
    void copyOverlap_data() {
        QTest::addColumn<QRect>("source"); QTest::addColumn<QPoint>("destination");
        QTest::newRow("right") << QRect(0, 0, 7, 8) << QPoint(1, 0);
        QTest::newRow("left") << QRect(1, 0, 7, 8) << QPoint(0, 0);
        QTest::newRow("down") << QRect(0, 0, 8, 7) << QPoint(0, 1);
        QTest::newRow("up") << QRect(0, 1, 8, 7) << QPoint(0, 0);
        QTest::newRow("diagonal") << QRect(0, 0, 7, 7) << QPoint(1, 1);
        QTest::newRow("self") << QRect(0, 0, 8, 8) << QPoint(0, 0);
    }
    void copyOverlap() {
        QFETCH(QRect, source); QFETCH(QPoint, destination);
        VncGraphics graphics; QVERIFY(graphics.geometry(8, 8));
        const auto bytes = wireImage(8, 8); QVERIFY(graphics.raw(0, 0, 8, 8, data(bytes), size_t(bytes.size())));
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, committed(graphics)));
        auto expected = desktop; referenceCopy(expected, source, destination);
        QVERIFY(graphics.copy(quint16(destination.x()), quint16(destination.y()), quint16(source.width()),
            quint16(source.height()), quint16(source.x()), quint16(source.y())));
        QVERIFY(applyDesktopUpdate(desktop, committed(graphics))); QCOMPARE(desktop, expected);
    }
    void prevalidationLeavesCanonicalPixelsAndPendingBatchUntouched() {
        VncGraphics graphics;
        const auto bytes = wireImage(4, 4);
        QVERIFY(!graphics.raw(0, 0, 4, 4, data(bytes), size_t(bytes.size())));
        QVERIFY(!graphics.copy(0, 0, 1, 1, 0, 0)); QVERIFY(!graphics.hasUpdate());
        QVERIFY(graphics.geometry(4, 4));
        QVERIFY(graphics.raw(0, 0, 4, 4, data(bytes), size_t(bytes.size())));
        const auto accepted = committed(graphics);
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, accepted)); const auto expected = desktop;
        QVERIFY(!graphics.geometry(0, 4)); QVERIFY(!graphics.geometry(8193, 1));
        QVERIFY(!graphics.geometry(8192, 1013));
        QVERIFY(!graphics.raw(0, 0, 4, 4, nullptr, size_t(bytes.size())));
        QVERIFY(!graphics.raw(0, 0, 4, 4, data(bytes), size_t(bytes.size() - 1)));
        QVERIFY(!graphics.raw(0, 0, 4, 4, data(bytes), size_t(bytes.size() + 1)));
        QVERIFY(!graphics.raw(3, 0, 2, 1, data(bytes), 8));
        QVERIFY(!graphics.raw(65535, 0, 2, 1, data(bytes), 8));
        QVERIFY(!graphics.raw(0, 0, 0, 1, data(bytes), 0));
        QVERIFY(!graphics.copy(0, 0, 2, 2, 3, 3));
        QVERIFY(!graphics.copy(3, 3, 2, 2, 0, 0));
        QVERIFY(!graphics.copy(65535, 0, 2, 1, 0, 0));
        QVERIFY(!graphics.copy(0, 0, 0, 1, 0, 0));
        QVERIFY(!graphics.hasUpdate()); QVERIFY(committed(graphics).patches.empty());
        // Capture canonical pixels again, proving invalid calls changed no pixels.
        QVERIFY(graphics.copy(0, 0, 4, 4, 0, 0));
        const auto after = committed(graphics);
        QCOMPARE(after.stamp.geometry, accepted.stamp.geometry);
        QCOMPARE(after.stamp.revision, accepted.stamp.revision + 1);
        QVERIFY(applyDesktopUpdate(desktop, after)); QCOMPARE(desktop, expected);
        // Invalid primitives must not discard a valid batch already waiting.
        QVERIFY(graphics.raw(0, 0, 1, 1, data(bytes), 4));
        QVERIFY(!graphics.raw(4, 0, 1, 1, data(bytes), 4)); QVERIFY(!graphics.hasUpdate()); QVERIFY(graphics.commit()); QVERIFY(graphics.hasUpdate());
        QVERIFY(applyDesktopUpdate(desktop, committed(graphics))); QCOMPARE(desktop, expected);
    }
    void actualResizeFullSnapshotSameSizePreservesAndLateBatchesOwnPixels() {
        VncGraphics graphics; QVERIFY(graphics.geometry(512, 512));
        const auto bytes = wireImage(512, 512); QVERIFY(graphics.raw(0, 0, 512, 512, data(bytes), size_t(bytes.size())));
        const auto old = committed(graphics); QImage desktop; QVERIFY(applyDesktopUpdate(desktop, old));
        const auto original = desktop;
        QVERIFY(graphics.geometry(512, 512)); QVERIFY(!graphics.hasUpdate());
        QVERIFY(graphics.copy(0, 0, 1, 1, 511, 511));
        const auto partial = committed(graphics); QVERIFY(!partial.full);
        QCOMPARE(partial.stamp.geometry, old.stamp.geometry);
        QVERIFY(graphics.geometry(513, 512));
        QVERIFY(graphics.raw(512, 511, 1, 1, data(bytes), 4));
        const auto resized = committed(graphics); QVERIFY(resized.full);
        QVERIFY(resized.stamp.geometry > old.stamp.geometry); QVERIFY(resized.stamp.revision > partial.stamp.revision);
        QVERIFY(applyDesktopUpdate(desktop, resized)); QCOMPARE(desktop.size(), QSize(513, 512));
        QCOMPARE(desktop.pixel(1, 1), qRgb(0, 0, 0)); QCOMPARE(desktop.pixel(512, 511), wirePixel(bytes, 0));
        QImage delayed; QVERIFY(applyDesktopUpdate(delayed, old)); QCOMPARE(delayed, original);
        QVERIFY(graphics.geometry(512, 512)); const auto back = committed(graphics); QVERIFY(back.full);
        QVERIFY(back.stamp.geometry > resized.stamp.geometry);
        graphics.reset(); QVERIFY(!graphics.hasUpdate()); QVERIFY(!graphics.hasCursor());
        QVERIFY(!graphics.copy(0, 0, 1, 1, 0, 0)); QVERIFY(graphics.geometry(512, 512));
        const auto restarted = committed(graphics); QVERIFY(restarted.full);
        QVERIFY(restarted.stamp.geometry > back.stamp.geometry); QVERIFY(restarted.stamp.revision > back.stamp.revision);
    }
    void dimensionsRespectUpstreamLimits() {
        VncGraphics graphics;
        QVERIFY(graphics.geometry(8192, 1)); QCOMPARE(committed(graphics).size, QSize(8192, 1));
        QVERIFY(graphics.geometry(1, 8192)); QCOMPARE(committed(graphics).size, QSize(1, 8192));
        QVERIFY(graphics.geometry(3840, 2160)); QCOMPARE(committed(graphics).size, QSize(3840, 2160));
        QVERIFY(!graphics.geometry(3841, 2160)); QVERIFY(!graphics.geometry(8192, 8192));
        QVERIFY(!graphics.geometry(1, 8193)); QVERIFY(!graphics.hasUpdate());
    }
    void slowGuiBoundedLatestPatchesAndCanonicalCopy() {
        VncGraphics graphics; QVERIFY(graphics.geometry(2048, 2048));
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, committed(graphics)));
        auto expected = desktop;
        for (int i = 0; i < 1200; ++i) {
            const int tile = i % 1024, x = (tile % 32) * 64, y = (tile / 32) * 64;
            const std::array<uchar, 4> pixel{uchar(i % 255), 25, 90, 0};
            // One FramebufferUpdate per pixel while the GUI never takes a batch.
            QVERIFY(graphics.raw(quint16(x), quint16(y), 1, 1, pixel.data(), pixel.size())); QVERIFY(graphics.commit());
            expected.setPixel(x, y, qRgb(90, 25, int(i % 255)));
        }
        // Copy uses the latest canonical source, not the GUI's older image.
        QVERIFY(graphics.copy(1, 1, 1, 1, 0, 0)); expected.setPixel(1, 1, expected.pixel(0, 0));
        const auto update = committed(graphics); QVERIFY(update.full);
        QVERIFY(update.patches.size() <= DesktopUpdateAccumulator::MaxPatches);
        quint64 ownedBytes = 0;
        for (const auto& patch : update.patches) ownedBytes += quint64(patch.pixels.sizeInBytes());
        QVERIFY(ownedBytes <= quint64(expected.sizeInBytes())); QVERIFY(update.stamp.replacements >= 1199);
        QVERIFY(applyDesktopUpdate(desktop, update)); QCOMPARE(desktop, expected);
        QVERIFY(!graphics.hasUpdate()); QVERIFY(committed(graphics).patches.empty());
    }
    void sparsePrimitiveCapturesCoalesceLatestTiles() {
        VncGraphics graphics; QVERIFY(graphics.geometry(1024, 768));
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, committed(graphics)));
        const std::array<uchar, 4> red{0, 0, 255, 0}, blue{255, 0, 0, 0};
        QVERIFY(graphics.raw(8, 8, 1, 1, red.data(), red.size()));
        QVERIFY(graphics.raw(9, 9, 1, 1, blue.data(), blue.size()));
        QVERIFY(graphics.raw(904, 650, 1, 1, blue.data(), blue.size()));
        const auto batch = committed(graphics); QVERIFY(!batch.full); QCOMPARE(batch.patches.size(), size_t(2));
        QVERIFY(applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop.pixel(8, 8), qRgb(255, 0, 0));
        QCOMPARE(desktop.pixel(9, 9), qRgb(0, 0, 255)); QCOMPARE(desktop.pixel(904, 650), qRgb(0, 0, 255));
    }
    void cursorChannelMaskHotspotBoundsAndMailbox() {
        VncGraphics graphics; QVERIFY(!graphics.takeCursor()); QVERIFY(!graphics.hasCursor());
        const std::array<uchar, 8> bytes{0x12, 0x34, 0x56, 0, 0x78, 0x9a, 0xbc, 255};
        QVERIFY(graphics.cursor(1, 0, 2, 1, bytes.data(), bytes.size())); QVERIFY(graphics.hasCursor());
        QVERIFY(!graphics.cursor(2, 0, 2, 1, bytes.data(), bytes.size()));
        QVERIFY(!graphics.cursor(0, 1, 2, 1, bytes.data(), bytes.size()));
        QVERIFY(!graphics.cursor(0, 0, 2, 1, nullptr, bytes.size()));
        QVERIFY(!graphics.cursor(0, 0, 2, 1, bytes.data(), bytes.size() - 1));
        QVERIFY(!graphics.cursor(0, 0, 385, 1, bytes.data(), bytes.size()));
        const auto cursor = graphics.takeCursor(); QVERIFY(cursor); QVERIFY(cursor->valid());
        QCOMPARE(cursor->kind, RemoteCursor::Kind::Bitmap); QCOMPARE(cursor->hotspot, QPoint(1, 0));
        QCOMPARE(cursor->image.format(), QImage::Format_ARGB32);
        QCOMPARE(cursor->image.pixel(0, 0), qRgba(0x56, 0x34, 0x12, 0));
        QCOMPARE(cursor->image.pixel(1, 0), qRgba(0xbc, 0x9a, 0x78, 255));
        QVERIFY(!graphics.hasCursor()); QVERIFY(!graphics.takeCursor());
        for (int i = 0; i < 2000; ++i) QVERIFY(graphics.cursor(0, 0, 1, 1, bytes.data(), 4));
        QVERIFY(graphics.cursor(0, 0, 0, 0, nullptr, 0));
        const auto hidden = graphics.takeCursor(); QVERIFY(hidden); QCOMPARE(hidden->kind, RemoteCursor::Kind::Hidden);
        QVERIFY(hidden->image.isNull()); QVERIFY(!graphics.takeCursor());
        QCOMPARE(cursor->image.pixel(1, 0), qRgba(0xbc, 0x9a, 0x78, 255));
        const auto maximum = wireImage(384, 384);
        QVERIFY(graphics.cursor(383, 383, 384, 384, data(maximum), size_t(maximum.size())));
        graphics.reset(); QVERIFY(!graphics.hasCursor()); QVERIFY(!graphics.takeCursor());
    }
    void partialUpdatesStayInvisibleUntilCommit() {
        VncGraphics graphics; QVERIFY(graphics.geometry(128, 64));
        QVERIFY(!graphics.hasUpdate()); QVERIFY(graphics.commit());
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, graphics.takeUpdate()));
        const auto left = wireImage(64, 64);
        // First half of a window move: the GUI must not observe it on its own.
        QVERIFY(graphics.raw(0, 0, 64, 64, data(left), size_t(left.size())));
        QVERIFY(!graphics.hasUpdate()); QVERIFY(graphics.takeUpdate().patches.empty());
        QVERIFY(graphics.copy(64, 0, 64, 64, 0, 0));
        QVERIFY(!graphics.hasUpdate());
        QVERIFY(graphics.commit()); QVERIFY(graphics.hasUpdate());
        const auto update = graphics.takeUpdate(); QVERIFY(!update.patches.empty());
        QVERIFY(applyDesktopUpdate(desktop, update));
        QCOMPARE(desktop.pixel(10, 10), wirePixel(left, 10 * 64 + 10)); QCOMPARE(desktop.pixel(74, 10), wirePixel(left, 10 * 64 + 10));
        // Empty FramebufferUpdates commit nothing and do not wake the GUI.
        QVERIFY(graphics.commit()); QVERIFY(!graphics.hasUpdate());
        // Geometry changes publish one full snapshot including the same update's pixels.
        QVERIFY(graphics.geometry(64, 64)); QVERIFY(graphics.raw(0, 0, 64, 64, data(left), size_t(left.size())));
        QVERIFY(!graphics.hasUpdate()); QVERIFY(graphics.commit());
        const auto resized = graphics.takeUpdate(); QVERIFY(resized.full); QVERIFY(resized.stamp.geometry > update.stamp.geometry);
        QImage fresh; QVERIFY(applyDesktopUpdate(fresh, resized)); QCOMPARE(fresh.pixel(5, 5), wirePixel(left, 5 * 64 + 5));
    }
    void workerMutationAndGuiTakeAreIndependent() {
        VncGraphics graphics; QVERIFY(graphics.geometry(256, 256));
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, committed(graphics)));
        std::atomic<bool> complete{false}, valid{true};
        std::thread worker([&] {
            for (int i = 0; i < 1000; ++i) {
                const std::array<uchar, 4> pixel{uchar(i & 255), 30, 40, 0};
                if (!graphics.raw(quint16(i % 256), quint16(i / 256), 1, 1, pixel.data(), pixel.size()) || !graphics.commit()) valid = false;
                if (!graphics.cursor(0, 0, 1, 1, pixel.data(), pixel.size())) valid = false;
            }
            complete = true;
        });
        bool batchesValid = true;
        while (!complete || graphics.hasUpdate()) {
            const auto batch = graphics.takeUpdate();
            if (!batch.patches.empty() && !applyDesktopUpdate(desktop, batch)) batchesValid = false;
            if (auto cursor = graphics.takeCursor(); cursor && !cursor->valid()) batchesValid = false;
            std::this_thread::yield();
        }
        worker.join(); QVERIFY(valid); QVERIFY(batchesValid);
        for (int i = 0; i < 1000; ++i) QCOMPARE(desktop.pixel(i % 256, i / 256), qRgb(40, 30, i & 255));
        const auto cursor = graphics.takeCursor();
        if (cursor) QCOMPARE(cursor->image.pixel(0, 0), qRgba(40, 30, 999 & 255, 0));
    }
};
QTEST_GUILESS_MAIN(VncGraphicsTest)
#include "vnc_graphics.moc"
