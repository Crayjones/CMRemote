#pragma once
#include <QSize>
#include "ClipboardTransfer.h"
#include <QString>
#include <deque>
#include <mutex>

// Owned by the worker; destroy only after its thread has joined.
class RdpInputQueue final {
public:
    struct Command {
        int type;
        quint32 scan = 0;
        bool down = false, repeat = false;
        quint16 flags = 0, x = 0, y = 0;
        QString text;
        quint64 queuedNs = 0;
    };
    struct Batch {
        std::deque<Command> commands;
        QSize size;
        QString clipboard;
        ClipboardStamp clipboardStamp;
        std::optional<ClipboardControl> clipboardControl;
        bool clipboardDirty = false, stopping = false, eventFailed = false;
    };
    struct EventOps {
        void* (*create)();
        bool (*set)(void*);
        bool (*reset)(void*);
        void (*close)(void*);
    };
    enum class Result { Queued, Stopped, Overflow, EventFailed };
    static EventOps systemEvents();
    explicit RdpInputQueue(EventOps operations = systemEvents());
    ~RdpInputQueue();
    RdpInputQueue(const RdpInputQueue&) = delete;
    RdpInputQueue& operator=(const RdpInputQueue&) = delete;
    void* event() const { return event_; }
    bool healthy() const;
    Result enqueue(Command command);
    Result resize(QSize size);
    Result clipboard(QString text, ClipboardStamp stamp = {});
    Result clipboardControl(ClipboardControl control);
    Batch takeClipboard();
    void stop();
    Batch take();
private:
    Result state() const;
    Result signal();
    EventOps operations_;
    void* event_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    QSize size_;
    QString clipboard_;
    ClipboardStamp clipboardStamp_;
    std::optional<ClipboardControl> clipboardControl_;
    bool clipboardDirty_ = false, stopping_ = false, failed_ = false;
};
