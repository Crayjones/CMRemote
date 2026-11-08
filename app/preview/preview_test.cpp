#include "PreviewWindow.h"
#include "PreviewData.h"
#include "Branding.h"
#include "ProfileEditor.h"
#include <QAbstractButton>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QTabBar>
#include <QComboBox>
#include "DockManager.h"
#include "DockWidget.h"
#include "DockAreaWidget.h"
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include "DockAreaWidget.h"
#include "DockWidgetTab.h"
#include <QPointer>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#ifdef Q_OS_MACOS
QStringList nativeMenuTitles(const QString& parent = {});
bool triggerNativeMenuItem(const QString& parent, const QString& title);
#endif

// A panel of preview session tabs: the Qt ADS dock area holding them, read like
// the QTabWidget panels used to be.
struct Tabs {
    QPointer<ads::CDockAreaWidget> area;
    const Tabs* operator->() const { return this; }
    QList<ads::CDockWidget*> docks() const {
        QList<ads::CDockWidget*> result;
        if (area) for (auto* dock : area->dockWidgets()) if (dock->property("sessionDock").toBool()) result.append(dock);
        return result;
    }
    int count() const { return int(docks().size()); }
    ads::CDockWidget* dock(int index) const { const auto all = docks(); return index >= 0 && index < all.size() ? all.at(index) : nullptr; }
    QWidget* widget(int index) const { auto* found = dock(index); return found ? found->widget() : nullptr; }
    ads::CDockWidgetTab* tab(int index) const { auto* found = dock(index); return found ? found->tabWidget() : nullptr; }
    int indexOf(QWidget* page) const { const auto all = docks(); for (int i = 0; i < all.size(); ++i) if (all.at(i)->widget() == page) return i; return -1; }
    QWidget* currentWidget() const { return area && area->currentDockWidget() ? area->currentDockWidget()->widget() : nullptr; }
    int currentIndex() const { return indexOf(currentWidget()); }
    QString tabText(int index) const { auto* found = dock(index); return found ? found->windowTitle() : QString(); }
    QIcon tabIcon(int index) const { auto* found = dock(index); return found ? found->icon() : QIcon(); }
    // Reorders like dragging the tab within its panel.
    void moveTab(int from, int to) const { area->dockManager()->addDockWidgetTabToArea(dock(from), area, int(area->dockWidgets().indexOf(dock(to)))); }
    // The tab's close button, after Qt ADS hands the request to the preview.
    void close(int index) const { dock(index)->requestCloseDockWidget(); QCoreApplication::sendPostedEvents(); }
};
static Tabs general(QWidget& window) { return {window.findChild<ads::CDockWidget*>("documentsDock")->dockAreaWidget()}; }
static Tabs panelOf(QWidget* page) {
    for (auto* widget = page ? page->parentWidget() : nullptr; widget; widget = widget->parentWidget())
        if (auto* dock = qobject_cast<ads::CDockWidget*>(widget)) return {dock->dockAreaWidget()};
    return {};
}

