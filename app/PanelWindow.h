#pragma once
#include <QMainWindow>

class QTabWidget;
class QAction;
class SessionFullscreenController;

// A browser-style secondary window created by dragging a session tab or a
// panel out of another window. It owns only presentation: a panel bar whose
// panels hold session tabs. The shell owns every session, panel and the
// Connections/Config docks, which it can lend to this window.
class PanelWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit PanelWindow(int number, QWidget* parent = nullptr);
    QTabWidget* groups() const { return groups_; }
    SessionFullscreenController* fullscreen() const { return fullscreen_; }
    void setDocksHere(bool here);
signals:
    void closeRequested(PanelWindow* window);
    void activated(PanelWindow* window);
    void newPanelRequested(PanelWindow* window);
    void docksRequested(PanelWindow* window, bool here);
protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    QTabWidget* groups_;
    QAction* docks_;
    SessionFullscreenController* fullscreen_;
};
