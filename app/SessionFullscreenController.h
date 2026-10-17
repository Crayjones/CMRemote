#pragma once
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QList>
class QMainWindow;
class QTabWidget;
class QWidget;
class QTimer;
class QToolButton;
class SessionPane;

// Presentation only: the captured pane never leaves its owning session tab.
class SessionFullscreenController final : public QObject {
    Q_OBJECT
public:
    explicit SessionFullscreenController(QMainWindow* window, QTabWidget* groups);
    ~SessionFullscreenController() override;
    bool active() const { return active_; }
    SessionPane* target() const;
    void enter(SessionPane* pane, QTabWidget* owner, const QString& title);
    void exit();
    void checkSelection(QTabWidget* current);
    bool pinned() const { return pinned_; }
    void setPinned(bool pinned);
    bool barRetracted() const { return active_ && !shown_; }
signals:
    void closeRequested(SessionPane* pane);
    // After exit; the pane is null when it was destroyed.
    void exited(SessionPane* pane);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void suppress(QWidget* widget);
    void unframe(QTabWidget* tabs);
    // The bar floats over the session so the desktop reaches every screen
    // edge; unpinned, it slides above the top edge until the pointer returns.
    void place(bool shown, bool animate);
    void track();
    QMainWindow* window_;
    QTabWidget* groups_;
    QPointer<SessionPane> pane_;
    QPointer<QTabWidget> owner_;
    QPointer<QWidget> focus_, bar_, row_;
    QList<QPointer<QWidget>> chrome_;
    QList<QPointer<QTabWidget>> frames_;
    Qt::WindowStates state_;
    QRect geometry_;
    quint64 generation_ = 0;
    bool active_ = false;
    bool shown_ = true;
    // Qt's window state does not follow AppKit moving a fullscreen window to
    // the Dock and back, so a timer watches the native window instead.
    void watchMinimize();
    bool minimizing_ = false, minimized_ = false;
    int minimizeChecks_ = 0;
    QTimer* minimizer_;
    // Shared by every window so the choice carries across fullscreen sessions.
    static inline bool pinned_ = false;
    QTimer* tracker_;
    QPointer<QToolButton> pin_;
    qint64 awaySince_ = -1;
    QMetaObject::Connection destroyed_;
};