class PreviewTest : public QObject {
    Q_OBJECT
private:
    template<class T> T* get(PreviewWindow& window, const char* name) {
        auto* widget = window.findChild<T*>(QString::fromLatin1(name));
        Q_ASSERT(widget);
        return widget;
    }
    void show(PreviewWindow& window) {
        window.show();
        QTest::qWait(80);
    }
    QMenu* context(QWidget* widget, const QPoint& position, bool keyboard = false) {
        if (auto* previous = QApplication::activePopupWidget()) previous->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QContextMenuEvent event(keyboard ? QContextMenuEvent::Keyboard : QContextMenuEvent::Mouse,
                                position, widget->mapToGlobal(position));
        QApplication::sendEvent(widget, &event);
        return qobject_cast<QMenu*>(QApplication::activePopupWidget());
    }
    QMenu* nodeMenu(QTreeWidget* tree, QTreeWidgetItem* node) {
        tree->scrollToItem(node);
        return context(tree->viewport(), tree->visualItemRect(node).center());
    }
    QMenu* tabMenu(const Tabs& tabs, int index) {
        auto* tab = tabs.tab(index); return tab ? context(tab, tab->rect().center()) : nullptr;
    }
    QAction* command(QMenu* menu, const char* name) {
        for (auto* action : menu->actions()) {
            if (action->objectName() == QString::fromLatin1(name)) return action;
            if (action->menu()) {
                if (auto* child = command(action->menu(), name)) return child;
            }
        }
        return nullptr;
    }
    void trigger(QMenu* menu, const char* name) {
        auto* action = command(menu, name);
        Q_ASSERT(action);
        menu->close();
        action->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void quick(PreviewWindow& window, const QString& host, int protocol = 0) {
        auto* button = get<QToolButton>(window, "quickConnectButton");
        button->menu()->actions()[protocol]->trigger();
        auto* combo = get<QComboBox>(window, "quickConnectCombo");
        combo->setEditText(host);
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
    }
    void rename(PreviewWindow& window, const QString& name, bool accept = true) {
        auto* dialog = get<QInputDialog>(window, "previewRename");
        dialog->setTextValue(name);
        if (accept) dialog->accept(); else dialog->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void confirmDelete(PreviewWindow& window, bool yes) {
        auto* dialog = get<QMessageBox>(window, "previewDelete");
        dialog->button(yes ? QMessageBox::Yes : QMessageBox::No)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
private slots:
    void initTestCase() { QApplication::setStyle("Fusion"); }

    void firstPartyBrandingUsesSourcePixelsAndPreservesIdentity() {
        const auto applicationName = QCoreApplication::applicationName();
        const auto organizationName = QCoreApplication::organizationName();
        PreviewWindow window;
        QCOMPARE(QCoreApplication::applicationName(), applicationName);
        QCOMPARE(QCoreApplication::organizationName(), organizationName);
        QCOMPARE(QGuiApplication::applicationDisplayName(), QString("CMRemote"));
        QCOMPARE(QGuiApplication::desktopFileName(), QString("org.remotemanager.RemoteManager"));
        QCOMPARE(window.windowTitle(), QString("CMRemote — Qt UI preview"));
        const QList<QIcon> icons{Branding::applicationIcon(), QGuiApplication::windowIcon(), window.windowIcon()};
        for (const auto& icon : icons) {
            QCOMPARE(icon.availableSizes().size(), 9);
            for (const int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
                const QImage expected(QString(":/branding/cmremote-%1.png").arg(size));
                QVERIFY(!expected.isNull()); QCOMPARE(expected.size(), QSize(size, size));
                QVERIFY(icon.availableSizes().contains(expected.size()));
                const auto actual = icon.pixmap(expected.size(), 1.0).toImage();
                QCOMPARE(actual.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                         expected.convertToFormat(QImage::Format_ARGB32_Premultiplied));
            }
        }
        auto* aboutAction = get<QAction>(window, "actionAbout");
        QCOMPARE(aboutAction->text(), QString("About CMRemote")); aboutAction->trigger();
        auto* about = get<QMessageBox>(window, "aboutDialog");
#ifndef Q_OS_MACOS
        // QMessageBox ignores window titles on macOS, including offscreen.
        QCOMPARE(about->windowTitle(), QString("About CMRemote UI preview"));
#endif
        QCOMPARE(about->textFormat(), Qt::PlainText);
        QCOMPARE(about->text(), QString("CMRemote — Cross-platform Multi Remote\nhttps://github.com/Crayjones/CMRemote"));
        const auto aboutImage = about->iconPixmap().toImage(); QVERIFY(!aboutImage.isNull());
        const QImage aboutExpected(QString(":/branding/cmremote-%1.png").arg(aboutImage.width()));
        QVERIFY(!aboutExpected.isNull());
        QCOMPARE(aboutImage.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                 aboutExpected.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        about->accept();
    }

    void previewCannotStoreSecrets() {
        PreviewWindow window; show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree"); tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        auto* config = get<QTreeWidget>(window, "configView"); QTreeWidgetItem* password = nullptr;
        for (QTreeWidgetItemIterator it(config); *it; ++it) if ((*it)->text(0) == "Password") password = *it;
        QVERIFY(password); QVERIFY(!(password->flags() & Qt::ItemIsEditable)); QCOMPARE(password->text(1), QString("Unavailable"));
        config->setCurrentItem(password, 1); config->setFocus(); QTest::keyClick(config, Qt::Key_Return); QTest::keyClick(config, Qt::Key_F2);
        QVERIFY(config->findChildren<QLineEdit*>().isEmpty());
        auto* editor = get<ProfileEditor>(window, "profileEditor"); QVERIFY(!editor->canEditPassword());
        editor->editPassword(); editor->clearPassword(); QVERIFY(!editor->passwordPending());
        QVERIFY(!editor->draft().contains("password") && !editor->draft()["overrides"].toObject().contains("password"));
    }

    void previewCannotAccessDocumentProtection() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        tree->setCurrentItem(tree->topLevelItem(0));
        auto* config = get<QTreeWidget>(window, "configView");
        QCOMPARE(config->columnCount(), 2);
        QCOMPARE(config->topLevelItem(0)->text(0), QString("Display"));
        auto* name = config->topLevelItem(0)->child(0);
        auto* protection = config->topLevelItem(0)->child(1);
        QCOMPARE(name->text(1), QString("Connections"));
        QCOMPARE(protection->text(0), QString("Password protect"));
        QCOMPARE(protection->text(1), QString("No"));
        QVERIFY(!(name->flags() & Qt::ItemIsEditable));
        QVERIFY(!(protection->flags() & Qt::ItemIsEditable));
        config->setCurrentItem(protection, 1);
        config->setFocus();
        QTest::keyClick(config, Qt::Key_F2);
        QTest::keyClick(config, Qt::Key_Return);
        QVERIFY(config->findChildren<QComboBox*>().isEmpty());
        QVERIFY(window.findChildren<QDialog*>().isEmpty());
        // The file's help text belongs to the root's Name row.
        config->setCurrentItem(name, 1);
        QVERIFY(get<QLabel>(window, "configHelpText")->text().contains("display-only"));
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        tree->setCurrentItem(tree->topLevelItem(0));
        QCOMPARE(config->topLevelItem(0)->child(1)->text(1), QString("No"));
        auto* toolbar = get<QToolBar>(window, "connectionsToolbar");
        QStringList actions;
        for (auto* action : toolbar->actions()) {
            QVERIFY(!action->isSeparator());
            QVERIFY(!action->icon().isNull());
            actions << action->objectName();
        }
        QCOMPARE(actions, QStringList({"actionAddConnection", "actionAddFolder", "actionExpandAll", "actionSort"}));
        auto* sort = get<QAction>(window, "actionSort");
        QVERIFY(sort->menu());
        QCOMPARE(sort->menu()->actions().size(), 2);
    }

    void previewSharesGridRootAndToolbarWithoutStorage() {
        PreviewWindow window; show(window);
        QStringList top; for (auto* action : window.menuBar()->actions()) top << action->text().remove('&'); QCOMPARE(top, QStringList({"File", "View", "Tools", "Help"}));
        QStringList order; for (auto* action : get<QMenu>(window, "menuView")->actions()) order << (action->isSeparator() ? "|" : action->text());
        QCOMPARE(order, QStringList({"New Panel", "Connection Panels", "|", "Connections", "Config", "Notifications", "Screenshots", "|", "Jump To", "Reset layout", "Lock toolbar positions", "|", "Quick Connect Toolbar", "External Tools Toolbar", "Multi SSH Toolbar", "|", "Full Screen"}));
        QCOMPARE(get<QAction>(window, "actionViewConfig")->menuRole(), QAction::NoRole);
        QCOMPARE(get<QAction>(window, "actionOptions")->menuRole(), QAction::PreferencesRole);
        QVERIFY(!get<QAction>(window, "actionViewScreenshots")->isEnabled()); QVERIFY(!get<QAction>(window, "actionExternalTools")->isEnabled());
        auto* tree = get<QTreeWidget>(window, "connectionTree"); QVERIFY(!tree->dragEnabled()); QCOMPARE(tree->topLevelItem(0)->text(0), QString("Connections"));
        int roots = 0; for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->text(0) == "Connections") ++roots; QCOMPARE(roots, 1);
        auto* grid = get<QTreeWidget>(window, "configView"); QCOMPARE(grid->columnCount(), 2); QVERIFY(grid->findChildren<QLineEdit*>().isEmpty());
        QCOMPARE(get<QToolBar>(window, "connectionsToolbar")->actions().size(), 4);
        for (auto* child : window.findChildren<QObject*>()) QVERIFY(QByteArray(child->metaObject()->className()) != "VaultService");
        auto tabs = general(window); auto* page = tabs->widget(0);
        quick(window, "second-host"); QCOMPARE(tabs->count(), 2); QVERIFY(tabs->currentWidget() != page);
        auto* jump = get<QMenu>(window, "menuJumpTo"); jump->popup(window.mapToGlobal(window.rect().center())); QTRY_VERIFY(jump->actions().size() >= 9); jump->hide();
        QAction* target = nullptr; for (auto* action : jump->actions()) if (action->data().value<QWidget*>() == page) target = action; QVERIFY(target);
        target->trigger(); QCOMPARE(tabs->currentWidget(), page); QCOMPARE(tabs->count(), 2);
        QVERIFY(get<QLabel>(window, "sessionMessage")->text().contains("no network connection"));
    }
    void sourceMenusAndDockGeometry() {
        PreviewWindow window;
        show(window);
        QStringList menus;
        for (auto* action : window.menuBar()->actions()) menus << action->text().remove('&');
        QCOMPARE(menus, QStringList({"File", "View", "Tools", "Help"}));
        QCOMPARE(window.menuBar()->isNativeMenuBar(),
                 QGuiApplication::platformName() == "cocoa");
        QCOMPARE(window.size(), QSize(1129, 571));
        for (const auto* name : {"actionUnlockPasswords", "actionLockPasswords", "actionChangeMaster", "actionDifferentCredentials"}) {
            auto* action = get<QAction>(window, name); QVERIFY(!action->isEnabled()); QVERIFY(!action->isVisible());
        }
        auto* connections = get<ads::CDockWidget>(window, "connectionsDock");
        auto* config = get<ads::CDockWidget>(window, "configDock");
        auto* notifications = get<ads::CDockWidget>(window, "notificationsDock");
        auto* quick = get<QToolBar>(window, "quickConnectToolbar");
        // Docks live in separate dock areas: compare where they sit in the window.
        const auto place = [&window](QWidget* widget) { return QRect(widget->mapTo(&window, QPoint()), widget->size()); };
        QWidget* documents = general(window).area.data(); QVERIFY(documents);
        QVERIFY(place(connections).right() < place(documents).left()); QVERIFY(place(config).right() < place(documents).left());
        QCOMPARE(place(connections).x(), place(config).x());
        QCOMPARE(connections->width(), config->width());
        QVERIFY(place(connections).bottom() < place(config).top());
        QVERIFY(notifications->isClosed()); QVERIFY(!get<QAction>(window, "actionViewNotifications")->isChecked());
        QVERIFY(quick->width() < window.width() / 2);
        get<QAction>(window, "actionViewNotifications")->trigger(); QTest::qWait(80);
        QVERIFY(place(config).bottom() < place(notifications).top());
        QVERIFY(place(quick).bottom() < place(connections).top());
        QVERIFY(place(notifications).x() <= place(connections).x());
        QVERIFY(notifications->width() >= window.width() - 4);
        auto* combo = get<QComboBox>(window, "quickConnectCombo");
        QVERIFY(combo->isVisible());
        QVERIFY(combo->width() >= 180 && combo->width() <= 320);
        QVERIFY(!get<QToolBar>(window, "multiSshToolbar")->isVisible());
        QVERIFY(!get<QToolBar>(window, "externalToolsToolbar")->isVisible());
        QVERIFY(!get<QAction>(window, "actionSaveFile")->isEnabled());
        auto* surface = get<QLabel>(window, "sessionMessage");
        QVERIFY(surface->text().contains("no network connection"));
        QVERIFY(surface->text().contains('\n'));
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == "cocoa") {
            for (const auto& title : menus)
                QTRY_VERIFY2(nativeMenuTitles().contains(title), qPrintable(nativeMenuTitles().join(", ")));
            QVERIFY(nativeMenuTitles("View").contains("Connections"));
            QVERIFY(nativeMenuTitles("View").contains("Config"));
            QVERIFY(!nativeMenuTitles().contains("Sessions"));
            QVERIFY(nativeMenuTitles("View").contains("Jump To"));
            QVERIFY(triggerNativeMenuItem("View", "Connections"));
            QTRY_VERIFY(!connections->isVisible());
            QVERIFY(triggerNativeMenuItem("View", "Connections"));
            QTRY_VERIFY(connections->isVisible());
        }
#endif
    }

    void searchAndPropertyHelp() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        auto* root = tree->topLevelItem(0);
        auto* server = root->child(0);
        auto* properties = get<QTreeWidget>(window, "configView");
        auto* title = get<QLabel>(window, "configHelpTitle");
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Display"));
        QCOMPARE(title->text(), QString("Name"));
        QCOMPARE(properties->topLevelItem(0)->child(0)->text(1), QString("Connections"));
        tree->setCurrentItem(server);
        // The real Config grid over in-memory sample settings.
        auto* editor = window.findChild<ProfileEditor*>(); QVERIFY(editor);
        auto* port = editor->row("port"); QVERIFY(port && !port->isHidden());
        QCOMPARE(port->text(0), QString("Port")); QCOMPARE(port->text(1), QString("3389")); QCOMPARE(port->parent()->text(0), QString("Protocol"));
        properties->setCurrentItem(port);
        QCOMPARE(title->text(), QString("Port"));
        QVERIFY(get<QLabel>(window, "configHelpText")->text().contains("port the selected protocol"));
        get<QAction>(window, "actionInheritance")->trigger();
        QCOMPARE(editor->row("port")->text(0), QString("Inherit Port"));
        get<QAction>(window, "actionAlphabetical")->trigger();
        QVERIFY(!editor->row("port")->parent());
        tree->setCurrentItem(root);
        QVERIFY(!get<QAction>(window, "actionInheritance")->isEnabled());
        QVERIFY(get<QAction>(window, "actionProperties")->isChecked());
        auto* search = get<QLineEdit>(window, "searchEdit");
        search->setText("SERVER01");
        QVERIFY(!root->isHidden());
        QVERIFY(!server->isHidden());
        QVERIFY(tree->topLevelItem(1)->isHidden());
        search->setText("no-match");
        QVERIFY(root->isHidden());
        search->clear();
        QVERIFY(!root->isHidden());
        QVERIFY(!tree->topLevelItem(1)->isHidden());
        get<QAction>(window, "actionFavorites")->trigger();
        QVERIFY(!root->isHidden());
        QVERIFY(!server->isHidden());
        QVERIFY(tree->topLevelItem(1)->isHidden());
    }

