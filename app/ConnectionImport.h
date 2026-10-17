#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Converts other connection lists into mRemoteNG XML, so every import uses the
// native reader and its field mapping. Plain-text passwords are encrypted with
// the public mR3m default key; mRemoteNG XML passes through unchanged.
namespace ConnectionImport {
// mRemoteNG's "Import from File" filter list.
QString fileFilter();
// .xml (mRemoteNG), .csv (mRemoteNG CSV), .rdp, .rdg (Remote Desktop Connection Manager), .dat (PuTTY Connection Manager).
std::optional<QByteArray> fromFile(const QString& path, const QByteArray& bytes, QString* error);
// Saved PuTTY sessions: the registry on Windows, ~/.putty/sessions elsewhere.
std::optional<QByteArray> fromPuttySessions(QString* error);
}
