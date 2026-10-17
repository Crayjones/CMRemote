#pragma once
#include <QObject>
#include <QTimer>

// Minutes to Idle (V104): GUI-thread single-shot timer, restarted by local
// session input, armed only while connected. Never blocks the UI thread.
class IdleTimeout final : public QObject {
    Q_OBJECT
public:
    explicit IdleTimeout(QObject* parent = nullptr) : QObject(parent) {
        timer_.setSingleShot(true); connect(&timer_, &QTimer::timeout, this, &IdleTimeout::expired);
    }
    // 0 disables. intervalMs overrides minutes for tests.
    void configure(int minutes, int intervalMs = 0) {
        timer_.stop(); minutes_ = minutes; timer_.setInterval(intervalMs > 0 ? intervalMs : minutes * 60 * 1000);
    }
    int minutes() const { return minutes_; }
    bool armed() const { return timer_.isActive(); }
    void start() { if (minutes_ > 0) timer_.start(); }
    void activity() { if (timer_.isActive()) timer_.start(); }
    void stop() { timer_.stop(); }
signals:
    void expired();
private:
    QTimer timer_;
    int minutes_ = 0;
};
