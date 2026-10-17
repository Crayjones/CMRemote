#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

struct FileResult { bool ok = false; QString error, warning; };

// Blocking, single-thread-owned storage. Successful replacement is the commit boundary.
class AtomicFile {
public:
    enum class Fault { None, BeforeReplace, DirectorySync };
    using CommitGate = std::function<bool(const std::function<bool()>&)>;
    explicit AtomicFile(QString path);
    QString path() const { return path_; }
    QByteArray revision() const { return revision_; }
    bool exists() const { return inspected_ && exists_; }
    bool load(QByteArray& bytes, bool& exists, qsizetype limit);
    bool unchanged(qsizetype limit) const;
    FileResult replace(const QByteArray& bytes, qsizetype limit, const QString& label,
                       const std::function<bool()>& prerequisite = {});
    void reset();
    void setCommitGate(CommitGate gate) { gate_ = std::move(gate); }
    void setFault(Fault fault) { fault_ = fault; }
private:
    bool safePath() const;
    bool read(QByteArray& bytes, bool& exists, qsizetype limit) const;
    QString path_;
    QByteArray revision_;
    bool inspected_ = false, exists_ = false;
    CommitGate gate_;
    Fault fault_ = Fault::None;
};
