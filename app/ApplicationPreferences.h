#pragma once
#include "AtomicFile.h"
#include "ConnectionDefaults.h"
#include <QObject>
#include <functional>
class QThread;

struct PreferenceValues {
    bool categorized = true, propertyHelp = true, quickConnect = true, lockToolbars = false;
    ConnectionDefaults defaults;
    bool openh264 = true; // use Cisco's separately downloaded H.264 decoder
    bool showPanels = false;       // View > Panel Tabs: the General/Panel N row above session tabs
    bool commandAsControl = false; // macOS remote desktops: Command sends Control (physical Control sends Windows/Super)
    bool autoUpdate = true;        // check GitHub for releases and install them
    QString theme = "light";       // "system", "light" or "dark"
    bool operator==(const PreferenceValues& o) const {
        return categorized == o.categorized && propertyHelp == o.propertyHelp && quickConnect == o.quickConnect && lockToolbars == o.lockToolbars &&
            defaults == o.defaults && openh264 == o.openh264 && showPanels == o.showPanels && commandAsControl == o.commandAsControl &&
            autoUpdate == o.autoUpdate && theme == o.theme;
    }
};
struct PreferenceReply {
    PreferenceValues values;
    FileResult result;
};

// Nonsecret application settings. File work stays off the GUI thread; callers
// supply the application-data path, never an opened connection file's directory.
class ApplicationPreferences final : public QObject {
    Q_OBJECT
public:
    explicit ApplicationPreferences(QString path, QObject* parent = nullptr);
    ~ApplicationPreferences() override;
    void load(std::function<void(PreferenceReply)> complete);
    void save(PreferenceValues values, std::function<void(PreferenceReply)> complete);
private:
    void run(bool write, PreferenceValues values, std::function<void(PreferenceReply)> complete);
    QString path_;
    QThread* worker_ = nullptr;
};
