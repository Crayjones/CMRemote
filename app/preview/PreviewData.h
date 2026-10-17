#pragma once
#include <QIcon>
#include <QTreeWidgetItem>

namespace preview {
constexpr int KindRole = Qt::UserRole;
constexpr int FavoriteRole = Qt::UserRole + 1;
constexpr int EndpointRole = Qt::UserRole + 2;
constexpr int ProtocolRole = Qt::UserRole + 3;

inline QIcon icon(const QString& name) {
    return QIcon(":/original/" + name);
}

inline QIcon nodeIcon(const QString& kind) {
    if (kind == "root") return icon("ASPWebSite_16x.png");
    if (kind == "folder") return icon("FolderClosed_16x.png");
    if (kind == "putty") return icon("PuttySessions.png");
    return icon("Connection.ico");
}
}
