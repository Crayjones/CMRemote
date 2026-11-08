#include "FullscreenWindow.h"
#include "Branding.h"
#include "SessionFullscreenController.h"
#include "Shell.h"
#include "Views.h"
#include "DockManager.h"
#include "DockWidget.h"
#include <QCloseEvent>

FullscreenWindow::FullscreenWindow(QWidget* parent) : QMainWindow(parent) {
    Branding::initialize();
    setObjectName("fullscreenWindow"); setWindowTitle("CMRemote");
    setWindowIcon(Branding::applicationIcon());
    DesktopView::prepareWindow(this);
    configureDocking();
    dockManager_ = new ads::CDockManager(this); dockManager_->setObjectName("fullscreenDockManager");
    fullscreen_ = new SessionFullscreenController(this);
    resize(900, 650);
}
ads::CDockWidget* FullscreenWindow::sessionDock() const {
    for (auto* dock : dockManager_->dockWidgetsMap()) if (isSessionDock(dock)) return dock;
    return nullptr;
}
void FullscreenWindow::closeEvent(QCloseEvent* event) {
    // The shell decides: it ends this window's session and retires it.
    event->ignore(); emit closeRequested(this);
}
