#include "PreviewWindow.h"
#include "PreviewData.h"
#include "ui_test.h"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTabBar>
#include <QTreeWidgetItemIterator>

using namespace preview;

namespace {
bool editable(QTreeWidgetItem* node) {
    if (!node) return false;
    const auto kind = node->data(0, KindRole).toString();
    return kind == "connection" || kind == "folder";
}

QList<QTreeWidgetItem*> topSelection(QTreeWidget* tree) {
    QList<QTreeWidgetItem*> result;
    const auto selected = tree->selectedItems();
    for (auto* node : selected) {
        bool ancestorSelected = false;
        for (auto* parent = node->parent(); parent; parent = parent->parent())
            ancestorSelected = ancestorSelected || selected.contains(parent);
        if (!ancestorSelected) result.append(node);
    }
    return result;
}

QList<QPersistentModelIndex> indexes(QTreeWidget* tree, const QList<QTreeWidgetItem*>& nodes) {
    QList<QPersistentModelIndex> result;
    for (auto* node : nodes) result.append(tree->indexFromItem(node));
    return result;
}

void askName(QWidget* owner, const QString& name, const std::function<void(const QString&)>& apply) {
    auto* dialog = new QInputDialog(owner);
    dialog->setObjectName("previewRename");
    dialog->setWindowTitle("Rename preview item");
    dialog->setLabelText("New name:");
    dialog->setTextValue(name);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QInputDialog::textValueSelected, owner, [apply](const QString& value) {
        const auto name = value.trimmed();
        if (!name.isEmpty()) apply(name);
    });
    dialog->open();
}

void confirm(QWidget* owner, const QString& text, const std::function<void()>& apply) {
    auto* dialog = new QMessageBox(QMessageBox::Question, "Delete local preview items?", text,
                                   QMessageBox::Yes | QMessageBox::No, owner);
    dialog->setObjectName("previewDelete");
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::No);
    dialog->setEscapeButton(QMessageBox::No);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::finished, owner, [apply](int result) {
        if (result == QMessageBox::Yes) apply();
    });
    dialog->open();
}
}

void PreviewWindow::rebuildJumpMenu() {
    auto* menu = ui->menuJumpTo; menu->clear();
    menu->addAction(ui->actionNextSession); menu->addAction(ui->actionPreviousSession); menu->addSeparator();
    for (auto* dock : {ui->connectionsDock, ui->configDock, ui->notificationsDock}) {
        auto* action = menu->addAction(dock->windowIcon(), dock->windowTitle()); action->setData(dock->objectName());
        connect(action, &QAction::triggered, this, [dock = QPointer<QDockWidget>(dock)] {
            if (dock) { dock->show(); dock->raise(); dock->widget()->setFocus(); }
        });
    }
    menu->addSeparator();
    for (int i = 0; i < ui->groupTabs->count(); ++i) {
        auto* panel = ui->groupTabs->widget(i); auto* tabs = panel->findChild<QTabWidget*>();
        auto* action = menu->addAction(ui->groupTabs->tabIcon(i), ui->groupTabs->tabText(i)); action->setData(QVariant::fromValue(panel));
        connect(action, &QAction::triggered, this, [this, panel = QPointer<QWidget>(panel)] {
            if (panel && ui->groupTabs->indexOf(panel) >= 0) { ui->groupTabs->setCurrentWidget(panel); if (auto* tabs = panel->findChild<QTabWidget*>()) tabs->setFocus(); }
        });
        if (!tabs) continue;
        for (int j = 0; j < tabs->count(); ++j) {
            auto* page = tabs->widget(j); auto* session = menu->addAction(tabs->tabIcon(j), ui->groupTabs->tabText(i) + ": " + tabs->tabText(j)); session->setData(QVariant::fromValue(page));
            connect(session, &QAction::triggered, this, [this, page = QPointer<QWidget>(page)] {
                if (!page) return;
                for (int i = 0; i < ui->groupTabs->count(); ++i) {
                    auto* tabs = ui->groupTabs->widget(i)->findChild<QTabWidget*>();
                    if (tabs && tabs->indexOf(page) >= 0) { ui->groupTabs->setCurrentIndex(i); tabs->setCurrentWidget(page); page->setFocus(); return; }
                }
            });
        }
    }
}
QAction* PreviewWindow::menuAction(QMenu* menu, const char* name, bool enabled,
                                  const std::function<void()>& command) {
    auto* source = findChild<QAction*>(QString::fromLatin1(name));
    Q_ASSERT(source);
    auto* action = menu->addAction(source->icon(), source->text());
    // Each popup has its own actions: state and command targets cannot leak between popups.
    action->setObjectName(QString::fromLatin1(name));
    action->setEnabled(enabled);
    action->setToolTip(enabled ? source->toolTip() : "Unavailable for this target or in the local-only preview");
    if (command) connect(action, &QAction::triggered, this, command);
    return action;
}

