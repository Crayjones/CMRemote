#include "SessionFullscreenController.h"
#include "SessionPane.h"
#include "Views.h"
#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QTimer>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef Q_OS_MACOS
bool rmNativeMiniaturized(QWidget* widget);
void rmNativeFinishMinimize(QWidget* widget);
#endif

namespace {
QIcon pinIcon(const QColor& color) {
    // No bundled pin artwork, so draw a pushpin: head, collar and needle.
    QIcon icon;
    for (const int side : {16, 32}) {
        QPixmap pixmap(side, side); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing); painter.scale(side / 16.0, side / 16.0);
        painter.translate(8, 8); painter.rotate(45); painter.translate(-8, -8);
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawRoundedRect(QRectF(5, 1, 6, 6), 1.5, 1.5);
        painter.drawRect(QRectF(3.5, 7, 9, 1.8));
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap)); painter.drawLine(QPointF(8, 9), QPointF(8, 15));
        icon.addPixmap(pixmap);
    }
    return icon;
}
bool inDock(QWidget* window) {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") return rmNativeMiniaturized(window);
#endif
    return window->isMinimized();
}
constexpr int RevealEdge = 3, RetractDelayMs = 900, InitialRetractDelayMs = 1500;
}

SessionFullscreenController::SessionFullscreenController(QMainWindow* window, QTabWidget* groups)
    : QObject(window), window_(window), groups_(groups), tracker_(new QTimer(this)) {
    setObjectName("sessionFullscreenController"); window_->installEventFilter(this);
    tracker_->setInterval(80); connect(tracker_, &QTimer::timeout, this, &SessionFullscreenController::track);
    minimizer_ = new QTimer(this); minimizer_->setInterval(200);
    connect(minimizer_, &QTimer::timeout, this, &SessionFullscreenController::watchMinimize);
}
void SessionFullscreenController::watchMinimize() {
    if (!active_ || !minimizing_) { minimizer_->stop(); return; }
    if (inDock(window_)) { minimized_ = true; return; }
    if (minimized_) {
        // Back from the Dock: AppKit may or may not have restored fullscreen.
        minimizing_ = minimized_ = false; minimizer_->stop(); ++generation_;
        if (window_->windowState() != Qt::WindowFullScreen) window_->showFullScreen();
        return;
    }
    // AppKit drops a minimize requested while a fullscreen animation runs; ask again.
    if (++minimizeChecks_ % 5 == 0) {
        if (window_->windowState() == Qt::WindowFullScreen) window_->showMinimized();
#ifdef Q_OS_MACOS
        else if (QGuiApplication::platformName() == "cocoa" && window_->isMinimized()) rmNativeFinishMinimize(window_);
#endif
    }
    if (minimizeChecks_ > 50) { minimizing_ = false; minimizer_->stop(); }
}
void SessionFullscreenController::setPinned(bool pinned) {
    pinned_ = pinned;
    if (pin_) {
        const QSignalBlocker block(pin_); pin_->setChecked(pinned);
        pin_->setToolTip(pinned ? "Unpin: hide the bar above the screen edge" : "Pin the bar in place");
    }
    if (!active_) return;
    awaySince_ = pinned ? -1 : QDateTime::currentMSecsSinceEpoch() - RetractDelayMs + InitialRetractDelayMs;
    if (pinned) place(true, true);
}
void SessionFullscreenController::place(bool shown, bool animate) {
    if (!row_) return;
    auto* central = window_->centralWidget(); row_->adjustSize();
    const QPoint target((central->width() - row_->width()) / 2, shown ? 0 : -row_->height());
    shown_ = shown; row_->raise();
    if (auto* running = row_->findChild<QPropertyAnimation*>()) { running->stop(); delete running; }
    if (!animate || row_->pos() == target || !window_->isVisible()) { row_->move(target); return; }
    auto* slide = new QPropertyAnimation(row_, "pos", row_); slide->setDuration(160);
    slide->setEasingCurve(shown ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
    slide->setEndValue(target); slide->start(QAbstractAnimation::DeleteWhenStopped);
}
void SessionFullscreenController::track() {
    if (!active_ || !row_) return;
    const QPoint local = window_->centralWidget()->mapFromGlobal(QCursor::pos());
    const bool atEdge = local.y() <= RevealEdge && local.x() >= 0 && local.x() < window_->centralWidget()->width();
    const QRect reach = QRect(row_->x(), 0, row_->width(), row_->height()).adjusted(-12, 0, 12, 24);
    if (!shown_) { if (atEdge) { awaySince_ = -1; place(true, true); } return; }
    if (pinned_) return;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // Keep the bar while the pointer or an open menu is on it.
    if (reach.contains(local) || QApplication::activePopupWidget()) { awaySince_ = -1; return; }
    if (awaySince_ < 0) awaySince_ = now;
    else if (now - awaySince_ >= RetractDelayMs) { awaySince_ = -1; place(false, true); }
}
SessionFullscreenController::~SessionFullscreenController() { exit(); }
SessionPane* SessionFullscreenController::target() const { return pane_.data(); }
void SessionFullscreenController::suppress(QWidget* widget) {
    if (!widget || chrome_.contains(widget)) return;
    widget->setProperty("requestedVisible", !widget->isHidden()); widget->setProperty("presentationSuppressed", true);
    chrome_.append(widget); widget->hide();
}
void SessionFullscreenController::unframe(QTabWidget* tabs) {
    // The style keeps a pane frame around each tab widget even with its bar
    // hidden, which leaves a light border around the session.
    if (!tabs || frames_.contains(tabs)) return;
    tabs->setProperty("fullscreenStyleSheet", tabs->styleSheet()); frames_.append(tabs);
    tabs->setStyleSheet(tabs->styleSheet() + "\nQTabWidget::pane { border: 0; margin: 0; padding: 0; }");
}
void SessionFullscreenController::enter(SessionPane* pane, QTabWidget* owner, const QString& title) {
    auto* layout = qobject_cast<QVBoxLayout*>(window_->centralWidget()->layout());
    if (!pane || !owner || owner->indexOf(pane) < 0 || !layout) return;
    if (active_ && pane_ == pane) return;
    exit(); pane_ = pane; owner_ = owner; focus_ = QApplication::focusWidget();
    state_ = window_->windowState() & ~Qt::WindowMinimized;
    geometry_ = state_.testFlag(Qt::WindowMaximized) || state_.testFlag(Qt::WindowFullScreen) ? window_->normalGeometry() : window_->geometry();
    active_ = true; ++generation_;
    for (auto* dock : window_->findChildren<QDockWidget*>()) suppress(dock);
    for (auto* toolbar : window_->findChildren<QToolBar*>()) suppress(toolbar);
    // statusBar() would create an empty bar that stays behind after exit.
    if (auto* status = window_->findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly)) suppress(status);
    if (!window_->menuBar()->isNativeMenuBar()) suppress(window_->menuBar());
    suppress(groups_->tabBar()); suppress(owner->tabBar()); unframe(groups_); unframe(owner);
    // An overlay, not a layout row: the session keeps the whole screen.
    auto* row = new QWidget(window_->centralWidget()); row_ = row; row->setObjectName("fullscreenControlsRow");
    auto* centered = new QHBoxLayout(row); centered->setContentsMargins(0, 0, 0, 0);
    auto* bar = new QFrame(row); bar_ = bar; bar->setObjectName("fullscreenBar"); bar->setFrameShape(QFrame::StyledPanel);
    bar->setAutoFillBackground(true); bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); bar->setMaximumWidth(500);
    auto* controls = new QHBoxLayout(bar); controls->setContentsMargins(8, 3, 8, 3); controls->setSpacing(4);
    auto* pin = new QToolButton(bar); pin_ = pin; pin->setObjectName("pinFullscreenBar"); pin->setCheckable(true);
    pin->setIcon(pinIcon(bar->palette().color(QPalette::ButtonText))); pin->setAccessibleName("Pin bar");
    pin->setFixedSize(28, 26); controls->addWidget(pin);
    auto* label = new QLabel(title, bar); label->setObjectName("fullscreenTitle"); label->setTextFormat(Qt::PlainText);
    label->setToolTip(title); label->setFixedWidth(240); controls->addWidget(label, 1);
    const auto button = [&](const char* name, const QString& text, QStyle::StandardPixmap icon) {
        auto* control = new QToolButton(bar); control->setObjectName(name); control->setIcon(bar->style()->standardIcon(icon));
        control->setAccessibleName(text); control->setToolTip(text); control->setFixedSize(28, 26); controls->addWidget(control); return control;
    };
    auto* minimize = button("minimizeFullscreenConnection", "Minimize", QStyle::SP_TitleBarMinButton);
    auto* leave = button("exitSessionFullscreen", "Exit fullscreen", QStyle::SP_TitleBarNormalButton);
    auto* close = button("closeFullscreenConnection", "Close connection", QStyle::SP_TitleBarCloseButton);
    centered->addWidget(bar); row->show(); window_->centralWidget()->installEventFilter(this);
    connect(pin, &QToolButton::toggled, this, &SessionFullscreenController::setPinned);
    setPinned(pinned_); place(true, false); tracker_->start();
    connect(minimize, &QToolButton::clicked, this, [this] {
        if (!active_) return;
        ++generation_; minimizing_ = true; minimized_ = false; minimizeChecks_ = 0; window_->showMinimized(); minimizer_->start();
    });
    connect(leave, &QToolButton::clicked, this, &SessionFullscreenController::exit);
    connect(close, &QToolButton::clicked, this, [this, target = QPointer<SessionPane>(pane)] {
        if (!active_ || pane_ != target) return;
        exit(); if (target) emit closeRequested(target);
    });
    destroyed_ = connect(pane, &QObject::destroyed, this, [this] { pane_.clear(); exit(); });
    window_->showFullScreen();
    if (auto* desktop = pane->findChild<DesktopView*>()) desktop->setFocus(); else pane->setFocus();
}
void SessionFullscreenController::exit() {
    if (!active_) return;
    active_ = false; ++generation_; disconnect(destroyed_); tracker_->stop(); awaySince_ = -1; shown_ = true;
    minimizing_ = minimized_ = false; minimizer_->stop(); const QPointer<SessionPane> left = pane_;
    window_->centralWidget()->removeEventFilter(this);
    if (row_) {
        for (auto* child : row_->findChildren<QObject*>()) { child->disconnect(this); child->setObjectName({}); }
        row_->setObjectName({}); row_->hide(); row_->deleteLater();
    }
    row_.clear(); bar_.clear(); pin_.clear();
    for (const auto& widget : chrome_) if (widget) {
        const bool visible = widget->property("requestedVisible").toBool();
        widget->setProperty("presentationSuppressed", false); widget->setVisible(visible);
    }
    chrome_.clear();
    for (const auto& tabs : frames_) if (tabs) tabs->setStyleSheet(tabs->property("fullscreenStyleSheet").toString());
    frames_.clear();
    if (state_.testFlag(Qt::WindowFullScreen)) window_->showFullScreen();
    else if (state_.testFlag(Qt::WindowMaximized)) window_->showMaximized();
    else { window_->showNormal(); window_->setGeometry(geometry_); }
    if (focus_ && focus_->isVisible()) focus_->setFocus();
    pane_.clear(); owner_.clear(); focus_.clear();
    emit exited(left);
}
void SessionFullscreenController::checkSelection(QTabWidget* current) {
    if (active_ && (!pane_ || !owner_ || current != owner_ || owner_->currentWidget() != pane_)) exit();
}
bool SessionFullscreenController::eventFilter(QObject* watched, QEvent* event) {
    if (active_ && watched == window_->centralWidget() && event->type() == QEvent::Resize) place(shown_, false);
    // Leaving fullscreen on the way to the Dock and back publishes intermediate states.
    if (watched == window_ && active_ && event->type() == QEvent::WindowStateChange && minimizing_) {
        if (inDock(window_)) minimized_ = true;
        QMetaObject::invokeMethod(this, &SessionFullscreenController::watchMinimize, Qt::QueuedConnection);
        return QObject::eventFilter(watched, event);
    }
    if (watched == window_ && active_ && event->type() == QEvent::WindowStateChange) {
        // Modal activation and native transitions can publish intermediate states.
        QMetaObject::invokeMethod(this, [this, generation = generation_] {
            if (active_ && generation_ == generation && !window_->isFullScreen() && !window_->isMinimized()) exit();
        }, Qt::QueuedConnection);
    }
    return QObject::eventFilter(watched, event);
}
