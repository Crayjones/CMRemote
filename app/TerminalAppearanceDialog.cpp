#include "TerminalAppearanceDialog.h"
#include "TerminalAppearanceService.h"
#include "SessionClipboard.h"
#include "Views.h"
#include "ui_TerminalAppearanceDialog.h"
#include <QColorDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>

namespace {
class PreviewClipboard final : public ClipboardEndpoint {
public:
    void watch() override {}
    std::optional<QString> readText() override { return {}; }
    void writeText(const QString&) override {}
};
bool sameColors(const TerminalAppearance& a, const TerminalAppearance& b) {
    return a.foreground == b.foreground && a.background == b.background && a.selectionForeground == b.selectionForeground &&
        a.selectionBackground == b.selectionBackground && a.cursorColor == b.cursorColor && a.ansi == b.ansi;
}
}
TerminalAppearanceDialog::TerminalAppearanceDialog(TerminalAppearanceService* service, QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::TerminalAppearanceDialog>()), service_(service) {
    ui_->setupUi(this); setObjectName("terminalAppearanceDialog"); resize(800, 720);
    ui_->cursorShape->addItems({"Block", "Underline", "Bar"}); ui_->boldStyle->addItems({"Font only", "Font and bright ANSI colors"});
    ui_->palettePreset->addItems({"Classic (PuTTY-inspired)", "libvterm original", "Tomorrow Night", "Custom"});
    ui_->categories->setCurrentRow(0);
    connect(ui_->categories, &QListWidget::currentRowChanged, ui_->pages, &QStackedWidget::setCurrentIndex);
    ui_->colorTable->verticalHeader()->hide(); ui_->colorTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui_->colorTable->setRowCount(21);
    const QStringList labels{"Foreground", "Background", "Selection foreground", "Selection background", "Cursor",
        "ANSI black", "ANSI red", "ANSI green", "ANSI yellow", "ANSI blue", "ANSI magenta", "ANSI cyan", "ANSI white",
        "Bright black", "Bright red", "Bright green", "Bright yellow", "Bright blue", "Bright magenta", "Bright cyan", "Bright white"};
    for (int row = 0; row < labels.size(); ++row) {
        ui_->colorTable->setItem(row, 0, new QTableWidgetItem(labels[row])); ui_->colorTable->setItem(row, 1, new QTableWidgetItem);
    }
    ui_->colorTable->selectRow(0);
    preview_ = new TerminalView(this); preview_->setObjectName("appearancePreview"); preview_->setMinimumHeight(180);
    preview_->initializeTerminal(std::make_shared<PreviewClipboard>()); preview_->setInputEnabled(false); preview_->setSessionShortcutsEnabled(false); ui_->previewLayout->addWidget(preview_);
    preview_->append("Normal text   \x1b[1mBold text\x1b[0m   Unicode: caf\xc3\xa9 \xe7\x95\x8c\r\n"
        "\x1b[30mBlack \x1b[31mRed \x1b[32mGreen \x1b[33mYellow \x1b[34mBlue \x1b[35mMagenta \x1b[36mCyan \x1b[37mWhite\x1b[0m\r\n"
        "\x1b[90mBlack \x1b[91mRed \x1b[92mGreen \x1b[93mYellow \x1b[94mBlue \x1b[95mMagenta \x1b[96mCyan \x1b[97mWhite\x1b[0m\r\n"
        "\x1b[38;2;120;180;220mExplicit RGB (unchanged)\x1b[0m\r\nSelection sample     Cursor: ");
    preview_->selectCells(QPoint(0, 4), QPoint(8, 4));
    connect(ui_->fontFamily, &QFontComboBox::currentFontChanged, this, [this] { readControls(); });
    connect(ui_->fontSize, &QSpinBox::valueChanged, this, [this] { readControls(); });
    connect(ui_->cursorShape, &QComboBox::currentIndexChanged, this, [this] { readControls(); });
    connect(ui_->cursorBlink, &QCheckBox::toggled, this, [this] { readControls(); });
    connect(ui_->boldStyle, &QComboBox::currentIndexChanged, this, [this] { readControls(); });
    connect(ui_->palettePreset, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (updating_ || index < 0 || index > 2) return;
        const auto preset = index == 0 ? TerminalAppearance::classic() : index == 1 ? TerminalAppearance::original() : TerminalAppearance::tomorrowNight();
        draft_.foreground = preset.foreground; draft_.background = preset.background; draft_.selectionForeground = preset.selectionForeground;
        draft_.selectionBackground = preset.selectionBackground; draft_.cursorColor = preset.cursorColor; draft_.ansi = preset.ansi;
        refreshColors(); preview_->setAppearance(draft_);
    });
    connect(ui_->changeColor, &QPushButton::clicked, this, &TerminalAppearanceDialog::chooseColor);
    connect(ui_->colorTable, &QTableWidget::cellDoubleClicked, this, [this] { chooseColor(); });
    connect(ui_->resetDefaults, &QPushButton::clicked, this, [this] { draft_ = TerminalAppearance::classic(); resetInvalid_ = true; populate(); });
    connect(ui_->buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(false); });
    connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, [this] { apply(true); });
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &TerminalAppearanceDialog::reject);
    connect(service, &TerminalAppearanceService::changed, this, &TerminalAppearanceDialog::serviceChanged);
    connect(service, &QObject::destroyed, this, &QDialog::reject);
    serviceChanged();
}
TerminalAppearanceDialog::~TerminalAppearanceDialog() = default;
void TerminalAppearanceDialog::reject() { if (!saving_) QDialog::reject(); }
void TerminalAppearanceDialog::populate() {
    updating_ = true; auto face = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!draft_.fontFamily.isEmpty() && QFontDatabase::isFixedPitch(draft_.fontFamily)) face.setFamily(draft_.fontFamily);
    ui_->fontFamily->setCurrentFont(face); ui_->fontSize->setValue(draft_.fontSize);
    ui_->cursorShape->setCurrentIndex(int(draft_.cursor)); ui_->cursorBlink->setChecked(draft_.blink); ui_->boldStyle->setCurrentIndex(draft_.boldBright ? 1 : 0);
    updating_ = false; refreshColors(); preview_->setAppearance(draft_);
}
void TerminalAppearanceDialog::readControls() {
    if (updating_) return;
    const auto family = ui_->fontFamily->currentFont().family();
    if (family != preview_->font().family()) draft_.fontFamily = family;
    draft_.fontSize = ui_->fontSize->value(); draft_.cursor = TerminalAppearance::Cursor(ui_->cursorShape->currentIndex());
    draft_.blink = ui_->cursorBlink->isChecked(); draft_.boldBright = ui_->boldStyle->currentIndex() == 1; preview_->setAppearance(draft_);
}
QColor& TerminalAppearanceDialog::color(int row) {
    switch (row) {
    case 0: return draft_.foreground; case 1: return draft_.background; case 2: return draft_.selectionForeground;
    case 3: return draft_.selectionBackground; case 4: return draft_.cursorColor; default: return draft_.ansi[size_t(row - 5)];
    }
}
void TerminalAppearanceDialog::refreshColors() {
    updating_ = true;
    for (int row = 0; row < 21; ++row) {
        const auto value = color(row); auto* item = ui_->colorTable->item(row, 1); item->setText(value.name()); item->setBackground(value);
        item->setForeground((value.red() * 299 + value.green() * 587 + value.blue() * 114) > 128000 ? Qt::black : Qt::white);
    }
    ui_->palettePreset->setCurrentIndex(sameColors(draft_, TerminalAppearance::classic()) ? 0 : sameColors(draft_, TerminalAppearance::original()) ? 1 : sameColors(draft_, TerminalAppearance::tomorrowNight()) ? 2 : 3);
    updating_ = false;
}
void TerminalAppearanceDialog::chooseColor() {
    const auto row = ui_->colorTable->currentRow(); if (row < 0 || row >= 21) return;
    auto* picker = new QColorDialog(color(row), this); picker->setAttribute(Qt::WA_DeleteOnClose); picker->setWindowTitle("Terminal color");
    connect(picker, &QColorDialog::colorSelected, this, [this, row](QColor value) { color(row) = value; refreshColors(); preview_->setAppearance(draft_); }); picker->open();
}
void TerminalAppearanceDialog::apply(bool close) {
    if (!service_ || !service_->ready() || service_->busy() || !service_->warning().isEmpty()) return;
    saving_ = true; closeAfterSave_ = close; service_->apply(draft_, resetInvalid_);
}
void TerminalAppearanceDialog::serviceChanged() {
    if (!service_) return;
    if (service_->ready() && !loaded_) { loaded_ = true; draft_ = service_->current(); populate(); }
    const bool busy = !service_->ready() || service_->busy();
    ui_->pages->setEnabled(!busy); ui_->resetDefaults->setEnabled(!busy); ui_->buttonBox->setEnabled(!busy);
    ui_->statusLabel->setText(busy ? "Loading or saving terminal appearance…" : service_->error());
    if (!busy && saving_) {
        saving_ = false;
        if (service_->error().isEmpty()) { draft_ = service_->current(); resetInvalid_ = false; populate(); }
        else closeAfterSave_ = false;
    }
    if (!busy && !service_->warning().isEmpty()) {
        ui_->buttonBox->setEnabled(false);
        auto* notice = new QMessageBox(QMessageBox::Warning, "Terminal appearance saved", service_->warning(), QMessageBox::Ok, this);
        notice->setAttribute(Qt::WA_DeleteOnClose);
        connect(notice, &QMessageBox::finished, this, [this] { if (service_) service_->acknowledgeWarning(); }); notice->open();
    } else if (!busy && closeAfterSave_ && service_->error().isEmpty()) { closeAfterSave_ = false; QDialog::accept(); }
}
