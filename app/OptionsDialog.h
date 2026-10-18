#pragma once
#include "ApplicationPreferences.h"
#include <QPointer>
#include <QDialog>
#include <memory>
namespace Ui { class OptionsDialog; }
class OpenH264Codec;
class TerminalAppearancePage;
class TerminalAppearanceService;
// The Settings dialog: application preferences plus the SSH terminal appearance.
class OptionsDialog final : public QDialog {
    Q_OBJECT
public:
    static constexpr int TerminalPage = 1;
    explicit OptionsDialog(PreferenceValues values, ApplicationPreferences* store, QWidget* parent = nullptr, OpenH264Codec* codec = nullptr,
                           TerminalAppearanceService* terminal = nullptr);
    ~OptionsDialog() override;
    void showPage(int page);
    void accept() override;
    void reject() override;
signals:
    void applied(PreferenceValues values);
private:
    PreferenceValues read() const;
    void save(bool close);
    void saveTerminal(bool close);
    void updateBusy();
    std::unique_ptr<Ui::OptionsDialog> ui_;
    ApplicationPreferences* store_;
    QPointer<OpenH264Codec> codec_;
    TerminalAppearancePage* terminal_ = nullptr;
    PreferenceValues values_;
    bool saving_ = false, saved_ = false, closeAfterTerminal_ = false;
};
