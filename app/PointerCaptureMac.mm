#import <AppKit/AppKit.h>
#include <QWidget>
#include <functional>

// Non-ARC. The local monitor sees events only while this app is active; the
// owner releases capture on focus loss or deactivation.
void* rmNativePointerCaptureStart(QWidget* widget, std::function<void(double, double)> motion) {
    const QPoint centre = widget->mapToGlobal(widget->rect().center());
    // Qt global and Quartz display coordinates share a top-left origin in points.
    CGWarpMouseCursorPosition(CGPointMake(centre.x(), centre.y()));
    if (CGAssociateMouseAndMouseCursorPosition(false) != kCGErrorSuccess) return nullptr;
    const NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
    id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* event) {
        if (event.deltaX != 0 || event.deltaY != 0) motion(event.deltaX, event.deltaY);
        return event;
    }];
    if (!monitor) { CGAssociateMouseAndMouseCursorPosition(true); return nullptr; }
    [NSCursor hide];
    return [monitor retain];
}
void rmNativePointerCaptureStop(void* capture) {
    id monitor = static_cast<id>(capture);
    [NSEvent removeMonitor:monitor]; [monitor release];
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
}
