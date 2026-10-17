#pragma once
#include <QByteArray>
#include <QtGlobal>
#include <mutex>
#include <vector>

// These records deliberately cannot carry strings, input values or framebuffer pixels.
enum class PerformanceStage : quint32 {
    Processing, Copy, Mailbox, GuiApply, UploadPreparation, WindowSubmission,
    InputQueue, ReceivedCodec, Count
};
enum class ReceivedCodec : quint32 { Unknown, Uncompressed, RemoteFx, Planar, Avc420, Avc444, Avc444v2, Interleaved, NsCodec, ClearCodec, Progressive, ProgressiveV2, Alpha, Count };
struct FrameStamp {
    quint64 geometry = 0, revision = 0, oldestNs = 0, latestNs = 0, replacements = 0;
};
struct PerformanceSample {
    PerformanceStage stage = PerformanceStage::Processing;
    FrameStamp frame;
    quint64 beginNs = 0, endNs = 0, bytes = 0;
    quint32 width = 0, height = 0;
    ReceivedCodec codec = ReceivedCodec::Unknown;
    quint64 resourceEpoch = 0, sequence = 0;
};
class PerformanceTrace final {
public:
    static constexpr size_t Capacity = 4096;
    explicit PerformanceTrace(bool enabled = false, quint64 generation = 0);
    bool enabled() const { return enabled_; }
    static quint64 now();
    void record(PerformanceSample sample);
    std::vector<PerformanceSample> samples() const;
    quint64 dropped() const;
    QByteArray toJson() const;
    QByteArray summaryJson(quint64 sinceNs = 0, quint64 untilNs = ~quint64(0)) const;
private:
    const bool enabled_;
    const quint64 generation_;
    mutable std::mutex mutex_;
    std::vector<PerformanceSample> samples_;
    quint64 dropped_ = 0;
};
