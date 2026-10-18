#include "Views.h"
#include "TerminalScreen.h"
#include "SessionClipboard.h"
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr qsizetype pasteBytes = 1024 * 1024;
VTermModifier modifiers(Qt::KeyboardModifiers value) {
    int flags = 0;
    if (value & Qt::ShiftModifier) flags |= VTERM_MOD_SHIFT;
    if (value & Qt::AltModifier) flags |= VTERM_MOD_ALT;
    if (value & Qt::ControlModifier) flags |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(flags);
}
VTermKey terminalKey(int key) {
    switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: return VTERM_KEY_ENTER;
    case Qt::Key_Tab: case Qt::Key_Backtab: return VTERM_KEY_TAB;
    case Qt::Key_Backspace: return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape: return VTERM_KEY_ESCAPE;
    case Qt::Key_Up: return VTERM_KEY_UP; case Qt::Key_Down: return VTERM_KEY_DOWN;
    case Qt::Key_Left: return VTERM_KEY_LEFT; case Qt::Key_Right: return VTERM_KEY_RIGHT;
    case Qt::Key_Insert: return VTERM_KEY_INS; case Qt::Key_Delete: return VTERM_KEY_DEL;
    case Qt::Key_Home: return VTERM_KEY_HOME; case Qt::Key_End: return VTERM_KEY_END;
    case Qt::Key_PageUp: return VTERM_KEY_PAGEUP; case Qt::Key_PageDown: return VTERM_KEY_PAGEDOWN;
    default: return key >= Qt::Key_F1 && key <= Qt::Key_F35 ? static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1)) : VTERM_KEY_NONE;
    }
}
Qt::KeyboardModifiers terminalModifiers(Qt::KeyboardModifiers flags) {
#ifdef Q_OS_MACOS
    // Qt swaps physical Control and Command on macOS.
    return (flags & (Qt::ShiftModifier | Qt::AltModifier)) | ((flags & Qt::MetaModifier) ? Qt::ControlModifier : Qt::NoModifier);
#else
    return flags & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
#endif
}
int controlByte(int key) {
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return key - Qt::Key_A + 1;
    switch (key) {
    case Qt::Key_Space: case Qt::Key_At: case Qt::Key_2: return 0;
    case Qt::Key_BracketLeft: case Qt::Key_3: return 27;
    case Qt::Key_Backslash: case Qt::Key_4: return 28;
    case Qt::Key_BracketRight: case Qt::Key_5: return 29;
    case Qt::Key_AsciiCircum: case Qt::Key_6: return 30;
    case Qt::Key_Underscore: case Qt::Key_Minus: case Qt::Key_7: return 31;
    case Qt::Key_Question: case Qt::Key_8: return 127;
    default: return -1;
    }
}
bool commandModifier(Qt::KeyboardModifiers flags) {
#ifdef Q_OS_MACOS
    return flags & Qt::ControlModifier;
#else
    return flags & Qt::MetaModifier;
#endif
}
VTermKey commandNavigation(const QKeyEvent& event, bool alternate) {
#ifdef Q_OS_MACOS
    if (alternate && event.modifiers() == Qt::ControlModifier && (event.key() == Qt::Key_Home || event.key() == Qt::Key_End)) return terminalKey(event.key());
#else
    Q_UNUSED(event); Q_UNUSED(alternate);
#endif
    return VTERM_KEY_NONE;
}
}
TerminalView::TerminalView(QWidget* parent) : QWidget(parent) {
    setObjectName("terminalView"); setFocusPolicy(Qt::StrongFocus);
    blinkTimer_ = new QTimer(this); blinkTimer_->setObjectName("terminalCursorBlink"); blinkTimer_->setInterval(500);
    connect(blinkTimer_, &QTimer::timeout, this, [this] { cursorOn_ = !cursorOn_; update(); });
    setAppearance(appearance_);
}
bool TerminalView::setAppearance(const TerminalAppearance& appearance) {
    if (!appearance.valid()) return false;
    baseAppearance_ = appearance; applyAppearance(); return true;
}
void TerminalView::applyAppearance() {
    appearance_ = baseAppearance_; appearance_.fontSize = std::clamp(baseAppearance_.fontSize + zoomOffset_, 6, 48);
    zoomOffset_ = appearance_.fontSize - baseAppearance_.fontSize;
    auto face = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!appearance_.fontFamily.isEmpty() && QFontDatabase::isFixedPitch(appearance_.fontFamily)) face.setFamily(appearance_.fontFamily);
    face.setPointSize(appearance_.fontSize); setFont(face);
    if (screen_) { screen_->setAppearance(appearance_); updateCells(); }
    updateBlink(); update();
}
void TerminalView::updateBlink() {
    cursorOn_ = true;
    if (blinkTimer_) {
        if (appearance_.blink && screen_ && hasFocus() && isVisible() && screen_->cursorVisible()) blinkTimer_->start();
        else blinkTimer_->stop();
    }
}
bool TerminalView::event(QEvent* event) {
    const auto type = event->type();
    if (type == QEvent::FocusOut || type == QEvent::WindowDeactivate || type == QEvent::Hide || type == QEvent::UngrabMouse) stopDrag();
    if (screen_ && type == QEvent::ShortcutOverride) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (localShortcut(*key) != Shortcut::None || (inputEnabled_ && (!commandModifier(key->modifiers()) || commandNavigation(*key, screen_->alternate()) != VTERM_KEY_NONE))) { event->accept(); return true; }
    }
    if (screen_ && type == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) { keyPressEvent(key); return true; }
    }
    const bool handled = QWidget::event(event);
    if (type == QEvent::FocusIn || type == QEvent::FocusOut || type == QEvent::Show || type == QEvent::Hide) { updateBlink(); update(); }
    return handled;
}
void TerminalView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event); if (event->type() == QEvent::FontChange && screen_) updateCells();
}
void TerminalView::report(const QString& text) { report_ = text; update(); }
void TerminalView::initializeTerminal(std::shared_ptr<ClipboardEndpoint> clipboard) {
    if (screen_) return;
    endpoint_ = clipboard ? std::move(clipboard) : systemClipboardEndpoint();
    screen_ = new TerminalScreen(24, 80, this); screen_->setAppearance(appearance_); scroll_ = new QScrollBar(Qt::Vertical, this);
    pasteTimer_ = new QTimer(this); pasteTimer_->setInterval(5);
    connect(pasteTimer_, &QTimer::timeout, this, &TerminalView::pumpPaste);
    connect(screen_, &TerminalScreen::changed, this, &TerminalView::refreshScreen);
    connect(screen_, &TerminalScreen::appearanceChanged, this, qOverload<>(&TerminalView::update));
    connect(screen_, &TerminalScreen::outgoing, this, [this](const QByteArray& bytes) { if (inputEnabled_) send(bytes); });
    connect(screen_, &TerminalScreen::encodingFailed, this, [this] { emit inputError("Terminal output encoding exceeds supported limits."); });
    selectionTimer_ = new QTimer(this); selectionTimer_->setObjectName("terminalSelectionScroll"); selectionTimer_->setInterval(50);
    connect(selectionTimer_, &QTimer::timeout, this, [this] {
        const int step = dragScrollStep();
        if (!step) { selectionTimer_->stop(); return; }
        scroll_->setValue(scroll_->value() + step);
    });
    connect(scroll_, &QScrollBar::valueChanged, this, [this] { if (dragging_) updateDrag(); update(); });
    setAttribute(Qt::WA_InputMethodEnabled, true); setMouseTracking(true); updateCells(); refreshScreen(); updateBlink();
}
bool TerminalView::append(const QByteArray& bytes) { return screen_ && screen_->feed(bytes); }
void TerminalView::setInputEnabled(bool enabled) {
    inputEnabled_ = enabled;
    if (!enabled && pasteTimer_) { pasteTimer_->stop(); pendingPaste_.clear(); pasteOffset_ = 0; }
    update();
}
void TerminalView::refreshScreen() {
    const bool follow = scroll_->value() == scroll_->maximum() && !dragging_;
    const quint64 top = lineOrigin_ + quint64(scroll_->value()), origin = lineOrigin();
    const int history = screen_->historyLines();
    const quint64 end = origin + quint64(history + screen_->cells().height());
    const bool epochChanged = selectionEpoch_ != screen_->selectionEpoch();
    // Retained history is immutable. Live-grid edits cannot safely keep a cell anchor.
    if (epochChanged || (selection_ && (selectionStart_.line < origin || selectionEnd_.line < origin ||
        selectionStart_.line >= end || selectionEnd_.line >= end ||
        (contentRevision_ != screen_->contentRevision() && (selectionStart_.line >= historyEnd_ || selectionEnd_.line >= historyEnd_))))) {
        selection_ = false; stopDrag();
    }
    lineOrigin_ = origin; historyEnd_ = origin + quint64(history);
    contentRevision_ = screen_->contentRevision(); selectionEpoch_ = screen_->selectionEpoch();
    {
        const QSignalBlocker blocker(scroll_);
        scroll_->setRange(0, history); scroll_->setPageStep(screen_->cells().height());
        scroll_->setValue(follow || screen_->alternate() ? history : int(std::clamp(top, origin, historyEnd_) - origin));
    }
    if (dragging_) updateDrag(); updateBlink(); update();
}
void TerminalView::updateCells() {
    if (!screen_) return;
    const QFontMetricsF metrics(font()); const QSize cell(std::max(1, int(std::ceil(metrics.horizontalAdvance('M')))), std::max(1, int(std::ceil(metrics.height()))));
    if (cell != cellSize_) stopDrag(); cellSize_ = cell;
    const int scrollbarWidth = scroll_->sizeHint().width(); scroll_->setGeometry(width() - scrollbarWidth, 0, scrollbarWidth, height());
    const int rows = std::clamp(height() / cellSize_.height(), 1, 256), columns = std::clamp((width() - scrollbarWidth) / cellSize_.width(), 1, 512);
    const auto old = screen_->cells(); screen_->resizeCells(rows, columns);
    if (old != screen_->cells()) emit cellsChanged(quint16(rows), quint16(columns));
}
void TerminalView::resizeEvent(QResizeEvent* event) {
    stopDrag(); QWidget::resizeEvent(event); if (screen_) updateCells(); else emit viewportChanged(event->size());
}
void TerminalView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (!screen_) {
        painter.fillRect(rect(), palette().brush(QPalette::Base)); painter.setPen(palette().color(QPalette::Text)); painter.setFont(font());
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignLeft | Qt::TextWordWrap, "MOCK TERMINAL SESSION\nSSH and VT emulation are not connected.\n\n" + report_); return;
    }
    painter.fillRect(rect(), appearance_.background); const auto size = screen_->cells(); const int first = scroll_->value(); const QFontMetricsF metrics(font());
    for (int row = 0; row < size.height(); ++row) {
        for (int column = 0; column < size.width(); ++column) {
            const auto cell = screen_->lineCell(first + row, column); if (!cell.width) continue;
            const QRect area(column * cellSize_.width(), row * cellSize_.height(), std::max(1, cell.width) * cellSize_.width(), cellSize_.height());
            const bool highlight = selected(first + row, column) || (cell.width == 2 && selected(first + row, column + 1));
            painter.fillRect(area, highlight ? appearance_.selectionBackground : cell.background);
            painter.setPen(highlight ? appearance_.selectionForeground : cell.foreground);
            auto face = font(); face.setBold(cell.bold); face.setItalic(cell.italic); face.setUnderline(cell.underline); face.setStrikeOut(cell.strike); painter.setFont(face);
            if (!cell.conceal) { painter.save(); painter.setClipRect(area); painter.drawText(QPointF(area.left(), area.top() + metrics.ascent()), cell.text); painter.restore(); }
        }
    }
    if (screen_->cursorVisible() && cursorOn_ && first == screen_->historyLines()) {
        const auto cursor = screen_->cursor(); const auto cell = screen_->cell(cursor.y(), cursor.x());
        const QRect area(cursor.x() * cellSize_.width(), cursor.y() * cellSize_.height(), std::max(1, cell.width) * cellSize_.width(), cellSize_.height());
        painter.setPen(appearance_.cursorColor); painter.setBrush(Qt::NoBrush);
        if (!hasFocus()) painter.drawRect(area.adjusted(0, 0, -1, -1));
        else if (appearance_.cursor == TerminalAppearance::Cursor::Underline) painter.fillRect(QRect(area.left(), area.bottom() - 1, area.width(), 2), appearance_.cursorColor);
        else if (appearance_.cursor == TerminalAppearance::Cursor::Bar) painter.fillRect(QRect(area.left(), area.top(), 2, area.height()), appearance_.cursorColor);
        else {
            painter.fillRect(area, appearance_.cursorColor); painter.setPen(appearance_.background);
            auto face = font(); face.setBold(cell.bold); face.setItalic(cell.italic); painter.setFont(face);
            if (!cell.conceal) { painter.setClipRect(area); painter.drawText(QPointF(area.left(), area.top() + metrics.ascent()), cell.text); }
        }
    }
}
bool TerminalView::send(const QByteArray& bytes) {
    if (!inputEnabled_ || !inputSink_ || bytes.isEmpty()) return false;
    if (!pendingPaste_.isEmpty() || bytes.size() > 32768 || !inputSink_(bytes)) { emit inputError("SSH input was not accepted. The input queue is full or the session stopped."); return false; }
    return true;
}
TerminalView::Shortcut TerminalView::localShortcut(const QKeyEvent& event) const {
    if (!screen_ || !sessionShortcuts_) return Shortcut::None;
    const auto flags = event.modifiers() & ~Qt::KeypadModifier; const auto key = event.key();
#ifdef Q_OS_MACOS
    const auto clipboard = Qt::KeyboardModifiers(Qt::ControlModifier);
    const auto edge = Qt::KeyboardModifiers(Qt::ControlModifier);
#else
    const auto clipboard = Qt::ControlModifier | Qt::ShiftModifier;
    const auto edge = Qt::ControlModifier | Qt::ShiftModifier;
    if (flags == Qt::ControlModifier && key == Qt::Key_Insert) return Shortcut::Copy;
#endif
    if (flags == clipboard) {
        if (key == Qt::Key_C) return Shortcut::Copy;
        if (key == Qt::Key_V) return Shortcut::Paste;
        if (key == Qt::Key_A) return Shortcut::SelectAll;
    }
#ifdef Q_OS_MACOS
    if (flags == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_C) return Shortcut::Copy;
        if (key == Qt::Key_V) return Shortcut::Paste;
    }
