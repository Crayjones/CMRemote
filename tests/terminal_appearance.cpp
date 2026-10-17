#include "TerminalAppearance.h"
#include "TerminalAppearanceService.h"
#include "TerminalAppearanceDialog.h"
#include "TerminalScreen.h"
#include "Views.h"
#include "ClipboardDriver.h"
#include "MainWindow.h"
#include "SessionPane.h"
#include "GridDriver.h"
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontMetricsF>
#include <QFontDatabase>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QJsonArray>
#include <QTimer>
#include <QThread>
#include <QWindow>
#include <cmath>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>

namespace {
QString temporaryPath() { return qEnvironmentVariable("RM_BUILD_DIR") + "/test-data/terminal-appearance-XXXXXX"; }
QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
bool activate(QWidget& widget) {
    widget.show(); if (!QTest::qWaitForWindowExposed(&widget)) return false;
    // Drain native focus changes from the preceding unmap before requesting activation.
    QTest::qWait(100);
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa") activateNativeTestApplication(&widget);
#endif
    widget.raise(); widget.activateWindow(); widget.windowHandle()->requestActivate();
    if (QGuiApplication::platformName() == "offscreen") QApplication::setActiveWindow(&widget);
    return QTest::qWaitForWindowActive(&widget);
}
QImage rendered(TerminalView& view) { return view.grab().toImage().scaled(view.size()); }
QByteArray indexed(int sgr, int index) { return "\x1b[" + QByteArray::number(sgr) + ";5;" + QByteArray::number(index) + "m"; }
QByteArray rgb(int sgr, const QColor& color) {
    return "\x1b[" + QByteArray::number(sgr) + ";2;" + QByteArray::number(color.red()) + ';' + QByteArray::number(color.green()) + ';' + QByteArray::number(color.blue()) + 'm';
}
QColor xtermColor(int index) {
    if (index >= 232) { const int gray = 8 + 10 * (index - 232); return QColor(gray, gray, gray); }
    const int cube = index - 16;
    const auto level = [](int value) { return value ? 55 + 40 * value : 0; };
    return QColor(level(cube / 36), level((cube / 6) % 6), level(cube % 6));
}
TerminalAppearance customAppearance() {
    auto value = TerminalAppearance::classic(); value.foreground = QColor("#123456"); value.background = QColor("#654321");
    for (int index = 0; index < 16; ++index) value.ansi[size_t(index)] = QColor(7 + index * 13, 11 + index * 9, 17 + index * 7);
    return value;
}
}
class TerminalAppearanceTest final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { QTRY_COMPARE_WITH_TIMEOUT(TerminalAppearanceService::activeWorkers(), 0, 5000); }
    void terminalTomorrowNightPresetMatchesScreenshot() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("appearance.json");
        TerminalAppearanceService service(path); QTRY_VERIFY(service.ready()); QCOMPARE(service.current(), TerminalAppearance::classic());
        TerminalAppearance expected; expected.fontSize = 18; expected.cursor = TerminalAppearance::Cursor::Bar; expected.blink = true; expected.boldBright = true;
        expected.foreground = QColor("#eaeaea"); expected.background = QColor("#1d1f21"); expected.selectionForeground = expected.background;
        expected.selectionBackground = expected.foreground; expected.cursorColor = expected.foreground;
        const char* colors[] = {"#1d1f21", "#cc6666", "#b4bd68", "#f0c674", "#81a2be", "#b294bb", "#8abeb7", "#c6c8c6",
            "#666666", "#d54e53", "#b9ca4b", "#e6c547", "#7aa6da", "#c397d8", "#70c0b1", "#eaeaea"};
        for (size_t i = 0; i < 16; ++i) expected.ansi[i] = QColor(colors[i]);
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); auto* preset = dialog.findChild<QComboBox*>("palettePreset"); QVERIFY(preset);
            const auto index = preset->findText("Tomorrow Night"); QVERIFY(index >= 0);
            dialog.findChild<QSpinBox*>("fontSize")->setValue(18); dialog.findChild<QComboBox*>("cursorShape")->setCurrentIndex(2);
            dialog.findChild<QCheckBox*>("cursorBlink")->setChecked(true); dialog.findChild<QComboBox*>("boldStyle")->setCurrentIndex(1);
            preset->setCurrentIndex(index); QCOMPARE(dialog.findChild<TerminalView*>("appearancePreview")->appearance(), expected);
            QCOMPARE(service.current(), TerminalAppearance::classic()); QVERIFY(!QFile::exists(path)); dialog.reject(); QVERIFY(!QFile::exists(path));
        }
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); dialog.findChild<QComboBox*>("palettePreset")->setCurrentText("Tomorrow Night");
            dialog.findChild<QSpinBox*>("fontSize")->setValue(18); dialog.findChild<QComboBox*>("cursorShape")->setCurrentIndex(2);
            dialog.findChild<QCheckBox*>("cursorBlink")->setChecked(true); dialog.findChild<QComboBox*>("boldStyle")->setCurrentIndex(1);
            dialog.findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Apply)->click(); QTRY_VERIFY(service.current() == expected);
            QCOMPARE(TerminalAppearance::decode(contents(path)).value(), expected); dialog.reject();
        }
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); QCOMPARE(dialog.findChild<QComboBox*>("palettePreset")->currentText(), QString("Tomorrow Night"));
            const auto bytes = contents(path); service.setFault(AtomicFile::Fault::BeforeReplace);
            dialog.findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Apply)->click(); QTRY_VERIFY(!service.busy());
            QVERIFY(service.error().isEmpty()); QCOMPARE(contents(path), bytes); dialog.reject();
        }
    }
    void terminalSessionZoomIsLocalBoundedAndSendsNoInput() {
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); TerminalView view, other; view.resize(800, 480); view.initializeTerminal(endpoint); other.initializeTerminal(endpoint);
        view.setInputEnabled(true); QByteArray received; view.setInputSink([&](const QByteArray& bytes) { received += bytes; return true; });
        QVERIFY(activate(view)); view.setFocus(); QTRY_VERIFY(view.hasFocus()); const auto cells = view.screen()->cells();
        QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier); QCOMPARE(view.appearance().fontSize, 13);
        QTest::keyClick(&view, Qt::Key_Equal, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 14);
        QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier); QCOMPARE(view.appearance().fontSize, 15);
        QTest::keyClick(&view, Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 14); QCOMPARE(other.appearance().fontSize, 12);
        QVERIFY(view.screen()->cells() != cells); QVERIFY(received.isEmpty());
        for (int i = 0; i < 60; ++i) QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier);
        QCOMPARE(view.appearance().fontSize, 48); QTest::keyClick(&view, Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 47);
        for (int i = 0; i < 60; ++i) QTest::keyClick(&view, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(view.appearance().fontSize, 6); QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 7);
        QTest::keyClick(&view, Qt::Key_0, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 12); QVERIFY(received.isEmpty());
        view.setInputEnabled(false); QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 13);
        view.setInputEnabled(true);
        QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(view.appearance().fontSize, 13);
        QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier | Qt::MetaModifier); QCOMPARE(view.appearance().fontSize, 13);
