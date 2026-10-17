#include "TerminalScreen.h"
#include "Views.h"
#include "SessionClipboard.h"
#include "GridDriver.h"
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWindow>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QWheelEvent>
#include <QTest>
#include <cmath>

class FakeTerminalClipboard final : public ClipboardEndpoint {
public:
    void watch() override {}
    std::optional<QString> readText() override { ++reads; return value; }
    void writeText(const QString& text) override { ++writes; value = text; }
    QString value; int reads = 0, writes = 0;
};
namespace {
Qt::KeyboardModifier physicalControl() {
#ifdef Q_OS_MACOS
    return Qt::MetaModifier;
#else
    return Qt::ControlModifier;
#endif
}
bool activateTerminal(QWidget& widget, TerminalView& view) {
    widget.show(); if (!QTest::qWaitForWindowExposed(&widget)) return false;
    QTest::qWait(100);
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") activateNativeTestApplication(&widget);
#endif
    widget.raise(); widget.activateWindow(); widget.windowHandle()->requestActivate();
    if (QGuiApplication::platformName() == "offscreen") QApplication::setActiveWindow(&widget);
    view.setFocus(); return QTest::qWaitFor([&] { return widget.isActiveWindow() && view.hasFocus(); });
}
void keyEvent(TerminalView& view, int key, Qt::KeyboardModifiers flags, const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, key, flags, text); QApplication::sendEvent(&view, &event);
}
QSize cellSize(const TerminalView& view) {
    const QFontMetricsF metrics(view.font());
    return QSize(int(std::ceil(metrics.horizontalAdvance('M'))), int(std::ceil(metrics.height())));
}
QPoint cellPosition(const TerminalView& view, int column, int row) {
    const auto size = cellSize(view); return QPoint(column * size.width() + size.width() / 2, row * size.height() + size.height() / 2);
}
void dragMove(TerminalView& view, QPoint position, Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent event(QEvent::MouseMove, position, view.mapToGlobal(position), Qt::NoButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}
void wheel(TerminalView& view, QPoint position, QPoint pixel, QPoint angle, Qt::ScrollPhase phase = Qt::NoScrollPhase) {
    QWheelEvent event(position, view.mapToGlobal(position), pixel, angle, Qt::LeftButton, Qt::NoModifier, phase, false);
    QApplication::sendEvent(&view, &event);
}
QString ownedLine(int line) { return QString("owned-%1").arg(line, 4, 10, QLatin1Char('0')); }
QString ownedRange(int first, int last) {
    QStringList lines; for (int line = first; line <= last; ++line) lines += ownedLine(line); return lines.join('\n');
}
bool populate(TerminalView& view, int first = 0, int last = 79) {
    for (int line = first; line <= last; ++line) if (!view.append(ownedLine(line).toUtf8() + "\r\n")) return false;
    return true;
}
}
class TerminalTest final : public QObject {
    Q_OBJECT
private slots:
    void terminalUtf8ChunkingWideAndCombiningCells() {
        TerminalScreen screen(4, 20); const auto bytes = QString::fromUtf8("Aé界é").toUtf8();
        for (const auto byte : bytes) QVERIFY(screen.feed(QByteArray(1, byte)));
        QVERIFY(screen.cell(0, 0).text == "A"); QVERIFY(screen.cell(0, 1).text == QString::fromUtf8("é"));
        QVERIFY(screen.cell(0, 2).text == QString::fromUtf8("界")); QCOMPARE(screen.cell(0, 2).width, 2);
        QCOMPARE(screen.cell(0, 3).width, 0); QVERIFY(screen.cell(0, 4).text == QString::fromUtf8("é"));
        QCOMPARE(screen.cursor(), QPoint(5, 0));
    }
    void terminalCursorColorsAndAlternateScreen() {
        TerminalScreen screen(4, 20); QVERIFY(screen.feed("\x1b[38;2;12;34;56m\x1b[48;2;65;43;21mX"));
        QCOMPARE(screen.cell(0, 0).foreground, QColor(12, 34, 56)); QCOMPARE(screen.cell(0, 0).background, QColor(65, 43, 21));
        QVERIFY(screen.feed("\x1b[?25l\x1b[3;4H")); QVERIFY(!screen.cursorVisible()); QCOMPARE(screen.cursor(), QPoint(3, 2));
        QVERIFY(screen.feed("\x1b[?1049hALT")); QVERIFY(screen.alternate()); QCOMPARE(screen.historyLines(), 0);
        QVERIFY(screen.feed("\x1b[?1049l")); QVERIFY(!screen.alternate()); QVERIFY(screen.cell(0, 0).text == "X");
        QCOMPARE(screen.cursor(), QPoint(3, 2));
    }
    void terminalKeyboardImeAndCellResize() {
        TerminalScreen screen(4, 20); QVERIFY(screen.encodeKey(VTERM_KEY_UP) == "\x1b[A");
        QVERIFY(screen.encodeText(QString::fromUtf8("λ界")) == QString::fromUtf8("λ界").toUtf8());
        QVERIFY(screen.encodeText("c", VTERM_MOD_CTRL) == QByteArray(1, '\3'));
        QVERIFY(screen.resizeCells(24, 80)); QCOMPARE(screen.cells(), QSize(80, 24));
        QVERIFY(!screen.resizeCells(0, 80)); QVERIFY(!screen.resizeCells(24, 513)); QCOMPARE(screen.cells(), QSize(80, 24));
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view;
        view.initializeTerminal(clipboard); view.setInputEnabled(true); QByteArray received;
        view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("é界")); QCoreApplication::sendEvent(&view, &ime);
        QVERIFY(received == QString::fromUtf8("é界").toUtf8());
        view.resize(640, 480); view.show(); QTest::qWait(1);
        const auto size = view.screen()->cells(); QVERIFY(size.width() >= 1 && size.width() <= 512); QVERIFY(size.height() >= 1 && size.height() <= 256);
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
    }
    void terminalControlShortcutsIgnoreNativeEventText_data() {
        QTest::addColumn<int>("key"); QTest::addColumn<QString>("text"); QTest::addColumn<QByteArray>("expected");
        for (int i = 0; i < 26; ++i) {
            const auto letter = QString(QChar('a' + i)); const auto bytes = QByteArray(1, char(i + 1));
            QTest::newRow(qPrintable(letter + "-letter")) << int(Qt::Key_A + i) << letter << bytes;
            QTest::newRow(qPrintable(letter + "-control")) << int(Qt::Key_A + i) << QString(QChar(i + 1)) << bytes;
            QTest::newRow(qPrintable(letter + "-empty")) << int(Qt::Key_A + i) << QString() << bytes;
        }
        const std::pair<int, int> punctuation[] = {{Qt::Key_Space, 0}, {Qt::Key_At, 0}, {Qt::Key_BracketLeft, 27},
            {Qt::Key_Backslash, 28}, {Qt::Key_BracketRight, 29}, {Qt::Key_AsciiCircum, 30}, {Qt::Key_Underscore, 31}, {Qt::Key_Question, 127},
            {Qt::Key_2, 0}, {Qt::Key_3, 27}, {Qt::Key_4, 28}, {Qt::Key_5, 29}, {Qt::Key_6, 30}, {Qt::Key_7, 31}, {Qt::Key_8, 127}};
        for (const auto [key, byte] : punctuation) {
            QTest::newRow(qPrintable(QString("punctuation-%1").arg(key))) << key << QString() << QByteArray(1, char(byte));
        }
    }
    void terminalControlShortcutsIgnoreNativeEventText() {
        QFETCH(int, key); QFETCH(QString, text); QFETCH(QByteArray, expected);
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.initializeTerminal(clipboard); view.setInputEnabled(true);
        QByteArray received; view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        keyEvent(view, key, physicalControl(), text); QCOMPARE(received, expected);
        received.clear(); keyEvent(view, Qt::Key_B, physicalControl(), QString(QChar(2))); keyEvent(view, Qt::Key_C, Qt::NoModifier, "c");
        QCOMPARE(received, QByteArray("\x02" "c")); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
        view.setInputEnabled(false); received.clear(); keyEvent(view, key, physicalControl(), text); QVERIFY(received.isEmpty());
    }
    void terminalMetaAndSpecialKeysPreserveApplicationModes() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.initializeTerminal(clipboard); view.setInputEnabled(true);
        QByteArray received; view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        for (const auto key : {Qt::Key_B, Qt::Key_F, Qt::Key_D, Qt::Key_T, Qt::Key_U, Qt::Key_L, Qt::Key_C}) {
            const auto letter = QString(QChar('a' + key - Qt::Key_A));
            for (const auto& text : {letter, QString(), QString("\x1b") + letter}) {
                received.clear(); keyEvent(view, key, Qt::AltModifier, text); QCOMPARE(received, QByteArray("\x1b") + letter.toUtf8());
            }
            received.clear(); keyEvent(view, key, Qt::AltModifier | physicalControl(), QString());
            QCOMPARE(received, QByteArray("\x1b") + QByteArray(1, char(key - Qt::Key_A + 1)));
        }
        received.clear(); keyEvent(view, Qt::Key_Backspace, Qt::AltModifier); QCOMPARE(received, QByteArray("\x1b\x7f"));
        received.clear(); keyEvent(view, Qt::Key_Return, Qt::AltModifier); QCOMPARE(received, QByteArray("\x1b\r"));
        received.clear(); keyEvent(view, Qt::Key_Up, Qt::NoModifier); QCOMPARE(received, QByteArray("\x1b[A"));
        QVERIFY(view.append("\x1b[?1h")); received.clear(); keyEvent(view, Qt::Key_Up, Qt::NoModifier); QCOMPARE(received, QByteArray("\x1bOA"));
        QVERIFY(view.append("\x1b[?1049h"));
        TerminalScreen reference(4, 20); QVERIFY(reference.feed("\x1b[?1h"));
        const std::pair<int, VTermKey> keys[] = {{Qt::Key_Left, VTERM_KEY_LEFT}, {Qt::Key_Home, VTERM_KEY_HOME}, {Qt::Key_End, VTERM_KEY_END},
            {Qt::Key_Insert, VTERM_KEY_INS}, {Qt::Key_Delete, VTERM_KEY_DEL}, {Qt::Key_PageUp, VTERM_KEY_PAGEUP},
            {Qt::Key_PageDown, VTERM_KEY_PAGEDOWN}, {Qt::Key_F1, VTermKey(VTERM_KEY_FUNCTION(1))}, {Qt::Key_F12, VTermKey(VTERM_KEY_FUNCTION(12))}};
        for (const auto [key, terminalKey] : keys) {
            for (int flags = 0; flags < 8; ++flags) {
                // These exact combinations are the documented local clipboard aliases.
                if (key == Qt::Key_Insert && flags == VTERM_MOD_SHIFT) continue;
#ifndef Q_OS_MACOS
                if (key == Qt::Key_Insert && flags == VTERM_MOD_CTRL) continue;
#endif
                Qt::KeyboardModifiers qt;
                if (flags & VTERM_MOD_SHIFT) qt |= Qt::ShiftModifier;
                if (flags & VTERM_MOD_ALT) qt |= Qt::AltModifier;
                if (flags & VTERM_MOD_CTRL) qt |= physicalControl();
                received.clear(); keyEvent(view, key, qt); QCOMPARE(received, reference.encodeKey(terminalKey, VTermModifier(flags)));
            }
        }
        received.clear(); QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("é界")); QApplication::sendEvent(&view, &ime);
        QCOMPARE(received, QString::fromUtf8("é界").toUtf8()); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
    }
    void terminalTabAndBacktabNeverMoveFocus() {
        QWidget window; auto* layout = new QVBoxLayout(&window); layout->addWidget(new QLineEdit(&window));
        auto* view = new TerminalView(&window); layout->addWidget(view); layout->addWidget(new QLineEdit(&window));
        view->initializeTerminal(std::make_shared<FakeTerminalClipboard>()); view->setInputEnabled(true);
        QByteArray received; view->setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        QVERIFY(activateTerminal(window, *view)); QTest::keyClick(view, Qt::Key_Tab); QCOMPARE(received, QByteArray("\t")); QVERIFY(view->hasFocus());
        received.clear(); QTest::keyClick(view, Qt::Key_Backtab, Qt::ShiftModifier); QCOMPARE(received, QByteArray("\x1b[Z")); QVERIFY(view->hasFocus());
    }
    void terminalShortcutOverridePreservesRemoteInputAndLocalClipboard() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(800, 480); view.initializeTerminal(clipboard); view.setInputEnabled(true);
        QByteArray received; view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        int triggered = 0;
        for (int key = Qt::Key_A; key <= Qt::Key_Z; ++key) {
            auto* action = new QAction(&view); action->setShortcut(QKeySequence(int(physicalControl()) | key)); view.addAction(action);
            connect(action, &QAction::triggered, &view, [&] { ++triggered; });
        }
        QVERIFY(activateTerminal(view, view));
        for (int key = Qt::Key_A; key <= Qt::Key_Z; ++key) {
            QKeyEvent override(QEvent::ShortcutOverride, key, physicalControl()); override.ignore(); QApplication::sendEvent(&view, &override); QVERIFY(override.isAccepted());
            received.clear(); QTest::keyClick(&view, Qt::Key(key), physicalControl()); QCOMPARE(received, QByteArray(1, char(key - Qt::Key_A + 1)));
        }
        QCOMPARE(triggered, 0); QVERIFY(view.append("owned")); view.selectCells(QPoint(0, 0), QPoint(4, 0));
