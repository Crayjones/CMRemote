#include "ExportDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

ExportDialog::ExportDialog(const QString& folderId, const QString& folderName, const QString& connectionId, const QString& connectionName, QWidget* parent)
    : QDialog(parent), folderId_(folderId), connectionId_(connectionId) {
    setObjectName("exportDialog"); setWindowTitle("Export");
    auto* layout = new QVBoxLayout(this);

    auto* file = new QGroupBox("Export File", this); auto* fileLayout = new QGridLayout(file);
    fileLayout->addWidget(new QLabel("Filename:", file), 0, 0, 1, 2);
    path_ = new QLineEdit(file); path_->setObjectName("exportPath"); fileLayout->addWidget(path_, 1, 0);
    auto* browse = new QPushButton("Browse…", file); browse->setObjectName("exportBrowse"); fileLayout->addWidget(browse, 1, 1);
    fileLayout->addWidget(new QLabel("File Format:", file), 2, 0, 1, 2);
    format_ = new QComboBox(file); format_->setObjectName("exportFormat"); format_->addItems({"mRemoteNG XML", "mRemoteNG CSV"});
    fileLayout->addWidget(format_, 3, 0);
    layout->addWidget(file);

    auto* items = new QGroupBox("Export Items", this); auto* itemsLayout = new QVBoxLayout(items);
    everything_ = new QRadioButton("Export everything", items); everything_->setObjectName("exportEverything"); everything_->setChecked(true);
    folder_ = new QRadioButton("Export the currently selected folder", items); folder_->setObjectName("exportFolder");
    connection_ = new QRadioButton("Export the currently selected connection", items); connection_->setObjectName("exportConnection");
    const auto detail = [items](const QString& text) { auto* label = new QLabel(text, items); label->setIndent(24); return label; };
    itemsLayout->addWidget(everything_);
    itemsLayout->addWidget(folder_); if (!folderName.isEmpty()) itemsLayout->addWidget(detail(folderName));
    itemsLayout->addWidget(connection_); if (!connectionName.isEmpty()) itemsLayout->addWidget(detail(connectionName));
    folder_->setEnabled(!folderId.isEmpty()); connection_->setEnabled(!connectionId.isEmpty());
    layout->addWidget(items);

    auto* properties = new QGroupBox("Export Properties", this); auto* propertiesLayout = new QVBoxLayout(properties);
    const auto check = [properties, propertiesLayout](const char* text, const char* name) {
        auto* box = new QCheckBox(text, properties); box->setObjectName(name); box->setChecked(true); propertiesLayout->addWidget(box); return box;
    };
    username_ = check("Username", "exportUsername"); password_ = check("Password", "exportPassword");
    domain_ = check("Domain", "exportDomain"); inheritance_ = check("Inheritance", "exportInheritance");
    propertiesLayout->addWidget(new QLabel("Uncheck the properties you want not to be saved!", properties));
    layout->addWidget(properties);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(path_, &QLineEdit::textChanged, this, &ExportDialog::update);
    connect(format_, &QComboBox::currentIndexChanged, this, [this] {
        // Keep a typed name's extension in step with the chosen format.
        const auto current = path_->text(); if (current.isEmpty()) return;
        const QFileInfo info(current); const auto suffix = format_->currentIndex() == 1 ? "csv" : "xml";
        if (info.suffix().compare(suffix, Qt::CaseInsensitive) != 0 && (info.suffix() == "xml" || info.suffix() == "csv"))
            path_->setText(info.path() + "/" + info.completeBaseName() + "." + suffix);
    });
    update();
}

QString ExportDialog::path() const { return path_->text().trimmed(); }
void ExportDialog::setPath(const QString& path) { path_->setText(path); }

ExportOptions ExportDialog::options() const {
    ExportOptions result;
    result.format = format_->currentIndex() == 1 ? ExportOptions::Format::Csv : ExportOptions::Format::Xml;
    result.scope = folder_->isChecked() ? folderId_ : connection_->isChecked() ? connectionId_ : QString();
    result.username = username_->isChecked(); result.password = password_->isChecked();
    result.domain = domain_->isChecked(); result.inheritance = inheritance_->isChecked();
    return result;
}

void ExportDialog::browse() {
    const bool csv = format_->currentIndex() == 1;
    const auto chosen = QFileDialog::getSaveFileName(this, "Export to File", path(), csv ? "mRemoteNG CSV (*.csv)" : "mRemoteNG XML (*.xml)");
    if (chosen.isEmpty()) return;
    const auto suffix = csv ? ".csv" : ".xml";
    path_->setText(chosen.endsWith(suffix, Qt::CaseInsensitive) ? chosen : chosen + suffix);
}

void ExportDialog::update() { ok_->setEnabled(!path().isEmpty()); }
