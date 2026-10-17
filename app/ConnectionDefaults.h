#pragma once
#include <QJsonObject>
#include <optional>

struct ConnectionDefaults {
    QJsonObject values, inheritance;
    QJsonObject effective() const;
    QJsonObject overrides() const;
    bool inherits(const QString& key) const { return inheritance.value(key).toBool(key != "password"); }
    QJsonObject json() const { return {{"values", values}, {"inheritance", inheritance}}; }
    static std::optional<ConnectionDefaults> parse(const QJsonObject& object);
    bool operator==(const ConnectionDefaults& other) const { return values == other.values && inheritance == other.inheritance; }
};
