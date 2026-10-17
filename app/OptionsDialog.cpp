#include "OptionsDialog.h"
#include "ui_OptionsDialog.h"
#include "OpenH264.h"
#include <QPointer>
#include <QPushButton>
OptionsDialog::OptionsDialog(PreferenceValues values, ApplicationPreferences* store, QWidget* parent, OpenH264Codec* codec)
    : QDialog(parent), ui_(std::make_unique<Ui::OptionsDialog>()), store_(store), codec_(codec), defaults_(values.defaults) {
    ui_->setupUi(this); setObjectName("optionsDialog"); setAttribute(Qt::WA_DeleteOnClose); setWindowModality(Qt::WindowModal);
    ui_->categorized->setChecked(values.categorized); ui_->propertyHelp->setChecked(values.propertyHelp);
    ui_->quickConnect->setChecked(values.quickConnect); ui_->lockToolbars->setChecked(values.lockToolbars);
    ui_->openh264->setChecked(values.openh264); ui_->openh264Credit->setText(OpenH264::Credit);
    const auto status = [this] { ui_->openh264Status->setText(codec_ ? codec_->statusText() : QString()); };
    if (codec_) connect(codec_, &OpenH264Codec::stateChanged, this, status);
    status();
    connect(ui_->categories, &QListWidget::currentRowChanged, ui_->pages, &QStackedWidget::setCurrentIndex); ui_->categories->setCurrentRow(0);
    connect(ui_->buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(ui_->buttons, &QDialogButtonBox::rejected, this, &OptionsDialog::reject);
}
OptionsDialog::~OptionsDialog() = default;
void OptionsDialog::reject() { if (!saving_) QDialog::reject(); }
void OptionsDialog::accept() {
    if (saving_) return;
    if (saved_) { QDialog::accept(); return; }
    const PreferenceValues draft{ui_->categorized->isChecked(), ui_->propertyHelp->isChecked(), ui_->quickConnect->isChecked(), ui_->lockToolbars->isChecked(), defaults_, ui_->openh264->isChecked()};
    saving_ = true; ui_->pages->setEnabled(false); ui_->buttons->setEnabled(false); ui_->error->clear();
    QPointer<OptionsDialog> target = this;
    store_->save(draft, [target](PreferenceReply reply) {
        if (!target) return;
        target->saving_ = false; target->ui_->pages->setEnabled(true); target->ui_->buttons->setEnabled(true);
        if (!reply.result.ok) { target->ui_->error->setText(reply.result.error); return; }
        emit target->applied(reply.values);
        if (!reply.result.warning.isEmpty()) { target->saved_ = true; target->ui_->pages->setEnabled(false); target->ui_->error->setText(reply.result.warning); target->ui_->buttons->button(QDialogButtonBox::Ok)->setText("Saved; close"); }
        else target->QDialog::accept();
    });
}
