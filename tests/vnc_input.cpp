#include "VncDesktopView.h"
#include "VncGraphics.h"
#include <QApplication>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <limits>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtTest>

namespace {
void key(VncDesktopView& view, QEvent::Type type, int qtKey, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
    const QString& text = {}, bool repeat = false, quint32 nativeScan = 0, quint32 nativeVirtual = 0) {
    QKeyEvent event(type, qtKey, modifiers, nativeScan, nativeVirtual, 0, text, repeat);
    QApplication::sendEvent(&view, &event);
}
void mouse(VncDesktopView& view, QEvent::Type type, QPointF position, Qt::MouseButton button = Qt::NoButton,
    Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, position, view.mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}
void wheel(VncDesktopView& view, QPointF position, QPoint angle, QPoint pixel = {}) {
    QWheelEvent event(position, view.mapToGlobal(position.toPoint()), pixel, angle, Qt::NoButton,
        Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
}
void ime(VncDesktopView& view, const QString& preedit, const QString& commit = {}) {
    QInputMethodEvent event(preedit, {}); event.setCommitString(commit); QApplication::sendEvent(&view, &event);
}
QVariantList keyValue(quint32 symbol, bool down) { return {QVariant::fromValue(symbol), QVariant(down)}; }
QVariantList pointerValue(quint8 mask, quint16 x, quint16 y) {
    return {QVariant::fromValue(mask), QVariant::fromValue(x), QVariant::fromValue(y)};
}
void desktop(VncDesktopView& view, QSize frame = QSize(800, 400), QSize widget = QSize(400, 400)) {
    QImage image(frame, QImage::Format_RGB32); image.fill(Qt::black); view.setFrame(image); view.resize(widget);
    view.setInputEnabled(true);
}
}
class VncInputTest final : public QObject {
    Q_OBJECT
private slots:
    void protocolLabelAndRdpSignalsStayIndependent() {
        DesktopView rdp(false); VncDesktopView vnc(false);
        QVERIFY(rdp.findChild<QLabel*>("desktopPlaceholder")->text().startsWith("RDP desktop"));
        QVERIFY(vnc.findChild<QLabel*>("desktopPlaceholder")->text().startsWith("VNC desktop"));
        QSignalSpy rdpKeys(&vnc, &DesktopView::keyInput), rdpText(&vnc, &DesktopView::textInput), rdpPointer(&vnc, &DesktopView::pointerInput);
        desktop(vnc); key(vnc, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        key(vnc, QEvent::KeyRelease, Qt::Key_A); ime(vnc, {}, QString::fromUtf8("界"));
        mouse(vnc, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
        wheel(vnc, QPointF(200, 200), QPoint(0, 120)); vnc.releaseInput();
        QCOMPARE(rdpKeys.size(), 0); QCOMPARE(rdpText.size(), 0); QCOMPARE(rdpPointer.size(), 0);
        // The narrow Tab dispatch change preserves the base RDP encoder.
        QSignalSpy baseKeys(&rdp, &DesktopView::keyInput);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier), release(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(&rdp, &press); QApplication::sendEvent(&rdp, &release);
        QCOMPARE(baseKeys.size(), 2); QCOMPARE(baseKeys.at(0).at(0).toUInt(), quint32(0x0f));
    }
    void namedKeys_data() {
        QTest::addColumn<int>("qtKey"); QTest::addColumn<quint32>("symbol"); QTest::addColumn<bool>("keypad");
        const std::pair<int, quint32> named[] = {
            {Qt::Key_Backspace, 0xff08}, {Qt::Key_Tab, 0xff09}, {Qt::Key_Backtab, 0xff09},
            {Qt::Key_Clear, 0xff0b}, {Qt::Key_Return, 0xff0d}, {Qt::Key_Enter, 0xff8d},
            {Qt::Key_Pause, 0xff13}, {Qt::Key_ScrollLock, 0xff14}, {Qt::Key_SysReq, 0xff15},
            {Qt::Key_Escape, 0xff1b}, {Qt::Key_Home, 0xff50}, {Qt::Key_Left, 0xff51},
            {Qt::Key_Up, 0xff52}, {Qt::Key_Right, 0xff53}, {Qt::Key_Down, 0xff54},
            {Qt::Key_PageUp, 0xff55}, {Qt::Key_PageDown, 0xff56}, {Qt::Key_End, 0xff57},
            {Qt::Key_Print, 0xff61}, {Qt::Key_Insert, 0xff63}, {Qt::Key_Menu, 0xff67},
            {Qt::Key_Help, 0xff6a}, {Qt::Key_NumLock, 0xff7f}, {Qt::Key_Shift, 0xffe1},
            {Qt::Key_CapsLock, 0xffe5}, {Qt::Key_Alt, 0xffe9}, {Qt::Key_AltGr, 0xfe03},
            {Qt::Key_Super_L, 0xffeb}, {Qt::Key_Super_R, 0xffec}, {Qt::Key_Delete, 0xffff},
            {Qt::Key_F1, 0xffbe}, {Qt::Key_F12, 0xffc9}, {Qt::Key_F35, 0xffe0}
        };
        for (const auto& entry : named) QTest::newRow(qPrintable(QString("key-%1").arg(entry.first))) << entry.first << entry.second << false;
        const std::pair<int, quint32> pad[] = {
            {Qt::Key_0, 0xffb0}, {Qt::Key_9, 0xffb9}, {Qt::Key_Space, 0xff80},
            {Qt::Key_Tab, 0xff89}, {Qt::Key_Return, 0xff8d}, {Qt::Key_Enter, 0xff8d},
            {Qt::Key_Home, 0xff95}, {Qt::Key_Left, 0xff96}, {Qt::Key_Up, 0xff97},
            {Qt::Key_Right, 0xff98}, {Qt::Key_Down, 0xff99}, {Qt::Key_PageUp, 0xff9a},
            {Qt::Key_PageDown, 0xff9b}, {Qt::Key_End, 0xff9c}, {Qt::Key_Clear, 0xff9d},
            {Qt::Key_Insert, 0xff9e}, {Qt::Key_Delete, 0xff9f}, {Qt::Key_Asterisk, 0xffaa},
            {Qt::Key_Plus, 0xffab}, {Qt::Key_Comma, 0xffac}, {Qt::Key_Minus, 0xffad},
            {Qt::Key_Period, 0xffae}, {Qt::Key_Slash, 0xffaf}, {Qt::Key_Equal, 0xffbd}
        };
        for (const auto& entry : pad) QTest::newRow(qPrintable(QString("pad-%1").arg(entry.first))) << entry.first << entry.second << true;
    }
    void namedKeys() {
        QFETCH(int, qtKey); QFETCH(quint32, symbol); QFETCH(bool, keypad);
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        const auto modifiers = keypad ? Qt::KeypadModifier : Qt::NoModifier;
        key(view, QEvent::KeyPress, qtKey, modifiers); key(view, QEvent::KeyRelease, qtKey, modifiers);
        QCOMPARE(keys.size(), 2); QCOMPARE(keys.takeFirst(), keyValue(symbol, true)); QCOMPARE(keys.takeFirst(), keyValue(symbol, false));
    }
    void macCommandAndPhysicalControlPolicy() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        quint32 control = 0xffe3, meta = 0xffeb;
#ifdef Q_OS_MACOS
        if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) { control = 0xffeb; meta = 0xffe3; }
#endif
        key(view, QEvent::KeyPress, Qt::Key_Control); key(view, QEvent::KeyRelease, Qt::Key_Control);
        key(view, QEvent::KeyPress, Qt::Key_Meta); key(view, QEvent::KeyRelease, Qt::Key_Meta);
        QCOMPARE(keys.size(), 4); QCOMPARE(keys.takeFirst(), keyValue(control, true)); QCOMPARE(keys.takeFirst(), keyValue(control, false));
        QCOMPARE(keys.takeFirst(), keyValue(meta, true)); QCOMPARE(keys.takeFirst(), keyValue(meta, false));
    }
    void leftAndRightModifierIdentitiesAreIndependent() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        quint32 leftVirtual = 0xffe1, rightVirtual = 0xffe2;
#ifdef Q_OS_MACOS
        leftVirtual = 56; rightVirtual = 60;
#elif defined(Q_OS_WIN)
        leftVirtual = 0xa0; rightVirtual = 0xa1;
#endif
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 0, leftVirtual);
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 0, rightVirtual);
        key(view, QEvent::KeyRelease, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 0, leftVirtual);
        key(view, QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, {}, false, 0, rightVirtual);
        QCOMPARE(keys.size(), 4);
        QCOMPARE(keys.takeFirst(), keyValue(0xffe1, true)); QCOMPARE(keys.takeFirst(), keyValue(0xffe2, true));
        QCOMPARE(keys.takeFirst(), keyValue(0xffe1, false)); QCOMPARE(keys.takeFirst(), keyValue(0xffe2, false));
        key(view, QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier, "!", false, 0, 1234);
        key(view, QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, "1", false, 0, 1234);
        QCOMPARE(keys.takeFirst(), keyValue('!', true)); QCOMPARE(keys.takeFirst(), keyValue('!', false));
    }
    void identicalModifierSymbolsReleaseOnlyLastPhysicalHolder() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        // Native identities can be available even when native left/right metadata
        // is not. Both map to Shift_L, but releasing one must not release both.
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 1000);
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 1001);
        QCOMPARE(keys.size(), 2); keys.clear();
        key(view, QEvent::KeyRelease, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 1000);
        QCOMPARE(keys.size(), 0);
        key(view, QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, {}, false, 1001);
        QCOMPARE(keys.takeFirst(), keyValue(0xffe1, false));
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 1000);
        key(view, QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, {}, false, 1001);
        keys.clear(); view.releaseInput(); QCOMPARE(keys.size(), 1); QCOMPARE(keys.takeFirst(), keyValue(0xffe1, false));
    }
    void unicodeScalarsAndSameKeysymAfterModifierChange() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        key(view, QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, "A");
        key(view, QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, "a");
        QCOMPARE(keys.takeFirst(), keyValue('A', true)); QCOMPARE(keys.takeFirst(), keyValue('A', false));
        const auto text = QString::fromUcs4(U"é界😀");
        key(view, QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, text);
        key(view, QEvent::KeyRelease, Qt::Key_unknown, Qt::NoModifier, {});
        const quint32 symbols[] = {0xe9, 0x0100754c, 0x0101f600};
        QCOMPARE(keys.size(), 6);
        for (auto symbol : symbols) QCOMPARE(keys.takeFirst(), keyValue(symbol, true));
        for (auto symbol : symbols) QCOMPARE(keys.takeFirst(), keyValue(symbol, false));
        // Control text must not become U+0001; preserve a physical shortcut key.
        key(view, QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier, QString(QChar(1)));
        key(view, QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
        QCOMPARE(keys.takeFirst(), keyValue('a', true)); QCOMPARE(keys.takeFirst(), keyValue('a', false));
    }
    void nativeIdentityAndAutoRepeatReleaseFiltering() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        key(view, QEvent::KeyPress, Qt::Key_Exclam, Qt::ShiftModifier, "!", false, 42);
        key(view, QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, "1", true, 42);
        key(view, QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, "1", true, 42);
        key(view, QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, "1", false, 42);
        key(view, QEvent::KeyRelease, Qt::Key_1, Qt::NoModifier, "1", false, 42);
        QCOMPARE(keys.size(), 3);
        QCOMPARE(keys.takeFirst(), keyValue('!', true)); QCOMPARE(keys.takeFirst(), keyValue('!', true)); QCOMPARE(keys.takeFirst(), keyValue('!', false));
        // Keypad and main keyboard identities may coexist even without native IDs.
        key(view, QEvent::KeyPress, Qt::Key_1, Qt::KeypadModifier, "1");
        key(view, QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, "1");
        key(view, QEvent::KeyRelease, Qt::Key_1, Qt::KeypadModifier);
        key(view, QEvent::KeyRelease, Qt::Key_1);
        QCOMPARE(keys.size(), 4);
        QCOMPARE(keys.takeFirst(), keyValue(0xffb1, true)); QCOMPARE(keys.takeFirst(), keyValue('1', true));
        QCOMPARE(keys.takeFirst(), keyValue(0xffb1, false)); QCOMPARE(keys.takeFirst(), keyValue('1', false));
    }
    void imeCompositionCommitsOnceAndRejectsMalformedSurrogates() {
        VncDesktopView view(false); view.setInputEnabled(true);
        QSignalSpy keys(&view, &VncDesktopView::vncKeyInput), errors(&view, &VncDesktopView::inputError);
        ime(view, QString::fromUtf8("に"));
        key(view, QEvent::KeyPress, Qt::Key_N, Qt::NoModifier, "n");
        key(view, QEvent::KeyRelease, Qt::Key_N); QCOMPARE(keys.size(), 0);
        ime(view, {}, QString::fromUcs4(U"界😀")); QCOMPARE(keys.size(), 4);
        QCOMPARE(keys.takeFirst(), keyValue(0x0100754c, true)); QCOMPARE(keys.takeFirst(), keyValue(0x0100754c, false));
        QCOMPARE(keys.takeFirst(), keyValue(0x0101f600, true)); QCOMPARE(keys.takeFirst(), keyValue(0x0101f600, false));
        key(view, QEvent::KeyRelease, Qt::Key_unknown); QCOMPARE(keys.size(), 0);
        const QString broken = QString("a") + QChar(0xd800);
        ime(view, {}, broken); QCOMPARE(keys.size(), 0); QCOMPARE(errors.size(), 1);
        key(view, QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, broken); QCOMPARE(keys.size(), 0); QCOMPARE(errors.size(), 2);
        key(view, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a"); key(view, QEvent::KeyRelease, Qt::Key_A);
        QCOMPARE(keys.size(), 2);
    }
    void shortcutOverrideAndTabStayInRemoteView() {
        VncDesktopView view(false); view.setInputEnabled(true); QSignalSpy keys(&view, &VncDesktopView::vncKeyInput);
        QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_A, Qt::ControlModifier);
        shortcut.ignore(); QApplication::sendEvent(&view, &shortcut); QVERIFY(shortcut.isAccepted());
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        tab.ignore(); QApplication::sendEvent(&view, &tab); QVERIFY(tab.isAccepted());
        key(view, QEvent::KeyRelease, Qt::Key_Tab);
        key(view, QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        key(view, QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier);
        QCOMPARE(keys.size(), 4);
        for (int i = 0; i < 2; ++i) { QCOMPARE(keys.takeFirst(), keyValue(0xff09, true)); QCOMPARE(keys.takeFirst(), keyValue(0xff09, false)); }
    }
    void letterboxMappingMasksAndClampedDrag() {
        VncDesktopView view(false); desktop(view); QSignalSpy pointers(&view, &VncDesktopView::vncPointerInput);
        mouse(view, QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseMove, QPointF(20, 20));
        mouse(view, QEvent::MouseButtonRelease, QPointF(20, 20), Qt::LeftButton);
        QCOMPARE(pointers.size(), 0);
        mouse(view, QEvent::MouseMove, QPointF(200, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(0, 400, 200));
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::MiddleButton, Qt::LeftButton | Qt::MiddleButton);
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::RightButton, Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(1, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(3, 400, 200));
        QCOMPARE(pointers.takeFirst(), pointerValue(7, 400, 200));
        mouse(view, QEvent::MouseMove, QPointF(1000, 1000), Qt::NoButton, Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(7, 799, 399));
        mouse(view, QEvent::MouseButtonRelease, QPointF(-100, -100), Qt::MiddleButton, Qt::LeftButton | Qt::RightButton);
        mouse(view, QEvent::MouseButtonRelease, QPointF(-100, -100), Qt::RightButton, Qt::LeftButton);
        mouse(view, QEvent::MouseButtonRelease, QPointF(-100, -100), Qt::LeftButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(5, 0, 0)); QCOMPARE(pointers.takeFirst(), pointerValue(1, 0, 0));
        QCOMPARE(pointers.takeFirst(), pointerValue(0, 0, 0));
        mouse(view, QEvent::MouseMove, QPointF(200, 300)); // Bottom edge is outside [100,300).
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 300), Qt::RightButton, Qt::RightButton);
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::BackButton, Qt::BackButton);
        QCOMPARE(pointers.size(), 0);
    }
    void newLetterboxClickIgnoredDuringDragAndLogicalHiDpiCoordinates() {
        VncDesktopView view(false); desktop(view);
        QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::black); image.setDevicePixelRatio(2); view.setFrame(image);
        QSignalSpy pointers(&view, &VncDesktopView::vncPointerInput);
        mouse(view, QEvent::MouseButtonPress, QPointF(100.5, 150.5), Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(1, 201, 101));
        mouse(view, QEvent::MouseButtonPress, QPointF(20, 20), Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        QCOMPARE(pointers.size(), 0);
        mouse(view, QEvent::MouseButtonRelease, QPointF(20, 20), Qt::RightButton, Qt::LeftButton);
        QCOMPARE(pointers.size(), 0);
        mouse(view, QEvent::MouseButtonRelease, QPointF(399.9, 299.9), Qt::LeftButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(0, 799, 399));
        // QWidget events are logical coordinates; DPR must not be applied twice.
        mouse(view, QEvent::MouseMove, QPointF(200, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(0, 400, 200));
    }
    void heldPointerReleaseClampsAfterServerGeometryShrinks() {
        VncDesktopView view(false); desktop(view); QSignalSpy pointers(&view, &VncDesktopView::vncPointerInput);
        mouse(view, QEvent::MouseButtonPress, QPointF(399.9, 299.9), Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(pointers.takeFirst(), pointerValue(1, 799, 399));
        QImage smaller(100, 50, QImage::Format_RGB32); smaller.fill(Qt::black); view.setFrame(smaller);
        view.releaseInput(); QCOMPARE(pointers.size(), 1); QCOMPARE(pointers.takeFirst(), pointerValue(0, 99, 49));
    }
    void wheelsAccumulateBothAxesPreserveHeldMaskAndIgnoreLetterbox() {
        VncDesktopView view(false); desktop(view); QSignalSpy pointers(&view, &VncDesktopView::vncPointerInput);
        wheel(view, QPointF(20, 20), QPoint(120, 120)); QCOMPARE(pointers.size(), 0);
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::RightButton, Qt::RightButton); pointers.clear();
        wheel(view, QPointF(200, 200), QPoint(60, 60)); QCOMPARE(pointers.size(), 0);
        wheel(view, QPointF(200, 200), QPoint(60, 60)); QCOMPARE(pointers.size(), 4);
        QCOMPARE(pointers.takeFirst(), pointerValue(68, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200));
        QCOMPARE(pointers.takeFirst(), pointerValue(12, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200));
        wheel(view, QPointF(200, 200), QPoint(-120, -240)); QCOMPARE(pointers.size(), 6);
        QCOMPARE(pointers.takeFirst(), pointerValue(36, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200));
        for (int i = 0; i < 2; ++i) { QCOMPARE(pointers.takeFirst(), pointerValue(20, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200)); }
        wheel(view, QPointF(200, 200), {}, QPoint(-20, 20)); QCOMPARE(pointers.size(), 0);
        wheel(view, QPointF(200, 200), {}, QPoint(-20, 20)); QCOMPARE(pointers.size(), 4);
        QCOMPARE(pointers.takeFirst(), pointerValue(36, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200));
        QCOMPARE(pointers.takeFirst(), pointerValue(12, 400, 200)); QCOMPARE(pointers.takeFirst(), pointerValue(4, 400, 200));
        wheel(view, QPointF(200, 200), QPoint(0, 120), QPoint(0, 40)); QCOMPARE(pointers.size(), 2); pointers.clear();
        wheel(view, QPointF(-20, -20), QPoint(0, 120)); QCOMPARE(pointers.takeFirst(), pointerValue(12, 0, 0));
        QCOMPARE(pointers.takeFirst(), pointerValue(4, 0, 0)); view.releaseInput();
        QCOMPARE(pointers.takeFirst(), pointerValue(0, 0, 0));
    }
    void wheelSafetyLimitAndReentrantDisableStopFurtherPulses() {
        VncDesktopView view(false); desktop(view);
        QSignalSpy pointers(&view, &VncDesktopView::vncPointerInput), errors(&view, &VncDesktopView::inputError);
        wheel(view, QPointF(200, 200), QPoint(std::numeric_limits<int>::max(), std::numeric_limits<int>::min()));
        QCOMPARE(pointers.size(), 0); QCOMPARE(errors.size(), 1);
        wheel(view, QPointF(200, 200), QPoint(0, 120)); QCOMPARE(pointers.size(), 0);
        view.setInputEnabled(true);
        connect(&view, &VncDesktopView::vncPointerInput, &view, [&](quint8 mask, quint16, quint16) {
            if (mask & 8) view.setInputEnabled(false);
        });
        wheel(view, QPointF(200, 200), QPoint(0, 120 * 20));
        QCOMPARE(pointers.size(), 2); QCOMPARE(pointers.takeFirst(), pointerValue(8, 400, 200));
        QCOMPARE(pointers.takeFirst(), pointerValue(0, 400, 200));
    }
    void mousePressFocusesRemoteView() {
        QWidget host; host.resize(440, 470);
        QLineEdit sibling(&host); sibling.setGeometry(0, 0, 200, 30);
        VncDesktopView view(false, &host); desktop(view); view.move(0, 40);
        host.show(); QApplication::setActiveWindow(&host); sibling.setFocus();
        QTRY_VERIFY(sibling.hasFocus());
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(view.hasFocus()); view.releaseInput();
    }
    void heldInputReleasedOnLifecycleEvents_data() {
        QTest::addColumn<int>("type");
        QTest::newRow("focus-out") << int(QEvent::FocusOut);
        QTest::newRow("hide") << int(QEvent::Hide);
        QTest::newRow("window-deactivate") << int(QEvent::WindowDeactivate);
        QTest::newRow("application-deactivate") << int(QEvent::ApplicationDeactivate);
        QTest::newRow("manual-shutdown") << -1;
    }
    void heldInputReleasedOnLifecycleEvents() {
        QFETCH(int, type); VncDesktopView view(false); desktop(view);
        QSignalSpy keys(&view, &VncDesktopView::vncKeyInput), pointers(&view, &VncDesktopView::vncPointerInput);
        key(view, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        key(view, QEvent::KeyPress, Qt::Key_Shift);
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
        keys.clear(); pointers.clear();
        if (type == -1) view.releaseInput();
        else if (type == QEvent::FocusOut) { QFocusEvent event(QEvent::FocusOut); QApplication::sendEvent(&view, &event); }
        else { QEvent event(static_cast<QEvent::Type>(type)); QApplication::sendEvent(&view, &event); }
        QCOMPARE(keys.size(), 2); QVERIFY(keys.contains(keyValue('a', false))); QVERIFY(keys.contains(keyValue(0xffe1, false)));
        QCOMPARE(pointers.size(), 1); QCOMPARE(pointers.takeFirst(), pointerValue(0, 400, 200));
        keys.clear(); view.releaseInput();
        key(view, QEvent::KeyRelease, Qt::Key_A); mouse(view, QEvent::MouseButtonRelease, QPointF(200, 200), Qt::LeftButton);
        QCOMPARE(keys.size(), 0); QCOMPARE(pointers.size(), 0);
    }
    void initiallyDisabledAndViewOnlyDropsAllInputAfterReleases() {
        VncDesktopView view(false); QImage image(800, 400, QImage::Format_RGB32); image.fill(Qt::black); view.setFrame(image); view.resize(400, 400);
        QSignalSpy keys(&view, &VncDesktopView::vncKeyInput), pointers(&view, &VncDesktopView::vncPointerInput);
        auto input = [&] {
            key(view, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
            key(view, QEvent::KeyPress, Qt::Key_Tab); key(view, QEvent::KeyRelease, Qt::Key_Tab);
            ime(view, {}, QString::fromUtf8("界"));
            mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
            mouse(view, QEvent::MouseMove, QPointF(201, 201), Qt::NoButton, Qt::LeftButton);
            wheel(view, QPointF(200, 200), QPoint(0, 120));
        };
        input(); QCOMPARE(keys.size(), 0); QCOMPARE(pointers.size(), 0);
        view.setInputEnabled(true); key(view, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        mouse(view, QEvent::MouseButtonPress, QPointF(200, 200), Qt::LeftButton, Qt::LeftButton);
        wheel(view, QPointF(200, 200), QPoint(0, 60)); keys.clear(); pointers.clear();
        view.setInputEnabled(false); QCOMPARE(keys.takeFirst(), keyValue('a', false)); QCOMPARE(pointers.takeFirst(), pointerValue(0, 400, 200));
        input(); QCOMPARE(keys.size(), 0); QCOMPARE(pointers.size(), 0);
        view.setInputEnabled(true); key(view, QEvent::KeyRelease, Qt::Key_A);
        wheel(view, QPointF(200, 200), QPoint(0, 60)); QCOMPARE(keys.size(), 0); QCOMPARE(pointers.size(), 0);
    }
    void explicitMockRetainsMockInputContract() {
        VncDesktopView view(true); QSignalSpy mock(&view, &DesktopView::mockInput);
        QSignalSpy keys(&view, &VncDesktopView::vncKeyInput), pointers(&view, &VncDesktopView::vncPointerInput);
        key(view, QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a"); key(view, QEvent::KeyRelease, Qt::Key_A);
        key(view, QEvent::KeyPress, Qt::Key_Tab); key(view, QEvent::KeyRelease, Qt::Key_Tab);
        mouse(view, QEvent::MouseButtonPress, QPointF(10, 10), Qt::LeftButton, Qt::LeftButton);
        wheel(view, QPointF(10, 10), QPoint(0, 120));
        QCOMPARE(mock.size(), 6); QCOMPARE(keys.size(), 0); QCOMPARE(pointers.size(), 0);
    }
    void sharedViewRejectsLateGeometryAndRevisionBatches() {
        VncDesktopView view(false); VncGraphics graphics;
        QVERIFY(graphics.geometry(256, 256)); QVERIFY(graphics.commit()); const auto first = graphics.takeUpdate(); QVERIFY(view.applyUpdate(first));
        const uchar red[] = {0, 0, 255, 0}; QVERIFY(graphics.raw(1, 1, 1, 1, red, sizeof(red))); QVERIFY(graphics.commit());
        const auto partial = graphics.takeUpdate(); QVERIFY(view.applyUpdate(partial)); QVERIFY(!view.applyUpdate(first));
        QVERIFY(graphics.geometry(512, 256)); QVERIFY(graphics.commit()); const auto resized = graphics.takeUpdate(); QVERIFY(view.applyUpdate(resized));
        QVERIFY(!view.applyUpdate(partial)); QVERIFY(!view.applyUpdate(first)); QVERIFY(!view.applyUpdate(resized));
        QCOMPARE(view.frame().size(), QSize(512, 256)); QCOMPARE(view.frame().pixel(1, 1), qRgb(0, 0, 0));
    }
};
QTEST_MAIN(VncInputTest)
#include "vnc_input.moc"
