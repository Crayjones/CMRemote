#include "PreviewWindow.h"
#include "PreviewData.h"
#include "Branding.h"
#include "ProfileEditor.h"
#include "PropertyDelegate.h"
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
#include <QDockWidget>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#ifdef Q_OS_MACOS
QStringList nativeMenuTitles(const QString& parent = {});
bool triggerNativeMenuItem(const QString& parent, const QString& title);
#endif

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
    QMenu* tabMenu(QTabWidget* tabs, int index) {
        return context(tabs->tabBar(), tabs->tabBar()->tabRect(index).center());
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
        auto* groups = get<QTabWidget>(window, "groupTabs");
        const int general = groups->indexOf(get<QWidget>(window, "generalPanel")); QVERIFY(general >= 0);
        get<QAction>(window, "actionAddPanel")->trigger();
        QCOMPARE(groups->count(), 2);
        const QList<QIcon> icons{Branding::applicationIcon(), QGuiApplication::windowIcon(), window.windowIcon(),
                                 groups->tabIcon(general), groups->tabIcon(groups->currentIndex())};
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
        QCOMPARE(about->text().section('\n', 0, 0), QString("CMRemote shell preview."));
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
        auto* config = get<QTreeWidget>(window, "configTree"); QTreeWidgetItem* password = nullptr;
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
        auto* config = get<QTreeWidget>(window, "configTree");
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
        QCOMPARE(order, QStringList({"Add Connection Panel", "Connection Panels", "|", "Connections", "Config", "Notifications", "Screenshots", "|", "Jump To", "Reset layout", "Lock toolbar positions", "|", "Quick Connect Toolbar", "External Tools Toolbar", "Multi SSH Toolbar", "|", "Full Screen"}));
        QCOMPARE(get<QAction>(window, "actionViewConfig")->menuRole(), QAction::NoRole);
        QCOMPARE(get<QAction>(window, "actionOptions")->menuRole(), QAction::PreferencesRole);
        QVERIFY(!get<QAction>(window, "actionViewScreenshots")->isEnabled()); QVERIFY(!get<QAction>(window, "actionExternalTools")->isEnabled());
        auto* tree = get<QTreeWidget>(window, "connectionTree"); QVERIFY(!tree->dragEnabled()); QCOMPARE(tree->topLevelItem(0)->text(0), QString("Connections"));
        int roots = 0; for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->text(0) == "Connections") ++roots; QCOMPARE(roots, 1);
        auto* grid = get<QTreeWidget>(window, "configTree"); QCOMPARE(grid->columnCount(), 2); QVERIFY(grid->findChildren<QLineEdit*>().isEmpty());
        QCOMPARE(get<QToolBar>(window, "connectionsToolbar")->actions().size(), 4);
        for (auto* child : window.findChildren<QObject*>()) QVERIFY(QByteArray(child->metaObject()->className()) != "VaultService");
        auto* groups = get<QTabWidget>(window, "groupTabs"); auto* tabs = get<QTabWidget>(window, "sessionTabs"); auto* page = tabs->widget(0);
        auto* jump = get<QMenu>(window, "menuJumpTo"); jump->popup(window.mapToGlobal(window.rect().center())); QTRY_VERIFY(jump->actions().size() >= 9); jump->hide();
        QAction* target = nullptr; for (auto* action : jump->actions()) if (action->data().value<QWidget*>() == page) target = action; QVERIFY(target);
        get<QAction>(window, "actionAddPanel")->trigger(); QCOMPARE(groups->count(), 2); target->trigger(); QCOMPARE(groups->currentWidget(), groups->widget(0)); QCOMPARE(tabs->currentWidget(), page); QCOMPARE(tabs->count(), 1);
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
        auto* connections = get<QDockWidget>(window, "connectionsDock");
        auto* config = get<QDockWidget>(window, "configDock");
        auto* notifications = get<QDockWidget>(window, "notificationsDock");
        auto* quick = get<QToolBar>(window, "quickConnectToolbar");
        QCOMPARE(window.dockWidgetArea(connections), Qt::LeftDockWidgetArea);
        QCOMPARE(window.dockWidgetArea(config), Qt::LeftDockWidgetArea);
        QCOMPARE(window.dockWidgetArea(notifications), Qt::BottomDockWidgetArea);
        QCOMPARE(connections->x(), config->x());
        QCOMPARE(connections->width(), config->width());
        QVERIFY(connections->geometry().bottom() < config->geometry().top());
        QVERIFY(notifications->isHidden()); QVERIFY(!get<QAction>(window, "actionViewNotifications")->isChecked());
        QVERIFY(quick->width() < window.width() / 2);
        get<QAction>(window, "actionViewNotifications")->trigger(); QTest::qWait(80);
        QVERIFY(config->geometry().bottom() < notifications->geometry().top());
        QVERIFY(quick->geometry().bottom() < connections->geometry().top());
        QVERIFY(notifications->x() <= connections->x());
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
        auto* properties = get<QTreeWidget>(window, "configTree");
        auto* title = get<QLabel>(window, "configHelpTitle");
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Display"));
        QCOMPARE(title->text(), QString("Name"));
        QCOMPARE(properties->topLevelItem(0)->child(0)->text(1), QString("Connections"));
        tree->setCurrentItem(server);
        QCOMPARE(properties->topLevelItemCount(), 2);
        auto* port = properties->topLevelItem(1)->child(2);
        QCOMPARE(port->text(0), QString("Port"));
        properties->setCurrentItem(port);
        QCOMPARE(title->text(), QString("Port"));
        QVERIFY(get<QLabel>(window, "configHelpText")->text().contains("Default RDP port"));
        get<QAction>(window, "actionInheritance")->trigger();
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Inheritance"));
        get<QAction>(window, "actionAlphabetical")->trigger();
        QCOMPARE(properties->topLevelItemCount(), 3);
        QCOMPARE(properties->topLevelItem(0)->text(0), QString("Hostname / IP"));
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

    void nestedTabsQuickConnectAndClose() {
        PreviewWindow window;
        show(window);
        auto* groups = get<QTabWidget>(window, "groupTabs");
        auto* tabs = get<QTabWidget>(window, "sessionTabs");
        QCOMPARE(groups->count(), 1);
        QCOMPARE(groups->tabText(0), QString("General"));
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QString("server01"));
        QVERIFY(groups->widget(0)->isAncestorOf(tabs));
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
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Q_ARG(int, 0)));
        QCOMPARE(tabs->count(), 2);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        combo->setEditText("server01");
        button->menu()->actions()[0]->trigger();
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(tabs->count(), 3);
        get<QAction>(window, "actionAddPanel")->trigger();
        QCOMPARE(groups->count(), 2);
        auto* panelTabs = groups->currentWidget()->findChild<QTabWidget*>();
        QVERIFY(panelTabs);
        QCOMPARE(panelTabs->count(), 0);
        combo->setEditText("panel-host");
        QTest::keyClick(combo->lineEdit(), Qt::Key_Return);
        QCOMPARE(panelTabs->count(), 1);
        QCOMPARE(tabs->count(), 3);
        get<QAction>(window, "actionGeneralPanel")->trigger();
        QCOMPARE(groups->currentIndex(), 0);
    }

    void doubleClickAndMemoryOnlyTreeActions() {
        PreviewWindow window;
        show(window);
        auto* tree = get<QTreeWidget>(window, "connectionTree");
        auto* server = tree->topLevelItem(0)->child(0);
        auto* tabs = get<QTabWidget>(window, "sessionTabs");
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Q_ARG(int, 0)));
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
        auto* config = get<QDockWidget>(window, "configDock");
        auto* view = get<QAction>(window, "actionViewConfig");
        view->trigger();
        QVERIFY(!config->isVisible());
        config->show();
        QVERIFY(view->isChecked());
        config->close();
        QVERIFY(!view->isChecked());
        get<QAction>(window, "actionViewMultiSsh")->trigger();
        QVERIFY(get<QToolBar>(window, "multiSshToolbar")->isVisible());
        get<QAction>(window, "actionLockToolbars")->trigger();
        QVERIFY(!get<QToolBar>(window, "quickConnectToolbar")->isMovable());
        config->setFloating(true);
        config->show();
        get<QAction>(window, "actionResetLayout")->trigger();
        QTest::qWait(80);
        QVERIFY(!config->isFloating());
        QVERIFY(config->isVisible());
        QVERIFY(view->isChecked());
        QVERIFY(!get<QToolBar>(window, "multiSshToolbar")->isVisible());
        QVERIFY(!get<QAction>(window, "actionViewMultiSsh")->isChecked());
        QCOMPARE(window.dockWidgetArea(config), Qt::LeftDockWidgetArea);
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
        auto* tabs = get<QTabWidget>(window, "sessionTabs");
        auto* initial = tabs->widget(0);
        QCOMPARE(tabs->currentIndex(), 1);
        menu = tabMenu(tabs, 0);
        QVERIFY(menu);
        QCOMPARE(tabs->currentIndex(), 1);
        trigger(menu, "actionTabRename");
        rename(window, "renamed inactive");
        QCOMPARE(tabs->tabText(tabs->indexOf(initial)), QString("renamed inactive"));
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("inactive-host"));
        QVERIFY(!context(tabs->tabBar(), QPoint(tabs->tabBar()->width() - 2, tabs->tabBar()->height() + 5)));
        menu = context(tabs->tabBar(), QPoint(), true);
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
        auto* config = get<QTreeWidget>(window, "configTree");
        QCOMPARE(config->topLevelItem(0)->child(0)->text(1), QString("Friendly display"));
        QCOMPARE(config->topLevelItem(1)->child(1)->text(1), QString("server01"));
        menu = nodeMenu(tree, server);
        if (QGuiApplication::platformName() == "offscreen") {
            trigger(menu, "actionCopyHostname");
            QCOMPARE(QApplication::clipboard()->text(), QString("server01"));
            menu = nodeMenu(tree, server);
        }
        trigger(menu, "actionTreeConnect");
        QCOMPARE(get<QTabWidget>(window, "sessionTabs")->count(), 1);
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
        auto* tabs = get<QTabWidget>(window, "sessionTabs");
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
        tabs->tabBar()->moveTab(1, 0);
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
        auto* stale = tabs->widget(0);
        tabs->removeTab(0);
        delete stale;
        trigger(menu, "actionTabDuplicate");
        QCOMPARE(tabs->count(), 0);
        get<QAction>(window, "actionAddPanel")->trigger();
        auto* panelTabs = get<QTabWidget>(window, "groupTabs")->currentWidget()->findChild<QTabWidget*>();
        quick(window, "new-panel-host");
        menu = tabMenu(panelTabs, 0);
        QVERIFY(menu);
        trigger(menu, "actionTabDuplicate");
        QCOMPARE(panelTabs->count(), 2);
    }

    void groupContextMenuLifetimeAndGeneralProtection() {
        PreviewWindow window;
        show(window);
        auto* groups = get<QTabWidget>(window, "groupTabs");
        auto* panels = get<QMenu>(window, "menuConnectionPanels");
        auto* menu = tabMenu(groups, 0);
        QVERIFY(!command(menu, "actionPanelClose")->isVisible());
        QVERIFY(!command(menu, "actionPanelClose")->isEnabled());
        trigger(menu, "actionPanelRename");
        rename(window, "Renamed General");
        QCOMPARE(panels->actions().first()->text(), QString("Renamed General"));
        menu = tabMenu(groups, 0);
        QVERIFY(!command(menu, "actionPanelClose")->isEnabled());
        menu->close();
        get<QAction>(window, "actionAddPanel")->trigger();
        auto* panel = groups->currentWidget();
        QPointer<QAction> viewAction(panels->actions().last());
        quick(window, "panel-host");
        menu = tabMenu(groups, 1);
        QVERIFY(!command(menu, "actionPanelSendTo")->isEnabled());
        trigger(menu, "actionPanelRename");
        rename(window, "Named panel");
        QCOMPARE(viewAction->text(), QString("Named panel"));
        menu = tabMenu(groups, 1);
        trigger(menu, "actionPanelClose");
        confirmDelete(window, false);
        QCOMPARE(groups->count(), 2);
        QCOMPARE(panel->findChild<QTabWidget*>()->count(), 1);
        menu = tabMenu(groups, 1);
        // Move it while the popup is open; closure still targets this page.
        groups->tabBar()->moveTab(1, 0);
        QPointer<QWidget> deleted(panel);
        trigger(menu, "actionPanelClose");
        confirmDelete(window, true);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!deleted);
        QVERIFY(!viewAction);
        QCOMPARE(groups->count(), 1);
        QCOMPARE(panels->actions().size(), 1);
        QCOMPARE(groups->tabText(0), QString("Renamed General"));
        quick(window, "surviving-group");
        QCOMPARE(get<QTabWidget>(window, "sessionTabs")->count(), 2);
        get<QAction>(window, "actionAddPanel")->trigger();
        menu = tabMenu(groups, 1);
        trigger(menu, "actionPanelClose");
        QCOMPARE(groups->count(), 1);
        QVERIFY(!window.findChild<QMessageBox*>("previewDelete"));
    }

    void propertyAndNotificationContextMenus() {
        PreviewWindow window;
        show(window);
        auto* config = get<QTreeWidget>(window, "configTree");
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
        quick(window, "first-message");
        quick(window, "second-message");
        auto* list = get<QListWidget>(window, "notificationsList");
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
        auto* tabs = get<QTabWidget>(window, "sessionTabs");
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