#ifdef Q_OS_MACOS
        const auto copyFlags = Qt::KeyboardModifiers(Qt::ControlModifier);
#else
        const auto copyFlags = Qt::ControlModifier | Qt::ShiftModifier;
#endif
        received.clear(); QTest::keyClick(&view, Qt::Key_C, copyFlags); QCOMPARE(clipboard->value, QString("owned")); QCOMPARE(clipboard->writes, 1); QVERIFY(received.isEmpty());
        clipboard->value = "paste"; QTest::keyClick(&view, Qt::Key_V, copyFlags); QCOMPARE(received, QByteArray("paste")); QCOMPARE(clipboard->reads, 1);
        received.clear(); QTest::keyClick(&view, Qt::Key_Insert, Qt::ShiftModifier); QCOMPARE(received, QByteArray("paste")); QCOMPARE(clipboard->reads, 2);
#ifndef Q_OS_MACOS
        received.clear(); QTest::keyClick(&view, Qt::Key_Insert, Qt::ControlModifier); QCOMPARE(clipboard->value, QString("owned")); QCOMPARE(clipboard->writes, 2); QVERIFY(received.isEmpty());
#endif
        const int writes = clipboard->writes;
        view.setInputEnabled(false); received.clear(); QTest::keyClick(&view, Qt::Key_V, copyFlags); QTest::keyClick(&view, Qt::Key_Insert, Qt::ShiftModifier);
        QCOMPARE(clipboard->reads, 2); QVERIFY(received.isEmpty());
        QTest::keyClick(&view, Qt::Key_C, copyFlags); QCOMPARE(clipboard->writes, writes + 1);
