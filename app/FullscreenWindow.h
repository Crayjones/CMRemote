#pragma once
#include <QMainWindow>

class SessionFullscreenController;
namespace ads { class CDockManager; class CDockWidget; }

// A session shown fullscreen: its tab's dock moves into this window's own dock
// manager and returns to its panel on exit. The shell owns the session.
class FullscreenWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit FullscreenWindow(QWidget* parent = nullptr);
    ads::CDockManager* dockManager() const { return dockManager_; }
    SessionFullscreenController* fullscreen() const { return fullscreen_; }
    // The session dock this window shows, if it still has one.
    ads::CDockWidget* sessionDock() const;
signals:
    void closeRequested(FullscreenWindow* window);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    ads::CDockManager* dockManager_;
    SessionFullscreenController* fullscreen_;
};
