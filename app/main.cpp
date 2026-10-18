#include "MainWindow.h"
#include "Appearance.h"
#include "Branding.h"
#include "DataPaths.h"
#include "UpdateCheck.h"
#ifdef RM_WITH_RDP
#include "RdpSecurity.h"
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <iostream>

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    applyLightAppearance(app);
    QCoreApplication::setOrganizationName("RemoteManager");
    QCoreApplication::setApplicationName("RemoteManager");
    QCoreApplication::setApplicationVersion(Updates::currentVersion());
    Branding::initialize();
    QCommandLineParser parser; parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"dev-root","Keep configuration in PROJECT/.dev/<platform>/data.","PROJECT"});
    parser.addOption({"smoke","Show the window and exit after 500 ms (no network connections)."});
    parser.addOption({"demo","Seed an empty development profile file for manual UI checks."});
    parser.process(app);
#ifdef RM_WITH_RDP
    // Packaged-artifact check: RDP sign-in crypto must not depend on the build machine.
    if (parser.isSet("smoke")) {
        if (const QString failure = rdpLegacyCryptoFailure(); !failure.isEmpty()) {
            std::cerr << "RDP crypto unavailable: " << failure.toStdString() << '\n'; return 3;
        }
    }
#endif
    QString directory;
    if (parser.isSet("dev-root") && parser.value("dev-root").isEmpty()) { std::cerr << "--dev-root must not be empty\n"; return 2; }
    try { directory = applicationDataDirectory(parser.value("dev-root")); }
    catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        std::cerr << "Cannot create application data directory\n"; return 2;
    }
    if (!parser.isSet("dev-root") && !migrateLegacyData(directory)) {
        const QString message = "Cannot copy existing data from " + legacyDataDirectory() + " to " + directory + ". Nothing was removed.";
        std::cerr << message.toStdString() << '\n';
        if (!parser.isSet("smoke")) QMessageBox::critical(nullptr, "Cannot move CMRemote data", message);
        return 2;
    }
    try {
        const QString path = applicationConnectionPath(directory);
        MainWindow window(path, false, nullptr, {}, applicationSshHostKeyPath(directory), applicationTerminalAppearancePath(directory), QDir(directory).filePath("preferences.json"));
        if (parser.isSet("demo") && parser.isSet("dev-root"))
            QObject::connect(&window, &MainWindow::documentReady, &window, &MainWindow::seedDemo);
        window.show();
        // Packaged runs only; development and smoke runs never update themselves.
        if (!parser.isSet("dev-root") && !parser.isSet("smoke")) window.enableAutomaticUpdates(directory);
        if (parser.isSet("smoke")) {
            // Close through MainWindow's shutdown path. QApplication::quit() goes via Cocoa's
            // terminate on macOS, which never completes for an app launched in the background.
            QObject::connect(&app, &QGuiApplication::lastWindowClosed, &app, [] { QCoreApplication::exit(0); });
            QTimer::singleShot(500, &window, [&window] { window.close(); });
        }
        std::cout << rm::core_version().c_str() << '\n';
        return app.exec();
    } catch (const rust::Error& error) {
        std::cerr << error.what() << '\n';
        if (!parser.isSet("smoke")) QMessageBox::critical(nullptr,"Cannot open profiles",QString::fromUtf8(error.what()) + "\nOriginal data was not changed.");
        return 2;
    }
}
