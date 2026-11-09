#pragma once
#include <QObject>
#include <QSize>
#include <QTimer>
#include <algorithm>

// DISP requests may be ignored while the server is still opening the desktop.
// Debounce layout changes and retry until a frame confirms the requested size.
class RdpDesktopFit final : public QObject {
    Q_OBJECT
public:
    explicit RdpDesktopFit(QObject* parent = nullptr) : QObject(parent) {
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (!pending() || attempts_ >= MaxAttempts) return;
            ++attempts_; emit resizeRequested(viewport_);
            if (pending() && attempts_ < MaxAttempts) timer_.start(1000);
        });
    }
    void setViewport(QSize size) {
        if (size.isEmpty()) { viewport_ = {}; timer_.stop(); return; }
        size = {std::clamp(size.width(), 200, 8192) & ~1, std::clamp(size.height(), 200, 8192)};
        if (size == viewport_) return;
        viewport_ = size; attempts_ = 0; timer_.stop(); schedule();
    }
    void setDesktop(QSize size) {
        if (desktop_ == size) return;
        desktop_ = size; schedule();
    }
    void setAvailable(bool available) {
        if (available_ == available) return;
        available_ = available; attempts_ = 0; schedule();
    }
    void setConnected(bool connected) {
        if (connected_ == connected) return;
        connected_ = connected; attempts_ = 0; schedule();
    }
signals:
    void resizeRequested(QSize size);
private:
    bool pending() const { return connected_ && available_ && !viewport_.isEmpty() && !desktop_.isEmpty() && desktop_ != viewport_; }
    void schedule() {
        if (!pending()) timer_.stop();
        else if (!timer_.isActive() && attempts_ < MaxAttempts) timer_.start(200);
    }
    static constexpr int MaxAttempts = 6;
    QTimer timer_;
    QSize viewport_, desktop_;
    bool connected_ = false, available_ = false;
    int attempts_ = 0;
};
