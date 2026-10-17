#pragma once
#include <QComboBox>
#include <QRegularExpression>
#include <QToolBar>
#include <QWidget>

inline void sizeQuickConnect(QWidget* controls, QComboBox* address, QToolBar* toolbar) {
    controls->setMinimumWidth(0);
    controls->setMaximumWidth(480);
    controls->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    address->setMinimumWidth(180);
    address->setMaximumWidth(320);
    address->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    toolbar->setMaximumWidth(500);
    toolbar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
}

inline int protocolDefaultPort(const QString& protocol) { return protocol == "ssh" ? 22 : protocol == "vnc" ? 5900 : 3389; }
// "host:3389" typed for RDP becomes "host:22" when SSH is chosen. Other ports stay.
inline QString followDefaultPort(const QString& endpoint, const QString& from, const QString& to) {
    static const QRegularExpression pattern(R"(^(\[[^\]]+\]|[^:\[\]]+):(\d+)$)");
    const auto match = pattern.match(endpoint.trimmed());
    if (!match.hasMatch() || match.captured(2).toInt() != protocolDefaultPort(from)) return endpoint;
    return match.captured(1) + ":" + QString::number(protocolDefaultPort(to));
}
