#pragma once
#include <QMetaType>
#include <QString>
#include <QUuid>

// Runtime identity, deliberately unrelated to RmFileId or copied profile UUIDs.
class DocumentId {
public:
    DocumentId() = default;
    static DocumentId create() { return DocumentId(QUuid::createUuid()); }
    bool valid() const { return !value_.isNull(); }
    QString text() const { return value_.toString(QUuid::WithoutBraces); }
    friend bool operator==(DocumentId a, DocumentId b) { return a.value_ == b.value_; }
    friend bool operator!=(DocumentId a, DocumentId b) { return !(a == b); }
    friend size_t qHash(DocumentId id, size_t seed = 0) { return qHash(id.value_, seed); }
private:
    explicit DocumentId(QUuid value) : value_(value) {}
    QUuid value_;
};
struct DocumentRootReference { DocumentId document; };
struct ProfileReference {
    DocumentId document;
    QString profile;
    bool valid() const { return document.valid() && !profile.isEmpty(); }
    friend bool operator==(const ProfileReference& a, const ProfileReference& b) {
        return a.document == b.document && a.profile == b.profile;
    }
    friend bool operator!=(const ProfileReference& a, const ProfileReference& b) { return !(a == b); }
    friend size_t qHash(const ProfileReference& ref, size_t seed = 0) {
        return qHashMulti(seed, ref.document, ref.profile);
    }
};
Q_DECLARE_METATYPE(DocumentId)
Q_DECLARE_METATYPE(DocumentRootReference)
Q_DECLARE_METATYPE(ProfileReference)
