#include "OptionsDialog.h"
#include "ui_OptionsDialog.h"
#include "OpenH264.h"
#include "TerminalAppearancePage.h"
#include "TerminalAppearanceService.h"
#include <QPointer>
#include <QPushButton>
OptionsDialog::OptionsDialog(PreferenceValues values, ApplicationPreferences* store, QWidget* parent, OpenH264Codec* codec, TerminalAppearanceService* terminal)
    : QDialog(parent), ui_(std::make_unique<Ui::OptionsDialog>()), store_(store), codec_(codec), values_(values) {
    ui_->setupUi(this); setObjectName("optionsDialog"); setAttribute(Qt::WA_DeleteOnClose); setWindowModality(Qt::WindowModal);
    ui_->theme->addItem("Match System", "system"); ui_->theme->addItem("Light", "light"); ui_->theme->addItem("Dark", "dark");
    ui_->theme->setCurrentIndex(std::max(0, ui_->theme->findData(values.theme)));
    ui_->categorized->setChecked(values.categorized); ui_->propertyHelp->setChecked(values.propertyHelp);
    ui_->quickConnect->setChecked(values.quickConnect); ui_->lockToolbars->setChecked(values.lockToolbars);
    ui_->openh264->setChecked(values.openh264); ui_->openh264Credit->setText(OpenH264::Credit);
    ui_->showPanels->setChecked(values.showPanels); ui_->autoUpdate->setChecked(values.autoUpdate);
    ui_->commandAsControl->setChecked(values.commandAsControl);
#ifndef Q_OS_MACOS
    ui_->keyboardTitle->hide(); ui_->commandAsControl->hide(); ui_->commandAsControlHelp->hide();
#endif
    const auto status = [this] { ui_->openh264Status->setText(codec_ ? codec_->statusText() : QString()); };
    if (codec_) connect(codec_, &OpenH264Codec::stateChanged, this, status);
    status();
    if (terminal) {
        terminal_ = new TerminalAppearancePage(terminal, ui_->terminalPage); ui_->terminalLayout->addWidget(terminal_);
        connect(terminal_, &TerminalAppearancePage::busyChanged, this, &OptionsDialog::updateBusy);
        connect(terminal_, &TerminalAppearancePage::saved, this, [this](bool ok) {
            saving_ = false; updateBusy();
            if (ok && closeAfterTerminal_) QDialog::accept();
        });
        connect(terminal, &QObject::destroyed, this, [this] { saving_ = false; QDialog::reject(); });
    } else ui_->categories->item(TerminalPage)->setHidden(true);
    connect(ui_->categories, &QListWidget::currentRowChanged, ui_->pages, &QStackedWidget::setCurrentIndex); ui_->categories->setCurrentRow(0);
    connect(ui_->buttons, &QDialogButtonBox::accepted, this, &OptionsDialog::accept);
    connect(ui_->buttons, &QDialogButtonBox::rejected, this, &OptionsDialog::reject);
    connect(ui_->buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { save(false); });
    updateBusy();
}
OptionsDialog::~OptionsDialog() = default;
void OptionsDialog::showPage(int page) { if (!ui_->categories->item(page)->isHidden()) ui_->categories->setCurrentRow(page); }
void OptionsDialog::reject() { if (!saving_) QDialog::reject(); }
void OptionsDialog::accept() { save(true); }
PreferenceValues OptionsDialog::read() const {
    auto draft = values_;
    draft.theme = ui_->theme->currentData().toString();
    draft.categorized = ui_->categorized->isChecked(); draft.propertyHelp = ui_->propertyHelp->isChecked();
    draft.quickConnect = ui_->quickConnect->isChecked(); draft.lockToolbars = ui_->lockToolbars->isChecked();
    draft.openh264 = ui_->openh264->isChecked(); draft.showPanels = ui_->showPanels->isChecked();
    draft.commandAsControl = ui_->commandAsControl->isChecked(); draft.autoUpdate = ui_->autoUpdate->isChecked();
    return draft;
}
void OptionsDialog::updateBusy() {
    const bool busy = saving_ || (terminal_ && terminal_->busy());
    ui_->pages->setEnabled(!busy && !saved_); ui_->buttons->setEnabled(!busy);
}
void OptionsDialog::save(bool close) {
    if (saving_ || (terminal_ && terminal_->busy())) return;
    if (saved_) { if (close) QDialog::accept(); return; }
    const auto draft = read();
    // Terminal-only edits leave the preferences file alone.
    if (draft == values_ && terminal_ && terminal_->dirty()) { saveTerminal(close); return; }
    saving_ = true; updateBusy(); ui_->error->clear();
    QPointer<OptionsDialog> target = this;
    store_->save(draft, [target, close](PreferenceReply reply) {
        if (!target) return;
        target->saving_ = false; target->updateBusy();
        if (!reply.result.ok) { target->ui_->error->setText(reply.result.error); return; }
        target->values_ = reply.values; emit target->applied(reply.values);
        if (!reply.result.warning.isEmpty()) {
            target->saved_ = true; target->updateBusy(); target->ui_->error->setText(reply.result.warning);
            target->ui_->buttons->button(QDialogButtonBox::Ok)->setText("Saved; close"); return;
        }
        target->saveTerminal(close);
    });
}
void OptionsDialog::saveTerminal(bool close) {
    if (terminal_ && terminal_->save()) { saving_ = true; closeAfterTerminal_ = close; updateBusy(); return; }
    if (close) QDialog::accept();
}