#endif
    if (flags == Qt::ShiftModifier && key == Qt::Key_Insert) return Shortcut::Paste;
    if (!screen_->alternate()) {
        if (flags == Qt::ShiftModifier && key == Qt::Key_PageUp) return Shortcut::PageUp;
        if (flags == Qt::ShiftModifier && key == Qt::Key_PageDown) return Shortcut::PageDown;
        if (flags == edge && key == Qt::Key_Home) return Shortcut::First;
        if (flags == edge && key == Qt::Key_End) return Shortcut::Last;
    }
    if (flags == Qt::ControlModifier || flags == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (key == Qt::Key_Plus || key == Qt::Key_Equal) return Shortcut::ZoomIn;
        if (key == Qt::Key_Minus) return Shortcut::ZoomOut;
        if (key == Qt::Key_0 && flags == Qt::ControlModifier) return Shortcut::ZoomReset;
    }
    return Shortcut::None;
}
void TerminalView::runShortcut(Shortcut shortcut) {
    switch (shortcut) {
    case Shortcut::Copy: copySelection(); break;
    case Shortcut::Paste: pasteClipboard(); break;
    case Shortcut::SelectAll: selectCells(QPoint(0, 0), QPoint(screen_->cells().width() - 1, screen_->historyLines() + screen_->cells().height() - 1)); break;
    case Shortcut::PageUp: scroll_->setValue(scroll_->value() - scroll_->pageStep()); break;
    case Shortcut::PageDown: scroll_->setValue(scroll_->value() + scroll_->pageStep()); break;
    case Shortcut::First: scroll_->setValue(scroll_->minimum()); break;
    case Shortcut::Last: scroll_->setValue(scroll_->maximum()); break;
    case Shortcut::ZoomIn: case Shortcut::ZoomOut: case Shortcut::ZoomReset: {
        const int size = shortcut == Shortcut::ZoomReset ? baseAppearance_.fontSize : std::clamp(appearance_.fontSize + (shortcut == Shortcut::ZoomIn ? 1 : -1), 6, 48);
        if (size != appearance_.fontSize) { zoomOffset_ = size - baseAppearance_.fontSize; applyAppearance(); }
        break;
    }
    case Shortcut::None: break;
    }
}
void TerminalView::keyPressEvent(QKeyEvent* event) {
    if (!screen_) { emit mockInput(QString("Key %1").arg(event->key())); event->accept(); return; }
    const auto shortcut = localShortcut(*event);
    if (shortcut != Shortcut::None) { runShortcut(shortcut); event->accept(); return; }
    const auto flags = terminalModifiers(event->modifiers());
    if (commandModifier(event->modifiers())) {
        // The local scrollback keys still navigate full-screen remote applications.
        const auto key = commandNavigation(*event, screen_->alternate());
        if (inputEnabled_ && key != VTERM_KEY_NONE) {
            send(screen_->encodeKey(key, VTermModifier(VTERM_MOD_CTRL | VTERM_MOD_SHIFT))); event->accept(); return;
        }
        event->ignore(); return;
    }
    if (inputEnabled_) {
        const auto key = terminalKey(event->key()); const int control = (flags & Qt::ControlModifier) ? controlByte(event->key()) : -1;
        if (control >= 0) {
            QByteArray bytes(1, char(control)); if (flags & Qt::AltModifier) bytes.prepend('\x1b'); send(bytes);
        } else if (key != VTERM_KEY_NONE) {
            auto modifier = modifiers(flags); if (event->key() == Qt::Key_Backtab) modifier = VTermModifier(modifier | VTERM_MOD_SHIFT);
            send(screen_->encodeKey(key, modifier));
        } else {
            auto text = event->text();
            // Meta letter keys may contain a composed character, ESC, or no native text.
            if (flags & Qt::AltModifier) {
                if (event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) text = QString(QChar((flags & Qt::ShiftModifier ? 'A' : 'a') + event->key() - Qt::Key_A));
                else if (event->key() >= Qt::Key_Space && event->key() <= Qt::Key_AsciiTilde) text = QString(QChar(event->key()));
            }
            send(screen_->encodeText(text, modifiers(flags)));
        }
    }
    event->accept();
}
void TerminalView::inputMethodEvent(QInputMethodEvent* event) {
    if (screen_ && inputEnabled_ && !event->commitString().isEmpty()) {
        const auto bytes = screen_->encodeText(event->commitString());
        if (bytes.isEmpty()) emit inputError("Terminal input exceeds 4096 UTF-8 bytes."); else send(bytes);
    }
    event->accept();
}
QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (query == Qt::ImEnabled) return screen_ && inputEnabled_;
    if (query == Qt::ImCursorRectangle && screen_) { const auto cursor = screen_->cursor(); return QRect(cursor.x() * cellSize_.width(), (cursor.y() + screen_->historyLines() - scroll_->value()) * cellSize_.height(), cellSize_.width(), cellSize_.height()); }
    return QWidget::inputMethodQuery(query);
}
QPoint TerminalView::cellAt(QPoint position) const {
    return QPoint(std::clamp(position.x() / cellSize_.width(), 0, screen_->cells().width() - 1), scroll_->value() + std::clamp(position.y() / cellSize_.height(), 0, screen_->cells().height() - 1));
}
quint64 TerminalView::lineOrigin() const { return screen_->alternate() ? 0 : screen_->firstLine(); }
TerminalView::SelectionCell TerminalView::selectionCell(QPoint cell) const {
    const int last = screen_->historyLines() + screen_->cells().height() - 1;
    return {std::clamp(cell.x(), 0, screen_->cells().width() - 1), lineOrigin() + quint64(std::clamp(cell.y(), 0, last))};
}
void TerminalView::selectCells(QPoint start, QPoint end) {
    if (!screen_) return;
    stopDrag(); selectionStart_ = selectionCell(start); selectionEnd_ = selectionCell(end); selection_ = true; update();
}
bool TerminalView::selected(int line, int column) const {
    if (!selection_) return false;
    auto first = selectionStart_, last = selectionEnd_; if (last < first) std::swap(first, last);
    const SelectionCell cell{column, lineOrigin() + quint64(line)}; return !(cell < first) && !(last < cell);
}
QString TerminalView::selectionText() const {
    if (!screen_ || !selection_) return {};
    auto first = selectionStart_, last = selectionEnd_; if (last < first) std::swap(first, last);
    const int firstRow = int(first.line - lineOrigin()), lastRow = int(last.line - lineOrigin());
    QString text;
    for (int line = firstRow; line <= lastRow; ++line) {
        QString part; const int begin = line == firstRow ? first.column : 0, end = line == lastRow ? last.column : screen_->cells().width() - 1;
        for (int column = begin; column <= end; ++column) {
            const auto cell = screen_->lineCell(line, column);
            if (cell.width == 0 && column == begin && column > 0) part += screen_->lineCell(line, column - 1).text;
            else part += cell.text;
        }
        while (part.endsWith(' ')) part.chop(1); text += part; if (line != lastRow) text += '\n';
    }
    return text;
}
bool TerminalView::copySelection() { if (!screen_ || !selection_ || !endpoint_) return false; endpoint_->writeText(selectionText()); return true; }
bool TerminalView::pasteClipboard() {
    if (!screen_ || !inputEnabled_ || !inputSink_ || !endpoint_ || !pendingPaste_.isEmpty()) return false;
    const auto text = endpoint_->readText(); if (!text || text->isEmpty()) return false;
    if (text->size() > pasteBytes) { emit inputError("Terminal paste exceeds 1 MiB UTF-8."); return false; }
    const auto bytes = text->toUtf8(); if (bytes.size() > pasteBytes) { emit inputError("Terminal paste exceeds 1 MiB UTF-8."); return false; }
    pendingPaste_ = screen_->pasteStart() + bytes + screen_->pasteEnd(); pasteOffset_ = 0; pumpPaste(); return true;
}
void TerminalView::pumpPaste() {
    if (!inputEnabled_ || !inputSink_) { pendingPaste_.clear(); pasteOffset_ = 0; pasteTimer_->stop(); return; }
    // At most 128 KiB per GUI turn; overload retains the entire unaccepted suffix.
    int sent = 0;
    while (pasteOffset_ < pendingPaste_.size() && sent < 128 * 1024) {
        const auto packet = pendingPaste_.mid(pasteOffset_, 32768); if (!inputSink_(packet)) break;
        pasteOffset_ += int(packet.size()); sent += int(packet.size());
    }
    if (pasteOffset_ == pendingPaste_.size()) { pendingPaste_.clear(); pasteOffset_ = 0; pasteTimer_->stop(); }
    else pasteTimer_->start();
}
void TerminalView::stopDrag() {
    dragging_ = false; if (selectionTimer_) selectionTimer_->stop(); pixelRemainder_ = 0; angleRemainder_ = 0;
}
int TerminalView::dragScrollStep() const {
    if (!dragging_ || !selection_ || !screen_ || screen_->alternate()) return 0;
    const int height = cellSize_.height(), bottom = screen_->cells().height() * height;
    if (dragPosition_.x() < 0 || dragPosition_.x() >= screen_->cells().width() * cellSize_.width()) return 0;
    if (dragPosition_.y() < height && scroll_->value() > scroll_->minimum())
        return -int(std::clamp<qint64>(1 + std::max<qint64>(0, -qint64(dragPosition_.y())) / height, 1, 8));
    if (dragPosition_.y() >= bottom - height && scroll_->value() < scroll_->maximum())
        return int(std::clamp<qint64>(1 + std::max<qint64>(0, qint64(dragPosition_.y()) - bottom) / height, 1, 8));
    return 0;
}
void TerminalView::updateDrag() {
    if (!dragging_ || !selection_) return;
    selectionEnd_ = selectionCell(cellAt(dragPosition_));
    if (dragScrollStep()) { if (!selectionTimer_->isActive()) selectionTimer_->start(); }
    else selectionTimer_->stop();
    update();
}
void TerminalView::mousePressEvent(QMouseEvent* event) {
    if (screen_ && event->button() == Qt::LeftButton) {
        setFocus(); dragPosition_ = event->position().toPoint(); const auto cell = cellAt(dragPosition_);
        selectCells(cell, cell); dragging_ = true; event->accept();
    } else QWidget::mousePressEvent(event);
}
void TerminalView::mouseMoveEvent(QMouseEvent* event) {
    if (screen_ && dragging_) {
        if (!(event->buttons() & Qt::LeftButton)) stopDrag();
        else { dragPosition_ = event->position().toPoint(); updateDrag(); }
        event->accept();
    } else QWidget::mouseMoveEvent(event);
}
void TerminalView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragPosition_ = event->position().toPoint(); updateDrag(); stopDrag(); event->accept();
    } else QWidget::mouseReleaseEvent(event);
}
void TerminalView::wheelEvent(QWheelEvent* event) {
    if (!screen_) { QWidget::wheelEvent(event); return; }
    if (event->phase() == Qt::ScrollBegin) { pixelRemainder_ = 0; angleRemainder_ = 0; }
    if (dragging_) dragPosition_ = event->position().toPoint();
    const bool pixel = !event->pixelDelta().isNull();
    const int vertical = pixel ? event->pixelDelta().y() : event->angleDelta().y();
    qint64 rows = 0;
    if ((vertical > 0 && scroll_->value() == scroll_->minimum()) || (vertical < 0 && scroll_->value() == scroll_->maximum())) {
        pixelRemainder_ = 0; angleRemainder_ = 0;
    } else if (pixel) {
        pixelRemainder_ += vertical; rows = pixelRemainder_ / cellSize_.height(); pixelRemainder_ %= cellSize_.height(); angleRemainder_ = 0;
    } else {
        angleRemainder_ += qint64(vertical) * 3; rows = angleRemainder_ / 120; angleRemainder_ %= 120; pixelRemainder_ = 0;
    }
    scroll_->setValue(int(std::clamp<qint64>(qint64(scroll_->value()) - rows, scroll_->minimum(), scroll_->maximum())));
    if (dragging_) updateDrag();
    if (event->phase() == Qt::ScrollEnd) { pixelRemainder_ = 0; angleRemainder_ = 0; }
    event->accept();
}
void TerminalView::contextMenuEvent(QContextMenuEvent* event) {
    if (!screen_) return;
    auto* menu = new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
#ifdef Q_OS_MACOS
    const auto clipboard = Qt::KeyboardModifiers(Qt::ControlModifier);
#else
    const auto clipboard = Qt::ControlModifier | Qt::ShiftModifier;
#endif
    const auto action = [this, menu](const QString& text, int key, Qt::KeyboardModifiers flags, Shortcut shortcut, bool enabled) {
        auto* item = menu->addAction(text); item->setShortcut(QKeySequence(int(flags) | key)); item->setEnabled(enabled);
        connect(item, &QAction::triggered, this, [this, shortcut] { runShortcut(shortcut); }); return item;
    };
    action("Copy", Qt::Key_C, clipboard, Shortcut::Copy, selection_);
    action("Paste", Qt::Key_V, clipboard, Shortcut::Paste, inputEnabled_ && pendingPaste_.isEmpty());
    action("Select All", Qt::Key_A, clipboard, Shortcut::SelectAll, sessionShortcuts_);
    bool extras = false;
    for (const auto& extra : contextActions_) if (extra && extra->isVisible()) { if (!extras) menu->addSeparator(); extras = true; menu->addAction(extra); }
    if (sessionShortcuts_) {
        menu->addSeparator();
        action("Scroll Page Up", Qt::Key_PageUp, Qt::ShiftModifier, Shortcut::PageUp, !screen_->alternate());
        action("Scroll Page Down", Qt::Key_PageDown, Qt::ShiftModifier, Shortcut::PageDown, !screen_->alternate());
        action("Scroll to Start", Qt::Key_Home, clipboard, Shortcut::First, !screen_->alternate());
        action("Scroll to End", Qt::Key_End, clipboard, Shortcut::Last, !screen_->alternate());
        menu->addSeparator();
        action("Increase Font Size", Qt::Key_Plus, Qt::ControlModifier, Shortcut::ZoomIn, appearance_.fontSize < 48);
        action("Decrease Font Size", Qt::Key_Minus, Qt::ControlModifier, Shortcut::ZoomOut, appearance_.fontSize > 6);
        action("Reset Session Font Size", Qt::Key_0, Qt::ControlModifier, Shortcut::ZoomReset, zoomOffset_ != 0);
    }
    menu->addSeparator(); auto* appearance = menu->addAction("Terminal Settings…"); appearance->setObjectName("actionTerminalAppearance");
    connect(appearance, &QAction::triggered, this, &TerminalView::appearanceRequested);
    menu->popup(event->globalPos());
}
