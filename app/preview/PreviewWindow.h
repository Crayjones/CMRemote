#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <functional>
#include <memory>

struct ShellUi;
class QAction;
class QMenu;
class QTreeWidgetItem;
class ProfileEditor;
namespace ads { class CDockWidget; class CDockAreaWidget; }

class PreviewWindow : public QMainWindow {
public:
    explicit PreviewWindow(QWidget* parent = nullptr);
    ~PreviewWindow() override;
    void resetLayout();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void showProperties();
    void rebuildJumpMenu();
    void filterConnections();
    void addNode(const QString& kind);
    void openPreview(const QString& endpoint, const QString& protocol, const QString& title = {});
    // Preview sessions are session docks like the application's: a panel is a
    // Qt ADS dock area of them, and General holds the empty-window placeholder.
    void createPreview(ads::CDockAreaWidget* panel, const QString& endpoint, const QString& protocol,
                       const QString& title, bool announce = true);
    void closeSession(ads::CDockWidget* dock);
    void addPanel();
    void renameSession(ads::CDockWidget* dock);
    void updateSessionLayout();
    QList<ads::CDockWidget*> sessionDocks() const;
    QList<ads::CDockWidget*> panelDocks(ads::CDockAreaWidget* panel) const;
    QList<ads::CDockAreaWidget*> sessionPanels() const;
    QString panelName(ads::CDockAreaWidget* panel) const;
    void rebuildPanelsMenu();
    void renameNode(QTreeWidgetItem* node);
    void updateNotificationDetails();
    ads::CDockAreaWidget* currentPanel() const;
    QMenu* treeMenu(QTreeWidgetItem* node);
    QMenu* sessionMenu(ads::CDockWidget* dock);
    QMenu* configMenu();
    QMenu* notificationMenu();
    QMenu* externalToolbarMenu();
    QAction* menuAction(QMenu* menu, const char* name, bool enabled,
                        const std::function<void()>& command = {});
    void popup(QMenu* menu, const QPoint& position);
    std::unique_ptr<ShellUi> ui;
    QJsonObject previewEffective;
    ProfileEditor* documentEditor = nullptr;
    QString protocol = QStringLiteral("RDP");
    int propertyMode = 0;
    int nextNode = 1;
    mutable int nextPanel = 1;
    int nextSession = 1;
    QPointer<ads::CDockWidget> lastSession;
};
