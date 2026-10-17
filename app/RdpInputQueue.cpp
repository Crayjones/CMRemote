#include "RdpInputQueue.h"
#include <winpr/synch.h>

RdpInputQueue::EventOps RdpInputQueue::systemEvents() {
    return {[]() -> void* { return CreateEvent(nullptr, TRUE, FALSE, nullptr); },
        [](void* event) { return bool(SetEvent(event)); },
        [](void* event) { return bool(ResetEvent(event)); },
        [](void* event) { CloseHandle(event); }};
}
RdpInputQueue::RdpInputQueue(EventOps operations) : operations_(operations) {
    if (operations_.create && operations_.set && operations_.reset && operations_.close)
        event_ = operations_.create();
    failed_ = !event_;
}
RdpInputQueue::~RdpInputQueue() { if (event_) operations_.close(event_); }
bool RdpInputQueue::healthy() const {
    std::lock_guard<std::mutex> lock(mutex_); return !failed_;
}
RdpInputQueue::Result RdpInputQueue::state() const {
    return failed_ ? Result::EventFailed : stopping_ ? Result::Stopped : Result::Queued;
}
RdpInputQueue::Result RdpInputQueue::signal() {
    if (!event_ || !operations_.set(event_)) failed_ = true;
    return state();
}
RdpInputQueue::Result RdpInputQueue::enqueue(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state() != Result::Queued) return state();
    if (commands_.size() >= 1024) return Result::Overflow;
    commands_.push_back(std::move(command)); return signal();
}
RdpInputQueue::Result RdpInputQueue::resize(QSize size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state() != Result::Queued) return state();
    size_ = size; return signal();
}
RdpInputQueue::Result RdpInputQueue::clipboard(QString text, ClipboardStamp stamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state() != Result::Queued) return state();
    if (text.size() > 512 * 1024 - 1) return Result::Queued;
    clipboard_ = std::move(text); clipboardStamp_ = stamp; clipboardDirty_ = true; return signal();
}
RdpInputQueue::Result RdpInputQueue::clipboardControl(ClipboardControl control) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state() != Result::Queued) return state();
    clipboardControl_ = control;
    if (!control.enabled) { clipboard_.clear(); clipboardDirty_ = false; }
    return signal();
}
RdpInputQueue::Batch RdpInputQueue::takeClipboard() {
    std::lock_guard<std::mutex> lock(mutex_); Batch batch;
    batch.clipboard.swap(clipboard_); batch.clipboardStamp = clipboardStamp_;
    batch.clipboardDirty = clipboardDirty_; clipboardDirty_ = false;
    batch.clipboardControl = clipboardControl_; clipboardControl_.reset();
    return batch;
}
void RdpInputQueue::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true; signal();
}
RdpInputQueue::Batch RdpInputQueue::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    Batch batch;
    // Bound protocol sends per drain so a busy input producer cannot starve network work.
    for (size_t i = 0; i < 64 && !commands_.empty(); ++i) {
        batch.commands.push_back(std::move(commands_.front())); commands_.pop_front();
    }
    batch.size = size_; size_ = {};
    batch.clipboard.swap(clipboard_); batch.clipboardStamp = clipboardStamp_;
    batch.clipboardControl = clipboardControl_; clipboardControl_.reset();
    batch.clipboardDirty = clipboardDirty_; clipboardDirty_ = false;
    // Enqueue and reset share this lock. A later enqueue always sets the event again.
    if (commands_.empty() && !stopping_ && !failed_ && !operations_.reset(event_)) failed_ = true;
    batch.stopping = stopping_; batch.eventFailed = failed_;
    return batch;
}
