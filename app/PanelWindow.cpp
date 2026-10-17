#include "PanelWindow.h"
#include "Branding.h"
#include "SessionFullscreenController.h"
#include <QCloseEvent>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

PanelWindow::PanelWindow(int number, QWidget* parent) : QMainWindow(parent) {
    Branding::initialize();
    setObjectName("panelWindow"); setWindowTitle(QString("CMRemote — Window %1").arg(number));
    setWindowIcon(Branding::applicationIcon());
    auto* central = new QWidget(this); auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    groups_ = new QTabWidget(central); groups_->setObjectName("windowGroupTabs"); groups_->setDocumentMode(true); groups_->setMovable(true);
    layout->addWidget(groups_); setCentralWidget(central);
    // The shell's menu bar lives in the main window, so this window offers
    // its own actions from the panel bar corner.
    auto* button = new QToolButton(groups_); button->setObjectName("panelWindowMenuButton"); button->setAutoRaise(true);
    button->setIcon(QIcon(":/original/Panel_16x.png")); button->setToolTip("Window options"); button->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(button); menu->setObjectName("panelWindowMenu");
    auto* panel = menu->addAction("New panel"); panel->setObjectName("actionWindowNewPanel");
    connect(panel, &QAction::triggered, this, [this] { emit newPanelRequested(this); });
    docks_ = menu->addAction("Show Connections and Config here"); docks_->setObjectName("actionWindowDocks"); docks_->setCheckable(true);
    connect(docks_, &QAction::triggered, this, [this](bool here) { emit docksRequested(this, here); });
    button->setMenu(menu); groups_->setCornerWidget(button, Qt::TopRightCorner);
    fullscreen_ = new SessionFullscreenController(this, groups_);
    resize(900, 650);
}
void PanelWindow::setDocksHere(bool here) { docks_->setChecked(here); }
void PanelWindow::closeEvent(QCloseEvent* event) {
    // The shell decides: it confirms, ends this window's sessions and retires it.
    event->ignore(); emit closeRequested(this);
}
void PanelWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) emit activated(this);
}
