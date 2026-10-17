#include "ExternalTools.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ExternalTools {
QList<ExternalTool> load(const QString& path) {
    QList<ExternalTool> tools; QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return tools;
    for (const auto& value : QJsonDocument::fromJson(file.readAll()).object().value("tools").toArray()) {
        const auto tool = value.toObject();
        tools.append({tool.value("name").toString(), tool.value("program").toString(), tool.value("arguments").toString(),
                      tool.value("working_directory").toString(), tool.value("toolbar").toBool()});
    }
    return tools;
}

bool save(const QString& path, const QList<ExternalTool>& tools) {
    QJsonArray list;
    for (const auto& tool : tools) list.append(QJsonObject{{"name", tool.name}, {"program", tool.program}, {"arguments", tool.arguments},
                                                           {"working_directory", tool.workingDirectory}, {"toolbar", tool.toolbar}});
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
           file.write(QJsonDocument(QJsonObject{{"schema", 1}, {"tools", list}}).toJson()) > 0 && file.commit();
}

QString expand(const QString& text, const QJsonObject& profile) {
    const auto effective = profile.value("effective").toObject();
    const auto port = effective.value("port").toInt();
    const std::pair<const char*, QString> values[] = {
        {"%NAME%", profile.value("name").toString()}, {"%HOSTNAME%", effective.value("host").toString()},
        {"%PORT%", port ? QString::number(port) : QString()}, {"%USERNAME%", effective.value("username").toString()},
        {"%DOMAIN%", effective.value("domain").toString()}, {"%DESCRIPTION%", effective.value("description").toString()},
        {"%PROTOCOL%", effective.value("protocol").toString().toUpper()}};
    auto result = text;
    for (const auto& [name, value] : values) result.replace(QString::fromLatin1(name), value, Qt::CaseInsensitive);
    return result;
}

bool launch(const ExternalTool& tool, const QJsonObject& profile, QString* error) {
    const auto program = expand(tool.program, profile).trimmed();
    if (program.isEmpty()) { if (error) *error = "External tool \"" + tool.name + "\" has no filename."; return false; }
    auto arguments = QProcess::splitCommand(expand(tool.arguments, profile));
    auto executable = program;
#ifdef Q_OS_MACOS
    // Application bundles start through LaunchServices.
    if (program.endsWith(".app") || program.endsWith(".app/")) { arguments = QStringList{"-a", program, "--args"} + arguments; executable = "open"; }
#endif
    const auto directory = expand(tool.workingDirectory, profile);
    if (!QProcess::startDetached(executable, arguments, directory)) { if (error) *error = "Cannot start " + program + "."; return false; }
    return true;
}
}

