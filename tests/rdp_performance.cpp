#include "PerformanceTrace.h"
#include "RhiPixelOracle.h"
#include "PerformanceMarker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <thread>
#include <type_traits>

class PerformanceTests : public QObject {
    Q_OBJECT
private slots:
    void completionMarkerRejectsInitialWhiteWindow() {
        QImage image(1024, 768, QImage::Format_RGB32); image.fill(Qt::white);
        QCOMPARE(performanceMarker(image.constBits(), image.width(), image.height(), image.bytesPerLine()), 511);
        for (int i = 0; i < 9; ++i) image.setPixelColor(4 + i * 8, 4, (436 & (1 << i)) ? Qt::white : Qt::black);
        QCOMPARE(performanceMarker(image.constBits(), image.width(), image.height(), image.bytesPerLine()), 436);
        image.setPixelColor(4, 4, Qt::red);
        QCOMPARE(performanceMarker(image.constBits(), image.width(), image.height(), image.bytesPerLine()), -1);
        QCOMPARE(performanceMarker(nullptr, 1024, 768, 4096), -1);
        QCOMPARE(performanceMarker(image.constBits(), 64, 768, 4096), -1);
    }
    void linearReadbackOracleUsesPhysicalPixelCenters() {
        QImage image(2, 2, QImage::Format_RGB32);
        image.setPixelColor(0, 0, QColor(240, 128, 32)); image.setPixelColor(1, 0, QColor(24, 48, 72));
        image.setPixelColor(0, 1, Qt::black); image.setPixelColor(1, 1, Qt::white);
        QCOMPARE(linearPixel(image, 0.75, 0), QColor(78, 68, 62));
        QCOMPARE(linearPixel(image, 1, 0), QColor(24, 48, 72));
        QCOMPARE(linearPixel(image, -1, 0), QColor(240, 128, 32));
        QCOMPARE(linearPixel(image, 2, 2), QColor(Qt::white));
    }
    void performanceTraceExcludesSecrets() {
        static_assert(std::is_trivially_copyable_v<PerformanceSample>);
        static_assert(std::is_trivially_copyable_v<FrameStamp>);
        PerformanceTrace trace(true, 7);
        const auto begin = PerformanceTrace::now();
        PerformanceSample sample;
        sample.stage = PerformanceStage::Copy;
        sample.frame = {2, 3, begin, begin, 4};
        sample.beginNs = begin; sample.endNs = PerformanceTrace::now();
        sample.bytes = 4096; sample.width = 32; sample.height = 32;
        trace.record(sample);
        const auto bytes = trace.toJson();
        const auto document = QJsonDocument::fromJson(bytes);
        QVERIFY(document.isObject());
        const auto object = document.object();
        QCOMPARE(object.keys(), QStringList({"dropped", "generation", "samples", "schema"}));
        QCOMPARE(object.value("generation").toInt(), 7);
        QCOMPARE(object.value("samples").toArray().size(), 1);
        for (const auto& row : object.value("samples").toArray()) {
            QVERIFY(row.isArray());
            QCOMPARE(row.toArray().size(), 14);
            for (const auto& value : row.toArray()) QVERIFY(value.isDouble());
        }
        // Fixed keys plus numeric rows: no string/pixel/endpoint field exists to populate.
        QVERIFY(!bytes.contains("password")); QVERIFY(!bytes.contains("clipboard"));
        QVERIFY(!bytes.contains("localhost")); QVERIFY(!bytes.contains("account"));
    }
    void performanceStagesUseMonotonicClock() {
        PerformanceTrace trace(true);
        const auto first = PerformanceTrace::now();
        std::thread producer([&] {
            PerformanceSample sample;
            sample.stage = PerformanceStage::Processing;
            sample.beginNs = PerformanceTrace::now(); sample.endNs = PerformanceTrace::now();
            trace.record(sample);
        });
        producer.join();
        const auto last = PerformanceTrace::now();
        const auto samples = trace.samples();
        QCOMPARE(samples.size(), size_t(1));
        QVERIFY(first <= samples[0].beginNs);
        QVERIFY(samples[0].beginNs <= samples[0].endNs);
        QVERIFY(samples[0].endNs <= last);
        auto invalid = samples[0]; invalid.endNs = invalid.beginNs - 1;
        trace.record(invalid);
        QCOMPARE(trace.samples().size(), size_t(1));
        QCOMPARE(trace.dropped(), quint64(1));
    }
    void performanceTraceIsBoundedAndOptIn() {
        PerformanceTrace disabled;
        PerformanceSample sample;
        sample.stage = PerformanceStage::Mailbox;
        sample.beginNs = PerformanceTrace::now(); sample.endNs = PerformanceTrace::now();
        disabled.record(sample);
        QVERIFY(!disabled.enabled()); QVERIFY(disabled.samples().empty());
        QCOMPARE(disabled.dropped(), quint64(0));
        PerformanceTrace trace(true);
        for (size_t i = 0; i < PerformanceTrace::Capacity + 17; ++i) {
            sample.frame.revision = i + 1;
            trace.record(sample);
        }
        QCOMPARE(trace.samples().size(), PerformanceTrace::Capacity);
        QCOMPARE(trace.dropped(), quint64(17));
        // Keep the first bounded capture rather than silently hiding baseline tails.
        QCOMPARE(trace.samples().front().frame.revision, quint64(1));
        QCOMPARE(trace.samples().back().frame.revision, quint64(PerformanceTrace::Capacity));
    }
    void performanceSummaryKeepsCountsTailsBytesAndOldestAge() {
        PerformanceTrace trace(true, 2);
        for (quint64 i = 1; i <= 100; ++i) {
            PerformanceSample sample{PerformanceStage::WindowSubmission, {1, i, 1, 50, i % 2}, 100, 100 + i, i * 4};
            trace.record(sample);
        }
        const auto report = QJsonDocument::fromJson(trace.summaryJson()).object();
        QCOMPARE(report.value("dropped").toInt(), 0);
        const auto stages = report.value("stages").toArray();
        const auto row = stages.at(int(PerformanceStage::WindowSubmission)).toArray();
        QCOMPARE(row.at(1).toInt(), 100); QCOMPARE(row.at(2).toInt(), 50);
        QCOMPARE(row.at(3).toInt(), 95); QCOMPARE(row.at(4).toInt(), 99);
        QCOMPARE(row.at(5).toInt(), 100); QCOMPARE(row.at(6).toInt(), 20200);
        QCOMPARE(row.at(7).toInt(), 50);
        QCOMPARE(row.at(8).toInt(), 194); QCOMPARE(row.at(11).toInt(), 145);
        const auto empty = QJsonDocument::fromJson(trace.summaryJson(1000)).object().value("stages").toArray();
        QCOMPARE(empty.at(int(PerformanceStage::WindowSubmission)).toArray().at(1).toInt(), 0);
    }
    void submissionGapsExcludeIdleRepaintsAndGeometryChanges() {
        PerformanceTrace trace(true);
        const auto submit = [&](quint64 geometry, quint64 revision, quint64 time) {
            trace.record({PerformanceStage::WindowSubmission, {geometry, revision}, time, time});
        };
        submit(1, 1, 100000000); submit(1, 2, 116000000);
        submit(1, 2, 150000000);  // Same remote revision is not useful new motion.
        submit(1, 3, 176000000);
        submit(2, 1, 200000000); submit(2, 2, 216000000);
        submit(2, 3, 300000000);  // Outside this capture interval.
        const auto report = QJsonDocument::fromJson(trace.summaryJson(100000000, 250000000)).object();
        QCOMPARE(report.value("schema").toInt(), 2);
        const auto gaps = report.value("submission_gaps").toArray();
        QCOMPARE(gaps, QJsonArray({3, 16000000, 60000000, 60000000, 60000000, 50000000, 1}));
        const auto empty = QJsonDocument::fromJson(trace.summaryJson(400000000)).object();
        QCOMPARE(empty.value("submission_gaps").toArray(), QJsonArray({0, 0, 0, 0, 0, 50000000, 0}));
    }
    void concurrentTraceProducersStayBounded() {
        PerformanceTrace trace(true);
        auto producer = [&] {
            for (size_t i = 0; i < PerformanceTrace::Capacity; ++i) {
                PerformanceSample sample;
                sample.beginNs = PerformanceTrace::now(); sample.endNs = PerformanceTrace::now();
                trace.record(sample);
            }
        };
        std::thread a(producer), b(producer); a.join(); b.join();
        QCOMPARE(trace.samples().size(), PerformanceTrace::Capacity);
        QCOMPARE(trace.dropped(), quint64(PerformanceTrace::Capacity));
    }
};
QTEST_GUILESS_MAIN(PerformanceTests)
#include "rdp_performance.moc"
