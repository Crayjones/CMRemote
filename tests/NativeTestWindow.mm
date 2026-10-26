#import <AppKit/AppKit.h>
#include <QWidget>
#include <QGuiApplication>

void activateNativeTestApplication(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activate];
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    [view.window makeKeyAndOrderFront:nil];
}

void toggleNativeFullScreen(QWidget* widget) {
    // What the green window button and Control-Command-F do.
    if (QGuiApplication::platformName() != "cocoa") return;
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    [view.window toggleFullScreen:nil];
}

bool isNativeFullScreen(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return false;
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    return (view.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
}

bool isNativeVisible(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return widget->isVisible();
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    return view.window.visible;
}

bool isNativeMiniaturized(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return widget->isMinimized();
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    return view.window.miniaturized;
}

void deminiaturizeNative(QWidget* widget) {
    // What clicking the window in the Dock does.
    if (QGuiApplication::platformName() != "cocoa") return;
    NSView* view = reinterpret_cast<NSView*>(widget->winId());
    [view.window deminiaturize:nil];
}

const void* nativeWindow(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return nullptr;
    return reinterpret_cast<NSView*>(widget->winId()).window;
}

bool isNativeTitleHidden(QWidget* widget) {
    if (QGuiApplication::platformName() != "cocoa") return false;
    return reinterpret_cast<NSView*>(widget->winId()).window.titleVisibility == NSWindowTitleHidden;
}

bool screenCaptureAllowed() {
    // Preflight never shows the Screen Recording prompt; capturing without access would.
    return QGuiApplication::platformName() != "cocoa" || CGPreflightScreenCaptureAccess();
}
