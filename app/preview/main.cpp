#include "PreviewWindow.h"
#include "../Appearance.h"
#include "../Branding.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QPalette>
#include <QPainter>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QListWidget>
#include <QAction>
#include <QTimer>
#include <cstdio>

namespace {
bool openMenu(PreviewWindow& window, const QString& name) {
    QWidget* target = nullptr;
    QPoint position;
    if (name == "tree" || name == "config") {
        auto* tree = window.findChild<QTreeWidget*>(name == "tree" ? "connectionTree" : "configTree");
        auto* item = name == "tree" ? tree->topLevelItem(0)->child(0) : tree->currentItem();
        target = tree->viewport();
        position = tree->visualItemRect(item).center();
    } else if (name == "session" || name == "group") {
        auto* tabs = window.findChild<QTabWidget*>(name == "session" ? "sessionTabs" : "groupTabs");
        target = tabs->tabBar();
        position = tabs->tabBar()->tabRect(0).center();
    } else if (name == "notifications") {
        auto* list = window.findChild<QListWidget*>("notificationsList");
        target = list->viewport();
        position = list->visualItemRect(list->item(0)).center();
    } else if (name == "external-tools") {
        window.findChild<QAction*>("actionViewExternalTools")->setChecked(true);
        target = window.findChild<QToolBar*>("externalToolsToolbar");
        position = QPoint(15, target->height() / 2);
    }
    if (!target) return false;
    QContextMenuEvent event(QContextMenuEvent::Mouse, position, target->mapToGlobal(position));
    QApplication::sendEvent(target, &event);
    return qobject_cast<QMenu*>(QApplication::activePopupWidget()) != nullptr;
}

QPixmap captureWidgets(PreviewWindow& window) {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    if (!menu) return window.grab();
    // Capture only this app's widgets, never unrelated screen contents.
    const QRect menuRect(window.mapFromGlobal(menu->mapToGlobal(QPoint())), menu->size());
    const auto bounds = window.rect().united(menuRect);
    const auto scale = window.devicePixelRatioF();
    QPixmap image((QSizeF(bounds.size()) * scale).toSize());
    image.setDevicePixelRatio(scale);
    image.fill(window.palette().color(QPalette::Window));
    QPainter painter(&image);
    painter.drawPixmap(-bounds.topLeft(), window.grab());
    painter.drawPixmap(menuRect.topLeft() - bounds.topLeft(), menu->grab());
    return image;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("RemoteManager");
    app.setApplicationName("RemoteManager");
    Branding::initialize();
    applyLightAppearance(app);
    QCommandLineParser parser;
    parser.setApplicationDescription("Standalone, non-networked WinForms-to-Qt shell preview");
    parser.addHelpOption();
    parser.addOption({"smoke", "Exit after displaying the window"});
    parser.addOption({"screenshot", "Save a widget capture and exit", "path"});
    parser.addOption({"menu", "Open a preview context menu: tree, session, group, config, notifications, external-tools", "surface"});
    parser.process(app);
    const QStringList surfaces{"tree", "session", "group", "config", "notifications", "external-tools"};
    if (parser.isSet("menu") && !surfaces.contains(parser.value("menu"))) {
        std::fprintf(stderr, "Unknown preview menu surface\n");
        return 1;
    }
    PreviewWindow window;
    window.show();
    if (parser.isSet("menu")) {
        QTimer::singleShot(200, &window, [&] {
            if (!openMenu(window, parser.value("menu"))) {
                std::fprintf(stderr, "Preview context menu did not open\n");
                app.exit(1);
            }
        });
    }
    if (parser.isSet("smoke") || parser.isSet("screenshot")) {
        QTimer::singleShot(500, &app, [&] {
            int result = 0;
            if (parser.isSet("screenshot")) {
                const auto path = parser.value("screenshot");
                const QFileInfo target(path);
                const auto outputDirectory = QDir(QStringLiteral(RM_PREVIEW_OUTPUT_DIR)).canonicalPath();
                if (!target.isAbsolute() || target.dir().canonicalPath() != outputDirectory ||
                    target.suffix() != "png" || target.isSymLink() || !captureWidgets(window).save(path)) {
                    std::fprintf(stderr, "Preview capture must be a PNG inside this preview build directory\n");
                    result = 1;
                }
            }
            app.exit(result);
        });
    }
    return app.exec();
}
