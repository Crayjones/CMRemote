#pragma once
#include "TerminalAppearance.h"
#include "AtomicFile.h"
#include <QObject>
#include <functional>
#include <memory>
#include <vector>

class QThread;
class TerminalAppearanceService final : public QObject {
    Q_OBJECT
public:
    struct Options { AtomicFile::Fault fault = AtomicFile::Fault::None; };
    explicit TerminalAppearanceService(QString path, QObject* parent = nullptr);
    TerminalAppearanceService(QString path, Options options, QObject* parent = nullptr);
    ~TerminalAppearanceService() override;
    bool ready() const { return ready_; }
    bool busy() const { return busy_; }
    const TerminalAppearance& current() const { return current_; }
    QString error() const { return error_; }
    QString warning() const { return warning_; }
    void apply(TerminalAppearance value, bool resetInvalid = false);
    void acknowledgeWarning();
    void shutdown(std::function<void()> complete = {});
    static int activeWorkers();
    Qt::HANDLE performedOn() const { return performedOn_; }
    void setFault(AtomicFile::Fault fault);
signals:
    void changed();
    void applied();
private:
    struct State;
    struct Reply;
    void execute(std::function<Reply(State&)> operation);
    std::shared_ptr<State> state_;
    QThread* workerThread_ = nullptr;
    QObject* worker_ = nullptr;
    QObject* delivery_ = nullptr;
    TerminalAppearance current_;
    QString error_, warning_;
    bool ready_ = false, busy_ = false, shuttingDown_ = false, finished_ = false;
    Qt::HANDLE performedOn_ = nullptr;
    std::vector<std::function<void()>> shutdownCompletions_;
};
