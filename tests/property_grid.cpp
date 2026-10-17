#include "MainWindow.h"
#include "Appearance.h"
#include "ProfileEditor.h"
#include "DocumentService.h"
#include "ConnectionIcons.h"
#include "SessionPane.h"
#include "OptionsDialog.h"
#include <QCheckBox>
#include <QDir>
#include <QTabWidget>
#include <QScrollBar>
#include <QHeaderView>
#include "GridDriver.h"
#include <QAction>
#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QAbstractButton>
#include <QDockWidget>
#include <QStyle>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#define OPEN_CONNECTION(window) do { window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); window.addConnection(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window); } while (false)
// Inheritance needs a parent folder.
#define OPEN_FOLDERED_CONNECTION(window) do { window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open); window.addFolder(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window); window.addConnection(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window); } while (false)
// Default modes belong to the connections file root.
#define SELECT_FILE_ROOT(window) QVERIFY((window).selectConnectionFile((window).selectedDocument()))
static QByteArray contents(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
class SaveWindow : public MainWindow {
    std::shared_ptr<std::atomic<int>> writes_ = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<bool>> fail_ = std::make_shared<std::atomic<bool>>(false);
public:
    SaveWindow(const QString& path, bool mock, const QString& preferences = {}) : MainWindow(path, mock, nullptr, {}, {}, {}, preferences) {
        if (!QTest::qWaitFor([&] { return documentState() == DocumentState::Open; })) qFatal("XML did not open");
        if (!setDocumentGate(*this, [writes = writes_, fail = fail_](const auto& commit) {
            ++*writes; return !fail->exchange(false) && commit();
        })) qFatal("XML commit gate was not installed");
    }
    int writes() const { return writes_->load(); }
    void failNext() { *fail_ = true; }
};
class PropertyGridTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { applyLightAppearance(*qApp); qApp->setQuitOnLastWindowClosed(false); }
    void configValueCellsStayVisibleInNarrowDock() {
        QTemporaryDir dir; SaveWindow window(dir.filePath("narrow.xml"), true, dir.filePath("preferences.json")); OPEN_FOLDERED_CONNECTION(window);
        auto* tree = window.findChild<QTreeWidget*>("configTree"); auto* dock = window.findChild<QDockWidget*>("configDock");
        const auto connection = window.selectedId(); const auto document = window.selectedDocument();
        auto* defaults = window.findChild<QAction*>("actionDefaultProperties");
        const int writes = window.writes();
        for (int width : {226, 330}) {
            window.resizeDocks({dock}, {width}, Qt::Horizontal); QTRY_COMPARE(dock->width(), width);
            for (const auto* mode : {"actionProperties", "actionInheritance", "actionDefaultProperties", "actionDefaultInheritance"}) {
                if (QString(mode).startsWith("actionDefault")) { QVERIFY(window.selectConnectionFile(document)); QTRY_VERIFY(defaults->isEnabled()); }
                else QVERIFY(window.selectConnectionProfile({document, connection}));
                QVERIFY(window.findChild<QAction*>(mode)->isEnabled()); window.findChild<QAction*>(mode)->trigger();
                for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                    auto* row = *it;
                    if (row->isHidden() || !(row->flags() & Qt::ItemIsEditable)) continue;
                    if (row->parent()) row->parent()->setExpanded(true);
                    tree->scrollToItem(row); tree->setCurrentItem(row, 1); tree->scrollTo(tree->currentIndex());
                    // The start of the row stays in view: the grid never scrolls sideways.
                    QCOMPARE(tree->horizontalScrollBar()->value(), 0); QCOMPARE(tree->columnViewportPosition(0), 0);
                    QTRY_VERIFY(tree->columnWidth(0) + tree->columnWidth(1) <= tree->viewport()->width());
                    QTRY_VERIFY2(tree->viewport()->rect().contains(QPoint(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2,
                        tree->visualItemRect(row).center().y())), qPrintable(row->text(0)));
                }
            }
        }
        QCOMPARE(window.writes(), writes);
        window.resizeDocks({dock}, {226}, Qt::Horizontal); QVERIFY(window.selectConnectionProfile({document, connection}));
        setGridText(window, "name", "Narrow Config"); QVERIFY(waitGridSaved(window));
        QCOMPARE(gridValue(window, "name").toString(), QString("Narrow Config"));
    }
    void defaultConfigModesDoNotEditSelectedProfile() {
        QTemporaryDir dir; const auto path = dir.filePath("modes.xml"), prefs = dir.filePath("preferences.json");
        MainWindow window(path, true, nullptr, {}, {}, {}, prefs); OPEN_CONNECTION(window);
        const auto selected = window.selectedId(); const auto before = contents(path); // file remains separate from app defaults
        auto* defaults = window.findChild<QAction*>("actionDefaultProperties"); QVERIFY(!defaults->isEnabled());
        SELECT_FILE_ROOT(window); QTRY_VERIFY(defaults->isEnabled()); defaults->trigger();
        QVERIFY(gridRow(window, "name")->isHidden()); QVERIFY(gridRow(window, "host")->isHidden()); QVERIFY(gridRow(window, "password")->isHidden());
        setGridText(window, "description", "New profiles only"); QVERIFY(waitGridSaved(window));
        QCOMPARE(contents(path), before); QVERIFY(window.documentRootSelected());
        window.findChild<QAction*>("actionDefaultInheritance")->trigger();
        QCOMPARE(gridRow(window, "password")->text(0), QString("Inherit Password"));
        window.findChild<QAction*>("actionProperties")->trigger(); QVERIFY(window.selectConnectionProfile({window.selectedDocument(), selected}));
        QVERIFY(gridValue(window, "description").toString().isEmpty()); QCOMPARE(contents(path), before);
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        QCOMPARE(gridValue(window, "description").toString(), QString("New profiles only"));
        QVERIFY(contents(prefs).contains("connection_defaults"));
    }
    void connectionDefaultsPersistAndApplyOnlyToNewProfiles() {
        QTemporaryDir dir; const auto path = dir.filePath("defaults.xml"), prefs = dir.filePath("preferences.json"); QString existing;
        {
            MainWindow window(path, true, nullptr, {}, {}, {}, prefs); OPEN_CONNECTION(window); existing = window.selectedId();
            setGridText(window, "host", "owned.example"); QVERIFY(waitGridSaved(window));
            window.connectSelected(true); QTRY_COMPARE(window.sessionCount(), size_t(1));
            const auto target = window.findChild<SessionPane*>()->target(); const auto before = contents(path);
            SELECT_FILE_ROOT(window); auto* action = window.findChild<QAction*>("actionDefaultProperties"); QTRY_VERIFY(action->isEnabled()); action->trigger();
            setGridText(window, "description", "Creation template"); QVERIFY(waitGridSaved(window));
            setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
            setGridChoice(window, "icon", "SSH"); QVERIFY(waitGridSaved(window));
            QCOMPARE(gridValue(window, "port").toInt(), 22); QVERIFY(gridRow(window, "domain")->isHidden());
            QCOMPARE(contents(path), before); QCOMPARE(window.findChild<SessionPane*>()->target(), target);
            window.findChild<QAction*>("actionOptions")->trigger(); auto* dialog = window.findChild<OptionsDialog*>(); QVERIFY(dialog);
            dialog->findChild<QCheckBox*>("quickConnect")->setChecked(false); dialog->accept();
            QTRY_VERIFY(!window.findChild<QAction*>("actionViewQuickConnect")->isChecked());
            QCOMPARE(QJsonDocument::fromJson(contents(prefs)).object().value("connection_defaults").toObject().value("values").toObject().value("icon").toString(), QString("SSH"));
            window.findChild<QAction*>("actionDefaultProperties")->trigger();
            setGridText(window, "username", "literal-user"); QVERIFY(waitGridSaved(window));
            QVERIFY(!QJsonDocument::fromJson(contents(prefs)).object().value("quick_connect").toBool());
        }
        MainWindow restart(path, true, nullptr, {}, {}, {}, prefs); restart.show();
        QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        SELECT_FILE_ROOT(restart); QTRY_VERIFY(restart.findChild<QAction*>("actionDefaultProperties")->isEnabled());
        QVERIFY(restart.selectConnectionProfile({restart.selectedDocument(), existing}));
        QVERIFY(gridValue(restart, "description").toString().isEmpty()); QCOMPARE(gridValue(restart, "protocol").toString(), QString("rdp"));
        restart.addFolder(); QVERIFY(waitGridSaved(restart)); closeGridEditors(restart); const auto parent = restart.selectedId();
        QCOMPARE(gridValue(restart, "description").toString(), QString("Creation template")); QVERIFY(gridRow(restart, "host")->isHidden());
        setGridText(restart, "description", "Parent setting"); QVERIFY(waitGridSaved(restart));
        SELECT_FILE_ROOT(restart); restart.findChild<QAction*>("actionDefaultInheritance")->trigger(); setGridChoice(restart, "description", true); QVERIFY(waitGridSaved(restart));
        restart.findChild<QAction*>("actionProperties")->trigger(); QVERIFY(restart.selectConnectionProfile({restart.selectedDocument(), parent})); restart.addConnection(); QVERIFY(waitGridSaved(restart)); closeGridEditors(restart);
        QCOMPARE(gridValue(restart, "parent").toString(), parent); QCOMPARE(gridValue(restart, "description").toString(), QString("Parent setting"));
        QVERIFY(!gridLocal(restart, "description")); QCOMPARE(gridValue(restart, "protocol").toString(), QString("ssh"));
        QCOMPARE(gridValue(restart, "username").toString(), QString("literal-user")); QCOMPARE(gridValue(restart, "icon").toString(), QString("SSH"));
        QCOMPARE(gridValue(restart, "port").toInt(), 22); QVERIFY(gridValue(restart, "host").toString().isEmpty());
    }
    void newConnectionDefaultsLeaveHostnameEmpty() {
        QTemporaryDir dir; const auto path = dir.filePath("parent-endpoint.xml"), prefs = dir.filePath("preferences.json"); QString parent;
        {
            SaveWindow seed(path, true, prefs); seed.show(); seed.addFolder(); QVERIFY(waitGridSaved(seed)); closeGridEditors(seed); parent = seed.selectedId();
            setGridText(seed, "username", "parent-account"); QVERIFY(waitGridSaved(seed));
        }
        // Imported folders can contain a preserved endpoint even though Config hides it.
        auto xml = contents(path); QVERIFY(xml.contains("Hostname=\"\"")); xml.replace("Hostname=\"\"", "Hostname=\"owned.example\"");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(xml), qint64(xml.size())); file.close();
        SaveWindow window(path, true, prefs); window.show(); QVERIFY(window.selectConnectionProfile({window.selectedDocument(), parent}));
        QCOMPARE(gridValue(window, "host").toString(), QString("owned.example")); QVERIFY(gridRow(window, "host")->isHidden());
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        QVERIFY(gridValue(window, "host").toString().isEmpty()); QVERIFY(gridLocal(window, "host"));
        QCOMPARE(gridValue(window, "username").toString(), QString("parent-account")); QCOMPARE(gridValue(window, "parent").toString(), parent);
    }
    void defaultPasswordInheritanceCreationUsesOneReplacement() {
        QTemporaryDir dir; const auto path = dir.filePath("password-policy.xml"), prefs = dir.filePath("preferences.json");
        SaveWindow window(path, true, prefs); window.show(); window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        setGridText(window, "password", "fixturepw"); QVERIFY(waitGridSaved(window)); const auto folder = window.selectedId();
        SELECT_FILE_ROOT(window); auto* action = window.findChild<QAction*>("actionDefaultInheritance"); QTRY_VERIFY(action->isEnabled()); action->trigger();
        setGridChoice(window, "password", true); QVERIFY(waitGridSaved(window));
        const auto writes = window.writes(); window.findChild<QAction*>("actionProperties")->trigger();
        QVERIFY(window.selectConnectionProfile({window.selectedDocument(), folder})); window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        QCOMPARE(window.writes(), writes + 1); QVERIFY(contents(path).contains("InheritPassword=\"true\""));
        const auto id = window.selectedId(); const auto status = window.findChild<DocumentService*>()->status(window.selectedDocument()); QVERIFY(status);
        const auto state = status->passwords.value(id); QVERIFY(state.inherited); QVERIFY(!state.authorized); QVERIFY(state.present);
        QVERIFY(!contents(prefs).contains("fixturepw")); QVERIFY(!contents(path).contains("fixturepw"));
        QVERIFY(!contents(prefs).contains("passphrase"));
        SELECT_FILE_ROOT(window); window.findChild<QAction*>("actionDefaultInheritance")->trigger(); setGridChoice(window, "password", false); QVERIFY(waitGridSaved(window));
        window.findChild<QAction*>("actionProperties")->trigger(); QVERIFY(window.selectConnectionProfile({window.selectedDocument(), folder})); window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        QVERIFY(!window.findChild<DocumentService*>()->status(window.selectedDocument())->passwords.value(window.selectedId()).inherited);
    }
    void defaultSaveFailureRetainsAppliedTemplateAndReverts() {
        QTemporaryDir dir; const auto path = dir.filePath("failure.xml"), prefs = dir.filePath("preferences.json");
        MainWindow window(path, true, nullptr, {}, {}, {}, prefs); OPEN_CONNECTION(window);
        SELECT_FILE_ROOT(window); auto* action = window.findChild<QAction*>("actionDefaultProperties"); QTRY_VERIFY(action->isEnabled()); action->trigger();
        setGridText(window, "description", "Applied"); QVERIFY(waitGridSaved(window)); const auto saved = contents(prefs), xml = contents(path);
        QFile file(prefs); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("{}"), qint64(2)); file.close();
        setGridText(window, "description", "Unsaved"); QTRY_VERIFY(!window.lastError().isEmpty()); QVERIFY(window.findChild<ProfileEditor*>()->dirty());
        QCOMPARE(contents(prefs), QByteArray("{}")); QCOMPARE(contents(path), xml);
        window.findChild<QAction*>("actionProperties")->trigger(); auto* recovery = window.findChild<QMessageBox*>("saveRecovery"); QVERIFY(recovery);
        QTest::mouseClick(recovery->button(QMessageBox::Discard), Qt::LeftButton); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->defaultsMode());
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); QCOMPARE(gridValue(window, "description").toString(), QString("Applied"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(saved), qint64(saved.size())); file.close();
        SELECT_FILE_ROOT(window); action->trigger();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("{}"), qint64(2)); file.close();
        setGridText(window, "description", "Retried"); QTRY_VERIFY(window.findChild<ProfileEditor*>()->dirty() && !window.lastError().isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(saved), qint64(saved.size())); file.close();
        window.findChild<QAction*>("actionProperties")->trigger(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery"));
        QTest::mouseClick(window.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Retry), Qt::LeftButton);
        QVERIFY(waitGridSaved(window)); QTRY_VERIFY(!window.findChild<ProfileEditor*>()->defaultsMode());
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); QCOMPARE(gridValue(window, "description").toString(), QString("Retried"));
    }
    void configModesFollowSelection() {
        QTemporaryDir dir; MainWindow window(dir.filePath("modes.xml"), true, nullptr, {}, {}, {}, dir.filePath("preferences.json")); OPEN_CONNECTION(window);
        const auto document = window.selectedDocument(); const auto rootConnection = window.selectedId();
        window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); const auto folder = window.selectedId();
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); const auto nested = window.selectedId();
        auto* properties = window.findChild<QAction*>("actionProperties"); auto* inheritance = window.findChild<QAction*>("actionInheritance");
        auto* defaults = window.findChild<QAction*>("actionDefaultProperties"); auto* defaultInheritance = window.findChild<QAction*>("actionDefaultInheritance");
        auto* editor = window.findChild<ProfileEditor*>();
        // Connection at the root: Properties only.
        QVERIFY(window.selectConnectionProfile({document, rootConnection}));
        QVERIFY(properties->isEnabled()); QVERIFY(!inheritance->isEnabled()); QVERIFY(!defaults->isEnabled()); QVERIFY(!defaultInheritance->isEnabled());
        // Folder at the root: no parent folder either.
        QVERIFY(window.selectConnectionProfile({document, folder})); QVERIFY(!inheritance->isEnabled()); QVERIFY(!defaults->isEnabled());
        // Connection inside a folder: Properties and Inheritance.
        QVERIFY(window.selectConnectionProfile({document, nested}));
        QVERIFY(properties->isEnabled()); QVERIFY(inheritance->isEnabled()); QVERIFY(!defaults->isEnabled()); QVERIFY(!defaultInheritance->isEnabled());
        inheritance->trigger(); QVERIFY(editor->inheritance());
        // Leaving the folder drops back to Properties.
        QVERIFY(window.selectConnectionProfile({document, rootConnection})); QVERIFY(!editor->inheritance()); QVERIFY(properties->isChecked());
        // Connections file root: Properties and both Default modes.
        QVERIFY(window.selectConnectionFile(document));
        QVERIFY(properties->isEnabled()); QVERIFY(!inheritance->isEnabled()); QTRY_VERIFY(defaults->isEnabled()); QVERIFY(defaultInheritance->isEnabled());
        defaultInheritance->trigger(); QVERIFY(editor->defaultsMode());
        // Selecting a connection leaves the Default modes.
        QVERIFY(window.selectConnectionProfile({document, nested})); QVERIFY(!editor->defaultsMode()); QVERIFY(!editor->inheritance());
        QVERIFY(properties->isChecked()); QCOMPARE(window.selectedId(), nested); QCOMPARE(editor->id(), nested);
        // A tree click does the same.
        QVERIFY(window.selectConnectionFile(document)); defaults->trigger(); QVERIFY(editor->defaultsMode());
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).value<ProfileReference>().profile == rootConnection) tree->setCurrentItem(*it);
        QTRY_VERIFY(!editor->defaultsMode()); QCOMPARE(editor->id(), rootConnection); QVERIFY(properties->isChecked());
    }
    void defaultModesRemainAvailableWithLockedSelectedFile() {
        QTemporaryDir dir; const auto path = dir.filePath("locked.xml"); MainWindow window(path, true, nullptr, {}, {}, {}, dir.filePath("preferences.json")); OPEN_CONNECTION(window);
        const auto document = window.selectedDocument(); const auto before = contents(path);
        window.findChild<DocumentService*>()->lock(document); QTRY_VERIFY(!window.findChild<DocumentService*>()->status(document)->unlocked);
        SELECT_FILE_ROOT(window); auto* action = window.findChild<QAction*>("actionDefaultProperties"); QTRY_VERIFY(action->isEnabled()); action->trigger();
        setGridText(window, "description", "While locked"); QVERIFY(waitGridSaved(window)); QCOMPARE(contents(path), before);
        QVERIFY(window.selectConnectionFile(document)); QVERIFY(window.findChild<ProfileEditor*>()->defaultsMode());
        window.findChild<QAction*>("actionDefaultInheritance")->trigger(); QCOMPARE(gridRow(window, "icon")->text(0), QString("Inherit Icon"));
        for (const auto* name : {"actionProperties", "actionInheritance", "actionDefaultProperties", "actionDefaultInheritance"})
            QCOMPARE(window.findChild<QAction*>(name)->isChecked(), QString(name) == "actionDefaultInheritance");
    }
    void connectionIconPickerUpdatesTreeMenuAndTabs() {
        QTemporaryDir dir; const auto path = dir.filePath("icons.xml"); MainWindow window(path, true, nullptr, {}, {}, {}, dir.filePath("preferences.json")); OPEN_CONNECTION(window);
        setGridText(window, "host", "owned.example"); QVERIFY(waitGridSaved(window)); window.connectSelected(true); QTRY_COMPARE(window.sessionCount(), size_t(1));
        auto* pane = window.findChild<SessionPane*>(); const auto target = pane->target();
        setGridChoice(window, "icon", "SSH"); QVERIFY(waitGridSaved(window));
        auto image = [](const QIcon& icon) { return icon.pixmap(16, 16).toImage(); };
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QCOMPARE(image(tree->currentItem()->icon(0)), image(ConnectionIcons::resolve("SSH")));
        auto* tabs = window.findChild<QTabWidget*>("sessionTabs"); QCOMPARE(image(tabs->tabIcon(tabs->indexOf(pane))), image(ConnectionIcons::resolve("SSH")));
        auto* savedMenu = window.findChild<QMenu*>("savedConnectionsMenu"); QVERIFY(savedMenu); QVERIFY(QMetaObject::invokeMethod(savedMenu, "aboutToShow"));
        QVERIFY(!savedMenu->actions().isEmpty()); auto* fileMenu = savedMenu->actions().first()->menu(); QVERIFY(fileMenu);
        QCOMPARE(image(fileMenu->actions().first()->icon()), image(ConnectionIcons::resolve("SSH")));
        auto* toolbar = window.findChild<QAction*>("actionConnectionIcon"); QVERIFY(toolbar->isEnabled()); auto* menu = toolbar->menu(); QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow")); QAction* linuxIcon = nullptr;
        for (auto* entry : menu->actions()) { QVERIFY(!entry->icon().isNull()); if (entry->data() == "Linux") linuxIcon = entry; }
        QVERIFY(linuxIcon); linuxIcon->trigger(); QVERIFY(waitGridSaved(window)); QCOMPARE(gridValue(window, "icon").toString(), QString("Linux"));
        QCOMPARE(image(tree->currentItem()->icon(0)), image(ConnectionIcons::resolve("Linux"))); QCOMPARE(image(tabs->tabIcon(tabs->indexOf(pane))), image(ConnectionIcons::resolve("Linux")));
        QCOMPARE(pane->target(), target); QCOMPARE(window.sessionCount(), size_t(1));
        const auto before = contents(path); SELECT_FILE_ROOT(window); auto* defaults = window.findChild<QAction*>("actionDefaultProperties"); QTRY_VERIFY(defaults->isEnabled()); defaults->trigger();
        QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow")); for (auto* entry : menu->actions()) if (entry->data() == "Windows") entry->trigger();
        QVERIFY(waitGridSaved(window)); QCOMPARE(contents(path), before); QCOMPARE(gridValue(window, "icon").toString(), QString("Windows"));
        QCOMPARE(image(ConnectionIcons::resolve("../../not-an-icon")), image(ConnectionIcons::resolve("")));
    }
    void iconPickerRetainsViewportAndScopesDuplicateIds() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"); MainWindow window(a, true); OPEN_CONNECTION(window);
        const auto first = window.selectedDocument(); const auto profile = window.selectedId(); const auto original = contents(a);
        QFile copy(b); QVERIFY(copy.open(QIODevice::WriteOnly)); QCOMPARE(copy.write(original), qint64(original.size())); copy.close();
        const auto second = window.openConnectionFile(b); QTRY_VERIFY(!documentBusy(window)); QVERIFY(window.selectConnectionProfile({first, profile}));
        auto* tree = window.findChild<QTreeWidget*>("configTree"); auto* row = gridRow(window, "icon"); tree->setCurrentItem(row); tree->scrollToItem(row);
        auto* menu = window.findChild<QAction*>("actionConnectionIcon")->menu(); QVERIFY(QMetaObject::invokeMethod(menu, "aboutToShow"));
        QAction* entry = nullptr; for (auto* action : menu->actions()) if (action->data() == "Linux") entry = action; QVERIFY(entry);
        QVERIFY(window.selectConnectionProfile({second, profile})); entry->trigger(); QVERIFY(waitGridSaved(window));
        QCOMPARE(window.selectedDocument(), first); QCOMPARE(gridValue(window, "icon").toString(), QString("Linux")); QCOMPARE(contents(b), original);
        row = gridRow(window, "icon"); tree->setCurrentItem(row); tree->scrollToItem(row); const auto scroll = tree->verticalScrollBar()->value();
        auto* category = gridRow(window, "resolution")->parent(); category->setExpanded(false);
        window.findChild<ProfileEditor*>()->setIcon("Unknown imported icon"); QVERIFY(waitGridSaved(window));
        QCOMPARE(tree->currentItem(), row); QCOMPARE(tree->verticalScrollBar()->value(), scroll); QVERIFY(!category->isExpanded());
        auto* combo = qobject_cast<QComboBox*>(gridEditor(window, "icon")); QVERIFY(combo); QVERIFY(combo->findData("Unknown imported icon") >= 0); closeGridEditors(window);
        const auto saved = contents(a); window.findChild<QAction*>("actionInheritance")->trigger(); window.findChild<QAction*>("actionProperties")->trigger();
        QCOMPARE(contents(a), saved); QVERIFY(window.selectConnectionProfile({second, profile})); QVERIFY(gridValue(window, "icon").toString().isEmpty());
    }
    void defaultPreferencesRejectSecretsAndMalformedData() {
        ConnectionDefaults defaults;
        QVERIFY(ConnectionDefaults::parse(defaults.json()));
        for (const auto* key : {"password", "passphrase", "host", "name", "unknown"}) {
            auto bad = defaults; bad.values[key] = "synthetic"; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        }
        auto bad = defaults; bad.values["port"] = 1.5; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        bad = defaults; bad.values["color_depth"] = 8; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        bad = defaults; bad.inheritance["password"] = true; QVERIFY(ConnectionDefaults::parse(bad.json()));
        bad.inheritance["passphrase"] = true; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        for (const auto& value : {QJsonValue(7), QJsonValue(QString(257, 'x')), QJsonValue(QString("bad\nicon"))}) {
            bad = defaults; bad.values["icon"] = value; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        }
        bad = defaults; bad.inheritance["icon"] = "true"; QVERIFY(!ConnectionDefaults::parse(bad.json()));
        bad = defaults; bad.values["description"] = QString(4097, 'x'); QVERIFY(!ConnectionDefaults::parse(bad.json()));
        auto object = defaults.json(); object["unknown"] = true; QVERIFY(!ConnectionDefaults::parse(object));
    }
    void defaultPreferencesBackwardCompatibilityAndNoOp() {
        QTemporaryDir dir; const auto path = dir.filePath("old-preferences.json");
        const QByteArray old("{\"version\":1,\"categorized\":false,\"property_help\":true,\"quick_connect\":false,\"lock_toolbars\":true}");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(old), qint64(old.size())); file.close();
        ApplicationPreferences store(path); std::optional<PreferenceReply> reply;
        store.load([&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(reply->result.ok);
        QVERIFY(!reply->values.categorized); QVERIFY(!reply->values.quickConnect); QVERIFY(reply->values.lockToolbars); QVERIFY(reply->values.defaults == ConnectionDefaults{});
        auto values = reply->values; values.defaults.values["description"] = "Nonsecret template"; values.defaults.inheritance["description"] = false;
        reply.reset(); store.save(values, [&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(reply->result.ok);
        const auto saved = contents(path); const auto modified = QFileInfo(path).lastModified();
        reply.reset(); store.save(values, [&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(reply->result.ok);
        QCOMPARE(contents(path), saved); QCOMPARE(QFileInfo(path).lastModified(), modified);
        reply.reset(); store.load([&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(reply->result.ok); QVERIFY(reply->values.defaults == values.defaults);
        auto invalid = QJsonDocument::fromJson(saved).object(); invalid["password"] = "must not persist";
        const auto rejected = QJsonDocument(invalid).toJson(QJsonDocument::Compact); QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(rejected), qint64(rejected.size())); file.close();
        reply.reset(); store.save(values, [&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(!reply->result.ok); QCOMPARE(contents(path), rejected);
        auto bad = values; bad.defaults.values["password"] = "must not persist";
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(saved), qint64(saved.size())); file.close();
        reply.reset(); store.save(bad, [&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(!reply->result.ok); QCOMPARE(contents(path), saved);
        reply.reset(); store.save(values, [&](PreferenceReply value) { reply = std::move(value); }); QTRY_VERIFY(reply); QVERIFY(reply->result.ok);
    }
    void configHelpMatchesReference() {
        QTemporaryDir dir; MainWindow window(dir.filePath("help.xml"), true); OPEN_FOLDERED_CONNECTION(window);
        auto* tree = window.findChild<QTreeWidget*>("configTree");
        tree->setCurrentItem(gridRow(window, "name"));
        QVERIFY(window.findChild<QLabel*>("configHelpText")->text().startsWith("This is the name that will be displayed in the connections tree."));
        {   // Description sits right under the title even when the pane is tall.
            auto* pane = window.findChild<QWidget*>("configHelp"); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
            pane->setMinimumHeight(200); QTRY_VERIFY(pane->height() >= 200);
            const auto* title = window.findChild<QLabel*>("configHelpTitle"); const auto* text = window.findChild<QLabel*>("configHelpText");
            QVERIFY(text->geometry().top() - title->geometry().bottom() <= 4); QVERIFY(title->geometry().top() <= 6);
            pane->setMinimumHeight(60);
        }
        tree->setCurrentItem(gridRow(window, "server_auth"));
        const auto certificateHelp = window.findChild<QLabel*>("configHelpText")->text();
        QVERIFY(certificateHelp.contains("without a prompt or saving trust"));
        QVERIFY(certificateHelp.contains("Don't connect if authentication fails"));
        tree->setCurrentItem(gridRow(window, "description"));
        QVERIFY(window.findChild<QLabel*>("configHelpText")->text().startsWith("Put your notes or a description for the host here."));
        window.findChild<QAction*>("actionInheritance")->trigger();
        QCOMPARE(gridRow(window, "description")->text(0), QString("Inherit Description"));
        QVERIFY(window.findChild<QLabel*>("configHelpText")->text().startsWith("Description of inherited property: Put your notes"));
        QVERIFY(gridRow(window, "name")->isHidden());
        const QMap<QString, QString> expected{
            {"icon", "Choose a icon that will be displayed when connected to the host."},
            {"host", "Enter the hostname or ip you want to connect to."}, {"username", "Enter your username."},
            {"password", "Enter your password."}, {"domain", "Enter your domain."},
            {"protocol", "Choose the protocol mRemoteNG should use to connect to the host."}, {"port", "Enter the port the selected protocol is listening on."},
            {"console_session", "Connect to the console session of the remote host."}, {"resolution", "Choose the resolution or mode this connection will open in."},
            {"automatic_resize", "Select whether to automatically resize the connection when the window is resized or when fullscreen mode is toggled. Requires RDC 8.0 or higher."},
            {"color_depth", "Select the color quality to be used."}, {"cache_bitmaps", "Select whether to use bitmap caching or not."},
            {"wallpaper", "Select yes if the wallpaper of the remote host should be displayed."}, {"themes", "Select yes if the theme of the remote host should be displayed."},
            {"font_smoothing", "Select whether to use font smoothing or not."}
        };
        auto* help = window.findChild<QLabel*>("configHelpText");
        for (bool inherited : {false, true}) {
            window.findChild<QAction*>(inherited ? "actionInheritance" : "actionProperties")->trigger();
            for (auto it = expected.begin(); it != expected.end(); ++it) {
                tree->setCurrentItem(gridRow(window, it.key()));
                QCOMPARE(help->text(), (inherited ? "Description of inherited property: " : "") + it.value());
            }
            if (inherited) { tree->setCurrentItem(gridRow(window, "inherit_all")); QCOMPARE(help->text(), QString("Description of inherited property: Toggle all inheritance options.")); }
        }
        window.findChild<QAction*>("actionProperties")->trigger(); setGridChoice(window, "protocol", "vnc"); QVERIFY(waitGridSaved(window));
        tree->setCurrentItem(gridRow(window, "smart_size")); QCOMPARE(help->text(), QString("Select the SmartSize mode to be used."));
        // Only the short description: no appended notes.
        for (const auto& key : {"host", "password", "protocol"}) { tree->setCurrentItem(gridRow(window, key)); QCOMPARE(help->text(), expected.value(key)); }
        tree->setCurrentItem(gridRow(window, "view_only")); QVERIFY(help->text().startsWith("If you want to establish a view only connection to the host select yes."));
        window.findChild<QAction*>("actionInheritance")->trigger(); tree->setCurrentItem(gridRow(window, "view_only"));
        QVERIFY(help->text().startsWith("Description of inherited property: If you want to establish a view only connection"));
    }
    void configInheritanceLabelsAndInheritAll() {
        QTemporaryDir dir; SaveWindow window(dir.filePath("all.xml"), true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        setGridText(window, "description", "local"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
        setGridText(window, "ssh_key_path", "/owned/hidden-key"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "protocol", "rdp"); QVERIFY(waitGridSaved(window)); const auto before = window.writes();
        window.findChild<QAction*>("actionInheritance")->trigger(); QCOMPARE(window.writes(), before);
        QCOMPARE(gridRow(window, "inherit_all")->text(0), QString("Inherit All"));
        setGridChoice(window, "inherit_all", true); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), before + 1);
        QVERIFY(!gridLocal(window, "description")); QVERIFY(gridLocal(window, "ssh_key_path"));
        setGridChoice(window, "inherit_all", false); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), before + 2);
        QVERIFY(gridLocal(window, "description")); QVERIFY(gridLocal(window, "protocol")); QVERIFY(gridLocal(window, "ssh_key_path"));
    }
    void configDockHeadersAreBlueWithWhiteText() {
        QTemporaryDir dir; MainWindow window(dir.filePath("headers.xml"), true); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        for (const auto* name : {"connectionsDock", "configDock"}) {
            auto* dock = window.findChild<QDockWidget*>(name); QVERIFY(dock);
            // Title bar: blue fill, white text; the dock's contents keep dark text.
            const auto title = dock->grab(QRect(0, 0, dock->width(), dock->style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, dock) + 6)).toImage();
            int blue = 0, white = 0;
            for (int y = 0; y < title.height(); ++y) for (int x = 0; x < title.width() / 2; ++x) {
                const QColor c = title.pixelColor(x, y);
                if (c == QColor("#0078d7")) ++blue; else if (c.red() > 230 && c.green() > 230 && c.blue() > 230) ++white;
            }
            QVERIFY2(blue > title.width(), name); QVERIFY2(white > 10, name);
            QVERIFY(dock->widget()->palette().color(QPalette::WindowText).lightness() < 128);
            // Float and close glyphs are white too.
            for (const auto* button : {"qt_dockwidget_floatbutton", "qt_dockwidget_closebutton"}) {
                auto* control = dock->findChild<QAbstractButton*>(button); QVERIFY2(control, button);
                const auto glyph = control->icon().pixmap(QSize(10, 10), 2.0).toImage(); int drawn = 0;
                for (int y = 0; y < glyph.height(); ++y) for (int x = 0; x < glyph.width(); ++x) {
                    const QColor c = glyph.pixelColor(x, y); if (c.alpha() < 128) continue;
                    ++drawn; QVERIFY2(c.red() > 230 && c.green() > 230 && c.blue() > 230, button);
                }
                QVERIFY2(drawn > 10, button);
            } QVERIFY(dock->features() & QDockWidget::DockWidgetClosable); QVERIFY(dock->features() & QDockWidget::DockWidgetFloatable);
            dock->setFloating(true); QVERIFY(dock->isFloating()); dock->setFloating(false); QVERIFY(!dock->isFloating());
            dock->close(); QVERIFY(dock->isHidden());
        }
        window.findChild<QAction*>("actionResetLayout")->trigger();
        QVERIFY(!window.findChild<QDockWidget*>("connectionsDock")->isHidden()); QVERIFY(!window.findChild<QDockWidget*>("configDock")->isHidden());
    }
    void configNativeVisualReceipt() {
        const auto platform = QGuiApplication::platformName();
        if (platform != "cocoa" && platform != "xcb") QSKIP("Native Config receipt requires Cocoa or XCB");
        const auto build = qEnvironmentVariable("RM_BUILD_DIR"); QVERIFY(!build.isEmpty());
        const auto evidence = QDir(build).filePath("config-evidence/" + platform); QVERIFY(QDir().mkpath(evidence));
        QTemporaryDir dir; MainWindow window(dir.filePath("synthetic-config.xml"), true, nullptr, {}, {}, {}, dir.filePath("preferences.json"));
        window.resize(1060, 780); OPEN_CONNECTION(window); setGridText(window, "name", "Synthetic connection"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "icon", "SSH"); QVERIFY(waitGridSaved(window));
        window.resizeDocks({window.findChild<QDockWidget*>("connectionsDock"), window.findChild<QDockWidget*>("configDock")}, {330, 330}, Qt::Horizontal);
        auto* tree = window.findChild<QTreeWidget*>("configTree"); tree->setCurrentItem(gridRow(window, "name")); tree->scrollToTop();
        QTest::qWait(150); QVERIFY(!window.findChild<ProfileEditor*>()->editing()); QVERIFY(window.grab().save(QDir(evidence).filePath("properties.png")));
        SELECT_FILE_ROOT(window); auto* defaults = window.findChild<QAction*>("actionDefaultProperties"); QTRY_VERIFY(defaults->isEnabled()); defaults->trigger();
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
        window.findChild<QAction*>("actionDefaultInheritance")->trigger(); tree->setCurrentItem(gridRow(window, "inherit_all")); tree->scrollToTop();
        QTest::qWait(150); QVERIFY(window.grab().save(QDir(evidence).filePath("default-inheritance.png")));
        defaults->trigger(); auto* menu = window.findChild<QAction*>("actionConnectionIcon")->menu();
        menu->popup(window.findChild<QToolBar*>("configToolbar")->mapToGlobal(QPoint(150, 0))); QTest::qWait(150);
        QVERIFY(menu->isVisible()); QVERIFY(menu->grab().save(QDir(evidence).filePath("icon-menu.png"))); menu->hide();
        QVERIFY(window.grab().save(QDir(evidence).filePath("default-properties.png")));
        auto* combo = qobject_cast<QComboBox*>(gridEditor(window, "icon")); QVERIFY(combo); combo->showPopup(); QTest::qWait(150);
        QVERIFY(combo->view()->window()->grab().save(QDir(evidence).filePath("icon-dropdown.png"))); combo->hidePopup(); closeGridEditors(window);
    }
    void connectionsRootContainsAllProfiles() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->text(0), QString("Connections — p"));
        QVERIFY(tree->topLevelItem(0)->data(0, Qt::UserRole).canConvert<DocumentRootReference>());
        window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
        QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), 1);
        window.findChild<QLineEdit*>("searchEdit")->setText("unmatched filter"); QVERIFY(!tree->topLevelItem(0)->isHidden());
    }
    void connectionsToolbarHasOnlyFourActions() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* bar = window.findChild<QToolBar*>("connectionsToolbar"); QVERIFY(bar);
        QStringList names; for (auto* action : bar->actions()) { QVERIFY(!action->isSeparator()); names.append(action->objectName()); QVERIFY(!action->icon().isNull()); }
        QCOMPARE(names, QStringList({"actionAddConnection", "actionAddFolder", "actionExpandAll", "actionSort"}));
        QStringList labels; for (auto* action : bar->actions()) labels << action->text();
        QCOMPARE(labels, QStringList({"New Connection", "New folder", "Expand all", "Sort"}));
        auto* sort = bar->actions().last(); QVERIFY(sort->menu()); QCOMPARE(sort->menu()->actions().size(), 2);
        QCOMPARE(qobject_cast<QToolButton*>(bar->widgetForAction(sort))->popupMode(), QToolButton::InstantPopup);
    }
    void documentRootCannotActAsProfile() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); window.show();
        QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        QVERIFY(window.selectedId().isEmpty()); QCOMPARE(gridValue(window, "name").toString(), QString("Connections"));
        QVERIFY(!(gridRow(window, "name")->flags() & Qt::ItemIsEditable));
        for (const auto* name : {"actionTreeConnect", "actionTreeDelete", "actionTreeRename", "actionTreeDuplicate"}) QVERIFY(!window.findChild<QAction*>(name)->isEnabled());
        window.connectSelected(true); window.deleteSelected(); QCOMPARE(window.sessionCount(), size_t(0)); QVERIFY(!QFile::exists(path));
        QVERIFY(!window.findChild<QAction*>("actionInheritance")->isEnabled()); // the file root has nothing to inherit from
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); QVERIFY(window.documentRootSelected());
        tree->setCurrentItem(nullptr); QVERIFY(!window.documentRootSelected()); QVERIFY(window.selectedId().isEmpty()); QVERIFY(!window.findChild<ProfileEditor*>()->documentRoot());
        tree->setCurrentItem(tree->topLevelItem(0)); QVERIFY(window.documentRootSelected()); QVERIFY(window.findChild<ProfileEditor*>()->documentRoot());
    }
    void rootKeepsIndependentPasswordAndFullEncryptionControls() {
        QTreeWidget tree; ProfileEditor editor(&tree);
        editor.loadDocument(false, true, "XML protection", true);
        QVERIFY(editor.draft().contains("full_encryption"));
        QVERIFY(editor.requestedFullEncryption()); QVERIFY(!editor.requestedProtection());
        editor.loadDocument(true, false, "XML protection", true);
        QVERIFY(editor.requestedProtection()); QVERIFY(!editor.requestedFullEncryption()); QVERIFY(!editor.dirty());
    }
    void rootSelectionWaitsForAutoSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Before root");
        auto* tree = window.findChild<QTreeWidget*>("connectionTree");
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(tree->topLevelItem(0)).center());
        QTRY_VERIFY(contents(path).contains("Before root")); QTRY_VERIFY(window.selectedId().isEmpty());
        QCOMPARE(gridValue(window, "name").toString(), QString("Connections")); QVERIFY(!window.findChild<QMessageBox*>("unsavedChanges"));
    }
    void newSettingsAutoSaveAndRestart() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); QString id;
        {
            MainWindow window(path, true); OPEN_CONNECTION(window); id = window.selectedId();
            QCOMPARE(gridRow(window, "description")->text(0), QString("Description"));
            QCOMPARE(gridRow(window, "domain")->text(0), QString("Domain"));
            QCOMPARE(gridValue(window, "cache_bitmaps").toBool(), false);
            setGridText(window, "description", "Owned fixture"); QVERIFY(waitGridSaved(window));
            setGridText(window, "username", "alice"); QVERIFY(waitGridSaved(window));
            setGridText(window, "domain", "EXAMPLE"); QVERIFY(waitGridSaved(window));
            for (const auto* key : {"console_session", "cache_bitmaps", "automatic_resize", "wallpaper", "themes", "font_smoothing"}) {
                const bool initial = QString(key) != "console_session" && QString(key) != "cache_bitmaps";
                QVERIFY2(gridValue(window, key).toBool() == initial, key);
                const bool changed = !initial;
                setGridChoice(window, key, changed); QVERIFY(waitGridSaved(window)); QCOMPARE(gridValue(window, key).toBool(), changed);
                const auto disk = storedXml(path).value("profiles").toArray().first().toObject();
                QCOMPARE(disk.value("overrides").toObject().value(key).toBool(), changed);
            }
            setGridChoice(window, "resolution", "1280x720"); QVERIFY(waitGridSaved(window));
            setGridChoice(window, "color_depth", 16); QVERIFY(waitGridSaved(window));
            const auto stored = storedXml(path); QCOMPARE(stored.value("schema_version").toInt(), 2);
            QCOMPARE(stored.value("profiles").toArray().first().toObject().value("id").toString(), id);
        }
        MainWindow restart(path, true); restart.show(); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        auto* tree = restart.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        QCOMPARE(restart.selectedId(), id); QCOMPARE(gridValue(restart, "description").toString(), QString("Owned fixture"));
        QCOMPARE(gridValue(restart, "domain").toString(), QString("EXAMPLE")); QCOMPARE(gridValue(restart, "resolution").toString(), QString("1280x720"));
        QCOMPARE(gridValue(restart, "automatic_resize").toBool(), false); QCOMPARE(gridValue(restart, "color_depth").toInt(), 16);
        setGridChoice(restart, "protocol", "ssh"); QVERIFY(waitGridSaved(restart));
        for (const auto* key : {"console_session", "resolution", "automatic_resize", "color_depth", "cache_bitmaps", "wallpaper", "themes", "font_smoothing"})
            QVERIFY(!(gridRow(restart, key)->flags() & Qt::ItemIsEditable));
    }
    void sshConfigCommitCancelAndInheritanceUseAutomaticSave() {
        QTemporaryDir dir; const auto path = dir.filePath("ssh.xml"); QString childId;
        {
            SaveWindow window(path, true); window.show(); window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window);
            QVERIFY(gridRow(window, "ssh_auth")); QVERIFY(gridRow(window, "ssh_key_path"));
            setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
            setGridChoice(window, "ssh_auth", "private_key"); QVERIFY(waitGridSaved(window));
            setGridText(window, "ssh_key_path", "/owned/fixture/id_ed25519"); QVERIFY(waitGridSaved(window));
            window.addConnection(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); childId = window.selectedId();
            QCOMPARE(gridValue(window, "ssh_auth").toString(), QString("private_key")); QVERIFY(!gridLocal(window, "ssh_auth"));
            QCOMPARE(gridValue(window, "ssh_key_path").toString(), QString("/owned/fixture/id_ed25519")); QVERIFY(!gridLocal(window, "ssh_key_path"));
            QVERIFY(!(gridRow(window, "domain")->flags() & Qt::ItemIsEditable));
            QVERIFY(!(gridRow(window, "password")->flags() & Qt::ItemIsEditable));
            const auto before = contents(path); const auto writes = window.writes();
            auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "ssh_key_path")); QVERIFY(edit);
            edit->selectAll(); QTest::keyClicks(edit, "/owned/canceled"); QTest::keyClick(edit, Qt::Key_Escape); closeGridEditors(window);
            QCOMPARE(contents(path), before); QCOMPARE(window.writes(), writes);
            setGridText(window, "ssh_key_path", ""); QVERIFY(waitGridSaved(window));
            QVERIFY(gridLocal(window, "ssh_key_path")); QVERIFY(gridValue(window, "ssh_key_path").toString().isEmpty());
            setGridLocal(window, "ssh_key_path", false); QVERIFY(waitGridSaved(window));
            QCOMPARE(gridValue(window, "ssh_key_path").toString(), QString("/owned/fixture/id_ed25519"));
            setGridChoice(window, "ssh_auth", "password"); QVERIFY(waitGridSaved(window));
            QVERIFY(gridLocal(window, "ssh_auth")); QVERIFY(gridRow(window, "password")->flags() & Qt::ItemIsEditable);
            setGridLocal(window, "ssh_auth", false); QVERIFY(waitGridSaved(window));
            QCOMPARE(gridValue(window, "ssh_auth").toString(), QString("private_key"));
        }
        MainWindow restart(path, true); restart.show(); QTRY_COMPARE(restart.documentState(), MainWindow::DocumentState::Open);
        auto* tree = restart.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(0));
        QCOMPARE(restart.selectedId(), childId); QCOMPARE(gridValue(restart, "ssh_auth").toString(), QString("private_key"));
        QCOMPARE(gridValue(restart, "ssh_key_path").toString(), QString("/owned/fixture/id_ed25519"));
        QVERIFY(!gridLocal(restart, "ssh_auth")); QVERIFY(!gridLocal(restart, "ssh_key_path"));
        setGridChoice(restart, "protocol", "rdp"); QVERIFY(waitGridSaved(restart));
        QVERIFY(!(gridRow(restart, "ssh_auth")->flags() & Qt::ItemIsEditable));
        QVERIFY(!(gridRow(restart, "ssh_key_path")->flags() & Qt::ItemIsEditable));
        QVERIFY(gridRow(restart, "domain")->flags() & Qt::ItemIsEditable);
    }
    void sshKeyPathPickerCapturesOwningProfile() {
        QTemporaryDir dir; const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml"), key = dir.filePath("owned_key");
        const QString profileId = "22222222-2222-4222-8222-222222222222";
        auto xml = ConnectionXml::create(); QVERIFY(xml); const Secret master(QByteArray("mR3m"));
        const QJsonObject profile{{"id", profileId}, {"name", "SSH picker"}, {"kind", "connection"}, {"parent", QJsonValue()},
            {"overrides", QJsonObject{{"protocol", "ssh"}, {"ssh_auth", "private_key"}, {"ssh_key_path", ""}}}};
        QVERIFY(xml->patch(master, QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact)));
        const auto data = xml->serialize(master); QVERIFY(data);
        { QFile file(a); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(*data), data->size()); }
        QVERIFY(QFile::copy(a, b)); { QFile file(key); QVERIFY(file.open(QIODevice::WriteOnly)); }
        MainWindow window(a, true); window.show(); const auto idA = window.connectionFiles().first(), idB = window.openConnectionFile(b);
        auto* files = window.findChild<DocumentService*>();
        QTRY_VERIFY(files->status(idA) && !files->status(idA)->busy && files->status(idB) && !files->status(idB)->busy);
        auto openMenu = [&]() -> QMenu* {
            auto* tree = window.findChild<QTreeWidget*>("configTree"); auto* row = gridRow(window, "ssh_key_path");
            if (row->parent()) row->parent()->setExpanded(true); tree->setCurrentItem(row, 1); tree->scrollToItem(row);
            const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2, tree->visualItemRect(row).center().y());
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, tree->viewport()->mapToGlobal(point)); QApplication::sendEvent(tree->viewport(), &event);
            return window.findChild<QMenu*>("configContextMenu");
        };
        QVERIFY(window.selectConnectionProfile({idA, profileId})); auto* menu = openMenu(); QVERIFY(menu);
        auto* choose = menu->findChild<QAction*>("actionChooseSshKey"); QVERIFY(choose && choose->isEnabled());
        const auto beforeB = contents(b);
        QVERIFY(window.selectConnectionProfile({idB, profileId}));
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(choose).center());
        QTRY_VERIFY(window.findChild<QFileDialog*>("sshKeyFilePicker")); auto* picker = window.findChild<QFileDialog*>("sshKeyFilePicker");
        QVERIFY(window.selectConnectionProfile({idB, profileId})); picker->setDirectory(dir.path());
        QVERIFY(QTest::qWaitForWindowExposed(picker));
        auto* filename = picker->findChild<QLineEdit*>("fileNameEdit"); QVERIFY(filename);
        filename->setFocus(); filename->selectAll(); QTest::keyClicks(filename, key);
        QTRY_COMPARE(picker->selectedFiles(), QStringList{key});
        QSignalSpy selected(picker, &QFileDialog::fileSelected);
        QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
        QTRY_COMPARE(selected.size(), 1); QCOMPARE(selected.first().first().toString(), key);
        QTRY_VERIFY2(contents(a).contains(key.toUtf8()), qPrintable(window.lastError())); QVERIFY(waitGridSaved(window)); QCOMPARE(contents(b), beforeB);
        QCOMPARE(window.selectedDocument(), idA); QCOMPARE(gridValue(window, "ssh_key_path").toString(), key);
        const auto beforeA = contents(a); menu = openMenu(); QVERIFY(menu);
        choose = menu->findChild<QAction*>("actionChooseSshKey"); QVERIFY(choose);
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(choose).center());
        QTRY_VERIFY(window.findChild<QFileDialog*>("sshKeyFilePicker")); window.findChild<QFileDialog*>("sshKeyFilePicker")->reject();
        QTRY_VERIFY(!window.findChild<QFileDialog*>("sshKeyFilePicker")); QCOMPARE(contents(a), beforeA); QCOMPARE(contents(b), beforeB);
    }
    void protocolVisibilityUsesEffectiveProtocol() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); OPEN_CONNECTION(window);
        const auto child = window.selectedId(); auto* row = gridRow(window, "resolution");
        QVERIFY(!row->isHidden());
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
        QVERIFY(row->isHidden()); QVERIFY(row->parent()->isHidden()); QVERIFY(!gridRow(window, "host")->isHidden());
        window.findChild<QAction*>("actionInheritance")->trigger(); QVERIFY(row->isHidden());
        window.findChild<QAction*>("actionProperties")->trigger();
        window.findChild<QAction*>("actionAlphabetical")->trigger(); QVERIFY(gridRow(window, "resolution")->isHidden());
        window.findChild<QAction*>("actionCategorized")->trigger();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0));
        window.addFolder(); QVERIFY(waitGridSaved(window)); closeGridEditors(window); const auto folder = window.selectedId();
        setGridChoice(window, "protocol", "vnc"); QVERIFY(waitGridSaved(window)); QVERIFY(gridRow(window, "resolution")->isHidden());
        QTreeWidgetItem* childItem = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).value<ProfileReference>().profile == child) childItem = *it;
        QVERIFY(childItem); tree->setCurrentItem(childItem);
        setGridChoice(window, "parent", folder); QVERIFY(waitGridSaved(window));
        window.findChild<QAction*>("actionInheritance")->trigger(); setGridChoice(window, "protocol", true); QVERIFY(waitGridSaved(window));
        QVERIFY(gridRow(window, "resolution")->isHidden());
        window.findChild<QAction*>("actionProperties")->trigger(); QCOMPARE(gridValue(window, "protocol").toString(), QString("vnc"));
        // Without a parent folder, the node's own Protocol value applies again.
        setGridChoice(window, "parent", QString()); QVERIFY(waitGridSaved(window));
        QCOMPARE(gridValue(window, "protocol").toString(), QString("ssh")); QVERIFY(gridRow(window, "resolution")->isHidden());
        setGridChoice(window, "protocol", "rdp"); QVERIFY(waitGridSaved(window));
        QVERIFY(!gridRow(window, "resolution")->isHidden());
    }
    void rdpSecurityAndGatewayRowsUseNativeEditors() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); OPEN_CONNECTION(window);
        for (const auto* key : {"security_protocol", "server_auth", "use_credssp", "idle_minutes", "load_balance_info"}) {
            QVERIFY2(!gridRow(window, key)->isHidden(), key); QCOMPARE(gridRow(window, key)->parent()->text(0), QString("Protocol"));
        }
        for (const auto* key : {"gateway_usage", "gateway_host", "gateway_port", "gateway_credentials", "gateway_username", "gateway_domain"}) {
            QVERIFY2(!gridRow(window, key)->isHidden(), key); QCOMPARE(gridRow(window, key)->parent()->text(0), QString("Gateway"));
        }
        // Automatic negotiation and session-only certificate acceptance for new connections.
        QCOMPARE(gridValue(window, "security_protocol").toString(), QString("auto")); QCOMPARE(gridRow(window, "security_protocol")->text(1), QString("Automatic"));
        QCOMPARE(gridRow(window, "server_auth")->text(1), QString("Accept unverified certificates for this session"));
        QCOMPARE(gridRow(window, "use_credssp")->text(1), QString("Yes")); QCOMPARE(gridValue(window, "idle_minutes").toInt(), 0);
        QCOMPARE(gridRow(window, "gateway_usage")->text(1), QString("Never")); QCOMPARE(gridValue(window, "gateway_port").toInt(), 443);
        auto* combo = qobject_cast<QComboBox*>(gridEditor(window, "security_protocol")); QVERIFY(combo);
        QCOMPARE(combo->count(), 5); QVERIFY(combo->findData("tls") >= 0); QVERIFY(combo->findData("ext") >= 0); closeGridEditors(window);
        combo = qobject_cast<QComboBox*>(gridEditor(window, "server_auth")); QVERIFY(combo); QCOMPARE(combo->count(), 3); closeGridEditors(window);
        auto* spin = qobject_cast<QSpinBox*>(gridEditor(window, "idle_minutes")); QVERIFY(spin);
        QCOMPARE(spin->minimum(), 0); QCOMPARE(spin->maximum(), 240); closeGridEditors(window);
        spin = qobject_cast<QSpinBox*>(gridEditor(window, "gateway_port")); QVERIFY(spin); QCOMPARE(spin->minimum(), 1); closeGridEditors(window);
        setGridChoice(window, "security_protocol", "tls"); QVERIFY(waitGridSaved(window));
        setGridChoice(window, "server_auth", "required"); QVERIFY(waitGridSaved(window));
        setGridText(window, "load_balance_info", "tsv://MS Terminal Services Plugin.1.Pool"); QVERIFY(waitGridSaved(window));
        const auto overrides = window.findChild<ProfileEditor*>()->draft().value("overrides").toObject();
        QCOMPARE(overrides.value("security_protocol").toString(), QString("tls"));
        QCOMPARE(overrides.value("server_auth").toString(), QString("required"));
        QCOMPARE(overrides.value("load_balance_info").toString(), QString("tsv://MS Terminal Services Plugin.1.Pool"));
        setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window));
        for (const auto* key : {"security_protocol", "server_auth", "gateway_usage", "gateway_host"}) QVERIFY2(gridRow(window, key)->isHidden(), key);
        QVERIFY(gridRow(window, "gateway_host")->parent()->isHidden());
    }
    void hiddenProtocolOverridesSurviveRestart() {
        QTemporaryDir dir; const auto path = dir.filePath("p");
        {
            MainWindow window(path, true); OPEN_CONNECTION(window);
            setGridChoice(window, "resolution", "1280x720"); QVERIFY(waitGridSaved(window));
            setGridChoice(window, "protocol", "ssh"); QVERIFY(waitGridSaved(window)); QVERIFY(gridRow(window, "resolution")->isHidden());
            QCOMPARE(window.findChild<ProfileEditor*>()->draft().value("overrides").toObject().value("resolution").toString(), QString("1280x720"));
        }
        MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        QVERIFY(gridRow(window, "resolution")->isHidden()); setGridChoice(window, "protocol", "rdp"); QVERIFY(waitGridSaved(window));
        QVERIFY(!gridRow(window, "resolution")->isHidden()); QCOMPARE(gridValue(window, "resolution").toString(), QString("1280x720"));
    }
    void compactGridHasNoPersistentEditors() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); OPEN_CONNECTION(window);
        auto* tree = window.findChild<QTreeWidget*>("configTree");
        for (QTreeWidgetItemIterator it(tree); *it; ++it) { QVERIFY(!tree->itemWidget(*it, 0)); QVERIFY(!tree->itemWidget(*it, 1)); }
        QVERIFY(!tree->findChild<QLineEdit*>()); QVERIFY(!tree->findChild<QComboBox*>()); QVERIFY(!tree->findChild<QSpinBox*>());
        QCOMPARE(tree->columnCount(), 2);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("General")); QVERIFY(tree->topLevelItem(0)->isHidden());
        QCOMPARE(tree->topLevelItem(1)->text(0), QString("Display")); QVERIFY(!tree->topLevelItem(1)->isHidden());
        QVERIFY(tree->visualItemRect(gridRow(window, "name")).height() <= tree->fontMetrics().height() + 8);
        QCOMPARE(gridRow(window, "password")->text(1), QString()); QVERIFY(gridRow(window, "password")->flags() & Qt::ItemIsEditable);
        QVERIFY(!window.findChild<QPushButton*>("saveButton")); QVERIFY(!window.findChild<QPushButton*>("discardButton"));
    }
    void mouseAndKeyboardOpenValueEditors() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); OPEN_CONNECTION(window);
        QVERIFY(qobject_cast<QLineEdit*>(gridEditor(window, "name", true))); closeGridEditors(window);
        QVERIFY(qobject_cast<QLineEdit*>(gridEditor(window, "name", false))); closeGridEditors(window);
        auto* config = window.findChild<QTreeWidget*>("configTree"); auto* delegate = qobject_cast<PropertyDelegate*>(config->itemDelegate());
        config->setCurrentItem(gridRow(window, "name"), 0); QTest::keyClick(config, Qt::Key_Enter, Qt::KeypadModifier);
        QTRY_VERIFY(qobject_cast<QLineEdit*>(delegate->activeEditor())); closeGridEditors(window);
        config->setCurrentItem(gridRow(window, "password"), 0); QTest::keyClick(config, Qt::Key_Return); QVERIFY(delegate->activeEditor()); closeGridEditors(window);
        QVERIFY(qobject_cast<QComboBox*>(gridEditor(window, "protocol", true))); closeGridEditors(window);
        QVERIFY(qobject_cast<QSpinBox*>(gridEditor(window, "port", false))); closeGridEditors(window);
        QVERIFY(qobject_cast<QComboBox*>(gridEditor(window, "protocol", false))); closeGridEditors(window);
        QVERIFY(qobject_cast<QSpinBox*>(gridEditor(window, "port", true))); closeGridEditors(window);
        for (const auto* key : {"name", "protocol", "port"}) {
            auto* row = gridRow(window, key); if (row->parent()) row->parent()->setExpanded(true);
            auto* tree = window.findChild<QTreeWidget*>("configTree"); tree->setCurrentItem(gridRow(window, "password"), 0); tree->scrollToItem(row);
            const auto cell = tree->visualItemRect(row); const QPoint point(tree->columnViewportPosition(1) + tree->columnWidth(1) / 2, cell.center().y());
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            auto* delegate = qobject_cast<PropertyDelegate*>(tree->itemDelegate()); QTRY_VERIFY(delegate->activeEditor()); closeGridEditors(window);
        }
    }
    void enterAndFocusLossAutoSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        window.raise(); window.activateWindow();
        const auto original = contents(path); const auto writes = window.writes();
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); QVERIFY(edit); QTRY_VERIFY(edit->hasFocus());
        edit->selectAll(); QTest::keyClicks(edit, "Enter saved"); QCOMPARE(window.writes(), writes); QCOMPARE(contents(path), original);
        QTest::keyClick(edit, Qt::Key_Return); QTRY_VERIFY(contents(path).contains("Enter saved")); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), writes + 1);
        edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); QTRY_VERIFY(edit->hasFocus()); edit->selectAll(); QTest::keyClicks(edit, "Focus saved");
        QTest::mouseClick(window.findChild<QLineEdit*>("searchEdit"), Qt::LeftButton);
        QTRY_VERIFY(contents(path).contains("Focus saved")); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), writes + 2);
        QVERIFY(!window.findChild<QMessageBox*>("unsavedChanges"));
    }
    void escapeCancelsWithoutWriting() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        const auto original = contents(path); const auto writes = window.writes();
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Canceled"); QTest::keyClick(edit, Qt::Key_Escape);
        closeGridEditors(window); QCOMPARE(gridValue(window, "name").toString(), QString("New connection")); QCOMPARE(contents(path), original); QCOMPARE(window.writes(), writes);
        auto* combo = qobject_cast<QComboBox*>(gridEditor(window, "protocol")); QTest::keyClick(combo, Qt::Key_Down); QTest::keyClick(combo, Qt::Key_Escape);
        closeGridEditors(window); QCOMPARE(gridValue(window, "protocol").toString(), QString("rdp")); QCOMPARE(contents(path), original); QCOMPARE(window.writes(), writes);
        auto* spin = qobject_cast<QSpinBox*>(gridEditor(window, "port")); spin->findChild<QLineEdit*>()->selectAll(); QTest::keyClicks(spin->findChild<QLineEdit*>(), "2201"); QTest::keyClick(spin, Qt::Key_Escape);
        closeGridEditors(window); QCOMPARE(gridValue(window, "port").toInt(), 3389); QCOMPARE(contents(path), original); QCOMPARE(window.writes(), writes);
    }
    void dropdownSelectionCommitsOnce() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        QVERIFY(gridLocal(window, "port")); QCOMPARE(gridValue(window, "port").toInt(), 3389);
        const auto writes = window.writes(); auto* combo = qobject_cast<QComboBox*>(gridEditor(window, "protocol")); QVERIFY(combo); QCOMPARE(combo->count(), 3);
        QTest::keyClick(combo, Qt::Key_Down); QTest::keyClick(combo, Qt::Key_Return);
        QTRY_COMPARE(gridValue(window, "protocol").toString(), QString("ssh")); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), writes + 1);
        const auto saved = storedXml(path)["profiles"].toArray().first().toObject()["overrides"].toObject();
        QCOMPARE(gridValue(window, "port").toInt(), 3389); QCOMPARE(saved["port"].toInt(), 3389);
        QCOMPARE(saved["protocol"].toString(), QString("ssh"));
        auto* spin = qobject_cast<QSpinBox*>(gridEditor(window, "port")); QVERIFY(spin); QCOMPARE(spin->minimum(), 1); QCOMPARE(spin->maximum(), 65535);
        auto* line = spin->findChild<QLineEdit*>(); line->selectAll(); QTest::keyClicks(line, "2201"); QTest::keyClick(spin, Qt::Key_Return);
        QTRY_COMPARE(gridValue(window, "port").toInt(), 2201); QVERIFY(waitGridSaved(window)); QVERIFY(contents(path).contains("2201"));
    }
    void popupSelectionAndEscape() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        // Room for a popup wide enough that the pointer lands inside its rows.
        auto* dock = window.findChild<QDockWidget*>("configDock"); window.resizeDocks({dock}, {330}, Qt::Horizontal); QTRY_VERIFY(dock->width() >= 300);
        const auto writes = window.writes(); const auto original = contents(path);
        QPointer<QComboBox> combo = qobject_cast<QComboBox*>(gridEditor(window, "protocol")); QVERIFY(combo);
        QTest::mouseClick(combo, Qt::LeftButton); QTRY_VERIFY(combo && combo->view()->isVisible());
        QTest::keyClick(combo->view(), Qt::Key_Down); QCoreApplication::processEvents();
        QCOMPARE(window.writes(), writes); QCOMPARE(contents(path), original);
        QTest::keyClick(combo->view(), Qt::Key_Escape); QTRY_VERIFY(!combo->view()->isVisible());
        closeGridEditors(window); QCOMPARE(window.writes(), writes); QCOMPARE(gridValue(window, "protocol").toString(), QString("rdp"));
        combo = qobject_cast<QComboBox*>(gridEditor(window, "protocol"));
        QTest::mouseClick(combo, Qt::LeftButton); QTRY_VERIFY(combo->view()->isVisible());
        const auto target = combo->model()->index(1, 0);
        QTest::qWait(QApplication::doubleClickInterval() + 1);
        QVERIFY2(combo && combo->view()->isVisible(), "Popup must remain open until the pointer selects a row");
        auto* viewport = combo->view()->viewport();
        auto* popup = viewport->window()->windowHandle(); QVERIFY(popup);
        const auto point = combo->view()->visualRect(target).center();
        QCOMPARE(combo->view()->indexAt(point), target);
        // QWidget mouseMove only warps the OS cursor. QWindow delivers a QPA
        // event, so native cursor events cannot race this popup interaction.
        const auto popupPoint = popup->mapFromGlobal(viewport->mapToGlobal(point));
        QTest::mouseMove(popup, popupPoint);
        QTRY_VERIFY(combo && combo->view()->currentIndex() == target);
        QTest::mouseClick(popup, Qt::LeftButton, Qt::NoModifier, popupPoint);
        QTRY_COMPARE(gridValue(window, "protocol").toString(), QString("ssh")); QVERIFY(waitGridSaved(window)); QCOMPARE(window.writes(), writes + 1);
    }
    void spinLineEnterAndFocusLossSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window); const auto writes = window.writes();
        window.raise(); window.activateWindow();
        auto* spin = qobject_cast<QSpinBox*>(gridEditor(window, "port")); auto* line = spin->findChild<QLineEdit*>();
        line->setFocus(); QTRY_VERIFY(line->hasFocus()); line->selectAll(); QTest::keyClicks(line, "3391"); QTest::keyClick(line, Qt::Key_Return);
        QVERIFY(waitGridSaved(window)); QCOMPARE(gridValue(window, "port").toInt(), 3391); QCOMPARE(window.writes(), writes + 1);
        spin = qobject_cast<QSpinBox*>(gridEditor(window, "port")); line = spin->findChild<QLineEdit*>();
        line->setFocus(); QTRY_VERIFY(line->hasFocus()); line->selectAll(); QTest::keyClicks(line, "3392"); QTest::mouseClick(window.findChild<QLineEdit*>("searchEdit"), Qt::LeftButton);
        QVERIFY(waitGridSaved(window)); QCOMPARE(gridValue(window, "port").toInt(), 3392); QCOMPARE(window.writes(), writes + 2);
    }
    void loadingAndPreviewNeverSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        const auto original = contents(path); const auto writes = window.writes();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); tree->setCurrentItem(nullptr); tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        window.findChild<QAction*>("actionAlphabetical")->trigger(); window.findChild<QAction*>("actionCategorized")->trigger();
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); QTest::keyClick(edit, Qt::Key_Return);
        closeGridEditors(window); QCoreApplication::processEvents(); QCOMPARE(contents(path), original); QCOMPARE(window.writes(), writes);
    }
    void failedAutoSaveRetainsValueAndBlocksTransition() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); SaveWindow window(path, true); OPEN_CONNECTION(window);
        const auto original = contents(path); const auto id = window.selectedId(); window.failNext();
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Recoverable");
        window.addFolder(); QTRY_VERIFY(!window.lastError().isEmpty()); QCOMPARE(window.selectedId(), id); QCOMPARE(contents(path), original);
        QCOMPARE(gridValue(window, "name").toString(), QString("Recoverable")); QVERIFY(window.findChild<ProfileEditor*>()->dirty());
        window.addFolder(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery")); window.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Cancel)->click();
        QCOMPARE(window.selectedId(), id); QCOMPARE(contents(path), original); QCOMPARE(gridValue(window, "name").toString(), QString("Recoverable"));
        window.addFolder(); QTRY_VERIFY(window.findChild<QMessageBox*>("saveRecovery")); window.findChild<QMessageBox*>("saveRecovery")->button(QMessageBox::Retry)->click();
        QTRY_VERIFY(contents(path).contains("Recoverable")); QTRY_VERIFY(window.selectedId() != id); QCOMPARE(window.findChild<QTreeWidget*>("connectionTree")->topLevelItem(0)->childCount(), 2);
    }
    void rapidTransitionsWaitForCommittedSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        const auto id = window.selectedId(); auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Before add"); window.addFolder();
        QTRY_VERIFY(contents(path).contains("Before add")); QTRY_VERIFY(window.selectedId() != id); QVERIFY(waitGridSaved(window));
        QVERIFY(!window.findChild<QMessageBox*>("unsavedChanges"));
    }
    void closeAndLockReconcileAutoSave() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Before close"); window.close();
        QTRY_VERIFY(!window.isVisible()); QVERIFY(contents(path).contains("Before close")); QVERIFY(!window.findChild<ProfileEditor*>()->dirty());
    }
    void lockBeforeReplacementRetainsValueAndBlocksClose() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        const auto original = contents(path); auto* service = window.findChild<DocumentService*>();
        XmlCommitBarrier barrier; QVERIFY(setDocumentGate(window, barrier.gate()));
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Abort at Lock"); window.close();
        QTRY_VERIFY(barrier.entered()); service->lock(window.selectedDocument()); barrier.release();
        QTRY_VERIFY(!window.lastError().isEmpty()); QVERIFY(window.isVisible()); QCOMPARE(contents(path), original);
        QCOMPARE(gridValue(window, "name").toString(), QString("Abort at Lock")); QVERIFY(window.findChild<ProfileEditor*>()->dirty());
        QVERIFY(!window.findChild<QMessageBox*>("saveRecovery"));
    }
    void lockAfterReplacementAdoptsButSuppressesClose() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        auto* service = window.findChild<DocumentService*>();
        QVERIFY(setDocumentFault(window, AtomicFile::Fault::DirectorySync));
        XmlCommitBarrier barrier; QVERIFY(setDocumentGate(window, barrier.gate(true)));
        auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Committed at Lock"); window.close();
        QTRY_VERIFY(barrier.entered()); service->lock(window.selectedDocument()); barrier.release();
        QTRY_VERIFY(window.findChild<QMessageBox*>("durabilityWarning")); QVERIFY(window.isVisible()); QVERIFY(contents(path).contains("Committed at Lock"));
        QVERIFY(!window.findChild<ProfileEditor*>()->dirty());
        window.findChild<QMessageBox*>("durabilityWarning")->button(QMessageBox::Ok)->click(); QCoreApplication::processEvents(); QVERIFY(window.isVisible());
        window.close(); QTRY_VERIFY(!window.isVisible());
    }
    void explicitEmptyOverrideSurvivesRestart() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); QString id;
        {
            MainWindow window(path, true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
            window.addFolder(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window);
            setGridText(window, "username", "parent-account"); QVERIFY(waitGridSaved(window)); window.addConnection(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window);
            id = window.selectedId(); setGridText(window, "username", ""); QVERIFY(waitGridSaved(window)); window.close();
        }
        MainWindow reopened(path, true); reopened.show(); QTRY_COMPARE(reopened.documentState(), MainWindow::DocumentState::Open);
        auto* tree = reopened.findChild<QTreeWidget*>("connectionTree");
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if (treeProfileId(*it) == id) tree->setCurrentItem(*it);
        QVERIFY(gridLocal(reopened, "username")); QCOMPARE(gridValue(reopened, "username").toString(), QString());
    }
    void gridModesPreserveValueAndSelection() {
        QTemporaryDir dir; const auto path = dir.filePath("p"); MainWindow window(path, true); OPEN_CONNECTION(window);
        auto* tree = window.findChild<QTreeWidget*>("configTree"); auto* edit = qobject_cast<QLineEdit*>(gridEditor(window, "name")); edit->selectAll(); QTest::keyClicks(edit, "Mode value");
        window.findChild<QAction*>("actionAlphabetical")->trigger(); QTRY_VERIFY(contents(path).contains("Mode value")); QVERIFY(waitGridSaved(window));
        QCOMPARE(tree->currentItem()->data(0, PropertyDelegate::KeyRole).toString(), QString("name")); QVERIFY(!gridRow(window, "name")->parent());
        QVERIFY(window.findChild<QAction*>("actionAlphabetical")->isChecked()); QVERIFY(!window.findChild<QAction*>("actionCategorized")->isChecked());
        window.findChild<QAction*>("actionCategorized")->trigger(); QVERIFY(gridRow(window, "name")->parent());
        gridRow(window, "host")->parent()->setExpanded(false); window.findChild<QAction*>("actionAlphabetical")->trigger(); window.findChild<QAction*>("actionCategorized")->trigger();
        QVERIFY(!gridRow(window, "host")->parent()->isExpanded()); QCOMPARE(gridValue(window, "name").toString(), QString("Mode value"));
    }
    void autoSavePreservesTreeSortFilterAndExpansion() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); OPEN_CONNECTION(window);
        setGridText(window, "name", "B server"); QVERIFY(waitGridSaved(window)); const auto first = window.selectedId();
        window.addConnection(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window);
        setGridText(window, "name", "A server"); QVERIFY(waitGridSaved(window)); const auto second = window.selectedId();
        auto* tree = window.findChild<QTreeWidget*>("connectionTree"); window.findChild<QAction*>("actionSort")->trigger();
        QCOMPARE(treeProfileId(tree->topLevelItem(0)->child(0)), second);
        window.findChild<QLineEdit*>("searchEdit")->setText("server");
        gridRow(window, "host")->parent()->setExpanded(false);
        setGridText(window, "name", "Z server"); QVERIFY(waitGridSaved(window));
        QCOMPARE(treeProfileId(tree->topLevelItem(0)->child(0)), first); QCOMPARE(window.selectedId(), second);
        QCOMPARE(window.findChild<QLineEdit*>("searchEdit")->text(), QString("server")); QVERIFY(!tree->currentItem()->isHidden());
        QVERIFY(!gridRow(window, "host")->parent()->isExpanded());
    }
    void adoptRefreshesParentChoicesWithoutRebuildingRows() {
        QWidget container; QTreeWidget tree(&container); tree.setObjectName("configTree"); ProfileEditor editor(&tree);
        QJsonObject parent{{"id", "parent"}, {"kind", "folder"}, {"name", "Before"}};
        QJsonObject child{{"id", "child"}, {"name", "Child"}, {"parent", "parent"}, {"overrides", QJsonObject{}}};
        editor.load(child, {{"parent", parent}, {"child", child}});
        auto* row = gridRow(container, "parent"); tree.setCurrentItem(row, 1);
        parent["name"] = "After"; editor.adopt(child, {{"parent", parent}, {"child", child}});
        QCOMPARE(gridRow(container, "parent"), row); QCOMPARE(tree.currentItem(), row);
        QCOMPARE(row->text(1), QString("After"));
        QCOMPARE(row->data(1, PropertyDelegate::OptionsRole).toList().at(1).toMap().value("text").toString(), QString("After"));
        QVERIFY(!editor.dirty());
    }
    void inheritanceWithoutSourceLabels() {
        QTemporaryDir dir; MainWindow window(dir.filePath("p"), true); window.show(); QTRY_COMPARE(window.documentState(), MainWindow::DocumentState::Open);
        window.addFolder(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window); setGridText(window, "name", "Team"); QVERIFY(waitGridSaved(window));
        setGridText(window, "username", "inherited-account"); QVERIFY(waitGridSaved(window)); window.addConnection(); QTRY_VERIFY(!documentBusy(window)); closeGridEditors(window);
        auto* tree = window.findChild<QTreeWidget*>("configTree"); tree->setCurrentItem(gridRow(window, "username"), 1);
        QCOMPARE(window.findChild<QLabel*>("configHelpTitle")->text(), QString("Username")); QVERIFY(!window.findChild<QLabel*>("configHelpText")->text().contains("Source:"));
        QCOMPARE(gridValue(window, "username").toString(), QString("inherited-account")); QVERIFY(!gridLocal(window, "username"));
        setGridText(window, "username", ""); QVERIFY(waitGridSaved(window)); QVERIFY(gridLocal(window, "username")); QCOMPARE(gridValue(window, "username").toString(), QString());
        setGridLocal(window, "username", false); QCOMPARE(gridValue(window, "username").toString(), QString("inherited-account"));
    }
};
QTEST_MAIN(PropertyGridTest)
#include "property_grid.moc"
