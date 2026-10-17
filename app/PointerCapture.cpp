#include "PointerCapture.h"
#include <QCursor>
#include <QWidget>

#ifdef Q_OS_MACOS
void* rmNativePointerCaptureStart(QWidget* widget, std::function<void(double, double)> motion);
void rmNativePointerCaptureStop(void* capture);
#endif

bool PointerCapture::start(QWidget* widget, Motion motion) {
    stop(); if (!widget || !motion) return false;
    widget_ = widget; motion_ = std::move(motion);
#ifdef Q_OS_MACOS
    native_ = rmNativePointerCaptureStart(widget, motion_);
    if (native_) return true;
#endif
    widget->grabMouse(Qt::BlankCursor);
    QCursor::setPos(widget->mapToGlobal(widget->rect().center()));
    return true;
}
void PointerCapture::stop() {
    if (!widget_) return;
#ifdef Q_OS_MACOS
    if (native_) { rmNativePointerCaptureStop(native_); native_ = nullptr; widget_ = nullptr; motion_ = {}; return; }
#endif
    widget_->releaseMouse(); widget_ = nullptr; motion_ = {};
}
bool PointerCapture::moved(QPoint position) {
    if (!widget_) return false;
    if (native_) return true; // deltas arrive from the native monitor
    const auto centre = widget_->rect().center(); const auto delta = position - centre;
    if (delta.isNull()) return true; // the warp's own synthetic move
    motion_(delta.x(), delta.y());
    QCursor::setPos(widget_->mapToGlobal(centre));
    return true;
}
