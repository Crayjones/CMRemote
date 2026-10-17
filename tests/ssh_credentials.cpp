#include "SshCredentialsDialog.h"
#include "SshAuthentication.h"
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTest>

class SshCredentialsTest : public QObject {
    Q_OBJECT
private slots:
    void sshPasswordPromptIsMaskedLiteralAndSessionOnly() {
        QWidget owner; bool completed = false;
        auto* dialog = new SshCredentialsDialog({"<b>owned-endpoint</b>:22", "TEAM\\literal", "password", "", false, false},
            [&](std::optional<TransientSshCredentials> reply) {
                completed = true; QVERIFY(reply); QCOMPARE(reply->username, QString("TEAM\\literal")); QCOMPARE(reply->auth, QString("password"));
                QVERIFY(reply->authentication.size() == size_t(26));
                auto auth = sshPasswordAuthentication(std::move(reply->authentication));
            }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("authenticationEdit"); QVERIFY(input); QCOMPARE(input->echoMode(), QLineEdit::Password);
        QVERIFY(!dialog->findChild<QLineEdit*>("domainEdit")); QVERIFY(!dialog->findChild<QObject*>("rememberPasswordCheck"));
        QVERIFY(!dialog->findChild<QLineEdit*>("usernameEdit")->isEnabled());
        QCOMPARE(dialog->findChild<QLabel*>("endpointLabel")->textFormat(), Qt::PlainText);
        input->setText("synthetic-session-password"); dialog->accept(); QVERIFY(completed); QVERIFY(input->text().isEmpty());
        input->undo(); QVERIFY(input->text().isEmpty());
    }
    void sshPrivateKeyPassphraseNeverUsesSavedPasswordOrReadsKey() {
        QWidget owner; bool completed = false;
        auto* dialog = new SshCredentialsDialog({"owned:22", "literal", "private_key", "/owned/missing-key", false, false},
            [&](std::optional<TransientSshCredentials> reply) {
                completed = true; QVERIFY(reply); QCOMPARE(reply->auth, QString("private_key")); QCOMPARE(reply->keyPath, QString("/owned/missing-key"));
                QVERIFY(reply->authentication.size() == size_t(0)); auto auth = sshPrivateKeyAuthentication(reply->keyPath, std::move(reply->authentication));
            }, &owner);
        QVERIFY(dialog->findChild<QLineEdit*>("authenticationEdit")->text().isEmpty());
        QCOMPARE(dialog->findChild<QLabel*>("authenticationLabel")->text(), QString("Passphrase"));
        QVERIFY(!dialog->findChild<QComboBox*>("authCombo")->isEnabled());
        dialog->accept(); QVERIFY(completed);
    }
    void sshPrivateKeyPassphraseTransfersAfterClearingQtInput() {
        QWidget owner; bool completed = false; QLineEdit* input = nullptr;
        auto* dialog = new SshCredentialsDialog({"owned:22", "literal", "private_key", "/owned/missing-key", false, false},
            [&](std::optional<TransientSshCredentials> reply) {
                completed = true; QVERIFY(reply); QVERIFY(input->text().isEmpty());
                const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(reply->authentication.data()), qsizetype(reply->authentication.size()));
                QVERIFY(bytes == QByteArray("synthetic-key-passphrase"));
                auto auth = sshPrivateKeyAuthentication(reply->keyPath, std::move(reply->authentication));
            }, &owner);
        input = dialog->findChild<QLineEdit*>("authenticationEdit"); input->setText("synthetic-key-passphrase");
        dialog->accept(); QVERIFY(completed); input->undo(); QVERIFY(input->text().isEmpty());
    }
    void sshCredentialCancelClearsInputAndCompletesOnce() {
        QWidget owner; int completed = 0;
        auto* dialog = new SshCredentialsDialog({"owned:22", "literal", "password", "", true, true},
            [&](std::optional<TransientSshCredentials> reply) { ++completed; QVERIFY(!reply); }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("authenticationEdit"); input->setText("synthetic-cancel-secret");
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty()); input->undo(); QVERIFY(input->text().isEmpty());
        dialog->reject(); QCOMPARE(completed, 1);
    }
    void sshTemporaryAuthChoiceClearsSecretAndChecksUtf8Bounds() {
        QWidget owner; int completed = 0;
        auto* dialog = new SshCredentialsDialog({"owned:22", "literal", "password", "", true, true},
            [&](std::optional<TransientSshCredentials> reply) { ++completed; QVERIFY(!reply); }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("authenticationEdit"); input->setText("synthetic-session-password");
        auto* method = dialog->findChild<QComboBox*>("authCombo"); method->setCurrentIndex(method->findData("private_key"));
        QVERIFY(input->text().isEmpty()); QCOMPARE(dialog->findChild<QLabel*>("authenticationLabel")->text(), QString("Passphrase"));
        dialog->accept(); QCOMPARE(completed, 0); QVERIFY(!dialog->findChild<QLabel*>("errorLabel")->text().isEmpty());
        dialog->findChild<QLineEdit*>("keyPathEdit")->setText("/owned/missing-key"); input->setText(QString(1366, QChar(0x1f80)));
        dialog->accept(); QCOMPARE(completed, 0);
        QCOMPARE(dialog->findChild<QLabel*>("errorLabel")->text(), QString("SSH authentication must be at most 4096 UTF-8 bytes."));
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty());
    }
};
QTEST_MAIN(SshCredentialsTest)
#include "ssh_credentials.moc"
