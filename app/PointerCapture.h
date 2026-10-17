#pragma once
#include <QPoint>
#include <functional>
class QWidget;

// Locks the local pointer to a widget and reports raw motion deltas in local
// points (mouse capture for games and other relative-mouse applications).
// macOS detaches the cursor and reads NSEvent deltas; elsewhere the cursor is
// grabbed, hidden and warped back to the widget centre after each move.
class PointerCapture final {
public:
    using Motion = std::function<void(double dx, double dy)>;
    PointerCapture() = default;
    PointerCapture(const PointerCapture&) = delete;
    PointerCapture& operator=(const PointerCapture&) = delete;
    ~PointerCapture() { stop(); }
    bool start(QWidget* widget, Motion motion);
    void stop();
    bool active() const { return widget_ != nullptr; }
    // Generic path only: feed widget-local mouse moves. Returns true if consumed.
    bool moved(QPoint position);
private:
    QWidget* widget_ = nullptr;
    Motion motion_;
    void* native_ = nullptr;
};