#ifdef Q_OS_MACOS
        QKeyEvent quit(QEvent::ShortcutOverride, Qt::Key_Q, Qt::ControlModifier); quit.ignore(); QApplication::sendEvent(&view, &quit); QVERIFY(!quit.isAccepted());
        view.setInputEnabled(true); QVERIFY(view.append("\x1b[?1049h"));
        for (const auto key : {Qt::Key_Home, Qt::Key_End}) {
            auto* action = new QAction(&view); action->setShortcut(QKeySequence(int(Qt::ControlModifier) | key)); view.addAction(action);
            connect(action, &QAction::triggered, &view, [&] { ++triggered; });
            QKeyEvent override(QEvent::ShortcutOverride, key, Qt::ControlModifier); override.ignore(); QApplication::sendEvent(&view, &override); QVERIFY(override.isAccepted());
            received.clear(); QTest::keyClick(&view, key, Qt::ControlModifier);
            QCOMPARE(received, key == Qt::Key_Home ? QByteArray("\x1b[1;6H") : QByteArray("\x1b[1;6F")); QCOMPARE(triggered, 0);
        }
#endif
    }
    void terminalSelectionAndScrollbackShortcutsStayLocal() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 140); view.initializeTerminal(clipboard); view.setInputEnabled(true);
        QByteArray received; view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        QVERIFY(activateTerminal(view, view));
        for (int line = 0; line < 40; ++line) QVERIFY(view.append(QByteArray("owned-") + QByteArray::number(line) + "\r\n"));
        auto* scroll = view.findChild<QScrollBar*>(); QVERIFY(scroll && scroll->maximum() > 0);
