#include "ApplicationPreferences.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace {
PreferenceReply access(const QString& path, bool write, PreferenceValues values) {
    PreferenceReply reply{values, {true, {}, {}}};
    if (path.isEmpty() && !write) return reply;
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()) { reply.result = {false, "An explicit application-data preferences path is required.", {}}; return reply; }
    AtomicFile file(path); QByteArray bytes; bool exists = false;
    if (!file.load(bytes, exists, 65536)) { reply.result = {false, "Cannot read application preferences: unsafe path or oversized file.", {}}; return reply; }
    if (exists) {
        const auto document = QJsonDocument::fromJson(bytes); const auto object = document.object();
        if (!document.isObject() || object.value("version") != 1 ||
            !object.value("categorized").isBool() || !object.value("property_help").isBool() ||
            !object.value("quick_connect").isBool() || !object.value("lock_toolbars").isBool() ||
            (object.contains("openh264") && !object.value("openh264").isBool())) {
            reply.result = {false, "Application preferences are invalid. The file was not changed.", {}}; return reply;
        }
        for (auto it = object.begin(); it != object.end(); ++it) if (!QStringList{"version", "categorized", "property_help", "quick_connect", "lock_toolbars", "connection_defaults", "openh264"}.contains(it.key())) {
            reply.result = {false, "Application preferences contain unsupported fields. The file was not changed.", {}}; return reply;
        }
        ConnectionDefaults defaults;
        if (object.contains("connection_defaults")) {
            const auto parsed = object.value("connection_defaults").isObject() ? ConnectionDefaults::parse(object.value("connection_defaults").toObject()) : std::nullopt;
            if (!parsed) { reply.result = {false, "Connection defaults are invalid. The file was not changed.", {}}; return reply; }
            defaults = *parsed;
        }
        if (!write) reply.values = {object.value("categorized").toBool(), object.value("property_help").toBool(), object.value("quick_connect").toBool(), object.value("lock_toolbars").toBool(), defaults, object.value("openh264").toBool(true)};
    }
    if (write) {
        if (!ConnectionDefaults::parse(values.defaults.json())) { reply.result = {false, "Connection defaults are invalid. The file was not changed.", {}}; return reply; }
        const auto encoded = QJsonDocument(QJsonObject{{"version", 1}, {"categorized", values.categorized},
            {"property_help", values.propertyHelp}, {"quick_connect", values.quickConnect}, {"lock_toolbars", values.lockToolbars},
            {"connection_defaults", values.defaults.json()}, {"openh264", values.openh264}}).toJson(QJsonDocument::Compact);
        if (bytes != encoded) reply.result = file.replace(encoded, 65536, "application preferences");
    }
    return reply;
}
}
ApplicationPreferences::ApplicationPreferences(QString path, QObject* parent) : QObject(parent), path_(std::move(path)) {}
ApplicationPreferences::~ApplicationPreferences() { if (worker_) { worker_->wait(); delete worker_; } }
void ApplicationPreferences::load(std::function<void(PreferenceReply)> complete) { run(false, {}, std::move(complete)); }
void ApplicationPreferences::save(PreferenceValues values, std::function<void(PreferenceReply)> complete) { run(true, values, std::move(complete)); }
void ApplicationPreferences::run(bool write, PreferenceValues values, std::function<void(PreferenceReply)> complete) {
    if (worker_) { complete({values, {false, "Application preferences are busy.", {}}}); return; }
    worker_ = QThread::create([this, write, values, complete = std::move(complete), path = path_]() mutable {
        auto reply = access(path, write, values);
        QMetaObject::invokeMethod(this, [this, reply = std::move(reply), complete = std::move(complete)]() mutable {
            worker_->wait(); delete worker_; worker_ = nullptr; complete(std::move(reply));
        }, Qt::QueuedConnection);
    });
    worker_->start();
}
