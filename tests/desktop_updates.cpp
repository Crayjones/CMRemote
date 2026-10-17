#include "DesktopUpdate.h"
#include <QtTest>
#include <algorithm>

namespace {
FrameStamp stamp(quint64 revision, quint64 geometry = 1) { return {geometry, revision, revision * 100, revision * 100, 0}; }
void fill(QImage& image, QRect rectangle, QRgb color) {
    for (int y = rectangle.top(); y <= rectangle.bottom(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        std::fill(row + rectangle.left(), row + rectangle.right() + 1, color);
    }
}
DesktopUpdateAccumulator::Capture capture(DesktopUpdateAccumulator& pending, const QImage& image, QRegion damage, FrameStamp frame) {
    return pending.capture(image.constBits(), image.size(), image.bytesPerLine(), damage, frame);
}
}
class DesktopUpdates : public QObject {
    Q_OBJECT
private slots:
    void damageBatchOwnsPixels() {
        QByteArray bytes((1024 * 4 + 16) * 768, '\0');
        QImage borrowed(reinterpret_cast<uchar*>(bytes.data()), 1024, 768, 1024 * 4 + 16, QImage::Format_RGB32);
        borrowed.fill(Qt::red);
        DesktopUpdateAccumulator pending;
        const auto result = capture(pending, borrowed, QRegion(borrowed.rect()), stamp(1));
        QVERIFY(result.accepted && result.notify);
        QCOMPARE(result.copiedBytes, quint64(1024 * 768 * 4));
        auto batch = pending.take(); borrowed = {}; bytes.fill('\0'); bytes.clear();
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, batch));
        QCOMPARE(desktop.pixelColor(1023, 767), QColor(Qt::red));
        QVERIFY(pending.take().patches.empty());
    }
    void coalescingPreservesDisjointDamage() {
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::black);
        DesktopUpdateAccumulator pending; QImage desktop;
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).notify);
        QVERIFY(applyDesktopUpdate(desktop, pending.take()));
        const QRect a(8, 8, 16, 16), b(904, 650, 16, 16);
        fill(source, a, qRgb(255, 0, 0));
        const auto first = capture(pending, source, a, stamp(2));
        fill(source, b, qRgb(0, 255, 0));
        const auto second = capture(pending, source, b, stamp(3));
        QVERIFY(first.notify && !second.notify);
        QVERIFY(first.copiedBytes + second.copiedBytes < quint64(source.sizeInBytes()));
        auto batch = pending.take(); QVERIFY(!batch.full); QCOMPARE(batch.patches.size(), size_t(2));
        QCOMPARE(batch.stamp.oldestNs, quint64(200)); QCOMPARE(batch.stamp.latestNs, quint64(300));
        QCOMPARE(batch.stamp.replacements, quint64(1));
        QVERIFY(applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop, source);
        QVERIFY(capture(pending, source, a, stamp(4)).notify);
    }
    void overlappingDamageKeepsNewestPixels() {
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::black);
        DesktopUpdateAccumulator pending; QImage desktop;
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).accepted);
        QVERIFY(applyDesktopUpdate(desktop, pending.take()));
        fill(source, QRect(8, 8, 24, 24), qRgb(255, 0, 0));
        QVERIFY(capture(pending, source, QRect(8, 8, 24, 24), stamp(2)).accepted);
        fill(source, QRect(16, 16, 24, 24), qRgb(0, 0, 255));
        QVERIFY(capture(pending, source, QRect(16, 16, 24, 24), stamp(3)).accepted);
        auto batch = pending.take(); QCOMPARE(batch.patches.size(), size_t(1));
        QVERIFY(applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop, source);
        QCOMPARE(desktop.pixelColor(9, 9), QColor(Qt::red)); QCOMPARE(desktop.pixelColor(17, 17), QColor(Qt::blue));
    }
    void pendingFullSnapshotReceivesSparseChanges() {
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::black);
        DesktopUpdateAccumulator pending;
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).notify);
        const QRect damage(900, 700, 16, 16); fill(source, damage, qRgb(0, 255, 0));
        const auto result = capture(pending, source, damage, stamp(2));
        QVERIFY(result.accepted && !result.notify);
        QCOMPARE(result.copiedBytes, quint64(16 * 16 * 4));
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, pending.take())); QCOMPARE(desktop, source);
    }
    void resizeAndGenerationResetDamage() {
        DesktopUpdateAccumulator pending;
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::red);
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).notify);
        QImage resized(1124, 768, QImage::Format_RGB32); resized.fill(Qt::blue);
        QVERIFY(!capture(pending, resized, QRect(1, 1, 1, 1), stamp(2)).accepted);
        const auto resize = capture(pending, resized, QRect(1, 1, 1, 1), stamp(2, 2));
        QVERIFY(resize.accepted && !resize.notify);
        QVERIFY(!capture(pending, source, source.rect(), stamp(3)).accepted);
        auto batch = pending.take(); QVERIFY(batch.full); QCOMPARE(batch.size, resized.size());
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop, resized);
        pending.reset(); QVERIFY(capture(pending, source, QRect(1, 1, 1, 1), stamp(1)).notify);
        batch = pending.take(); QVERIFY(batch.full); QCOMPARE(batch.stamp.geometry, quint64(1));
    }
    void damageBacklogIsBounded() {
        QImage source(2048, 2048, QImage::Format_RGB32); source.fill(Qt::black);
        DesktopUpdateAccumulator pending;
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).accepted); pending.take();
        for (quint64 i = 0; i < 1200; ++i) {
            const int tile = int(i % 1024);
            const QRect rectangle((tile % 32) * 64, (tile / 32) * 64, 1, 1);
            fill(source, rectangle, qRgb(int(i % 255), 25, 90));
            QVERIFY(capture(pending, source, rectangle, stamp(i + 2)).accepted);
            QVERIFY(pending.patchCount() <= DesktopUpdateAccumulator::MaxPatches);
            QVERIFY(pending.bytes() <= quint64(source.sizeInBytes()));
        }
        auto batch = pending.take(); QVERIFY(batch.full);
        QImage desktop; QVERIFY(applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop, source);
    }
    void malformedDamageLeavesDesktopUnchanged() {
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::red);
        DesktopUpdateAccumulator pending;
        QVERIFY(!pending.capture(nullptr, source.size(), source.bytesPerLine(), source.rect(), stamp(1)).accepted);
        QVERIFY(!pending.capture(source.constBits(), QSize(8193, 1), 8193 * 4, {}, stamp(1)).accepted);
        QVERIFY(!pending.capture(source.constBits(), source.size(), 1, source.rect(), stamp(1)).accepted);
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).accepted);
        auto batch = pending.take(); QImage desktop;
        QVERIFY(applyDesktopUpdate(desktop, batch)); const auto snapshot = desktop;
        batch.patches.front().rectangle.moveLeft(1);
        QVERIFY(!applyDesktopUpdate(desktop, batch)); QCOMPARE(desktop, snapshot);
        QVERIFY(!capture(pending, source, source.rect(), stamp(1)).accepted);
    }
    void publicSnapshotRemainsIndependent() {
        QImage source(1024, 768, QImage::Format_RGB32); source.fill(Qt::red);
        DesktopUpdateAccumulator pending; QImage desktop;
        QVERIFY(capture(pending, source, source.rect(), stamp(1)).accepted);
        QVERIFY(applyDesktopUpdate(desktop, pending.take())); const auto snapshot = desktop;
        fill(source, QRect(4, 4, 16, 16), qRgb(0, 0, 255));
        QVERIFY(capture(pending, source, QRect(4, 4, 16, 16), stamp(2)).accepted);
        QVERIFY(applyDesktopUpdate(desktop, pending.take()));
        QCOMPARE(snapshot.pixelColor(4, 4), QColor(Qt::red)); QCOMPARE(desktop.pixelColor(4, 4), QColor(Qt::blue));
    }
};
QTEST_GUILESS_MAIN(DesktopUpdates)
#include "desktop_updates.moc"
