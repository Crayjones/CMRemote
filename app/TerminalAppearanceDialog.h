#pragma once
#include "TerminalAppearance.h"
#include <QDialog>
#include <QPointer>
#include <memory>

namespace Ui { class TerminalAppearanceDialog; }
class TerminalAppearanceService;
class TerminalView;
class TerminalAppearanceDialog final : public QDialog {
    Q_OBJECT
public:
    explicit TerminalAppearanceDialog(TerminalAppearanceService* service, QWidget* parent = nullptr);
    ~TerminalAppearanceDialog() override;
    void reject() override;
private:
    void populate();
    void readControls();
    void refreshColors();
    void chooseColor();
    void apply(bool close);
    void serviceChanged();
    QColor& color(int row);
    std::unique_ptr<Ui::TerminalAppearanceDialog> ui_;
    QPointer<TerminalAppearanceService> service_;
    TerminalView* preview_ = nullptr;
    TerminalAppearance draft_;
    bool updating_ = false, loaded_ = false, saving_ = false, closeAfterSave_ = false, resetInvalid_ = false;
};