ExternalToolsDialog::ExternalToolsDialog(QList<ExternalTool> tools, QJsonObject connection, QWidget* parent)
    : QDialog(parent), tools_(std::move(tools)), connection_(std::move(connection)) {
    setObjectName("externalToolsDialog"); setWindowTitle("External Tools"); resize(720, 520);
    auto* layout = new QVBoxLayout(this);
    list_ = new QTreeWidget(this); list_->setObjectName("externalToolsList"); list_->setRootIsDecorated(false);
    list_->setHeaderLabels({"Display Name", "Filename", "Arguments"}); list_->header()->setStretchLastSection(true);
    layout->addWidget(list_, 1);

    auto* form = new QFormLayout;
    name_ = new QLineEdit(this); name_->setObjectName("toolName"); form->addRow("Display Name:", name_);
    auto* programRow = new QHBoxLayout; program_ = new QLineEdit(this); program_->setObjectName("toolProgram");
    auto* browse = new QPushButton("Browse…", this); programRow->addWidget(program_); programRow->addWidget(browse);
    form->addRow("Filename:", programRow);
    arguments_ = new QLineEdit(this); arguments_->setObjectName("toolArguments"); form->addRow("Arguments:", arguments_);
    directory_ = new QLineEdit(this); directory_->setObjectName("toolDirectory"); form->addRow("Working Directory:", directory_);
    toolbar_ = new QCheckBox("Show on toolbar", this); toolbar_->setObjectName("toolToolbar"); form->addRow({}, toolbar_);
    layout->addLayout(form);
    auto* help = new QLabel("Variables: %NAME%, %HOSTNAME%, %PORT%, %USERNAME%, %DOMAIN%, %DESCRIPTION%, %PROTOCOL%", this);
    help->setWordWrap(true); layout->addWidget(help);

    auto* buttons = new QDialogButtonBox(this);
    auto* add = buttons->addButton("New", QDialogButtonBox::ActionRole); add->setObjectName("toolNew");
    auto* remove = buttons->addButton("Delete", QDialogButtonBox::ActionRole); remove->setObjectName("toolDelete");
    auto* run = buttons->addButton("Launch", QDialogButtonBox::ActionRole); run->setObjectName("toolLaunch");
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    connect(list_, &QTreeWidget::currentItemChanged, this, &ExternalToolsDialog::select);
    for (auto* edit : {name_, program_, arguments_, directory_}) connect(edit, &QLineEdit::textEdited, this, &ExternalToolsDialog::store);
    connect(toolbar_, &QCheckBox::toggled, this, &ExternalToolsDialog::store);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Choose program");
        if (!path.isEmpty()) { program_->setText(path); store(); }
    });
    connect(add, &QPushButton::clicked, this, [this] {
        tools_.append({"New External Tool", {}, {}, {}, false}); refresh(); list_->setCurrentItem(list_->topLevelItem(tools_.size() - 1));
        name_->setFocus(); name_->selectAll();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (current_ < 0) return;
        tools_.removeAt(current_); current_ = -1; refresh();
        if (!tools_.isEmpty()) list_->setCurrentItem(list_->topLevelItem(0));
    });
    // Launch needs a connection to fill in the variables.
    run->setEnabled(connection_.value("kind") == "connection");
    connect(run, &QPushButton::clicked, this, [this] {
        if (current_ < 0) return;
        QString error;
        if (!ExternalTools::launch(tools_.at(current_), connection_, &error)) QMessageBox::warning(this, "External Tools", error);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    refresh();
    if (!tools_.isEmpty()) list_->setCurrentItem(list_->topLevelItem(0)); else select();
}

void ExternalToolsDialog::refresh() {
    const int keep = current_; loading_ = true; list_->clear();
    for (const auto& tool : tools_) list_->addTopLevelItem(new QTreeWidgetItem({tool.name, tool.program, tool.arguments}));
    loading_ = false;
    if (keep >= 0 && keep < tools_.size()) list_->setCurrentItem(list_->topLevelItem(keep));
}

void ExternalToolsDialog::select() {
    if (loading_) return;
    current_ = list_->currentItem() ? list_->indexOfTopLevelItem(list_->currentItem()) : -1;
    const bool editing = current_ >= 0; loading_ = true;
    const auto tool = editing ? tools_.at(current_) : ExternalTool{};
    name_->setText(tool.name); program_->setText(tool.program); arguments_->setText(tool.arguments);
    directory_->setText(tool.workingDirectory); toolbar_->setChecked(tool.toolbar);
    for (QWidget* field : {static_cast<QWidget*>(name_), static_cast<QWidget*>(program_), static_cast<QWidget*>(arguments_),
                           static_cast<QWidget*>(directory_), static_cast<QWidget*>(toolbar_)}) field->setEnabled(editing);
    loading_ = false;
}

void ExternalToolsDialog::store() {
    if (loading_ || current_ < 0) return;
    auto& tool = tools_[current_];
    tool = {name_->text(), program_->text(), arguments_->text(), directory_->text(), toolbar_->isChecked()};
    if (auto* item = list_->topLevelItem(current_)) { item->setText(0, tool.name); item->setText(1, tool.program); item->setText(2, tool.arguments); }
}
