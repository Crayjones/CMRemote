#include "VncCredentialsDialog.h"
#include "VncAuthentication.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <stdexcept>

namespace {
QByteArray bytes(const Secret& secret) { return QByteArray(reinterpret_cast<const char*>(secret.data()), qsizetype(secret.size())); }
}
class VncCredentialsTest : public QObject {
    Q_OBJECT
private slots:
    void passwordPromptMatchesMinimalLayoutAndIsSessionOnly() {
        QWidget owner; int completed = 0; QLineEdit* input = nullptr;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) {
            ++completed; QVERIFY(reply); QVERIFY(input->text().isEmpty()); QVERIFY(reply->password); QVERIFY(reply->username.isEmpty());
            QCOMPARE(bytes(*reply->password), QByteArray("s3ssion!"));
            auto authentication = vncPasswordAuthentication(std::move(*reply->password));
        }, &owner);
        QCOMPARE(dialog->objectName(), QString("vncCredentialsDialog")); QCOMPARE(dialog->windowTitle(), QString("Password"));
        QCOMPARE(dialog->windowModality(), Qt::ApplicationModal); // A titled window, never a macOS sheet.
        input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        QCOMPARE(input->echoMode(), QLineEdit::Password); QVERIFY(input->text().isEmpty());
        // Only a password field and stacked OK/Cancel, like mRemoteNG; no username for VncAuth.
        auto* buttons = dialog->findChild<QDialogButtonBox*>("buttonBox"); QVERIFY(buttons);
        QCOMPARE(buttons->orientation(), Qt::Vertical); QVERIFY(buttons->button(QDialogButtonBox::Ok)); QVERIFY(buttons->button(QDialogButtonBox::Cancel));
        QVERIFY(dialog->findChild<QLineEdit*>("usernameEdit")->isHidden());
        QVERIFY(!dialog->findChild<QComboBox*>()); QVERIFY(!dialog->findChild<QCheckBox*>());
        QVERIFY(dialog->findChild<QLabel*>("errorLabel")->isHidden());
        input->setText("s3ssion!"); dialog->accept(); QCOMPARE(completed, 1);
        QVERIFY(input->text().isEmpty()); input->undo(); QVERIFY(input->text().isEmpty());
        dialog->accept(); dialog->reject(); QCOMPARE(completed, 1);
    }
    void plainPromptAsksForUsername() {
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) {
            ++completed; QVERIFY(reply); QCOMPARE(reply->username, QString("operator"));
            QVERIFY(reply->password); QCOMPARE(bytes(*reply->password), QByteArray("a longer passphrase"));
        }, &owner, {true, "previous", false});
        QCOMPARE(dialog->windowTitle(), QString("Username and password"));
        auto* username = dialog->findChild<QLineEdit*>("usernameEdit"); QVERIFY(username && !username->isHidden());
        QCOMPARE(username->text(), QString("previous"));
        username->clear(); dialog->findChild<QLineEdit*>("passwordEdit")->setText("a longer passphrase");
        dialog->accept(); QCOMPARE(completed, 0); QVERIFY(!dialog->findChild<QLabel*>("errorLabel")->isHidden());
        username->setText("operator"); dialog->findChild<QLineEdit*>("passwordEdit")->setText("a longer passphrase");
        dialog->accept(); QCOMPARE(completed, 1);
    }
    void savedPasswordMayBeLeftEmpty() {
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) {
            ++completed; QVERIFY(reply); QCOMPARE(reply->username, QString("operator")); QVERIFY(!reply->password);
        }, &owner, {true, "operator", true});
        QCOMPARE(dialog->findChild<QLineEdit*>("passwordEdit")->placeholderText(), QString("Saved password"));
        dialog->accept(); QCOMPARE(completed, 1);
    }
    void acceptedPasswords_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("minimum-space") << QString(" ");
        QTest::newRow("eight-printable") << QString("~~~~~~~~");
        QTest::newRow("literal-punctuation") << QString(" <>&\\\"'~");
        // VncAuth uses the first eight bytes, as every VNC client does; Plain uses all of it.
        QTest::newRow("nine-ascii") << QString("123456789");
        QTest::newRow("latin1") << QString(QChar(0xe9));
        QTest::newRow("unicode") << QString::fromUtf8("\xf0\x9f\x94\x90");
        QTest::newRow("maximum") << QString(4096, QChar('x'));
    }
    void acceptedPasswords() {
        QFETCH(QString, password);
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) {
            ++completed; QVERIFY(reply && reply->password);
            QCOMPARE(bytes(*reply->password), password.toUtf8());
            auto authentication = vncPasswordAuthentication(std::move(*reply->password));
        }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        input->setText(password); dialog->accept(); QCOMPARE(completed, 1);
        QVERIFY(input->text().isEmpty()); input->undo(); QVERIFY(input->text().isEmpty());
    }
    void invalidPasswordIsRejectedWithoutTruncation_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("empty") << QString();
        QTest::newRow("long-ascii") << QString(4097, QChar('x'));
        QTest::newRow("tab") << QString("a\tb");
        QTest::newRow("newline") << QString("a\nb");
        QTest::newRow("nul") << (QString("a") + QChar(0) + "b");
        QTest::newRow("below-printable") << QString(QChar(0x1f));
        QTest::newRow("delete") << QString(QChar(0x7f));
    }
    void invalidPasswordIsRejectedWithoutTruncation() {
        QFETCH(QString, password);
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) { ++completed; QVERIFY(!reply); }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        input->setText(password); QCOMPARE(input->text(), password.left(input->maxLength()));
        dialog->accept(); QCOMPARE(completed, 0); QCOMPARE(input->text(), password.left(input->maxLength()));
        auto* error = dialog->findChild<QLabel*>("errorLabel"); QVERIFY(error); QVERIFY(!error->isHidden()); QVERIFY(!error->text().isEmpty());
        QCOMPARE(error->textFormat(), Qt::PlainText);
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty());
        input->undo(); QVERIFY(input->text().isEmpty());
    }
    void cancelClearsInputAndCompletesOnce() {
        QWidget owner; int completed = 0; QLineEdit* input = nullptr;
        auto* dialog = new VncCredentialsDialog([&](std::optional<VncCredentials> reply) {
            ++completed; QVERIFY(!reply); QVERIFY(input->text().isEmpty());
        }, &owner);
        input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input); input->setText("cancel!!");
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty());
        input->undo(); QVERIFY(input->text().isEmpty()); dialog->reject(); QCOMPARE(completed, 1);
    }
    void promptAuthenticationCarriesNoSecret() { auto authentication = vncPromptAuthentication(); QVERIFY(!authentication.password); }
    void opaqueAuthenticationRejectsInvalidBytesWithoutTruncation_data() {
        QTest::addColumn<QByteArray>("password");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("nul") << QByteArray("a\0b", 3);
        QTest::newRow("too-long") << QByteArray(4097, 'x');
    }
    void opaqueAuthenticationRejectsInvalidBytesWithoutTruncation() {
        QFETCH(QByteArray, password);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, vncPasswordAuthentication(Secret(password)));
    }
    void opaqueAuthenticationKeepsLongPasswords() {
        auto authentication = vncPasswordAuthentication(Secret(QByteArray("123456789")));
        QVERIFY(authentication.password); QCOMPARE(bytes(*authentication.password), QByteArray("123456789"));
    }
};
QTEST_MAIN(VncCredentialsTest)
#include "vnc_credentials.moc"
