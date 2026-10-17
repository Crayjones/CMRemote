#import <AppKit/AppKit.h>
#include <QStringList>

namespace {
NSMenu* submenu(const QString& title) {
    for (NSMenuItem* item in NSApp.mainMenu.itemArray) {
        if (QString::fromNSString(item.title) == title) return item.submenu;
    }
    return nil;
}
}

QStringList nativeMenuTitles(const QString& parent) {
    NSMenu* menu = parent.isEmpty() ? NSApp.mainMenu : submenu(parent);
    QStringList titles;
    for (NSMenuItem* item in menu.itemArray) {
        if (!item.isSeparatorItem && !item.isHidden) titles << QString::fromNSString(item.title);
    }
    return titles;
}

QStringList nativeApplicationMenuTitles() {
    // The first main-menu item is the application menu, whatever its title.
    NSMenu* menu = NSApp.mainMenu.numberOfItems ? [NSApp.mainMenu itemAtIndex:0].submenu : nil;
    QStringList titles;
    for (NSMenuItem* item in menu.itemArray) {
        if (!item.isSeparatorItem && !item.isHidden) titles << QString::fromNSString(item.title);
    }
    return titles;
}

bool triggerNativePreferencesShortcut() {
    NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
        modifierFlags:NSEventModifierFlagCommand timestamp:NSProcessInfo.processInfo.systemUptime
        windowNumber:NSApp.keyWindow.windowNumber context:nil characters:@"," charactersIgnoringModifiers:@","
        isARepeat:NO keyCode:43];
    return [NSApp.mainMenu performKeyEquivalent:event];
}

bool triggerNativeMenuItem(const QString& parent, const QString& title) {
    NSMenu* menu = submenu(parent);
    for (NSInteger index = 0; index < menu.numberOfItems; ++index) {
        NSMenuItem* item = [menu itemAtIndex:index];
        if (QString::fromNSString(item.title) == title && item.isEnabled) {
            [menu performActionForItemAtIndex:index];
            return true;
        }
    }
    return false;
}
