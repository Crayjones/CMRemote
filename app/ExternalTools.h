#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QString>

class QCheckBox;
class QLineEdit;
class QTreeWidget;

// mRemoteNG-style external tools: a program and arguments launched for a connection.
struct ExternalTool {
    QString name, program, arguments, workingDirectory;
    bool toolbar = false;
};

namespace ExternalTools {
QList<ExternalTool> load(const QString& path);
bool save(const QString& path, const QList<ExternalTool>& tools);
// Replaces %NAME%, %HOSTNAME%, %PORT%, %USERNAME%, %DOMAIN%, %DESCRIPTION% and %PROTOCOL%
// (any case) with the connection's values. Saved passwords are never substituted.
QString expand(const QString& text, const QJsonObject& profile);
bool launch(const ExternalTool& tool, const QJsonObject& profile, QString* error);
}

// Tools > External tools: list, edit, and launch for the selected connection.
class ExternalToolsDialog final : public QDialog {
    Q_OBJECT
public:
    ExternalToolsDialog(QList<ExternalTool> tools, QJsonObject connection, QWidget* parent = nullptr);
    QList<ExternalTool> tools() const { return tools_; }
private:
    void select();
    void store();
    void refresh();
    QList<ExternalTool> tools_;
    QJsonObject connection_;
    QTreeWidget* list_;
    QLineEdit *name_, *program_, *arguments_, *directory_;
    QCheckBox* toolbar_;
    int current_ = -1;
    bool loading_ = false;
};
