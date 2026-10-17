#pragma once
#include <QIcon>
#include <QJsonObject>
#include <QStringList>
#include <QVariant>

namespace ConnectionIcons {
const QStringList& names();
QIcon resolve(const QString& name, bool folder = false);
QIcon profile(const QJsonObject& profile);
QVariantList choices(const QString& current = {});
}
