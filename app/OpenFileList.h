#pragma once
#include <QObject>
#include <QStringList>
#include <functional>
class QThread;

// Connection files the user opened beyond the default one. They reopen at
// startup until the user closes them. File work stays off the GUI thread.
class OpenFileList final : public QObject {
    Q_OBJECT
public:
    explicit OpenFileList(QString path, QObject* parent = nullptr);
    ~OpenFileList() override;
    void load(std::function<void(QStringList)> complete);
    // Writes are serialized; an unchanged list is not rewritten.
    void save(QStringList paths);
private:
    QString path_;
    QThread* thread_ = nullptr;
    QObject* worker_ = nullptr;
    QStringList saved_;
    bool loaded_ = false;
};