#ifdef Q_OS_MACOS
        QTest::keyClick(&view, Qt::Key_Minus, Qt::MetaModifier); QCOMPARE(view.appearance().fontSize, 13);
#endif
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); TerminalAppearanceService service(directory.filePath("appearance.json")); QTRY_VERIFY(service.ready());
        TerminalAppearanceDialog dialog(&service); auto* preview = dialog.findChild<TerminalView*>("appearancePreview"); QVERIFY(preview);
        const auto draft = preview->appearance(); QTest::keyClick(preview, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(preview->appearance(), draft);
        QTest::keyClick(dialog.findChild<QSpinBox*>("fontSize"), Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 13); QCOMPARE(other.appearance().fontSize, 12);
        QVERIFY(!QFile::exists(directory.filePath("appearance.json")));
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0);
    }
    void terminalSessionZoomTracksGlobalAppearance() {
        TerminalView view; view.resize(800, 480); view.initializeTerminal(std::make_shared<TestClipboardEndpoint>()); QVERIFY(activate(view)); view.setFocus();
        QVERIFY(view.append("owned")); QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 13);
        auto global = TerminalAppearance::tomorrowNight(); global.fontSize = 18; global.fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family(); QVERIFY(view.setAppearance(global));
        QCOMPARE(view.appearance().fontSize, 19); QCOMPARE(view.appearance().fontFamily, global.fontFamily); QCOMPARE(view.appearance().foreground, global.foreground);
        QCOMPARE(view.appearance().ansi, global.ansi); QVERIFY(view.screen()->lineText(0).contains("owned"));
        QTest::keyClick(&view, Qt::Key_0, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 18);
        global.fontSize = 48; QVERIFY(view.setAppearance(global)); QTest::keyClick(&view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view.appearance().fontSize, 48);
        global.fontSize = 20; QVERIFY(view.setAppearance(global)); QCOMPARE(view.appearance().fontSize, 20);
    }
    void terminalSessionZoomResetsOnReconnectWithoutPersisting() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto xml = directory.filePath("connections.xml"), settings = directory.filePath("appearance.json");
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); MainWindow window(xml, true, nullptr, endpoint, {}, settings);
        QVERIFY(activate(window)); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        window.addConnection(); QVERIFY(waitGridSaved(window)); setGridText(window, "host", "saved.invalid"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window)); window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* saved = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(saved);
        const auto id = saved->id(); const auto target = saved->target(); const auto before = contents(xml);
        auto* view = saved->findChild<TerminalView*>(); view->initializeTerminal(endpoint); view->setFocus(); QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view->appearance().fontSize, 13);
        window.setQuickConnectProtocol("ssh"); window.quickConnect("temporary.invalid", true);
        auto* temporary = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(temporary); auto* tempView = temporary->findChild<TerminalView*>(); tempView->initializeTerminal(endpoint);
        QCOMPARE(tempView->appearance().fontSize, 12); tabs->setCurrentWidget(saved); window.disconnectCurrent(); QCOMPARE(view->appearance().fontSize, 13);
        window.findChild<QAction*>("actionTerminalAppearance")->trigger(); QTRY_VERIFY(window.findChild<TerminalAppearanceDialog*>());
        auto* dialog = window.findChild<TerminalAppearanceDialog*>(); dialog->findChild<QSpinBox*>("fontSize")->setValue(18);
        dialog->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Ok)->click(); QTRY_COMPARE(view->appearance().fontSize, 19); QCOMPARE(tempView->appearance().fontSize, 18);
        const auto applied = contents(settings); view->setFocus(); QTest::keyClick(view, Qt::Key_Minus, Qt::ControlModifier); QCOMPARE(view->appearance().fontSize, 18);
        QCOMPARE(saved->id(), id); QVERIFY(saved->target() == target); QCOMPARE(contents(xml), before); QCOMPARE(contents(settings), applied);
        window.reconnectCurrent(); QTRY_VERIFY(qobject_cast<SessionPane*>(tabs->currentWidget())->id() != id);
        QCOMPARE(qobject_cast<SessionPane*>(tabs->currentWidget())->findChild<TerminalView*>()->appearance().fontSize, 18);
        tabs->setCurrentWidget(temporary); tempView->setFocus(); QTest::keyClick(tempView, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(tempView->appearance().fontSize, 19);
        const auto temporaryId = temporary->id(); window.reconnectCurrent(); QTRY_VERIFY(qobject_cast<SessionPane*>(tabs->currentWidget())->id() != temporaryId);
        QCOMPARE(qobject_cast<SessionPane*>(tabs->currentWidget())->findChild<TerminalView*>()->appearance().fontSize, 18);
        window.closeTab(0); window.connectSelected(true);
        saved = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(saved); view = saved->findChild<TerminalView*>(); QCOMPARE(view->appearance().fontSize, 18); view->initializeTerminal(endpoint);
        window.setQuickConnectProtocol("rdp"); window.quickConnect("rdp.invalid", true);
        auto* rdp = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(rdp); auto* desktop = rdp->findChild<DesktopView*>(); QVERIFY(desktop);
        QImage frame(32, 24, QImage::Format_RGB32); frame.fill(Qt::red); desktop->setFrame(frame); const auto rdpSize = desktop->size(); const auto rdpId = rdp->id();
        tabs->setCurrentWidget(saved); view->setFocus(); QTest::keyClick(view, Qt::Key_Plus, Qt::ControlModifier); QCOMPARE(view->appearance().fontSize, 19);
        QCOMPARE(desktop->frame(), frame); QCOMPARE(desktop->size(), rdpSize); QCOMPARE(rdp->id(), rdpId);
        QCOMPARE(contents(xml), before); QCOMPARE(contents(settings), applied); QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); window.close();
    }
    void terminalAnsiIndexedColorsRespectThemes_data() {
        QTest::addColumn<int>("index"); QTest::addColumn<QString>("theme");
        for (const auto& theme : {QString("classic"), QString("tomorrow"), QString("original"), QString("custom")})
            for (int index = 0; index < 16; ++index) QTest::newRow(qPrintable(theme + '-' + QString::number(index))) << index << theme;
    }
    void terminalAnsiIndexedColorsRespectThemes() {
        QFETCH(int, index); QFETCH(QString, theme); TerminalScreen screen(2, 8);
        auto value = theme == "tomorrow" ? TerminalAppearance::tomorrowNight() : theme == "original" ? TerminalAppearance::original() :
            theme == "custom" ? customAppearance() : TerminalAppearance::classic();
        QVERIFY(screen.setAppearance(value)); QVERIFY(screen.feed(indexed(38, index) + indexed(48, index) + 'X'));
        QCOMPARE(screen.cell(0, 0).foreground, value.ansi[size_t(index)]); QCOMPARE(screen.cell(0, 0).background, value.ansi[size_t(index)]);
        value = customAppearance(); value.ansi[size_t(index)] = QColor("#abcdef"); QVERIFY(screen.setAppearance(value));
        QCOMPARE(screen.cell(0, 0).foreground, value.ansi[size_t(index)]); QCOMPARE(screen.cell(0, 0).background, value.ansi[size_t(index)]);
        QCOMPARE(screen.cell(0, 0).text, QString("X"));
    }
    void terminalExtendedIndexedColorsMatchXterm_data() {
        QTest::addColumn<int>("index"); QTest::addColumn<QColor>("expected");
        for (int index = 16; index < 256; ++index) {
            auto expected = xtermColor(index);
            switch (index) {
            case 16: expected = QColor("#000000"); break;
            case 28: expected = QColor("#008700"); break;
            case 231: expected = QColor("#ffffff"); break;
            case 232: expected = QColor("#080808"); break;
            case 255: expected = QColor("#eeeeee"); break;
            }
            QTest::newRow(qPrintable(QString::number(index))) << index << expected;
        }
    }
    void terminalExtendedIndexedColorsMatchXterm() {
        QFETCH(int, index); QFETCH(QColor, expected); TerminalScreen screen(2, 8);
        auto value = customAppearance(); value.boldBright = true; QVERIFY(screen.setAppearance(value));
        QVERIFY(screen.feed(indexed(38, index) + indexed(48, index) + 'I' + rgb(38, expected) + rgb(48, expected) + 'T' +
            "\x1b[1m" + indexed(38, index) + indexed(48, index) + 'B'));
        for (int column = 0; column < 3; ++column) {
            QCOMPARE(screen.cell(0, column).foreground, expected); QCOMPARE(screen.cell(0, column).background, expected);
        }
        QVERIFY(screen.cell(0, 2).bold);
    }
    void terminalTruecolorAndSgrResetsPreserveRgb() {
        TerminalScreen screen(2, 20); const auto value = customAppearance(); QVERIFY(screen.setAppearance(value));
        const QColor foreground(12, 135, 247), background(213, 64, 19);
        QVERIFY(screen.feed(rgb(38, foreground) + rgb(48, background) + "A\x1b[39mB\x1b[49mC" +
            indexed(38, 28) + indexed(48, 232) + "D\x1b[39mE\x1b[49mF" +
            rgb(38, foreground) + rgb(48, background) + "\x1b[1;3mG\x1b[0mH" +
            indexed(38, 28) + rgb(48, background) + "I\x1b[49mJ\x1b[0mK"));
        QCOMPARE(screen.cell(0, 0).foreground, foreground); QCOMPARE(screen.cell(0, 0).background, background);
        QCOMPARE(screen.cell(0, 1).foreground, value.foreground); QCOMPARE(screen.cell(0, 1).background, background);
        QCOMPARE(screen.cell(0, 3).foreground, QColor("#008700")); QCOMPARE(screen.cell(0, 3).background, QColor("#080808"));
        QCOMPARE(screen.cell(0, 4).foreground, value.foreground); QCOMPARE(screen.cell(0, 4).background, QColor("#080808"));
        QCOMPARE(screen.cell(0, 6).foreground, foreground); QCOMPARE(screen.cell(0, 6).background, background);
        QVERIFY(screen.cell(0, 6).bold && screen.cell(0, 6).italic);
        for (int column : {2, 5, 7, 10}) {
            QCOMPARE(screen.cell(0, column).foreground, value.foreground); QCOMPARE(screen.cell(0, column).background, value.background);
            QVERIFY(!screen.cell(0, column).bold && !screen.cell(0, column).italic);
        }
        for (int column : {8, 9}) QCOMPARE(screen.cell(0, column).foreground, QColor("#008700"));
        QCOMPARE(screen.cell(0, 8).background, background); QCOMPARE(screen.cell(0, 9).background, value.background);
    }
    void terminalExtendedColorsSurviveThemeAndBufferChanges() {
        TerminalScreen screen(4, 30); const QColor direct(12, 135, 247);
        const auto sample = indexed(38, 2) + indexed(48, 2) + 'A' + indexed(38, 28) + indexed(48, 28) + 'I' +
            indexed(38, 232) + indexed(48, 232) + 'G' + rgb(38, direct) + rgb(48, direct) + "T\x1b[0mD";
        QVERIFY(screen.feed(sample + "\r\n1\r\n2\r\n3\r\n")); QVERIFY(screen.historyLines() > 0);
        QVERIFY(screen.feed("\x1b[H" + sample));
        auto value = customAppearance(); QVERIFY(screen.setAppearance(value));
        const auto check = [&](const TerminalCell& cell, const QColor& expected) {
            return cell.foreground == expected && cell.background == expected;
        };
        const auto verify = [&] {
            QVERIFY(check(screen.cell(0, 0), value.ansi[2])); QVERIFY(check(screen.lineCell(0, 0), value.ansi[2]));
            for (int column = 1; column < 4; ++column) {
                const auto expected = column == 1 ? QColor("#008700") : column == 2 ? QColor("#080808") : direct;
                QVERIFY(check(screen.cell(0, column), expected)); QVERIFY(check(screen.lineCell(0, column), expected));
            }
            QCOMPARE(screen.cell(0, 4).foreground, value.foreground); QCOMPARE(screen.cell(0, 4).background, value.background);
            QCOMPARE(screen.lineCell(0, 4).foreground, value.foreground); QCOMPARE(screen.lineCell(0, 4).background, value.background);
            QCOMPARE(screen.lineText(0), QString("AIGTD"));
        };
        verify();
        QVERIFY(screen.feed("\x1b[?1049h\x1b[H" + sample)); value = TerminalAppearance::tomorrowNight(); QVERIFY(screen.setAppearance(value));
        QVERIFY(screen.alternate());
        QVERIFY(check(screen.cell(0, 0), value.ansi[2])); QVERIFY(check(screen.cell(0, 1), QColor("#008700")));
        QVERIFY(check(screen.cell(0, 2), QColor("#080808"))); QVERIFY(check(screen.cell(0, 3), direct));
        QCOMPARE(screen.cell(0, 4).foreground, value.foreground); QCOMPARE(screen.cell(0, 4).background, value.background);
        QVERIFY(screen.feed("\x1b[?1049l")); QVERIFY(!screen.alternate()); verify();
    }
    void terminalIndexedColorsRenderLikeTruecolor() {
        auto endpoint = std::make_shared<TestClipboardEndpoint>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(endpoint);
        auto value = customAppearance(); value.blink = false; QVERIFY(view.setAppearance(value)); QVERIFY(activate(view));
        const int indices[] = {2, 16, 28, 231, 232, 255}; QByteArray sample("\x1b[?25l\x1b[H");
        for (int index : indices) {
            const auto expected = index < 16 ? value.ansi[size_t(index)] : xtermColor(index);
            sample += indexed(48, index) + ' ' + rgb(48, expected) + ' ';
        }
        sample += "\x1b[0m\x1b[2;1H\x1b[7m";
        for (int index : indices) {
            const auto expected = index < 16 ? value.ansi[size_t(index)] : xtermColor(index);
            sample += indexed(38, index) + ' ' + rgb(38, expected) + ' ';
        }
        sample += "\x1b[0m\x1b[3;1H \x1b[7m \x1b[0m"; QVERIFY(view.append(sample));
        const QFontMetricsF metrics(view.font()); const int width = int(std::ceil(metrics.horizontalAdvance('M'))), height = int(std::ceil(metrics.height()));
        const auto image = rendered(view);
        for (int pair = 0; pair < 6; ++pair) {
            const int index = indices[pair]; const auto expected = index < 16 ? value.ansi[size_t(index)] : xtermColor(index);
            for (int row = 0; row < 2; ++row) for (int column : {2 * pair, 2 * pair + 1}) {
                QCOMPARE(view.screen()->cell(row, column).background, expected);
                QCOMPARE(image.pixelColor(column * width + width / 2, row * height + height / 2), expected);
            }
        }
        QCOMPARE(image.pixelColor(width / 2, 2 * height + height / 2), value.background);
        QCOMPARE(image.pixelColor(width + width / 2, 2 * height + height / 2), value.foreground);
        QCOMPARE(endpoint->reads, 0); QCOMPARE(endpoint->writes, 0); QCOMPARE(endpoint->watches, 0);
    }
    void terminalClassicPaletteUsesSofterAnsiColors() {
        const auto appearance = TerminalAppearance::classic(); TerminalScreen screen(4, 40);
        QVERIFY(screen.feed("\x1b[31mR\x1b[32mG\x1b[34mB\x1b[91mH\x1b[0mD"));
        QCOMPARE(screen.cell(0, 0).foreground, QColor(187, 0, 0)); QCOMPARE(screen.cell(0, 1).foreground, QColor(0, 187, 0));
        QCOMPARE(screen.cell(0, 2).foreground, QColor(0, 0, 187)); QCOMPARE(screen.cell(0, 3).foreground, appearance.ansi[9]);
        QCOMPARE(screen.cell(0, 4).foreground, appearance.foreground);
        QVERIFY(screen.feed("\x1b[38;2;12;34;56mT\x1b[38;5;200mE\x1b[0;1;31mN"));
        const auto extended = screen.cell(0, 6).foreground;
        auto changed = appearance; changed.ansi[1] = QColor("#123456"); changed.boldBright = true; QVERIFY(screen.setAppearance(changed));
        QCOMPARE(screen.cell(0, 5).foreground, QColor(12, 34, 56)); QCOMPARE(screen.cell(0, 6).foreground, extended);
        QCOMPARE(screen.cell(0, 7).foreground, changed.ansi[9]); QVERIFY(screen.cell(0, 7).bold);
        const std::array<QColor, 16> expected{{QColor(0, 0, 0), QColor(187, 0, 0), QColor(0, 187, 0), QColor(187, 187, 0),
            QColor(0, 0, 187), QColor(187, 0, 187), QColor(0, 187, 187), QColor(187, 187, 187),
            QColor(85, 85, 85), QColor(255, 0, 0), QColor(0, 255, 0), QColor(255, 255, 0),
            QColor(0, 0, 255), QColor(255, 0, 255), QColor(0, 255, 255), QColor(255, 255, 255)}};
        QCOMPARE(appearance.ansi, expected); QCOMPARE(appearance.foreground, QColor(187, 187, 187)); QCOMPARE(appearance.background, QColor(0, 0, 0));
        TerminalScreen swatches(2, 40);
        for (int index = 0; index < 16; ++index) QVERIFY(swatches.feed(QByteArray("\x1b[") + QByteArray::number(index < 8 ? 30 + index : 90 + index - 8) + "mX"));
        for (int index = 0; index < 16; ++index) QCOMPARE(swatches.cell(0, index).foreground, expected[size_t(index)]);
        QVERIFY(swatches.setAppearance(TerminalAppearance::original()));
        for (int index = 0; index < 16; ++index) QCOMPARE(swatches.cell(0, index).foreground, TerminalAppearance::original().ansi[size_t(index)]);
    }
    void terminalAppearanceUpdatesExistingCellsAndHistory() {
        auto clipboard = std::make_shared<TestClipboardEndpoint>(); TerminalView view; view.resize(600, 180); view.initializeTerminal(clipboard);
        const auto rows = view.screen()->cells().height();
        QVERIFY(view.append("\x1b[31mowned red\x1b[0m default\r\n"));
        for (int row = 0; row < rows; ++row) QVERIFY(view.append("owned default\r\n"));
        QVERIFY(view.screen()->historyLines() > 0); const auto cursor = view.screen()->cursor(); const auto history = view.screen()->historyLines();
        view.selectCells(QPoint(0, 0), QPoint(4, 0));
        auto value = view.appearance(); value.foreground = QColor("#abc123"); value.background = QColor("#182838"); value.ansi[1] = QColor("#723456");
        QVERIFY(view.setAppearance(value)); QCOMPARE(view.screen()->cursor(), cursor); QCOMPARE(view.screen()->historyLines(), history);
        QCOMPARE(view.screen()->lineCell(0, 0).foreground, value.ansi[1]); QCOMPARE(view.screen()->lineCell(0, 10).foreground, value.foreground);
        QCOMPARE(view.screen()->lineCell(0, 0).background, value.background); QVERIFY(view.copySelection()); QCOMPARE(clipboard->text.value_or(QString()), QString("owned"));
        QVERIFY(view.append("\x1b[?1049h\x1b[0mALT")); value.foreground = QColor("#998877"); QVERIFY(view.setAppearance(value));
        QVERIFY(view.screen()->alternate()); QCOMPARE(view.screen()->cell(0, 0).foreground, value.foreground);
        QVERIFY(view.append("\x1b[?1049l")); QCOMPARE(view.screen()->lineCell(0, 10).foreground, value.foreground);
        QCOMPARE(clipboard->reads, 0);
        const int lineHeight = int(std::ceil(QFontMetricsF(view.font()).height())); view.resize(600, lineHeight * 8 + 1);
        view.show(); QTest::qWait(100); QVERIFY(view.append("\x1b[H\x1b[41mX"));
        QVERIFY(view.screen()->cell(0, 0).background != value.background);
        QCOMPARE(rendered(view).pixelColor(0, view.height() - 1), value.background);
    }
    void terminalFontAppearanceResizesLogicalCells() {
        TerminalView view; view.resize(800, 480); view.initializeTerminal(std::make_shared<TestClipboardEndpoint>()); view.show(); QTest::qWait(10);
        const auto previous = view.screen()->cells(); QSignalSpy resize(&view, &TerminalView::cellsChanged);
        auto value = view.appearance(); value.fontSize = 24; QVERIFY(view.setAppearance(value));
        QVERIFY(view.screen()->cells().width() < previous.width()); QVERIFY(view.screen()->cells().height() < previous.height()); QCOMPARE(resize.size(), 1);
        QVERIFY(view.setAppearance(value)); QCOMPARE(resize.size(), 1);
        value.fontSize = 49; QVERIFY(!view.setAppearance(value)); QCOMPARE(resize.size(), 1);
    }
    void terminalCursorAndSelectionAppearance() {
        auto clipboard = std::make_shared<TestClipboardEndpoint>(); TerminalView view; view.resize(400, 160); view.initializeTerminal(clipboard);
        QVERIFY(activate(view)); view.setFocus(); QTRY_VERIFY(view.hasFocus());
        QVERIFY(view.append("owned")); view.selectCells(QPoint(0, 0), QPoint(4, 0));
        const QFontMetricsF metrics(view.font()); const int width = int(std::ceil(metrics.horizontalAdvance('M'))), height = int(std::ceil(metrics.height()));
        auto value = view.appearance(); value.cursorColor = QColor("#112233"); value.selectionBackground = QColor("#345678");
        for (auto cursor : {TerminalAppearance::Cursor::Block, TerminalAppearance::Cursor::Underline, TerminalAppearance::Cursor::Bar}) {
            value.cursor = cursor; QVERIFY(view.setAppearance(value)); const auto image = rendered(view);
            QCOMPARE(image.pixelColor(0, height - 1), value.selectionBackground);
            int colored = 0;
            for (int y = 0; y < height; ++y) for (int x = 5 * width; x < 6 * width; ++x) if (image.pixelColor(x, y) == value.cursorColor) ++colored;
            QCOMPARE(colored, cursor == TerminalAppearance::Cursor::Block ? width * height : cursor == TerminalAppearance::Cursor::Underline ? width * 2 : height * 2);
            QVERIFY(view.copySelection()); QCOMPARE(clipboard->text.value_or(QString()), QString("owned"));
        }
        value.cursor = TerminalAppearance::Cursor::Block; value.blink = true; QVERIFY(view.setAppearance(value));
        auto* timer = view.findChild<QTimer*>("terminalCursorBlink"); QVERIFY(timer); QVERIFY(timer->isActive());
        QSignalSpy ticks(timer, &QTimer::timeout); QTRY_VERIFY(ticks.size() > 0);
        QCOMPARE(rendered(view).pixelColor(5 * width + width / 2, height / 2), value.background);
        view.clearFocus(); QVERIFY(!timer->isActive()); view.setFocus(); QTRY_VERIFY(timer->isActive());
        view.hide(); QVERIFY(!timer->isActive()); QTRY_VERIFY(!view.windowHandle()->isExposed()); QTRY_VERIFY(!view.isActiveWindow());
        QVERIFY(activate(view)); view.setFocus();
        QTRY_VERIFY2(view.hasFocus(), qPrintable(QString("active=%1 exposed=%2 focusWidget=%3 windowFocus=%4").arg(view.isActiveWindow()).arg(view.windowHandle()->isExposed()).arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : "null").arg(view.focusWidget() ? view.focusWidget()->objectName() : "null")));
        QTRY_VERIFY2(timer->isActive(), qPrintable(QString("visible=%1 focus=%2 cursor=%3 blink=%4").arg(view.isVisible()).arg(view.hasFocus()).arg(view.screen()->cursorVisible()).arg(view.appearance().blink)));
        value.blink = false; QVERIFY(view.setAppearance(value)); QVERIFY(!timer->isActive());
        QVERIFY(view.copySelection()); view.setInputEnabled(false); QVERIFY(!view.pasteClipboard()); QCOMPARE(clipboard->reads, 0);
        QVERIFY(view.append("\x1b[2;1H\xe7\x95\x8c\x1b[2;1H"));
        const auto wide = rendered(view); QCOMPARE(wide.pixelColor(2 * width - 1, 2 * height - 1), value.cursorColor);
        QCOMPARE(wide.pixelColor(2 * width, 2 * height - 1), value.background);
    }
    void terminalAppearancePersistsSeparatelyAndRejectsInvalidFiles() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("appearance.json");
        auto changed = TerminalAppearance::classic(); changed.fontSize = 18; changed.ansi[2] = QColor("#448855");
        {
            TerminalAppearanceService service(path); QTRY_VERIFY(service.ready()); QVERIFY(service.error().isEmpty()); QVERIFY(!QFile::exists(path));
            QVERIFY(service.performedOn() != QThread::currentThreadId());
            service.apply(service.current()); QTRY_VERIFY(!service.busy()); QVERIFY(!QFile::exists(path));
            service.apply(changed); QTRY_VERIFY(!service.busy()); QVERIFY(service.error().isEmpty()); QCOMPARE(service.current(), changed);
            QVERIFY(TerminalAppearance::decode(contents(path)) == changed);
        }
        {
            TerminalAppearanceService service(path); QTRY_VERIFY(service.ready()); QCOMPARE(service.current(), changed);
            const auto bytes = contents(path); service.apply(changed); QTRY_VERIFY(!service.busy()); QCOMPARE(contents(path), bytes);
        }
        QVERIFY(write(path, "{bad-json"));
        {
            TerminalAppearanceService service(path); QTRY_VERIFY(service.ready()); QVERIFY(!service.error().isEmpty());
            service.apply(changed); QTRY_VERIFY(!service.busy()); QCOMPARE(contents(path), QByteArray("{bad-json"));
            service.apply(changed, true); QTRY_VERIFY(!service.busy()); QCOMPARE(service.current(), changed); QVERIFY(service.error().isEmpty());
        }
        for (const auto& bytes : {QByteArray(65537, 'x'), QByteArray("{}"), QByteArray("{\"schema\":2}")}) QVERIFY(!TerminalAppearance::decode(bytes));
        const auto encoded = QJsonDocument::fromJson(changed.encode()).object();
        for (const auto& field : {QString("schema"), QString("font_size"), QString("cursor"), QString("blink"), QString("bold_bright"), QString("foreground"), QString("ansi")}) {
            auto object = encoded; object[field] = QJsonValue("invalid-type"); QVERIFY(!TerminalAppearance::decode(QJsonDocument(object).toJson()));
        }
        for (const auto& field : {QString("font_size"), QString("cursor")}) {
            auto object = encoded; object[field] = 1.5; QVERIFY(!TerminalAppearance::decode(QJsonDocument(object).toJson()));
        }
        auto unknown = encoded; unknown["unexpected"] = true; QVERIFY(!TerminalAppearance::decode(QJsonDocument(unknown).toJson()));
        auto invalid = changed; invalid.fontSize = 0; QVERIFY(!invalid.valid()); invalid = changed; invalid.ansi[0] = QColor(); QVERIFY(!invalid.valid());
        const auto before = contents(path);
        {
            TerminalAppearanceService service(path, {AtomicFile::Fault::BeforeReplace}); QTRY_VERIFY(service.ready());
            service.apply(TerminalAppearance::classic()); QTRY_VERIFY(!service.busy()); QVERIFY(!service.error().isEmpty()); QCOMPARE(service.current(), changed); QCOMPARE(contents(path), before);
        }
        {
            TerminalAppearanceService service(path, {AtomicFile::Fault::DirectorySync}); QTRY_VERIFY(service.ready());
            service.apply(TerminalAppearance::classic()); QTRY_VERIFY(!service.busy()); QCOMPARE(service.current(), TerminalAppearance::classic()); QVERIFY(!service.warning().isEmpty());
        }
        const auto unsafe = directory.filePath("unsafe.json"); QVERIFY(QFile::link(path, unsafe));
        {
            TerminalAppearanceService service(unsafe); QTRY_VERIFY(service.ready()); QVERIFY(!service.error().isEmpty());
            service.apply(changed, true); QTRY_VERIFY(!service.busy()); QVERIFY(!service.error().isEmpty()); QCOMPARE(contents(path), TerminalAppearance::classic().encode());
        }
    }
    void terminalAppearanceDraftApplyCancelAndReset() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto path = directory.filePath("appearance.json");
        TerminalAppearanceService service(path); QTRY_VERIFY(service.ready());
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); auto* size = dialog.findChild<QSpinBox*>("fontSize"); QVERIFY(size);
            size->setValue(20); QCOMPARE(service.current().fontSize, 12); QVERIFY(!QFile::exists(path));
            QCOMPARE(dialog.findChild<TerminalView*>("appearancePreview")->appearance().fontSize, 20); dialog.reject(); QVERIFY(!QFile::exists(path));
        }
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); dialog.findChild<QSpinBox*>("fontSize")->setValue(20);
            auto* buttons = dialog.findChild<QDialogButtonBox*>("buttonBox"); buttons->button(QDialogButtonBox::Apply)->click(); QTRY_COMPARE(service.current().fontSize, 20);
            const auto saved = contents(path); dialog.findChild<QPushButton*>("resetDefaults")->click(); QCOMPARE(dialog.findChild<QSpinBox*>("fontSize")->value(), 12);
            QCOMPARE(service.current().fontSize, 20); QCOMPARE(contents(path), saved); dialog.reject(); QCOMPARE(contents(path), saved);
        }
        {
            TerminalAppearanceDialog dialog(&service); dialog.show(); auto* size = dialog.findChild<QSpinBox*>("fontSize");
            auto* buttons = dialog.findChild<QDialogButtonBox*>("buttonBox"); const auto saved = contents(path);
            service.setFault(AtomicFile::Fault::BeforeReplace); buttons->button(QDialogButtonBox::Apply)->click();
            QTRY_VERIFY(!service.busy()); QVERIFY(service.error().isEmpty()); QCOMPARE(contents(path), saved);
            size->setValue(24); buttons->button(QDialogButtonBox::Ok)->click(); QTRY_VERIFY(!service.busy());
            QVERIFY(!service.error().isEmpty()); QVERIFY(dialog.isVisible()); QCOMPARE(size->value(), 24);
            QCOMPARE(service.current().fontSize, 20); QCOMPARE(contents(path), saved); QVERIFY(!dialog.findChild<QLabel*>("statusLabel")->text().isEmpty());
            service.setFault(AtomicFile::Fault::DirectorySync); buttons->button(QDialogButtonBox::Ok)->click();
            QTRY_COMPARE(service.current().fontSize, 24); QTRY_VERIFY(dialog.findChild<QMessageBox*>());
            QVERIFY(dialog.isVisible()); QVERIFY(!service.warning().isEmpty()); QCOMPARE(TerminalAppearance::decode(contents(path))->fontSize, 24);
            dialog.findChild<QMessageBox*>()->button(QMessageBox::Ok)->click(); QTRY_VERIFY(!dialog.isVisible());
            QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(service.warning().isEmpty());
        }
    }
    void terminalAppearanceUpdatesAllSshTabsWithoutRetargeting() {
        QTemporaryDir directory(temporaryPath()); QVERIFY(directory.isValid()); const auto xml = directory.filePath("connections.xml"), settings = directory.filePath("appearance.json");
        const auto trust = directory.filePath("trust.json"); QVERIFY(write(trust, "owned untouched trust bytes")); const auto trustBytes = contents(trust);
        auto clipboard = std::make_shared<TestClipboardEndpoint>(); MainWindow window(xml, true, nullptr, clipboard, {}, settings);
        window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        window.addConnection(); QVERIFY(waitGridSaved(window)); setGridText(window, "host", "saved.invalid"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window)); window.connectSelected(true);
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); auto* saved = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(saved);
        const auto savedId = saved->id(); const auto savedTarget = saved->target(); const auto xmlBytes = contents(xml);
        window.setQuickConnectProtocol("ssh");
        window.quickConnect("first.invalid", true); auto* first = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(first);
        window.quickConnect("second.invalid", true); auto* second = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(second);
        const auto firstId = first->id(), secondId = second->id(); const auto firstTarget = first->target(), secondTarget = second->target(); window.disconnectCurrent();
        window.setQuickConnectProtocol("rdp"); window.quickConnect("rdp.invalid", true);
        auto* rdp = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(rdp); const auto rdpId = rdp->id(); const auto rdpTarget = rdp->target();
        auto* desktop = rdp->findChild<DesktopView*>(); QVERIFY(desktop); QImage frame(32, 24, QImage::Format_RGB32); frame.fill(Qt::red); desktop->setFrame(frame);
        window.findChild<QAction*>("actionOptions")->trigger(); QTRY_VERIFY(window.findChild<TerminalAppearanceDialog*>());
        auto* dialog = window.findChild<TerminalAppearanceDialog*>(); window.findChild<QAction*>("actionTerminalAppearance")->trigger();
        QCOMPARE(window.findChildren<TerminalAppearanceDialog*>().size(), 1); QVERIFY(!QFile::exists(settings));
        dialog->findChild<QSpinBox*>("fontSize")->setValue(18); QCOMPARE(first->findChild<TerminalView*>()->appearance().fontSize, 12);
        dialog->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Apply)->click();
        QTRY_COMPARE(first->findChild<TerminalView*>()->appearance().fontSize, 18); QCOMPARE(second->findChild<TerminalView*>()->appearance().fontSize, 18);
        QCOMPARE(saved->findChild<TerminalView*>()->appearance().fontSize, 18); QCOMPARE(saved->id(), savedId); QVERIFY(saved->target() == savedTarget);
        QCOMPARE(first->id(), firstId); QCOMPARE(second->id(), secondId); QVERIFY(first->target() == firstTarget && second->target() == secondTarget);
        QCOMPARE(rdp->id(), rdpId); QVERIFY(rdp->target() == rdpTarget); QCOMPARE(desktop->frame(), frame);
        QCOMPARE(contents(xml), xmlBytes); QCOMPARE(contents(trust), trustBytes); dialog->reject(); QTRY_VERIFY(!window.findChild<TerminalAppearanceDialog*>());
        auto* view = first->findChild<TerminalView*>(); view->initializeTerminal(clipboard);
        QContextMenuEvent context(QContextMenuEvent::Mouse, QPoint(2, 2), view->mapToGlobal(QPoint(2, 2))); QApplication::sendEvent(view, &context);
        auto* menu = view->findChild<QMenu*>(); QVERIFY(menu); auto* action = menu->findChild<QAction*>("actionTerminalAppearance"); QVERIFY(action);
        action->trigger(); menu->close(); QTRY_VERIFY(window.findChild<TerminalAppearanceDialog*>());
        window.findChild<TerminalAppearanceDialog*>()->reject(); QTRY_VERIFY(!window.findChild<TerminalAppearanceDialog*>());
        const auto point = tabs->tabBar()->tabRect(tabs->indexOf(saved)).center();
        QContextMenuEvent tabContext(QContextMenuEvent::Mouse, point, tabs->tabBar()->mapToGlobal(point)); QApplication::sendEvent(tabs->tabBar(), &tabContext);
        auto* tabMenu = window.findChild<QMenu*>("sessionContextMenu"); QVERIFY(tabMenu);
        auto* tabAppearance = tabMenu->findChild<QAction*>("actionTerminalAppearance"); QVERIFY(tabAppearance && tabAppearance->isEnabled());
        tabAppearance->trigger(); tabMenu->close(); QTRY_VERIFY(window.findChild<TerminalAppearanceDialog*>());
        window.findChild<TerminalAppearanceDialog*>()->reject(); QTRY_VERIFY(!window.findChild<TerminalAppearanceDialog*>());
        tabs->setCurrentWidget(saved); window.reconnectCurrent();
        QTRY_VERIFY(qobject_cast<SessionPane*>(tabs->currentWidget())->id() != savedId);
        auto* reconnect = qobject_cast<SessionPane*>(tabs->currentWidget()); QVERIFY(reconnect->target() == savedTarget);
        QCOMPARE(reconnect->findChild<TerminalView*>()->appearance().fontSize, 18);
        window.setQuickConnectProtocol("ssh"); window.quickConnect("third.invalid", true);
        QCOMPARE(qobject_cast<SessionPane*>(tabs->currentWidget())->findChild<TerminalView*>()->appearance().fontSize, 18);
        QCOMPARE(contents(xml), xmlBytes); QCOMPARE(contents(trust), trustBytes); QCOMPARE(clipboard->reads, 0); QCOMPARE(clipboard->writes, 0); QCOMPARE(clipboard->watches, 0);
        window.close(); QTRY_VERIFY(!window.isVisible()); QCOMPARE(window.sessionCount(), size_t(0));
    }
};
QTEST_MAIN(TerminalAppearanceTest)
#include "terminal_appearance.moc"