#ifdef Q_OS_MACOS
        const auto selectFlags = Qt::KeyboardModifiers(Qt::ControlModifier);
        const auto endFlags = Qt::KeyboardModifiers(Qt::ControlModifier);
#else
        const auto selectFlags = Qt::ControlModifier | Qt::ShiftModifier;
        const auto endFlags = Qt::ControlModifier | Qt::ShiftModifier;
#endif
        QTest::keyClick(&view, Qt::Key_A, selectFlags); QCOMPARE(clipboard->writes, 0); QVERIFY(view.copySelection());
        QVERIFY(clipboard->value.contains("owned-0")); QVERIFY(clipboard->value.contains("owned-39"));
        QTest::keyClick(&view, Qt::Key_Home, endFlags); QCOMPARE(scroll->value(), 0);
        QTest::keyClick(&view, Qt::Key_End, endFlags); QCOMPARE(scroll->value(), scroll->maximum());
        QTest::keyClick(&view, Qt::Key_PageUp, Qt::ShiftModifier); QVERIFY(scroll->value() < scroll->maximum());
        QTest::keyClick(&view, Qt::Key_PageDown, Qt::ShiftModifier); QCOMPARE(scroll->value(), scroll->maximum()); QVERIFY(received.isEmpty());
        QVERIFY(view.append("\x1b[?1049h")); QTest::keyClick(&view, Qt::Key_PageUp, Qt::ShiftModifier); QCOMPARE(received, QByteArray("\x1b[5;2~"));
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 1);
    }
    void terminalDragSelectionSpansVisibleRows() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        QByteArray received; view.setInputEnabled(true); view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        QVERIFY(activateTerminal(view, view)); QVERIFY(view.append(QString::fromUtf8("alpha\r\nbeta 界é\r\ngamma").toUtf8()));
        for (const bool reverse : {false, true}) {
            const auto first = cellPosition(view, 2, 0), last = cellPosition(view, 3, 2);
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, reverse ? last : first); dragMove(view, reverse ? first : last);
            QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, reverse ? first : last);
            const int writes = clipboard->writes; QVERIFY(view.copySelection()); QCOMPARE(clipboard->writes, writes + 1);
            QCOMPARE(clipboard->value, QString::fromUtf8("pha\nbeta 界é\ngamm"));
        }
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 1, 1)); dragMove(view, cellPosition(view, 3, 2));
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString::fromUtf8("eta 界é\ngamm"));
        dragMove(view, cellPosition(view, 2, 0)); QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 2, 0));
        view.setInputEnabled(false); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString("pha\nbe"));
        view.selectCells(QPoint(6, 1), QPoint(6, 1)); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString::fromUtf8("界"));
        QCOMPARE(clipboard->reads, 0); QVERIFY(received.isEmpty());
    }
    void terminalWheelAndTrackpadExtendActiveSelection() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        QVERIFY(activateTerminal(view, view)); QVERIFY(populate(view)); auto* scroll = view.findChild<QScrollBar*>(); scroll->setValue(10);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 9, 3)); const auto pointer = cellPosition(view, 0, 2); dragMove(view, pointer);
        wheel(view, pointer, {}, QPoint(0, 120)); QCOMPARE(scroll->value(), 7); QCOMPARE(clipboard->writes, 0);
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(9, 13));
        const int height = cellSize(view).height(); wheel(view, pointer, QPoint(0, height - 1), {}, Qt::ScrollBegin); QCOMPARE(scroll->value(), 7);
        wheel(view, pointer, QPoint(0, 1), {}, Qt::ScrollUpdate); QCOMPARE(scroll->value(), 6);
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(8, 13));
        wheel(view, pointer, QPoint(0, height), QPoint(0, 120), Qt::ScrollUpdate); QCOMPARE(scroll->value(), 5);
        wheel(view, pointer, QPoint(30, 0), QPoint(120, 0), Qt::ScrollUpdate); QCOMPARE(scroll->value(), 5);
        wheel(view, pointer, {}, {}, Qt::ScrollEnd);
        wheel(view, pointer, {}, QPoint(0, 20)); QCOMPARE(scroll->value(), 5);
        wheel(view, pointer, {}, QPoint(0, 20)); QCOMPARE(scroll->value(), 4);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, pointer); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(6, 13));
        const auto selected = clipboard->value; wheel(view, pointer, {}, QPoint(0, 120)); QCOMPARE(scroll->value(), 1);
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, selected); QCOMPARE(clipboard->reads, 0);
    }
    void terminalEdgeDragAutoScrollsThroughHistory() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        QVERIFY(activateTerminal(view, view)); QVERIFY(populate(view)); auto* scroll = view.findChild<QScrollBar*>(); scroll->setValue(20);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 9, 3)); const int height = cellSize(view).height();
        dragMove(view, QPoint(1, -3 * height)); QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), 0, 3000);
        auto* timer = view.findChild<QTimer*>("terminalSelectionScroll"); QVERIFY(timer); QVERIFY(!timer->isActive());
        QCOMPARE(clipboard->writes, 0); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(0, 23));
        dragMove(view, QPoint(cellPosition(view, view.screen()->cells().width() - 1, 0).x(), (view.screen()->cells().height() + 3) * height));
        QTRY_COMPARE_WITH_TIMEOUT(scroll->value(), scroll->maximum(), 3000); QVERIFY(!timer->isActive());
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedLine(23).mid(9) + '\n' + ownedRange(24, 79) + '\n');
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 0, 3));
        scroll->setValue(20); QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 9, 3));
        dragMove(view, QPoint(1, -height)); QTRY_VERIFY(scroll->value() < 20); dragMove(view, cellPosition(view, 0, 2));
        QVERIFY(!timer->isActive()); const int stopped = scroll->value(); QTest::qWait(160); QCOMPARE(scroll->value(), stopped);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 0, 2)); QCOMPARE(clipboard->reads, 0);
    }
    void terminalDragAutoScrollStopsOnLifecycleEvents_data() {
        QTest::addColumn<int>("reason"); const char* names[] = {"release", "buttons-lost", "ungrab", "focus", "deactivate", "hide", "resize", "destroy"};
        for (int reason = 0; reason < 8; ++reason) QTest::newRow(names[reason]) << reason;
    }
    void terminalDragAutoScrollStopsOnLifecycleEvents() {
        QFETCH(int, reason); auto clipboard = std::make_shared<FakeTerminalClipboard>(); auto view = std::make_unique<TerminalView>();
        view->resize(600, 180); view->initializeTerminal(clipboard); QVERIFY(activateTerminal(*view, *view)); QVERIFY(populate(*view));
        auto* scroll = view->findChild<QScrollBar*>(); scroll->setValue(30);
        QTest::mousePress(view.get(), Qt::LeftButton, Qt::NoModifier, cellPosition(*view, 9, 3)); dragMove(*view, QPoint(1, -1));
        auto* timer = view->findChild<QTimer*>("terminalSelectionScroll"); QVERIFY(timer); QVERIFY(timer->isActive());
        if (reason == 7) { QPointer<QTimer> owned = timer; view.reset(); QVERIFY(owned.isNull()); QTest::qWait(100); return; }
        if (reason == 0) QTest::mouseRelease(view.get(), Qt::LeftButton, Qt::NoModifier, QPoint(1, -1));
        else if (reason == 1) dragMove(*view, QPoint(1, -1), Qt::NoButton);
        else if (reason == 5) view->hide();
        else if (reason == 6) view->resize(600, 220);
        else if (reason == 3) { QFocusEvent event(QEvent::FocusOut); QApplication::sendEvent(view.get(), &event); }
        else {
            QEvent event(reason == 2 ? QEvent::UngrabMouse : QEvent::WindowDeactivate); QApplication::sendEvent(view.get(), &event);
        }
        QVERIFY(!timer->isActive()); const int stopped = scroll->value(); QTest::qWait(160); QCOMPARE(scroll->value(), stopped);
        QTest::mouseRelease(view.get(), Qt::LeftButton, Qt::NoModifier, QPoint(1, -1)); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
    }
    void terminalHistorySelectionKeepsStableAnchors() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        QVERIFY(activateTerminal(view, view)); QVERIFY(populate(view)); auto* scroll = view.findChild<QScrollBar*>(); scroll->setValue(10);
        view.selectCells(QPoint(0, 10), QPoint(9, 12)); QVERIFY(view.append("\x1b[?25l\x1b[1;1H"));
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(10, 12));
        QVERIFY(view.append("\x1b[999;1H")); QVERIFY(populate(view, 80, 99)); QCOMPARE(scroll->value(), 10);
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(10, 12));
        QVERIFY(populate(view, 100, 11099)); QVERIFY(view.screen()->firstLine() > 12); QCOMPARE(scroll->value(), 0);
        QVERIFY(!view.copySelection()); QVERIFY(view.screen()->historyLines() <= 10000); QVERIFY(view.screen()->historyBytes() <= 16 * 1024 * 1024);
        const auto base = view.screen()->firstLine(); scroll->setValue(10); view.selectCells(QPoint(0, 10), QPoint(9, 12));
        QVERIFY(view.copySelection()); const auto retained = clipboard->value; QVERIFY(populate(view, 11100, 11102));
        QCOMPARE(view.screen()->firstLine(), base + 3); QCOMPARE(scroll->value(), 7); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, retained);
        scroll->setValue(scroll->maximum()); QVERIFY(populate(view, 11103, 11105)); QCOMPARE(scroll->value(), scroll->maximum());
    }
    void terminalSelectionInvalidatesOnContentAndBufferChanges() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        QVERIFY(activateTerminal(view, view)); QVERIFY(view.append("alpha")); view.selectCells(QPoint(0, 0), QPoint(4, 0));
        QVERIFY(view.append("\x1b[?25l\x1b[1;1H")); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString("alpha"));
        auto appearance = view.appearance(); appearance.selectionBackground = QColor("#123456"); QVERIFY(view.setAppearance(appearance));
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString("alpha"));
        QVERIFY(view.append("X")); QVERIFY(!view.copySelection());
        QVERIFY(populate(view)); view.selectCells(QPoint(0, 0), QPoint(9, 2)); QVERIFY(view.append("\x1b[?1049h\x1b[?1049l")); QVERIFY(!view.copySelection());
        view.selectCells(QPoint(0, 0), QPoint(9, 2)); QVERIFY(view.append("\x1b[3J")); QVERIFY(!view.copySelection());
        view.selectCells(QPoint(0, 0), QPoint(4, 0)); view.resize(600, 220); QVERIFY(!view.copySelection());
        view.selectCells(QPoint(0, 0), QPoint(4, 0)); keyEvent(view, Qt::Key_Plus, Qt::ControlModifier); QVERIFY(!view.copySelection());
        QCOMPARE(clipboard->reads, 0);
    }
    void terminalDragSelectionRemainsLocalAndSessionScoped() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view, other; view.resize(600, 180); view.initializeTerminal(clipboard);
        other.initializeTerminal(clipboard); QVERIFY(other.append("other-owned")); const auto otherSize = other.screen()->cells();
        other.selectCells(QPoint(0, 0), QPoint(10, 0)); QByteArray received; view.setInputEnabled(true);
        view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; }); QVERIFY(activateTerminal(view, view)); QVERIFY(populate(view));
        auto* scroll = view.findChild<QScrollBar*>(); scroll->setValue(10); QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 9, 3));
        dragMove(view, cellPosition(view, 0, 2)); wheel(view, cellPosition(view, 0, 2), {}, QPoint(0, 120));
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 0, 2));
        QVERIFY(received.isEmpty()); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(other.screen()->cells(), otherSize);
        QVERIFY(other.copySelection()); QCOMPARE(clipboard->value, QString("other-owned"));
        view.setInputEnabled(false); QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, ownedRange(9, 13));
        QVERIFY(view.append("\x1b[?1049h\x1b[HALT\r\nSECOND")); QCOMPARE(scroll->maximum(), 0);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 0, 0)); dragMove(view, QPoint(1, -1));
        auto* timer = view.findChild<QTimer*>("terminalSelectionScroll"); QVERIFY(timer); QVERIFY(!timer->isActive());
        dragMove(view, cellPosition(view, 5, 1)); QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, cellPosition(view, 5, 1));
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->value, QString("ALT\nSECOND")); QVERIFY(received.isEmpty()); QCOMPARE(clipboard->reads, 0);
    }
    void terminalScrollbackAndDrainAreBounded() {
        TerminalScreen screen(2, 512);
        for (int i = 0; i < 11000; ++i) QVERIFY(screen.feed("owned line\r\n"));
        QVERIFY(screen.historyLines() > 0); QVERIFY(screen.historyLines() <= 10000);
        QVERIFY(screen.historyBytes() <= 16 * 1024 * 1024); QVERIFY(screen.firstLine() > 0);
        QVERIFY(!screen.feed(QByteArray(128 * 1024 + 1, 'x')));
        const auto history = screen.historyLines(); QVERIFY(screen.feed("\x1b[?1049h"));
        for (int i = 0; i < 10; ++i) QVERIFY(screen.feed("alternate\r\n"));
        QCOMPARE(screen.historyLines(), 0); QVERIFY(screen.feed("\x1b[?1049l")); QCOMPARE(screen.historyLines(), history);
    }
    void terminalOutputIsNeverDroppedUnderBackpressure() {
        TerminalScreen screen(4, 40); QByteArray expected;
        for (int i = 0; i < 1000; ++i) {
            const auto packet = QString("owned-%1\r\n").arg(i).toUtf8(); expected += packet; QVERIFY(screen.feed(packet));
        }
        QString all; for (int line = 0; line < screen.historyLines() + screen.cells().height(); ++line) all += screen.lineText(line) + '\n';
        QVERIFY(all.contains("owned-0")); QVERIFY(all.contains("owned-999"));
    }
    void terminalSelectionAndExplicitCopyPasteUseFakeEndpoint() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.initializeTerminal(clipboard);
        QVERIFY(view.append("owned visible text")); view.selectCells(QPoint(0, 0), QPoint(4, 0));
        QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
        QVERIFY(view.copySelection()); QCOMPARE(clipboard->writes, 1); QVERIFY(clipboard->value == "owned");
        QVERIFY(!view.pasteClipboard()); QCOMPARE(clipboard->reads, 0);
        view.setInputEnabled(true); QByteArray accepted; view.setInputSink([&](const QByteArray& bytes) { accepted += bytes; return true; });
        clipboard->value = QString::fromUtf8("explicit λ paste\n"); QVERIFY(view.pasteClipboard());
        QTRY_VERIFY(accepted == clipboard->value.toUtf8()); QCOMPARE(clipboard->reads, 1);
        view.setInputEnabled(false); QVERIFY(view.copySelection()); QCOMPARE(clipboard->writes, 2);
    }
    void terminalBracketedPasteAndInactiveInput() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.initializeTerminal(clipboard);
        QVERIFY(view.append("\x1b[?2004h")); view.setInputEnabled(true); QByteArray accepted; bool credit = false;
        view.setInputSink([&](const QByteArray& bytes) { if (!credit) return false; accepted += bytes; return true; });
        clipboard->value = QString(40000, 'p'); QVERIFY(view.pasteClipboard()); QTest::qWait(15); QVERIFY(accepted.isEmpty());
        credit = true; QTRY_COMPARE(accepted.size(), 40012); QVERIFY(accepted.startsWith("\x1b[200~")); QVERIFY(accepted.endsWith("\x1b[201~"));
        view.setInputEnabled(false); QTest::keyClick(&view, Qt::Key_A); QVERIFY(!view.pasteClipboard()); QCOMPARE(accepted.size(), 40012);
        view.setInputEnabled(true); clipboard->value = QString(1024 * 1024 + 1, 'x'); QVERIFY(!view.pasteClipboard()); QCOMPARE(accepted.size(), 40012);
    }
    void terminalOscClipboardAndLocalActionsNeverTouchHost() {
        auto clipboard = std::make_shared<FakeTerminalClipboard>(); TerminalView view; view.initializeTerminal(clipboard);
        QVERIFY(view.append("\x1b]52;c;c3ludGhldGlj\7\x1b]52;c;?\7\x1b]0;ignored-title\7\x1b]8;;file:///owned/no-open\7owned\x1b]8;;\7"));
        QVERIFY(view.append("\x1bP+qowned-no-exec\x1b\\")); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0);
        QVERIFY(view.windowTitle().isEmpty()); QVERIFY(view.screen()->lineText(0).contains("owned"));
    }
};
QTEST_MAIN(TerminalTest)
#include "terminal.moc"
