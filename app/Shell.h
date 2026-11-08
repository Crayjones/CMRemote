#pragma once
#include "ui_test.h"
#include "ui_ConnectionsPane.h"
#include "ui_ConfigPane.h"
#include "ui_NotificationsPane.h"
namespace ads { class CDockManager; class CDockWidget; class CDockAreaWidget; }

// The main window's Designer forms and its docking layout, shared by the
// application and the standalone preview. test.ui holds the menus, toolbars,
// actions and the empty session area; each dock pane has its own form.
//
// Everything in the window is a Qt Advanced Docking System dock: the panes,
// and every session tab. A panel is a dock area of session tabs; dragging a
// tab splits a panel or floats it as its own window, like Visual Studio's
// documents.
struct ShellUi : Ui::MainWindow, Ui::ConnectionsPane, Ui::ConfigPane, Ui::NotificationsPane {
    ads::CDockManager* dockManager = nullptr;
    // Shown, without a tab, only while no session is open in the window.
    ads::CDockWidget* documentsDock = nullptr;
    ads::CDockWidget* connectionsDock = nullptr;
    ads::CDockWidget* configDock = nullptr;
    ads::CDockWidget* notificationsDock = nullptr;
    void setupUi(QMainWindow* window);
    // Connections above Config on the left, Notifications closed along the bottom.
    void resetDocks();
    // The General panel: where sessions open when no other panel was used.
    ads::CDockAreaWidget* generalPanel() const;
};

// Global dock behavior. Applies to every dock manager created afterwards.
void configureDocking();
// A session tab's dock: closing it asks the shell, which ends the session.
ads::CDockWidget* createSessionDock(ads::CDockManager* manager, QWidget* contents, const QString& name);
bool isSessionDock(const ads::CDockWidget* dock);
// Marks dock areas holding session tabs (sessionPanel="true") for theme.qss,
// and lets them, not the side panes, take or give up space as panels come and go.
void markSessionPanels(ads::CDockManager* manager);
// Shows or hides General's tabless placeholder. When it hides, General's room
// goes to the session panels beside it rather than to the side panes.
void showGeneralPlaceholder(ads::CDockWidget* placeholder, bool show);
// Moves a session's dock into a new panel on one side of its panel, taking half of it.
void splitPanel(ads::CDockManager* manager, ads::CDockWidget* dock, ads::CDockAreaWidget* panel, int side);
