#pragma once
#include "ApplicationPreferences.h"
#include <QPointer>
#include <QDialog>
#include <memory>
namespace Ui { class OptionsDialog; }
class OpenH264Codec;
class OptionsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(PreferenceValues values, ApplicationPreferences* store, QWidget* parent = nullptr, OpenH264Codec* codec = nullptr);
    ~OptionsDialog() override;
    void accept() override;
    void reject() override;
signals:
    void applied(PreferenceValues values);
private:
    std::unique_ptr<Ui::OptionsDialog> ui_;
    ApplicationPreferences* store_;
    QPointer<OpenH264Codec> codec_;
    ConnectionDefaults defaults_;
    bool saving_ = false, saved_ = false;
};