    void sessionTabsQuickConnectPanelsAndClose() {
        PreviewWindow window;
        show(window);
        auto tabs = general(window);
        QVERIFY(get<ads::CDockWidget>(window, "documentsDock")->isClosed());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString("server01"));
        auto* combo = get<QComboBox>(window, "quickConnectCombo");
        combo->setEditText("demo-host");
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("demo-host"));
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(tabs->count(), 2);
        get<QAction>(window, "actionPreviousSession")->trigger();
        QCOMPARE(tabs->currentIndex(), 0);
        get<QAction>(window, "actionNextSession")->trigger();
        QCOMPARE(tabs->currentIndex(), 1);
        auto* button = get<QToolButton>(window, "quickConnectButton");
        button->menu()->actions()[1]->trigger();
        QCOMPARE(button->text(), QString("VNC"));
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(tabs->count(), 3);
        QVERIFY(get<QListWidget>(window, "notificationsList")->currentItem()->text().contains("VNC"));
        QVERIFY(get<QPlainTextEdit>(window, "notificationText")->toPlainText().contains("demo-host"));
        tabs.close(0);
        QCOMPARE(tabs->count(), 2);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        combo->setEditText("server01");
        button->menu()->actions()[0]->trigger();
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(tabs->count(), 3);
        // New Panel moves the current tab into a panel of its own; new tabs follow it.
        auto* moved = tabs->currentWidget();
        get<QAction>(window, "actionAddPanel")->trigger();
        auto panel = panelOf(moved); QVERIFY(panel.area && panel.area != tabs.area);
        QCOMPARE(panel->count(), 1); QCOMPARE(tabs->count(), 2);
        combo->setEditText("panel-host");
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(panel->count(), 2);
        QCOMPARE(tabs->count(), 2);
        auto* panels = get<QMenu>(window, "menuConnectionPanels"); emit panels->aboutToShow();
        QStringList names; for (auto* action : panels->actions()) names << action->text(); QCOMPARE(names, QStringList({"General", "Panel 1"}));
        // Closing every tab shows the empty General placeholder.
        while (panel->count()) panel.close(0);
        while (tabs->count()) tabs.close(0);
        QVERIFY(!get<ads::CDockWidget>(window, "documentsDock")->isClosed());
    }
    void doubleClickAndMemoryOnlyTreeActions() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        auto* server = tree->topLevelItem(0)->child(0);
        auto tabs = general(window);
        tabs.close(0);
        QCOMPARE(tabs->count(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto pos = tree->visualItemRect(server).center();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, pos);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString("server01"));
        get<QAction>(window, "actionAddFolder")->trigger();
        auto* folder = tree->currentItem();
        QCOMPARE(folder->data(0, Qt::UserRole).toString(), QString("folder"));
        get<QAction>(window, "actionAddConnection")->trigger();
        QCOMPARE(tree->currentItem()->parent(), folder);
        QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("connection"));
        get<QAction>(window, "actionCollapseAll")->trigger();
        QVERIFY(!folder->isExpanded());
        get<QAction>(window, "actionExpandAll")->trigger();
        QVERIFY(folder->isExpanded());
    }

    void visibilityAndResetLayout() {
        PreviewWindow window;
        show(window);
        auto* config = get<ads::CDockWidget>(window, "configDock");
        auto* view = get<QAction>(window, "actionViewConfig");
        view->trigger();
        QVERIFY(!config->isVisible());
        config->toggleView(true);
        QVERIFY(view->isChecked());
        config->toggleView(false);
        QVERIFY(!view->isChecked());
        get<QAction>(window, "actionViewMultiSsh")->trigger();
        QVERIFY(get<QToolBar>(window, "multiSshToolbar")->isVisible());
        get<QAction>(window, "actionLockToolbars")->trigger();
        QVERIFY(!get<QToolBar>(window, "quickConnectToolbar")->isMovable());
        config->toggleView(true); config->setFloating();
        QVERIFY(config->isFloating());
        get<QAction>(window, "actionResetLayout")->trigger();
        QTest::qWait(80);
        QVERIFY(!config->isFloating());
        QTRY_VERIFY(config->isVisible());
        QVERIFY(view->isChecked());
        QVERIFY(!get<QToolBar>(window, "multiSshToolbar")->isVisible());
        QVERIFY(!get<QAction>(window, "actionViewMultiSsh")->isChecked());
        const auto documents = get<ads::CDockWidget>(window, "documentsDock");
        QVERIFY(config->mapTo(&window, QPoint(config->width(), 0)).x() < documents->mapTo(&window, QPoint()).x());
    }

    void contextMenuTargetsClickedObject() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        auto* root = tree->topLevelItem(0);
        auto* server = root->child(0);
        auto* menu = nodeMenu(tree, server);
        QVERIFY(menu);
        QCOMPARE(menu->objectName(), QString("treeContextMenu"));
        QCOMPARE(tree->currentItem(), server);
        QCOMPARE(tree->selectedItems().size(), 1);
        menu->close();
        QCOMPARE(root->childCount(), 1);
        QVERIFY(!context(tree->viewport(), QPoint(10, tree->viewport()->height() - 3)));
        menu = context(tree, QPoint(), true);
        QVERIFY(menu);
        QVERIFY(command(menu, "actionTreeConnect")->isEnabled());
        menu->close();
        quick(window, "inactive-host");
        auto tabs = general(window);
        auto* initial = tabs->widget(0);
        QCOMPARE(tabs->currentIndex(), 1);
        menu = tabMenu(tabs, 0);
        QVERIFY(menu);
        QCOMPARE(tabs->currentIndex(), 1);
        trigger(menu, "actionTabRename");
        rename(window, "renamed inactive");
        QCOMPARE(tabs->tabText(tabs->indexOf(initial)), QString("renamed inactive"));
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("inactive-host"));
        menu = context(tabs.tab(tabs->currentIndex()), QPoint(), true);
        QVERIFY(menu);
        trigger(menu, "actionTabRename");
        rename(window, "keyboard current");
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("keyboard current"));
    }

    void treeContextMenuRestrictionsAndLocalActions() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        auto* root = tree->topLevelItem(0);
        auto* server = root->child(0);
        auto* menu = nodeMenu(tree, root);
        for (const auto* name : {"actionTreeConnect", "actionTreeDuplicate", "actionTreeDelete", "actionMoveUp", "actionCopyHostname"})
            QVERIFY(!command(menu, name)->isEnabled());
        QVERIFY(command(menu, "actionTreeRename")->isEnabled());
        menu = nodeMenu(tree, tree->topLevelItem(1));
        for (const auto* name : {"actionTreeConnect", "actionTreeRename", "actionTreeDelete", "actionAddConnection", "actionAddRoot", "actionTreeSort"})
            QVERIFY(!command(menu, name)->isEnabled());
        menu = nodeMenu(tree, server);
        for (const auto* name : {"actionWakeOnLan", "actionClearCredentials", "actionTreeExternalTools", "actionImport", "actionExport", "actionTreeInheritance", "actionConnectOptions"})
            QVERIFY(!command(menu, name)->isEnabled());
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                          menu->actionGeometry(command(menu, "actionTreeRename")).center());
        QTRY_VERIFY(window.findChild<QInputDialog*>("previewRename"));
        rename(window, "cancelled", false);
        QCOMPARE(server->text(0), QString("server01"));
        menu = nodeMenu(tree, server);
        trigger(menu, "actionTreeRename");
        rename(window, "Friendly display");
        QCOMPARE(server->text(0), QString("Friendly display"));
        QCOMPARE(server->data(0, preview::EndpointRole).toString(), QString("server01"));
        auto* config = get<QTreeWidget>(window, "configView");
        QCOMPARE(config->window()->findChild<ProfileEditor*>()->row("name")->text(1), QString("Friendly display"));
        QCOMPARE(config->window()->findChild<ProfileEditor*>()->row("host")->text(1), QString("server01"));
        menu = nodeMenu(tree, server);
        if (QGuiApplication::platformName() == "offscreen") {
            trigger(menu, "actionCopyHostname");
            QCOMPARE(QApplication::clipboard()->text(), QString("server01"));
            menu = nodeMenu(tree, server);
        }
        trigger(menu, "actionTreeConnect");
        QCOMPARE(general(window)->count(), 1);
        menu = nodeMenu(tree, server);
        trigger(menu, "actionTreeDuplicate");
        QCOMPARE(root->childCount(), 2);
        auto* copy = root->child(1);
        QCOMPARE(copy->text(0), QString("Friendly display (copy)"));
        menu = nodeMenu(tree, copy);
        trigger(menu, "actionMoveUp");
        QCOMPARE(root->child(0), copy);
        menu = nodeMenu(tree, copy);
        trigger(menu, "actionMoveDown");
        QCOMPARE(root->child(1), copy);
        menu = nodeMenu(tree, root);
        trigger(menu, "actionSortDescending");
        QCOMPARE(root->child(0), copy);
        menu = nodeMenu(tree, copy);
        trigger(menu, "actionAddFolder");
        auto* folder = tree->currentItem();
        QCOMPARE(folder->parent(), root);
        menu = nodeMenu(tree, folder);
        trigger(menu, "actionAddConnection");
        QCOMPARE(tree->currentItem()->parent(), folder);
        folder->setSelected(true);
        tree->currentItem()->setSelected(true);
        menu = nodeMenu(tree, folder);
        QCOMPARE(tree->selectedItems().size(), 2);
        QVERIFY(!command(menu, "actionTreeRename")->isEnabled());
        QVERIFY(command(menu, "actionTreeDelete")->text().contains("(1)"));
        trigger(menu, "actionTreeDelete");
        confirmDelete(window, false);
        QCOMPARE(folder->childCount(), 1);
        menu = nodeMenu(tree, folder);
        trigger(menu, "actionTreeDelete");
        confirmDelete(window, true);
        QCOMPARE(root->childCount(), 2);
        // A popup target removed by another command must not be dereferenced.
        menu = nodeMenu(tree, copy);
        delete copy;
        trigger(menu, "actionTreeRename");
        QVERIFY(!window.findChild<QInputDialog*>("previewRename"));
        QCOMPARE(root->childCount(), 1);
        get<QLineEdit>(window, "searchEdit")->setText("Friendly");
        QVERIFY(!server->isHidden());
    }

    void sessionContextMenuActionsAndProtocolGating() {
        PreviewWindow window;
        show(window);
        auto tabs = general(window);
        auto* initial = tabs->widget(0);
        auto* menu = tabMenu(tabs, 0);
        QVERIFY(command(menu, "actionTabFullscreen")->isVisible());
        QVERIFY(!command(menu, "actionTabFullscreen")->isEnabled());
        QVERIFY(!command(menu, "actionTabChat")->isVisible());
        QVERIFY(!command(menu, "actionPuttySettings")->isVisible());
        QVERIFY(!command(menu, "actionTabReconnect")->isEnabled());
        trigger(menu, "actionTabRename");
        rename(window, "Caption");
        quick(window, "server01");
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString("Caption"));
        menu = tabMenu(tabs, 0);
        trigger(menu, "actionTabDuplicate");
        QCOMPARE(tabs->count(), 2);
        QVERIFY(tabs->widget(1) != initial);
        quick(window, "server01");
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentWidget(), initial);
        quick(window, "vnc-host", 1);
        menu = tabMenu(tabs, tabs->currentIndex());
        QVERIFY(!command(menu, "actionTabFullscreen")->isVisible());
        for (const auto* name : {"actionTabSmartSize", "actionTabViewOnly", "actionTabChat", "actionTabRefresh", "actionSpecialKeys"}) {
            QVERIFY(command(menu, name)->isVisible());
            QVERIFY(!command(menu, name)->isEnabled());
        }
        menu->close();
        quick(window, "ssh-host", 3);
        menu = tabMenu(tabs, tabs->currentIndex());
        QVERIFY(command(menu, "actionTabTransfer")->isVisible());
        QVERIFY(command(menu, "actionPuttySettings")->isVisible());
        QVERIFY(!command(menu, "actionTabTransfer")->isEnabled());
        QVERIFY(!command(menu, "actionTabSmartSize")->isVisible());
        menu->close();
        // Resolve the target's position when executing, not when the popup opens.
        menu = tabMenu(tabs, 1);
        auto* kept = tabs->widget(1);
        tabs.moveTab(1, 0);
        trigger(menu, "actionTabDisconnectRight");
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->widget(0), kept);
        quick(window, "other-host");
        menu = tabMenu(tabs, 0);
        trigger(menu, "actionTabDisconnectOthers");
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->widget(0), kept);
        menu = tabMenu(tabs, 0);
        QPointer<QWidget> deleted(kept);
        trigger(menu, "actionTabDisconnect");
        QVERIFY(!deleted);
        QCOMPARE(tabs->count(), 0);
        quick(window, "stale-host");
        menu = tabMenu(tabs, 0);
        auto* stale = tabs->dock(0);
        stale->dockManager()->removeDockWidget(stale);
        delete stale;
        trigger(menu, "actionTabDuplicate");
        QCOMPARE(tabs->count(), 0);
        quick(window, "first-host"); quick(window, "new-panel-host");
        auto* moved = tabs->currentWidget(); get<QAction>(window, "actionAddPanel")->trigger();
        auto panelTabs = panelOf(moved);
        menu = tabMenu(panelTabs, 0);
        QVERIFY(menu);
        trigger(menu, "actionTabDuplicate");
        QCOMPARE(panelTabs->count(), 2);
    }

    void sessionTabsMoveBetweenPanelsAndWindows() {
        PreviewWindow window;
        show(window);
        auto tabs = general(window);
        quick(window, "second-host");
        auto* first = tabs->widget(0); auto* second = tabs->widget(1);
        auto* menu = tabMenu(tabs, 1);
        QVERIFY(command(menu, "actionMoveToNewPanelRight")->isEnabled());
        trigger(menu, "actionMoveToNewPanelRight");
        auto panel = panelOf(second); QVERIFY(panel.area != tabs.area); QCOMPARE(tabs->count(), 1);
        // A panel's only tab cannot split again but can return to General by name.
        menu = tabMenu(panel, 0);
        QVERIFY(!command(menu, "actionMoveToNewPanelRight")->isEnabled());
        QAction* back = nullptr; for (auto* action : menu->findChild<QMenu*>("moveSessionMenu")->actions()) if (action->text() == "General") back = action;
        QVERIFY(back && back->isEnabled()); back->trigger(); menu->close();
        QPointer<ads::CDockAreaWidget> gone = panel.area; QTRY_VERIFY(gone.isNull());
        QCOMPARE(tabs->count(), 2); QCOMPARE(tabs->widget(1), second); QCOMPARE(tabs->widget(0), first);
        // New window floats the tab in a Qt ADS window of its own.
        menu = tabMenu(tabs, 1); trigger(menu, "actionMoveToNewWindow");
        QVERIFY(panelOf(second).area->dockContainer()->isFloating()); QCOMPARE(tabs->count(), 1);
    }
    void propertyAndNotificationContextMenus() {
        PreviewWindow window;
        show(window);
        auto* config = get<QTreeWidget>(window, "configView");
        auto* menu = context(config->viewport(), QPoint(10, 10));
        QVERIFY(menu);
        QVERIFY(!command(menu, "actionPropertyReset")->isEnabled());
        QVERIFY(command(menu, "actionShowHelp")->isChecked());
        trigger(menu, "actionShowHelp");
        QVERIFY(!get<QWidget>(window, "configHelp")->isVisible());
        menu = context(config, QPoint(), true);
        QVERIFY(!command(menu, "actionShowHelp")->isChecked());
        trigger(menu, "actionShowHelp");
        QVERIFY(get<QWidget>(window, "configHelp")->isVisible());
        get<QAction>(window, "actionViewNotifications")->setChecked(true);
        quick(window, "first-message");
        quick(window, "second-message");
        auto* list = get<QListWidget>(window, "notificationsList");
        QTRY_VERIFY(list->viewport()->height() > 3 * list->sizeHintForRow(0));
        auto* details = get<QPlainTextEdit>(window, "notificationText");
        QCOMPARE(list->count(), 3);
        menu = context(list->viewport(), list->visualItemRect(list->item(1)).center());
        QCOMPARE(list->selectedItems().size(), 1);
        QCOMPARE(command(menu, "actionNotificationCopy")->text(), QString("Copy"));
        if (QGuiApplication::platformName() == "offscreen") {
            trigger(menu, "actionNotificationCopy");
            QVERIFY(QApplication::clipboard()->text().contains("first-message"));
            QVERIFY(!QApplication::clipboard()->text().contains("second-message"));
            menu = context(list, QPoint(), true);
        }
        trigger(menu, "actionNotificationDelete");
        QCOMPARE(list->count(), 2);
        QVERIFY(!details->toPlainText().contains("first-message"));
        list->clearSelection();
        list->item(0)->setSelected(true);
        list->item(1)->setSelected(true);
        menu = context(list->viewport(), list->visualItemRect(list->item(0)).center());
        QCOMPARE(list->selectedItems().size(), 2);
        menu->close();
        menu = context(list->viewport(), QPoint(10, list->viewport()->height() - 3));
        QCOMPARE(list->selectedItems().size(), 0);
        QCOMPARE(command(menu, "actionNotificationDelete")->text(), QString("Delete All"));
        QCOMPARE(command(menu, "actionNotificationCopy")->text(), QString("Copy All"));
        if (QGuiApplication::platformName() == "offscreen") {
            trigger(menu, "actionNotificationCopy");
            QVERIFY(QApplication::clipboard()->text().contains("second-message"));
            QVERIFY(QApplication::clipboard()->text().contains("Qt preview loaded"));
            menu = context(list, QPoint(), true);
        }
        trigger(menu, "actionNotificationDelete");
        QCOMPARE(list->count(), 0);
        QVERIFY(details->toPlainText().isEmpty());
        QVERIFY(get<QLabel>(window, "notificationDate")->text().isEmpty());
        menu = context(list, QPoint(), true);
        QVERIFY(!command(menu, "actionNotificationCopy")->isEnabled());
        QVERIFY(!command(menu, "actionNotificationDelete")->isEnabled());
        menu->close();
    }

    void externalToolbarContextAndRemoteSurface() {
        PreviewWindow window;
        show(window);
        get<QAction>(window, "actionViewExternalTools")->trigger();
        auto* toolbar = get<QToolBar>(window, "externalToolsToolbar");
        auto* menu = context(toolbar, QPoint(toolbar->width() - 10, 10));
        QVERIFY(menu);
        QCOMPARE(menu->objectName(), QString("externalToolbarContextMenu"));
        QVERIFY(command(menu, "actionShowToolText")->isChecked());
        trigger(menu, "actionShowToolText");
        QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
        menu = context(toolbar, QPoint(), true);
        QVERIFY(!command(menu, "actionShowToolText")->isChecked());
        QVERIFY(command(menu, "actionViewConfig"));
        trigger(menu, "actionShowToolText");
        QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QVERIFY(!get<QAction>(window, "actionExternalTools")->isEnabled());
        auto* surface = get<QWidget>(window, "sessionSurface");
        QVERIFY(!context(surface, surface->rect().center()));
        auto* edit = get<QLineEdit>(window, "searchEdit");
        QCOMPARE(edit->contextMenuPolicy(), Qt::DefaultContextMenu);
        QVERIFY(!window.findChild<QMenu*>("treeContextMenu"));
        // Ordinary Qt text controls retain their editing menu, rather than our tree popup.
        QScopedPointer<QMenu> standard(edit->createStandardContextMenu());
        QVERIFY(standard->actions().size() > 3);
        QCOMPARE(get<QPlainTextEdit>(window, "notificationText")->contextMenuPolicy(), Qt::DefaultContextMenu);
    }

    void originalAndConnectionIconsAreCompleteAndDecode() {
        PreviewWindow window;
        show(window);
        for (const auto& entry : {qMakePair(QString(":/original"), 47), qMakePair(QString(":/connection-icons"), 39)}) {
            const auto files = QDir(entry.first).entryList(QDir::Files);
            QCOMPARE(files.size(), entry.second);
            for (const auto& file : files) {
                const auto path = entry.first + "/" + file;
                QImageReader reader(path);
                QVERIFY2(reader.canRead(), qPrintable(path + ": " + reader.errorString()));
                QVERIFY2(!reader.read().isNull(), qPrintable(path));
            }
        }
        QVERIFY(!QFile::exists(":/original/mRemoteNG_Icon.ico"));
        for (const auto* name : {"connectionsToolbar", "configToolbar"})
            for (auto* action : get<QToolBar>(window, name)->actions())
                if (!action->isSeparator()) QVERIFY2(!action->icon().pixmap(16, 16).isNull(), qPrintable(action->objectName()));
        for (const auto* name : {"actionNewFile", "actionOpenFile", "actionOptions", "actionExit", "actionAbout",
                                 "actionHelpContents", "actionCheckUpdates", "actionFullscreen", "actionAddPanel"})
            QVERIFY2(!get<QAction>(window, name)->icon().pixmap(16, 16).isNull(), name);
        QVERIFY(!get<QToolButton>(window, "quickConnectButton")->icon().pixmap(16, 16).isNull());
        QVERIFY(!get<QToolButton>(window, "savedConnectionsButton")->icon().pixmap(16, 16).isNull());
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        QVERIFY(!tree->topLevelItem(0)->icon(0).pixmap(16, 16).isNull());
        QVERIFY(!tree->topLevelItem(0)->child(0)->icon(0).pixmap(16, 16).isNull());
        QVERIFY(!tree->topLevelItem(1)->icon(0).pixmap(16, 16).isNull());
        auto tabs = general(window);
        auto* menu = tabMenu(tabs, 0);
        for (auto* action : menu->actions())
            if (!action->isSeparator()) QVERIFY2(!action->icon().pixmap(16, 16).isNull(), qPrintable(action->objectName()));
        menu->close();
        quick(window, "icon-host");
        QVERIFY(!tabs->tabIcon(tabs->currentIndex()).pixmap(16, 16).isNull());
        QVERIFY(!get<QListWidget>(window, "notificationsList")->currentItem()->icon().pixmap(16, 16).isNull());
    }

    void extractedImagesDecodeAsImages() {
        const auto assets = qEnvironmentVariable("RM_PREVIEW_ASSET_DIR");
        if (assets.isEmpty()) QSKIP("Optional local reference assets not extracted");
        QVERIFY(!QFile::exists(assets + "/main.ico"));
        for (const auto& name : {"reconnect.png", "external-tools.png"}) {
            QImageReader reader(assets + "/" + QString::fromLatin1(name));
            QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
            const auto image = reader.read();
            QVERIFY2(!image.isNull(), qPrintable(reader.errorString()));
            QVERIFY(image.width() <= 2048 && image.height() <= 2048);
        }
    }
};
QTEST_MAIN(PreviewTest)
#include "preview_test.moc"
