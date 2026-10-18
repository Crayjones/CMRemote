#pragma once
#include "TerminalAppearance.h"
#include <QPointer>
#include <QWidget>
#include <memory>

namespace Ui { class TerminalAppearancePage; }
class TerminalAppearanceService;
class TerminalView;
// The Settings dialog's Terminal page: edits a draft and saves it through the service.
class TerminalAppearancePage final : public QWidget {
    Q_OBJECT
public:
    explicit TerminalAppearancePage(TerminalAppearanceService* service, QWidget* parent = nullptr);
    ~TerminalAppearancePage() override;
    bool busy() const;
    bool dirty() const;
    // Starts a save; false when there is nothing to save or the service cannot take it.
    bool save();
signals:
    void busyChanged();
    // A save started by save() ended; ok is false on error (the page shows it).
    void saved(bool ok);
private:
    void populate();
    void readControls();
    void refreshColors();
    void chooseColor();
    void renderSample();
    void serviceChanged();
    QColor& color(int row);
    std::unique_ptr<Ui::TerminalAppearancePage> ui_;
    QPointer<TerminalAppearanceService> service_;
    TerminalView* preview_ = nullptr;
    TerminalAppearance draft_;
    bool updating_ = false, loaded_ = false, saving_ = false, resetInvalid_ = false;
};
