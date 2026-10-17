#pragma once
#include "DocumentIdentity.h"
#include <QHash>
#include <QJsonObject>
#include <variant>

using ConnectionDestination = std::variant<DocumentRootReference, ProfileReference>;
Q_DECLARE_METATYPE(ConnectionDestination)
struct DocumentTreeState {
    QHash<QString, QJsonObject> profiles;
    quint64 generation = 0;
    bool movesEnabled = false;
    QStringList order; // Saved file order; empty disables same-folder reordering.
};
#include <QPersistentModelIndex>
#include <QTreeWidget>

class ConnectionTree final : public QTreeWidget {
    Q_OBJECT
public:
    explicit ConnectionTree(QWidget* parent = nullptr);
    enum ItemType { DocumentItem = QTreeWidgetItem::UserType + 1, ProfileItem };
    void setCommittedDocuments(const QHash<DocumentId, DocumentTreeState>& documents);
    // `before` names the sibling the source is placed ahead of; empty appends.
    bool validMove(const ProfileReference& source, const ConnectionDestination& target, const QString& before = {}) const;
    void setMovesEnabled(bool enabled);
signals:
    void dragStarting();
    void scopedMoveRequested(ProfileReference source, ConnectionDestination destination, QString before = {});
protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDropActions() const override;
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
private:
    enum class DropZone { Before, Into, After };
    bool destination(const QMimeData* mime, QPoint point, ProfileReference& source, ConnectionDestination& target, QString& before, DropZone& zone) const;
    QString nextSibling(const DocumentTreeState& document, const QString& parent, const QString& id, const QString& skip) const;
    bool owns(const QMimeData* mime) const;
    void hover(QTreeWidgetItem* item, DropZone zone = DropZone::Into);
    QPersistentModelIndex hovered_;
    DropZone hoveredZone_ = DropZone::Into;
    bool movesEnabled_ = false;
    quint64 gestureGeneration_ = 0;
    QHash<DocumentId, DocumentTreeState> documents_;
};
