#pragma once
#include "MainWindow.h"
#include "DocumentService.h"
#include <QFile>
#include <QJsonDocument>
#include <QSemaphore>
#include <QTest>
#include <atomic>
#include <memory>

inline bool documentBusy(MainWindow& window) {
    auto* service = window.findChild<DocumentService*>();
    for (const auto id : window.connectionFiles()) {
        const auto status = service->status(id);
        if (status && status->busy) return true;
    }
    return false;
}
inline QJsonObject storedXml(const QString& path, const Secret& master = Secret(QByteArray("mR3m"))) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    auto xml = ConnectionXml::read(file.readAll(), &master); if (!xml) return {};
    const auto projection = xml->projection();
    return projection ? QJsonDocument::fromJson(*projection).object() : QJsonObject();
}
inline QString treeProfileId(const QTreeWidgetItem* item) {
    const auto reference = item->data(0, Qt::UserRole).value<ProfileReference>();
    return reference.profile;
}
inline bool setDocumentFault(MainWindow& window, AtomicFile::Fault fault) {
    std::optional<DocumentReply> reply;
    window.findChild<DocumentService*>()->setFault(window.selectedDocument(), fault,
        [&](DocumentReply value) { reply = std::move(value); });
    return QTest::qWaitFor([&] { return reply.has_value(); }) && reply->result.ok;
}
inline bool setDocumentGate(MainWindow& window, AtomicFile::CommitGate gate) {
    std::optional<DocumentReply> reply;
    window.findChild<DocumentService*>()->setCommitGate(window.selectedDocument(), std::move(gate),
        [&](DocumentReply value) { reply = std::move(value); });
    return QTest::qWaitFor([&] { return reply.has_value(); }) && reply->result.ok;
}
// Worker waits only on a semaphore; GUI remains free to exercise Lock/close.
// Release before the window/service destructor even when a test assertion fails.
class XmlCommitBarrier {
    struct State { QSemaphore entered, release; };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    ~XmlCommitBarrier() { release(); }
    bool entered(int count = 1) const { return state_->entered.available() >= count; }
    void release() { state_->release.release(); }
    AtomicFile::CommitGate gate(bool after = false) {
        return [state = state_, after](const auto& commit) {
            const bool result = after && commit();
            state->entered.release(); state->release.acquire();
            return after ? result : commit();
        };
    }
};