void PreviewWindow::popup(QMenu* menu, const QPoint& position) {
    menu->setToolTipsVisible(true);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(position);
}

bool PreviewWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::ContextMenu) return QMainWindow::eventFilter(watched, event);
    const auto* context = static_cast<QContextMenuEvent*>(event);
    const bool keyboard = context->reason() == QContextMenuEvent::Keyboard;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget) return false;
    auto location = context->globalPos();
    if (keyboard) location = widget->mapToGlobal(widget->rect().center());
    if (watched == ui->connectionTree || watched == ui->connectionTree->viewport()) {
        auto* node = keyboard ? ui->connectionTree->currentItem() :
            ui->connectionTree->itemAt(ui->connectionTree->viewport()->mapFromGlobal(location));
        if (!node) return true;
        if (!node->isSelected()) ui->connectionTree->clearSelection();
        ui->connectionTree->setCurrentItem(node, 0, QItemSelectionModel::NoUpdate);
        node->setSelected(true);
        popup(treeMenu(node), location);
        return true;
    }
    if (auto* bar = qobject_cast<QTabBar*>(watched)) {
        auto* tabs = qobject_cast<QTabWidget*>(bar->parentWidget());
        if (!tabs) return false;
        const int index = keyboard ? tabs->currentIndex() : bar->tabAt(context->pos());
        if (index < 0) return true;
        auto* page = tabs->widget(index);
        popup(tabs == ui->groupTabs ? groupMenu(page) : sessionMenu(tabs, page), location);
        return true;
    }
    if (watched == ui->configTree || watched == ui->configTree->viewport()) {
        if (!keyboard) {
            auto* row = ui->configTree->itemAt(ui->configTree->viewport()->mapFromGlobal(location));
            if (row) ui->configTree->setCurrentItem(row);
        }
        popup(configMenu(), location);
        return true;
    }
    if (watched == ui->notificationsList || watched == ui->notificationsList->viewport()) {
        auto* list = ui->notificationsList;
        if (!keyboard) {
            auto* row = list->itemAt(list->viewport()->mapFromGlobal(location));
            if (!row || !row->isSelected()) list->clearSelection();
            if (row) {
                list->setCurrentItem(row, QItemSelectionModel::NoUpdate);
                row->setSelected(true);
            }
        }
        popup(notificationMenu(), location);
        return true;
    }
    if (watched == ui->externalToolsToolbar) {
        popup(externalToolbarMenu(), location);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

QMenu* PreviewWindow::treeMenu(QTreeWidgetItem* node) {
    auto* menu = new QMenu(this);
    menu->setObjectName("treeContextMenu");
    const QPersistentModelIndex target(ui->connectionTree->indexFromItem(node));
    const auto selected = topSelection(ui->connectionTree);
    const auto targets = indexes(ui->connectionTree, selected);
    const auto kind = node->data(0, KindRole).toString();
    const bool putty = kind == "putty";
    const bool single = ui->connectionTree->selectedItems().size() == 1;
    bool allEditable = !selected.isEmpty();
    for (auto* item : selected) allEditable = allEditable && editable(item);
    auto* connectAction = menuAction(menu, "actionTreeConnect", allEditable, [this, targets] {
        for (const auto& index : targets) {
            auto* item = ui->connectionTree->itemFromIndex(index);
            if (!editable(item)) continue;
            for (QTreeWidgetItemIterator it(item); *it; ++it) {
                auto* child = *it;
                // The iterator walks beyond a subtree; stop before a sibling/ancestor.
                bool within = child == item;
                for (auto* parent = child->parent(); parent; parent = parent->parent()) within = within || parent == item;
                if (!within) break;
                if (child->data(0, KindRole).toString() == "connection")
                    openPreview(child->data(0, EndpointRole).toString(), child->data(0, ProtocolRole).toString(), child->text(0));
            }
        }
    });
    connectAction->setToolTip("Open local preview tabs only; no network connection");
    auto* options = new QMenu(menu);
    menuAction(menu, "actionConnectOptions", false)->setMenu(options);
    for (const auto* name : {"actionConsoleSession", "actionNoConsoleSession", "actionConnectFullscreen",
                             "actionNoCredentials", "actionChoosePanel", "actionConnectViewOnly"}) {
        auto* action = menuAction(options, name, false);
        if (QString::fromLatin1(name) == "actionNoConsoleSession") action->setVisible(false);
    }
    menuAction(menu, "actionTreeDisconnect", false);
    menu->addSeparator();
    menuAction(menu, "actionTreeExternalTools", false);
    menu->addSeparator();
    auto* duplicate = menuAction(menu, "actionTreeDuplicate", allEditable, [this, targets] {
        for (const auto& index : targets) {
            auto* item = ui->connectionTree->itemFromIndex(index);
            if (!editable(item)) continue;
            auto* copy = item->clone();
            copy->setText(0, item->text(0) + " (copy)");
            if (auto* parent = item->parent()) parent->insertChild(parent->indexOfChild(item) + 1, copy);
            else ui->connectionTree->addTopLevelItem(copy);
            ui->connectionTree->setCurrentItem(copy);
        }
        filterConnections();
        showProperties();
    });
    menuAction(menu, "actionTreeRename", single && (editable(node) || kind == "root"), [this, target] {
        if (auto* item = ui->connectionTree->itemFromIndex(target)) renameNode(item);
    });
    auto* remove = menuAction(menu, "actionTreeDelete", allEditable, [this, targets] {
        confirm(this, QString("Delete %1 selected local preview item(s) and their children?\nNo profile file is changed.").arg(targets.size()),
                [this, targets] {
            for (const auto& index : targets) {
                auto* item = ui->connectionTree->itemFromIndex(index);
                if (editable(item)) delete item;
            }
            filterConnections();
            showProperties();
        });
    });
    if (!single) {
        connectAction->setText(QString("Connect selected (%1)").arg(targets.size()));
        duplicate->setText(QString("Duplicate selected (%1)").arg(targets.size()));
        remove->setText(QString("Delete selected (%1)").arg(targets.size()));
    }
    const auto endpoint = node->data(0, EndpointRole).toString();
    menuAction(menu, "actionCopyHostname", single && kind == "connection" && !endpoint.isEmpty(), [endpoint] {
        QApplication::clipboard()->setText(endpoint);
    });
    menuAction(menu, "actionWakeOnLan", false);
    menuAction(menu, "actionClearCredentials", false);
    auto* inheritance = new QMenu(menu);
    menuAction(menu, "actionTreeInheritance", false)->setMenu(inheritance);
    menuAction(inheritance, "actionApplyInheritance", false);
    menuAction(inheritance, "actionApplyDefaultInheritance", false);
    menu->addSeparator();
    auto* imports = new QMenu(menu);
    menuAction(menu, "actionImport", false)->setMenu(imports);
    for (const auto* name : {"actionImportFile", "actionImportRdm", "actionImportAd", "actionImportPutty"})
        menuAction(imports, name, false);
    menuAction(menu, "actionExport", false);
    menu->addSeparator();
    for (const auto* kindName : {"connection", "folder", "root"}) {
        const auto name = QString::fromLatin1(kindName);
        const auto* actionName = name == "connection" ? "actionAddConnection" : name == "folder" ? "actionAddFolder" : "actionAddRoot";
        menuAction(menu, actionName, single && (editable(node) || kind == "root"), [this, target, name] {
            auto* item = ui->connectionTree->itemFromIndex(target);
            if (!item) return;
            ui->connectionTree->setCurrentItem(item);
            addNode(name);
        });
    }
    menu->addSeparator();
    auto* sort = new QMenu(menu);
    menuAction(menu, "actionTreeSort", !putty && single)->setMenu(sort);
    for (auto order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
        menuAction(sort, order == Qt::AscendingOrder ? "actionSortAscending" : "actionSortDescending", !putty && single,
                   [this, target, order] {
            auto* item = ui->connectionTree->itemFromIndex(target);
            if (!item) return;
            if (item->data(0, KindRole).toString() == "connection") item = item->parent();
            if (item) item->sortChildren(0, order);
        });
    }
    auto* parent = node->parent();
    const int position = parent ? parent->indexOfChild(node) : -1;
    for (const int offset : {-1, 1}) {
        menuAction(menu, offset < 0 ? "actionMoveUp" : "actionMoveDown",
                   single && editable(node) && parent && position + offset >= 0 && position + offset < parent->childCount(),
                   [this, target, offset] {
            auto* item = ui->connectionTree->itemFromIndex(target);
            if (!editable(item) || !item->parent()) return;
            auto* parent = item->parent();
            const int position = parent->indexOfChild(item), destination = position + offset;
            if (destination < 0 || destination >= parent->childCount()) return;
            parent->takeChild(position);
            parent->insertChild(destination, item);
            ui->connectionTree->setCurrentItem(item);
            filterConnections();
        });
    }
    return menu;
}

void PreviewWindow::renameNode(QTreeWidgetItem* node) {
    if (node->data(0, KindRole).toString() == "putty") return;
    const QPersistentModelIndex target(ui->connectionTree->indexFromItem(node));
    askName(this, node->text(0), [this, target](const QString& name) {
        auto* item = ui->connectionTree->itemFromIndex(target);
        if (!item) return;
        item->setText(0, name);
        filterConnections();
        showProperties();
    });
}

QMenu* PreviewWindow::sessionMenu(QTabWidget* tabs, QWidget* page) {
    auto* menu = new QMenu(this);
    menu->setObjectName("sessionContextMenu");
    const QPointer<QTabWidget> group(tabs);
    const QPointer<QWidget> target(page);
    const auto selectedProtocol = page->property("previewProtocol").toString();
    const bool rdp = selectedProtocol == "RDP", vnc = selectedProtocol == "VNC";
    const bool ssh = selectedProtocol == "SSH1" || selectedProtocol == "SSH2";
    const bool putty = ssh || selectedProtocol == "Telnet" || selectedProtocol == "Rlogin" || selectedProtocol == "RAW";
    menuAction(menu, "actionTabReconnect", false);
    menuAction(menu, "actionTabDisconnect", true, [this, group, target] {
        if (group && target) closeSession(group, target);
    });
    menuAction(menu, "actionTabMinimize", false);
    for (bool rightOnly : {false, true}) {
        menuAction(menu, rightOnly ? "actionTabDisconnectRight" : "actionTabDisconnectOthers",
                   rightOnly ? tabs->indexOf(page) < tabs->count() - 1 : tabs->count() > 1,
                   [this, group, target, rightOnly] {
            if (!group || !target || group->indexOf(target) < 0) return;
            const int keep = group->indexOf(target);
            for (int i = group->count() - 1; i >= 0; --i)
                if (i != keep && (!rightOnly || i > keep)) closeSession(group, group->widget(i));
        });
    }
    menuAction(menu, "actionTabRename", true, [this, group, target] {
        if (group && target) renamePage(group, target, false);
    });
    menuAction(menu, "actionTabDuplicate", true, [this, group, target] {
        if (!group || !target || group->indexOf(target) < 0) return;
        createPreview(group, target->property("previewTitle").toString(), target->property("previewProtocol").toString(),
                      group->tabText(group->indexOf(target)));
    });
    menu->addSeparator();
    menuAction(menu, "actionTabFullscreen", false)->setVisible(rdp);
    menuAction(menu, "actionTabSmartSize", false)->setVisible(rdp || vnc);
    menuAction(menu, "actionTabViewOnly", false)->setVisible(rdp || vnc);
    menuAction(menu, "actionTabChat", false)->setVisible(vnc);
    menuAction(menu, "actionTabRefresh", false)->setVisible(vnc);
    menuAction(menu, "actionTabTransfer", false)->setVisible(ssh);
    auto* keys = new QMenu(menu);
    auto* special = menuAction(menu, "actionSpecialKeys", false);
    special->setMenu(keys);
    special->setVisible(vnc);
    menuAction(keys, "actionCtrlAltDel", false);
    menuAction(keys, "actionCtrlEsc", false);
    menu->addSeparator();
    menuAction(menu, "actionPuttySettings", false)->setVisible(putty);
    menuAction(menu, "actionTabExternalTools", false);
    return menu;
}

void PreviewWindow::renamePage(QTabWidget* tabs, QWidget* page, bool panel) {
    if (tabs->indexOf(page) < 0) return;
    const QPointer<QTabWidget> group(tabs);
    const QPointer<QWidget> target(page);
    askName(this, tabs->tabText(tabs->indexOf(page)), [group, target, panel](const QString& name) {
        if (!group || !target || group->indexOf(target) < 0) return;
        group->setTabText(group->indexOf(target), name);
        if (panel) {
            if (auto* action = target->property("panelAction").value<QAction*>()) action->setText(name);
        }
    });
}

QMenu* PreviewWindow::groupMenu(QWidget* panel) {
    auto* menu = new QMenu(this);
    menu->setObjectName("groupContextMenu");
    const QPointer<QWidget> target(panel);
    menuAction(menu, "actionPanelRename", true, [this, target] {
        if (target) renamePage(ui->groupTabs, target, true);
    });
    menuAction(menu, "actionPanelSendTo", false)->setToolTip("Detachable multi-monitor panels are not implemented");
    auto* close = menuAction(menu, "actionPanelClose", !panel->property("isGeneralPanel").toBool(), [this, target] {
        if (target) closePanel(target);
    });
    close->setVisible(!panel->property("isGeneralPanel").toBool());
    return menu;
}

void PreviewWindow::closePanel(QWidget* panel) {
    if (panel->property("isGeneralPanel").toBool() || ui->groupTabs->indexOf(panel) < 0) return;
    const QPointer<QWidget> target(panel);
    auto remove = [this, target] {
        if (!target || target->property("isGeneralPanel").toBool()) return;
        const int index = ui->groupTabs->indexOf(target);
        if (index < 0) return;
        if (auto* action = target->property("panelAction").value<QAction*>()) {
            ui->menuConnectionPanels->removeAction(action);
            action->setEnabled(false);
        }
        ui->groupTabs->removeTab(index);
        target->deleteLater();
    };
    auto* tabs = panel->findChild<QTabWidget*>();
    if (tabs && tabs->count()) confirm(this, "Close this panel and its local preview tabs?\nNo network sessions or profiles are affected.", remove);
    else remove();
}

QMenu* PreviewWindow::configMenu() {
    auto* menu = new QMenu(this);
    menu->setObjectName("configContextMenu");
    menu->addAction(ui->actionPropertyReset);
    menu->addSeparator();
    menu->addAction(ui->actionShowHelp);
    return menu;
}

QMenu* PreviewWindow::notificationMenu() {
    auto* menu = new QMenu(this);
    menu->setObjectName("notificationContextMenu");
    auto* list = ui->notificationsList;
    const bool all = list->selectedItems().isEmpty();
    QList<QPersistentModelIndex> targets;
    for (int i = 0; i < list->count(); ++i)
        if (all || list->item(i)->isSelected()) targets.append(list->model()->index(i, 0));
    auto* copy = menuAction(menu, "actionNotificationCopy", !targets.isEmpty(), [this, targets] {
        QStringList messages;
        for (const auto& index : targets)
            if (index.isValid()) messages << ui->notificationsList->item(index.row())->text();
        if (!messages.isEmpty()) QApplication::clipboard()->setText(messages.join("\n----------\n"));
    });
    auto* remove = menuAction(menu, "actionNotificationDelete", !targets.isEmpty(), [this, targets] {
        for (const auto& index : targets)
            if (index.isValid()) delete ui->notificationsList->takeItem(index.row());
        updateNotificationDetails();
    });
    if (all) { copy->setText("Copy All"); remove->setText("Delete All"); }
    return menu;
}

QMenu* PreviewWindow::externalToolbarMenu() {
    auto* menu = new QMenu(this);
    menu->setObjectName("externalToolbarContextMenu");
    menu->addAction(ui->actionShowToolText);
    menu->addSeparator();
    for (auto* action : {ui->actionViewConnections, ui->actionViewConfig, ui->actionViewNotifications,
                         ui->actionViewQuickConnect, ui->actionViewExternalTools, ui->actionViewMultiSsh}) menu->addAction(action);
    return menu;
}
