#include "VncDesktopView.h"
#include <QCoreApplication>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace {
quint8 buttonMask(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 4;
    default: return 0;
    }
}
bool rightModifier(const QKeyEvent& event) {
    const auto key = event.nativeVirtualKey(), scan = event.nativeScanCode();
#ifdef Q_OS_MACOS
    Q_UNUSED(scan);
    return key == 60 || key == 62 || key == 61 || key == 54; // right Shift, Control, Option, Command
#elif defined(Q_OS_WIN)
    return key == 0xa1 || key == 0xa3 || key == 0xa5 || key == 0x5c ||
        (event.key() == Qt::Key_Shift && scan == 0x36) ||
        (event.key() == Qt::Key_Control && (scan == 0x11d || scan == 0xe01d)) ||
        (event.key() == Qt::Key_Alt && (scan == 0x138 || scan == 0xe038));
#else
    Q_UNUSED(scan);
    // Qt's X11/Wayland native virtual value may identify the right keysym;
    // use portable numeric RFB constants, with no Xlib headers or calls.
    return key == 0xffe2 || key == 0xffe4 || key == 0xffea || key == 0xffe8 || key == 0xffec;
#endif
}
quint32 scalarKeysym(char32_t scalar) {
    if ((scalar >= 0x20 && scalar <= 0x7e) || (scalar >= 0xa0 && scalar <= 0xff)) return quint32(scalar);
    return 0x01000000u | quint32(scalar);
}
}
VncDesktopView::VncDesktopView(bool mock, QWidget* parent) : DesktopView(mock, parent), mock_(mock) {
    setDesktopLabel(QStringLiteral("VNC desktop"));
    setAttribute(Qt::WA_InputMethodEnabled, true); // VNC composes text from input-method commits
}
VncDesktopView::~VncDesktopView() { releaseInput(); }
void VncDesktopView::setInputEnabled(bool enabled) {
    if (!enabled) releaseInput();
    inputEnabled_ = enabled;
}
void VncDesktopView::releaseInput() {
    // Clear state before emitting: shutdown/error handlers may reenter this API.
    ++inputEpoch_;
    heldKeys_.clear();
    auto symbols = std::move(symbolHolds_); symbolHolds_.clear();
    const bool buttons = heldButtons_ != 0;
    heldButtons_ = 0;
    angleX_ = angleY_ = pixelX_ = pixelY_ = 0;
    composing_ = false;
    setPointerDragActive(false);
    for (auto it = symbols.cbegin(); it != symbols.cend(); ++it) emit vncKeyInput(it.key(), false);
    if (buttons) sendPointer(0);
}
bool VncDesktopView::event(QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::ApplicationDeactivate) releaseInput();
    // DesktopView accepts shortcuts and dispatches Tab/backtab through the
    // virtual handlers, so neither local actions nor focus traversal steal input.
    return DesktopView::event(event);
}
quint64 VncDesktopView::identity(const QKeyEvent& event) {
    if (event.nativeScanCode()) return (quint64(1) << 63) | event.nativeScanCode();
    // Cocoa supplies native virtual identities, often with no scan code.
    if (event.nativeVirtualKey()) return (quint64(1) << 62) | event.nativeVirtualKey();
    const int key = event.key() == Qt::Key_Backtab ? Qt::Key_Tab : event.key();
    return quint64(quint32(key)) | ((event.modifiers() & Qt::KeypadModifier) ? (quint64(1) << 32) : 0);
}
quint32 VncDesktopView::namedKeysym(const QKeyEvent& event) {
    const int key = event.key();
    const bool right = rightModifier(event);
    if (event.modifiers() & Qt::KeypadModifier) {
        if (key >= Qt::Key_0 && key <= Qt::Key_9) return 0xffb0 + key - Qt::Key_0;
        switch (key) {
        case Qt::Key_Space: return 0xff80;
        case Qt::Key_Tab: case Qt::Key_Backtab: return 0xff89;
        case Qt::Key_Return: case Qt::Key_Enter: return 0xff8d;
        case Qt::Key_Home: return 0xff95;
        case Qt::Key_Left: return 0xff96;
        case Qt::Key_Up: return 0xff97;
        case Qt::Key_Right: return 0xff98;
        case Qt::Key_Down: return 0xff99;
        case Qt::Key_PageUp: return 0xff9a;
        case Qt::Key_PageDown: return 0xff9b;
        case Qt::Key_End: return 0xff9c;
        case Qt::Key_Clear: return 0xff9d;
        case Qt::Key_Insert: return 0xff9e;
        case Qt::Key_Delete: return 0xff9f;
        case Qt::Key_Asterisk: return 0xffaa;
        case Qt::Key_Plus: return 0xffab;
        case Qt::Key_Comma: return 0xffac;
        case Qt::Key_Minus: return 0xffad;
        case Qt::Key_Period: return 0xffae;
        case Qt::Key_Slash: return 0xffaf;
        case Qt::Key_Equal: return 0xffbd;
        default: break;
        }
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return 0xffbe + key - Qt::Key_F1;
    switch (key) {
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Tab: case Qt::Key_Backtab: return 0xff09;
    case Qt::Key_Clear: return 0xff0b;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return 0xff8d;
    case Qt::Key_Pause: return 0xff13;
    case Qt::Key_ScrollLock: return 0xff14;
    case Qt::Key_SysReq: return 0xff15;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_Print: return 0xff61;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Menu: return 0xff67;
    case Qt::Key_Help: return 0xff6a;
    case Qt::Key_NumLock: return 0xff7f;
    case Qt::Key_Shift: return right ? 0xffe2 : 0xffe1;
    case Qt::Key_Control:
#ifdef Q_OS_MACOS
        // Qt swaps Command/Control by default. Preserve physical semantics:
        // Command -> Super, physical Control -> Control; Option -> Alt.
        // Options > Remote Desktop can make Command send Control instead.
        return (QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) != DesktopView::commandAsControl() ? 0xffe3 : 0xffeb) + quint32(right);
#else
        return right ? 0xffe4 : 0xffe3;
#endif
    case Qt::Key_Meta:
#ifdef Q_OS_MACOS
        return (QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) != DesktopView::commandAsControl() ? 0xffeb : 0xffe3) + quint32(right);
#else
        return right ? 0xffec : 0xffeb;
#endif
    case Qt::Key_CapsLock: return 0xffe5;
    case Qt::Key_Alt: return right ? 0xffea : 0xffe9;
    case Qt::Key_AltGr: return 0xfe03;
    case Qt::Key_Super_L: return 0xffeb;
    case Qt::Key_Super_R: return 0xffec;
    case Qt::Key_Hyper_L: return 0xffed;
    case Qt::Key_Hyper_R: return 0xffee;
    case Qt::Key_Delete: return 0xffff;
    default: return 0;
    }
}
bool VncDesktopView::textKeysyms(const QString& text, QVector<quint32>& symbols) {
    for (qsizetype index = 0; index < text.size(); ++index) {
        const auto first = text.at(index);
        char32_t scalar = first.unicode();
        if (first.isHighSurrogate()) {
            if (index + 1 == text.size() || !text.at(index + 1).isLowSurrogate()) return false;
            scalar = QChar::surrogateToUcs4(first, text.at(++index));
        } else if (first.isLowSurrogate()) return false;
        // Control text is not a printable RFB Unicode key (e.g. Ctrl+A's U+0001).
        // The caller instead uses the Qt identity for physical shortcut keys.
        if (scalar < 0x20 || (scalar >= 0x7f && scalar < 0xa0)) continue;
        symbols.push_back(scalarKeysym(scalar));
    }
    return true;
}
void VncDesktopView::key(QKeyEvent* event, bool down) {
    event->accept();
    if (!inputEnabled_) return;
    if (event->isAutoRepeat() && !down) return;
    const auto id = identity(*event);
    auto existing = heldKeys_.find(id);
    if (!down) {
        if (existing == heldKeys_.end()) return;
        const auto symbols = existing.value(); heldKeys_.erase(existing);
        QVector<quint32> releases;
        for (auto symbol : symbols) {
            auto count = symbolHolds_.find(symbol);
            if (count != symbolHolds_.end() && --count.value() == 0) {
                symbolHolds_.erase(count); releases.push_back(symbol);
            }
        }
        for (auto symbol : releases) emit vncKeyInput(symbol, false);
        return;
    }
    if (existing != heldKeys_.end()) {
        const auto symbols = existing.value();
        const auto epoch = inputEpoch_;
        for (auto symbol : symbols) {
            if (!inputEnabled_ || epoch != inputEpoch_) break;
            emit vncKeyInput(symbol, true);
        }
        return;
    }
    QVector<quint32> symbols;
    if (const auto named = namedKeysym(*event)) symbols.push_back(named);
    else {
        if (composing_) return; // IME commits, not composition keystrokes, own text.
        if (!textKeysyms(event->text(), symbols)) {
            emit inputError(QStringLiteral("VNC input contains an invalid Unicode scalar.")); return;
        }
        if (symbols.isEmpty()) {
            const auto qtKey = event->key();
            if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
                symbols.push_back((event->modifiers() & Qt::ShiftModifier) ? quint32(qtKey) : quint32(qtKey + 0x20));
            else if ((qtKey >= 0x20 && qtKey <= 0x7e) || (qtKey >= 0xa0 && qtKey <= 0xff))
                symbols.push_back(quint32(qtKey));
        }
    }
    if (symbols.isEmpty()) return;
    heldKeys_.insert(id, symbols);
    for (auto symbol : symbols) ++symbolHolds_[symbol];
    const auto epoch = inputEpoch_;
    for (auto symbol : symbols) {
        if (!inputEnabled_ || epoch != inputEpoch_) break;
        emit vncKeyInput(symbol, true);
    }
}
void VncDesktopView::keyPressEvent(QKeyEvent* event) {
    if (mock_) DesktopView::keyPressEvent(event); else key(event, true);
}
void VncDesktopView::keyReleaseEvent(QKeyEvent* event) {
    if (mock_) DesktopView::keyReleaseEvent(event); else key(event, false);
}
void VncDesktopView::inputMethodEvent(QInputMethodEvent* event) {
    if (mock_) { DesktopView::inputMethodEvent(event); return; }
    event->accept();
    if (!inputEnabled_) return;
    composing_ = !event->preeditString().isEmpty();
    QVector<quint32> symbols;
    if (!textKeysyms(event->commitString(), symbols)) {
        emit inputError(QStringLiteral("VNC input contains an invalid Unicode scalar.")); return;
    }
    const auto epoch = inputEpoch_;
    for (auto symbol : symbols) {
        if (!inputEnabled_ || epoch != inputEpoch_) break;
        // Do not release a physical key with the same symbol during an IME pulse.
        ++symbolHolds_[symbol]; emit vncKeyInput(symbol, true);
        auto count = symbolHolds_.find(symbol);
        if (count != symbolHolds_.end() && --count.value() == 0) {
            symbolHolds_.erase(count); emit vncKeyInput(symbol, false);
        }
    }
}
void VncDesktopView::focusOutEvent(QFocusEvent* event) {
    releaseInput(); DesktopView::focusOutEvent(event);
}
void VncDesktopView::sendPointer(quint8 mask) {
    // A server-driven shrink can occur between the last drag and focus/shutdown
    // release. Clamp against the current GUI desktop before sending that release.
    const auto size = frame().size();
    lastPointer_.setX(qBound(0, lastPointer_.x(), qMax(0, size.width() - 1)));
    lastPointer_.setY(qBound(0, lastPointer_.y(), qMax(0, size.height() - 1)));
    emit vncPointerInput(mask, quint16(lastPointer_.x()), quint16(lastPointer_.y()));
}
void VncDesktopView::pointer(QMouseEvent* event, bool down, bool button) {
    event->accept(); trackPointer(event->position());
    if (!inputEnabled_) return;
    const auto bit = button ? buttonMask(event->button()) : quint8(0);
    if (button && (!bit || (!down && !(heldButtons_ & bit)))) return;
    QPoint remote;
    // A new click must start inside the desktop even when another button is held.
    // Motion/release of an existing drag remains clamped outside the letterbox.
    if (!mapPointer(event->position(), remote, heldButtons_ && (!button || !down))) return;
    lastPointer_ = remote;
    if (button) {
        if (down) heldButtons_ |= bit;
        else heldButtons_ &= quint8(~bit);
    }
    setPointerDragActive(heldButtons_ != 0);
    sendPointer(heldButtons_);
}
void VncDesktopView::mouseMoveEvent(QMouseEvent* event) {
    if (mock_) DesktopView::mouseMoveEvent(event); else pointer(event, false, false);
}
void VncDesktopView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    if (mock_) DesktopView::mousePressEvent(event); else pointer(event, true, true);
}
void VncDesktopView::mouseReleaseEvent(QMouseEvent* event) {
    if (mock_) DesktopView::mouseReleaseEvent(event); else pointer(event, false, true);
}
void VncDesktopView::wheelAxis(qint64& remainder, int delta, int unit, quint8 positive, quint8 negative) {
    if (!inputEnabled_) return;
    const auto epoch = inputEpoch_;
    remainder += delta;
    while (remainder >= unit || remainder <= -unit) {
        if (!inputEnabled_ || epoch != inputEpoch_) break;
        const bool up = remainder > 0;
        remainder += up ? -unit : unit;
        sendPointer(quint8(heldButtons_ | (up ? positive : negative)));
        sendPointer(heldButtons_);
    }
}
void VncDesktopView::wheelEvent(QWheelEvent* event) {
    if (mock_) { DesktopView::wheelEvent(event); return; }
    event->accept(); trackPointer(event->position());
    if (!inputEnabled_) return;
    QPoint remote;
    if (!mapPointer(event->position(), remote, heldButtons_ != 0)) return;
    lastPointer_ = remote;
    const auto angle = event->angleDelta(), pixel = event->pixelDelta();
    const auto pulses = [](qint64 remainder, int delta, int unit) { return qAbs(remainder + qint64(delta)) / unit; };
    const auto xPulses = angle.x() ? pulses(angleX_, angle.x(), 120) : pulses(pixelX_, pixel.x(), 40);
    const auto yPulses = angle.y() ? pulses(angleY_, angle.y(), 120) : pulses(pixelY_, pixel.y(), 40);
    if (xPulses + yPulses > 256) {
        setInputEnabled(false);
        emit inputError(QStringLiteral("VNC wheel input exceeds the safe per-event limit."));
        return;
    }
    const auto epoch = inputEpoch_;
    // Angle deltas use 120 units/notch. Pixel-only trackpads use 40 logical pixels
    // per RFB notch; separate accumulators avoid mixing unlike device units.
    // When Qt reports both, angle is authoritative (do not double-scroll).
    if (angle.x()) wheelAxis(angleX_, angle.x(), 120, 64, 32);
    else if (pixel.x()) wheelAxis(pixelX_, pixel.x(), 40, 64, 32);
    if (!inputEnabled_ || epoch != inputEpoch_) return;
    if (angle.y()) wheelAxis(angleY_, angle.y(), 120, 8, 16);
    else if (pixel.y()) wheelAxis(pixelY_, pixel.y(), 40, 8, 16);
}
