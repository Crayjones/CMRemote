#include "SshAuthentication.h"
#include "remotemanager-core/src/bridge.rs.h"
#include <QTest>

namespace {
Secret secret(const QByteArray& value) { return Secret(value); }
QString publicError(Secret value, const QString& path = {}) {
    try {
        if (path.isNull()) { auto auth = sshPasswordAuthentication(std::move(value)); }
        else { auto auth = sshPrivateKeyAuthentication(path, std::move(value)); }
    } catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
    return {};
}
}
class SshAuthenticationTest : public QObject {
    Q_OBJECT
private slots:
    void sshAuthHandoffKeepsCoreMetadataSecretFree() {
        auto core = rm::detached_core();
        core->load_document(R"({"schema_version":2,"profiles":[]})");
        const auto before = QString::fromStdString(std::string(core->stored_document()));
        const auto snapshot = QString::fromStdString(std::string(core->snapshot()));
        Secret password = secret("synthetic-session-password");
        {
            auto auth = sshPasswordAuthentication(std::move(password));
            QCOMPARE(password.size(), size_t(0));
            QVERIFY(QString::fromStdString(std::string(core->stored_document())) == before);
            QVERIFY(QString::fromStdString(std::string(core->snapshot())) == snapshot);
            QCOMPARE(core->session_count(), size_t(0));
            QVERIFY(core->poll_events().empty());
        }
        QCOMPARE(password.size(), size_t(0));
    }
    void sshPrivateKeyHandoffIsSessionOnlyAndDoesNotReadFiles() {
        Secret passphrase = secret("synthetic-key-passphrase");
        // A nonexistent synthetic path is metadata here. Only the worker may read it.
        auto auth = sshPrivateKeyAuthentication("/owned-fixture/missing-key", std::move(passphrase));
        QCOMPARE(passphrase.size(), size_t(0));
        auto unencrypted = sshPrivateKeyAuthentication("/owned-fixture/missing-key", Secret());
        auto emptyPassword = sshPasswordAuthentication(Secret());
    }
    void sshAuthBoundsAndBridgeErrorsAreSecretFree() {
        auto boundary = sshPasswordAuthentication(secret(QByteArray(4096, 'x')));
        auto unicode = sshPasswordAuthentication(secret(QString(1024, QChar(0x1f80)).toUtf8()));
        for (const auto& bytes : {QByteArray(4097, 'x'), QByteArray(1, char(0xff))}) {
            Secret value = secret(bytes);
            const auto error = publicError(std::move(value));
            QCOMPARE(value.size(), size_t(0));
            QCOMPARE(error, QString("SSH authentication must be at most 4096 UTF-8 bytes."));
        }
        Secret passphrase = secret("synthetic-key-passphrase");
        const auto error = publicError(std::move(passphrase), QString("/owned/key\n"));
        QCOMPARE(passphrase.size(), size_t(0));
        QVERIFY(!error.contains("synthetic-key-passphrase"));
        QCOMPARE(error, QString("Choose a valid OpenSSH private-key path."));
        QCOMPARE(publicError(secret("synthetic-key-passphrase"), QString("")), QString("Choose a valid OpenSSH private-key path."));
        QCOMPARE(publicError(secret(QByteArray(4097, 'x')), "/owned/key"), QString("SSH authentication must be at most 4096 UTF-8 bytes."));
    }
};
QTEST_GUILESS_MAIN(SshAuthenticationTest)
#include "ssh_authentication.moc"
