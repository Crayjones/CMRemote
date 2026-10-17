#pragma once
#include "ConnectionXml.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QRadioButton;

// mRemoteNG's "Export" dialog: file, format, items and properties.
class ExportDialog final : public QDialog {
    Q_OBJECT
public:
    // Empty folder/connection ids disable those item choices.
    ExportDialog(const QString& folderId, const QString& folderName, const QString& connectionId, const QString& connectionName, QWidget* parent = nullptr);
    QString path() const;
    ExportOptions options() const;
    void setPath(const QString& path);
private:
    void browse();
    void update();
    QString folderId_, connectionId_;
    QLineEdit* path_;
    QComboBox* format_;
    QRadioButton *everything_, *folder_, *connection_;
    QCheckBox *username_, *password_, *domain_, *inheritance_;
    QPushButton* ok_;
};
