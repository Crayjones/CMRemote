#include "ConnectionTree.h"
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QSet>

namespace {
const QString Mime = "application/x-remotemanager-profile";
// Bytes never authorize a drop. The issuing tree, runtime document and live
// generation must all match; copied UUIDs are not cross-file identities.
class ProfileMime final : public QMimeData {
public:
    ProfileMime(const ConnectionTree* tree, ProfileReference profile, quint64 generation, quint64 gesture)
        : owner(tree), reference(std::move(profile)), generation(generation), gesture(gesture) {
        setData(Mime, reference.document.text().toUtf8() + '\n' + QByteArray::number(generation) + '\n' + reference.profile.toUtf8());
    }
    const QPointer<const ConnectionTree> owner;
    const ProfileReference reference;
    const quint64 generation, gesture;
};
}
ConnectionTree::ConnectionTree(QWidget* parent) : QTreeWidget(parent) {
    setSelectionMode(SingleSelection); setDragDropMode(DragDrop); setDragEnabled(false);
    setAcceptDrops(false); setDropIndicatorShown(false); setDefaultDropAction(Qt::MoveAction);
}
void ConnectionTree::setCommittedDocuments(const QHash<DocumentId, DocumentTreeState>& documents) {
    documents_ = documents; hover(nullptr);
}
void ConnectionTree::setMovesEnabled(bool enabled) {
    movesEnabled_ = enabled; setDragEnabled(enabled); setAcceptDrops(enabled); if (!enabled) hover(nullptr);
}
bool ConnectionTree::validMove(const ProfileReference& source, const ConnectionDestination& target, const QString& before) const {
    if (!movesEnabled_ || !source.valid()) return false;
    DocumentId destination; QString parent;
    if (const auto* root = std::get_if<DocumentRootReference>(&target)) destination = root->document;
    else { const auto& profile = std::get<ProfileReference>(target); if (!profile.valid()) return false; destination = profile.document; parent = profile.profile; }
    const auto document = documents_.constFind(source.document);
    if (destination != source.document || document == documents_.cend() || !document->movesEnabled) return false;
    const auto& nodes = document->profiles;
    if (!nodes.contains(source.profile)) return false;
    if (!before.isEmpty() && (before == source.profile || !nodes.contains(before) || nodes.value(before).value("parent").toString() != parent)) return false;
    if (nodes.value(source.profile).value("parent").toString() == parent) {
        // Same folder: only an actual position change is a move.
        if (document->order.isEmpty() || !document->order.contains(source.profile)) return false;
        if (before == nextSibling(*document, parent, source.profile, {})) return false;
    }
    if (!parent.isEmpty() && (!nodes.contains(parent) || nodes.value(parent).value("kind") != "folder")) return false;
    QSet<QString> seen;
    for (auto ancestor = parent; !ancestor.isEmpty(); ancestor = nodes.value(ancestor).value("parent").toString()) {
        if (ancestor == source.profile || seen.contains(ancestor) || !nodes.contains(ancestor)) return false;
        seen.insert(ancestor);
    }
    return true;
}
QString ConnectionTree::nextSibling(const DocumentTreeState& document, const QString& parent, const QString& id, const QString& skip) const {
    bool found = false;
    for (const auto& candidate : document.order) {
        if (document.profiles.value(candidate).value("parent").toString() != parent) continue;
        if (found && candidate != skip) return candidate;
        if (candidate == id) found = true;
    }
    return {};
}
bool ConnectionTree::owns(const QMimeData* mime) const {
    const auto* data = dynamic_cast<const ProfileMime*>(mime);
    if (!movesEnabled_ || !data || data->owner != this || data->gesture != gestureGeneration_) return false;
    const auto document = documents_.constFind(data->reference.document);
    return data->reference.valid() && document != documents_.cend() && document->movesEnabled &&
        document->generation == data->generation && document->profiles.contains(data->reference.profile);
}
QMimeData* ConnectionTree::mimeData(const QList<QTreeWidgetItem*>& items) const {
    if (!movesEnabled_ || items.size() != 1 || items.first()->type() != ProfileItem) return nullptr;
    const auto reference = items.first()->data(0, Qt::UserRole).value<ProfileReference>();
    const auto document = documents_.constFind(reference.document);
    return reference.valid() && document != documents_.cend() && document->movesEnabled && document->profiles.contains(reference.profile)
        ? new ProfileMime(this, reference, document->generation, gestureGeneration_) : nullptr;
}
QStringList ConnectionTree::mimeTypes() const { return {Mime}; }
Qt::DropActions ConnectionTree::supportedDropActions() const { return Qt::MoveAction; }
void ConnectionTree::startDrag(Qt::DropActions) {
    if (!movesEnabled_) return; emit dragStarting();
    auto* mime = mimeData(selectedItems()); if (!mime) return;
    QDrag drag(this); drag.setMimeData(mime); drag.exec(Qt::MoveAction, Qt::MoveAction);
    // Never call internal-move: it can remove source rows before persistence.
    hover(nullptr);
}
bool ConnectionTree::destination(const QMimeData* mime, QPoint point, ProfileReference& source, ConnectionDestination& destination,
                                 QString& before, DropZone& zone) const {
    const auto* data = dynamic_cast<const ProfileMime*>(mime); auto* target = itemAt(point);
    if (!owns(mime) || !target) return false;
    source = data->reference; before.clear(); zone = DropZone::Into;
    const auto role = target->data(0, Qt::UserRole);
    if (target->type() == DocumentItem && role.canConvert<DocumentRootReference>()) { destination = role.value<DocumentRootReference>(); return validMove(source, destination); }
    if (target->type() != ProfileItem || !role.canConvert<ProfileReference>()) return false;
    const auto profile = role.value<ProfileReference>(); const auto document = documents_.constFind(profile.document);
    if (document == documents_.cend()) return false;
    const auto node = document->profiles.value(profile.profile); const bool folder = node.value("kind") == "folder";
    // Folders: outer quarters insert beside, the middle drops inside.
    // Connections: upper half inserts before, lower half after.
    const auto rect = visualItemRect(target); const int offset = point.y() - rect.top();
    if (folder) zone = offset < rect.height() / 4 ? DropZone::Before : offset >= rect.height() - rect.height() / 4 ? DropZone::After : DropZone::Into;
    else zone = offset < rect.height() / 2 ? DropZone::Before : DropZone::After;
    if (zone == DropZone::Into) { destination = profile; return validMove(source, destination); }
    const auto parent = node.value("parent").toString();
    if (parent.isEmpty()) destination = DocumentRootReference{profile.document}; else destination = ProfileReference{profile.document, parent};
    if (zone == DropZone::Before) before = profile.profile;
    else if (document->order.isEmpty()) return false;
    else before = nextSibling(*document, parent, profile.profile, source.profile);
    if (before == source.profile) return false;
    return validMove(source, destination, before);
}
void ConnectionTree::hover(QTreeWidgetItem* item, DropZone zone) {
    hovered_ = item ? indexFromItem(item) : QModelIndex(); hoveredZone_ = zone; viewport()->update();
}
void ConnectionTree::dragEnterEvent(QDragEnterEvent* event) {
    // Entry over the source is valid; only valid destinations get move feedback.
    if ((event->possibleActions() & Qt::MoveAction) && owns(event->mimeData())) {
        event->setDropAction(Qt::MoveAction); event->accept();
    } else event->ignore();
}
void ConnectionTree::dragMoveEvent(QDragMoveEvent* event) {
    ProfileReference source; ConnectionDestination target; QString before; DropZone zone;
    if ((event->possibleActions() & Qt::MoveAction) && destination(event->mimeData(), event->position().toPoint(), source, target, before, zone)) {
        hover(itemAt(event->position().toPoint()), zone); event->setDropAction(Qt::MoveAction); event->accept();
    } else { hover(nullptr); event->ignore(); }
}
void ConnectionTree::dragLeaveEvent(QDragLeaveEvent* event) { hover(nullptr); event->accept(); }
void ConnectionTree::dropEvent(QDropEvent* event) {
    ProfileReference source; ConnectionDestination target; QString before; DropZone zone; hover(nullptr);
    if (!(event->possibleActions() & Qt::MoveAction) || !destination(event->mimeData(), event->position().toPoint(), source, target, before, zone)) { event->ignore(); return; }
    event->setDropAction(Qt::MoveAction); event->accept();
    emit scopedMoveRequested(source, target, before);
}
void ConnectionTree::paintEvent(QPaintEvent* event) {
    QTreeWidget::paintEvent(event);
    if (!hovered_.isValid()) return;
    QPainter painter(viewport()); const auto rect = visualRect(hovered_).adjusted(0, 0, -1, -1);
    if (hoveredZone_ == DropZone::Into) { painter.setPen(palette().color(QPalette::Highlight)); painter.drawRect(rect); return; }
    // Insertion line between rows, with a marker at its start like mRemoteNG.
    const int y = hoveredZone_ == DropZone::Before ? rect.top() : rect.bottom();
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2)); painter.drawLine(rect.left(), y, rect.right(), y);
    painter.setBrush(palette().color(QPalette::Highlight)); painter.drawEllipse(QPoint(rect.left() + 3, y), 3, 3);
}
