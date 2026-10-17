#include "Branding.h"
#include <QGuiApplication>

// Q_INIT_RESOURCE must be outside a namespace so static-library linking finds
// the generated qInitResources_branding symbol.
static void initializeBrandingResources() {
    static const bool initialized = [] { Q_INIT_RESOURCE(branding); return true; }();
    Q_UNUSED(initialized);
}

namespace Branding {
QIcon applicationIcon() {
    initializeBrandingResources();
    static const QIcon icon = [] {
        QIcon result;
        for (const int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024})
            result.addFile(QString(":/branding/cmremote-%1.png").arg(size), QSize(size, size));
        return result;
    }();
    return icon;
}

void initialize() {
    initializeBrandingResources();
    QGuiApplication::setApplicationDisplayName("CMRemote");
    QGuiApplication::setDesktopFileName("org.remotemanager.RemoteManager");
    QGuiApplication::setWindowIcon(applicationIcon());
}
}
