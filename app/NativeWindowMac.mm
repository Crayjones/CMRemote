#import <AppKit/AppKit.h>
#include <QPointer>
#include <QWidget>
#include <memory>

// Non-ARC. AppKit orders a window hidden during its fullscreen exit animation
// back on screen when the animation ends; hide it again if Qt still has it hidden.
void rmNativeKeepHiddenAfterFullScreen(QWidget* widget) {
    if (!widget->testAttribute(Qt::WA_WState_Created)) return;
    NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
    if (!window) return;
    auto observer = std::make_shared<id>(nil);
    const auto remove = [observer] {
        if (!*observer) return;
        [[NSNotificationCenter defaultCenter] removeObserver:*observer]; [*observer release]; *observer = nil;
    };
    QPointer<QWidget> target = widget;
    *observer = [[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidExitFullScreenNotification object:window
                                                                   queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) {
        if (target && !target->isVisible()) [window orderOut:nil];
        remove();
    }] retain];
    QObject::connect(widget, &QObject::destroyed, remove);
}

// Qt reports a requested minimize before AppKit has left fullscreen and moved
// the window to the Dock; this is true only once it is in the Dock.
bool rmNativeMiniaturized(QWidget* widget) {
    if (!widget->testAttribute(Qt::WA_WState_Created)) return false;
    NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
    return window && window.miniaturized;
}

// Qt minimizes a fullscreen window by leaving fullscreen first, but sometimes
// never takes the second step; take it once the window has left fullscreen.
void rmNativeFinishMinimize(QWidget* widget) {
    if (!widget->testAttribute(Qt::WA_WState_Created)) return;
    NSWindow* window = reinterpret_cast<NSView*>(widget->winId()).window;
    if (window && window.visible && !window.miniaturized && !(window.styleMask & NSWindowStyleMaskFullScreen)) [window miniaturize:nil];
}
