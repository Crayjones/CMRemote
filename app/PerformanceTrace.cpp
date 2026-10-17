#include "PerformanceTrace.h"
#include <array>
#include <algorithm>
#include <chrono>

PerformanceTrace::PerformanceTrace(bool enabled, quint64 generation) : enabled_(enabled), generation_(generation) {
    if (enabled_) samples_.reserve(Capacity);
}
quint64 PerformanceTrace::now() {
    return quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void PerformanceTrace::record(PerformanceSample sample) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() == Capacity || sample.endNs < sample.beginNs ||
        sample.frame.latestNs < sample.frame.oldestNs ||
        quint32(sample.stage) >= quint32(PerformanceStage::Count) ||
        quint32(sample.codec) >= quint32(ReceivedCodec::Count)) {
        ++dropped_; return;
    }
    sample.sequence = samples_.size() + 1;
    samples_.push_back(sample);
}
std::vector<PerformanceSample> PerformanceTrace::samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_;
}
quint64 PerformanceTrace::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}
QByteArray PerformanceTrace::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    QByteArray json = "{\"schema\":1,\"generation\":" + QByteArray::number(generation_) +
        ",\"dropped\":" + QByteArray::number(dropped_) + ",\"samples\":[";
    bool first = true;
    for (const auto& sample : samples_) {
        if (!first) json += ',';
        first = false; json += '[';
        const std::array<quint64, 14> values{sample.sequence, quint32(sample.stage),
            sample.frame.geometry, sample.frame.revision, sample.frame.oldestNs,
            sample.frame.latestNs, sample.frame.replacements, sample.beginNs, sample.endNs,
            sample.bytes, sample.width, sample.height, quint32(sample.codec), sample.resourceEpoch};
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) json += ',';
            json += QByteArray::number(values[i]);
        }
        json += ']';
    }
    return json + "]}";
}
QByteArray PerformanceTrace::summaryJson(quint64 sinceNs, quint64 untilNs) const {
    std::vector<PerformanceSample> capture;
    quint64 dropped;
    { std::lock_guard<std::mutex> lock(mutex_); capture = samples_; dropped = dropped_; }
    QByteArray json = "{\"schema\":2,\"generation\":" + QByteArray::number(generation_) +
        ",\"dropped\":" + QByteArray::number(dropped) + ",\"stages\":[";
    auto quantile = [](const std::vector<quint64>& sorted, size_t percent) -> quint64 {
        return sorted.empty() ? 0 : sorted[(sorted.size() * percent + 99) / 100 - 1];
    };
    for (quint32 stage = 0; stage < quint32(PerformanceStage::Count); ++stage) {
        std::vector<quint64> durations, oldest, newest;
        quint64 bytes = 0, replacements = 0;
        for (const auto& sample : capture) {
            if (quint32(sample.stage) != stage || sample.beginNs < sinceNs || sample.endNs > untilNs) continue;
            durations.push_back(sample.endNs - sample.beginNs); bytes += sample.bytes;
            replacements += sample.frame.replacements;
            if (sample.frame.oldestNs && sample.endNs >= sample.frame.latestNs) {
                oldest.push_back(sample.endNs - sample.frame.oldestNs);
                newest.push_back(sample.endNs - sample.frame.latestNs);
            }
        }
        std::sort(durations.begin(), durations.end()); std::sort(oldest.begin(), oldest.end()); std::sort(newest.begin(), newest.end());
        const std::array<quint64, 14> values{stage, durations.size(), quantile(durations, 50),
            quantile(durations, 95), quantile(durations, 99), quantile(durations, 100), bytes, replacements,
            quantile(oldest, 95), quantile(oldest, 99), quantile(oldest, 100),
            quantile(newest, 95), quantile(newest, 99), quantile(newest, 100)};
        if (stage) json += ',';
        json += '[';
        for (size_t i = 0; i < values.size(); ++i) { if (i) json += ','; json += QByteArray::number(values[i]); }
        json += ']';
    }
    std::vector<PerformanceSample> submissions;
    for (const auto& sample : capture) {
        if (sample.stage == PerformanceStage::WindowSubmission && sample.frame.revision &&
            sample.beginNs >= sinceNs && sample.endNs <= untilNs) submissions.push_back(sample);
    }
    std::sort(submissions.begin(), submissions.end(), [](const auto& a, const auto& b) { return a.endNs < b.endNs; });
    std::vector<quint64> gaps;
    PerformanceSample previous;
    for (const auto& sample : submissions) {
        if (sample.frame.geometry == previous.frame.geometry) {
            if (sample.frame.revision <= previous.frame.revision) continue;
            if (previous.frame.revision) gaps.push_back(sample.endNs - previous.endNs);
        }
        previous = sample;
    }
    std::sort(gaps.begin(), gaps.end());
    constexpr quint64 stallThresholdNs = 50000000;
    const std::array<quint64, 7> gapValues{gaps.size(), quantile(gaps, 50), quantile(gaps, 95),
        quantile(gaps, 99), quantile(gaps, 100), stallThresholdNs,
        quint64(std::count_if(gaps.begin(), gaps.end(), [](quint64 gap) { return gap >= stallThresholdNs; }))};
    json += "],\"submission_gaps\":[";
    for (size_t i = 0; i < gapValues.size(); ++i) { if (i) json += ','; json += QByteArray::number(gapValues[i]); }
    json += "],\"codecs\":[";
    bool first = true;
    for (quint32 codec = 0; codec < quint32(ReceivedCodec::Count); ++codec) {
        const bool received = std::any_of(capture.begin(), capture.end(), [codec](const auto& sample) {
            return sample.stage == PerformanceStage::ReceivedCodec && quint32(sample.codec) == codec;
        });
        if (!received) continue;
        if (!first) json += ',';
        first = false; json += QByteArray::number(codec);
    }
    return json + "]}";
}
