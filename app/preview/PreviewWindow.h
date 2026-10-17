#pragma once
#include <QMainWindow>
#include <functional>
#include <memory>

namespace Ui { class MainWindow; }
class QAction;
class QMenu;
class QTreeWidgetItem;
class QTabWidget;
class ProfileEditor;

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
    void createPreview(QTabWidget* tabs, const QString& endpoint, const QString& protocol,
                       const QString& title);
    void configureTabs(QTabWidget* tabs);
    void closeSession(QTabWidget* tabs, QWidget* page);
    void addPanel();
    void closePanel(QWidget* panel);
    void renamePage(QTabWidget* tabs, QWidget* page, bool panel);
    void renameNode(QTreeWidgetItem* node);
    void updateNotificationDetails();
    QTabWidget* currentSessions() const;
    QMenu* treeMenu(QTreeWidgetItem* node);
    QMenu* sessionMenu(QTabWidget* tabs, QWidget* page);
    QMenu* groupMenu(QWidget* panel);
    QMenu* configMenu();
    QMenu* notificationMenu();
    QMenu* externalToolbarMenu();
    QAction* menuAction(QMenu* menu, const char* name, bool enabled,
                        const std::function<void()>& command = {});
    void popup(QMenu* menu, const QPoint& position);
    std::unique_ptr<Ui::MainWindow> ui;
    ProfileEditor* documentEditor = nullptr;
    QString protocol = QStringLiteral("RDP");
    int propertyMode = 0;
    int nextNode = 1;
    int nextPanel = 1;
};
