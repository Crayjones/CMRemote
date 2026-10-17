#include "VncCredentialsDialog.h"
#include "VncAuthentication.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QTest>
#include <stdexcept>

class VncCredentialsTest : public QObject {
    Q_OBJECT
private slots:
    void passwordPromptMatchesMinimalLayoutAndIsSessionOnly() {
        QWidget owner; int completed = 0; QLineEdit* input = nullptr;
        auto* dialog = new VncCredentialsDialog([&](std::optional<Secret> reply) {
            ++completed; QVERIFY(reply); QVERIFY(input->text().isEmpty()); QCOMPARE(reply->size(), size_t(8));
            const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(reply->data()), qsizetype(reply->size()));
            QCOMPARE(bytes, QByteArray("s3ssion!"));
            auto authentication = vncPasswordAuthentication(std::move(*reply));
        }, &owner);
        QCOMPARE(dialog->objectName(), QString("vncCredentialsDialog")); QCOMPARE(dialog->windowTitle(), QString("Password"));
        QCOMPARE(dialog->windowModality(), Qt::ApplicationModal); // A titled window, never a macOS sheet.
        input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        QCOMPARE(input->echoMode(), QLineEdit::Password); QVERIFY(input->text().isEmpty());
        // Only a password field and stacked OK/Cancel, like mRemoteNG.
        auto* buttons = dialog->findChild<QDialogButtonBox*>("buttonBox"); QVERIFY(buttons);
        QCOMPARE(buttons->orientation(), Qt::Vertical); QVERIFY(buttons->button(QDialogButtonBox::Ok)); QVERIFY(buttons->button(QDialogButtonBox::Cancel));
        QCOMPARE(dialog->findChildren<QLineEdit*>().size(), 1); QVERIFY(!dialog->findChild<QComboBox*>()); QVERIFY(!dialog->findChild<QCheckBox*>());
        QVERIFY(dialog->findChild<QLabel*>("errorLabel")->isHidden());
        input->setText("s3ssion!"); dialog->accept(); QCOMPARE(completed, 1);
        QVERIFY(input->text().isEmpty()); input->undo(); QVERIFY(input->text().isEmpty());
        dialog->accept(); dialog->reject(); QCOMPARE(completed, 1);
    }
    void printableAsciiBoundaryPasswords_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("minimum-space") << QString(" ");
        QTest::newRow("maximum-printable") << QString("~~~~~~~~");
        QTest::newRow("literal-punctuation") << QString(" <>&\\\"'~");
    }
    void printableAsciiBoundaryPasswords() {
        QFETCH(QString, password);
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<Secret> reply) {
            ++completed; QVERIFY(reply);
            const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(reply->data()), qsizetype(reply->size()));
            QCOMPARE(bytes, password.toLatin1());
            auto authentication = vncPasswordAuthentication(std::move(*reply));
        }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        input->setText(password); dialog->accept(); QCOMPARE(completed, 1);
        QVERIFY(input->text().isEmpty()); input->undo(); QVERIFY(input->text().isEmpty());
    }
    void invalidPasswordIsRejectedWithoutTruncation_data() {
        QTest::addColumn<QString>("password");
        QTest::newRow("empty") << QString();
        QTest::newRow("nine-ascii") << QString("123456789");
        QTest::newRow("long-ascii") << QString(4097, QChar('x'));
        QTest::newRow("tab") << QString("a\tb");
        QTest::newRow("newline") << QString("a\nb");
        QTest::newRow("nul") << (QString("a") + QChar(0) + "b");
        QTest::newRow("below-printable") << QString(QChar(0x1f));
        QTest::newRow("delete") << QString(QChar(0x7f));
        QTest::newRow("latin1") << QString(QChar(0xe9));
        QTest::newRow("unicode") << QString::fromUtf8("\xf0\x9f\x94\x90");
    }
    void invalidPasswordIsRejectedWithoutTruncation() {
        QFETCH(QString, password);
        QWidget owner; int completed = 0;
        auto* dialog = new VncCredentialsDialog([&](std::optional<Secret> reply) { ++completed; QVERIFY(!reply); }, &owner);
        auto* input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input);
        input->setText(password); QCOMPARE(input->text(), password);
        dialog->accept(); QCOMPARE(completed, 0); QCOMPARE(input->text(), password);
        auto* error = dialog->findChild<QLabel*>("errorLabel"); QVERIFY(error); QVERIFY(!error->isHidden()); QVERIFY(!error->text().isEmpty());
        QCOMPARE(error->textFormat(), Qt::PlainText);
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty());
        input->undo(); QVERIFY(input->text().isEmpty());
    }
    void cancelClearsInputAndCompletesOnce() {
        QWidget owner; int completed = 0; QLineEdit* input = nullptr;
        auto* dialog = new VncCredentialsDialog([&](std::optional<Secret> reply) {
            ++completed; QVERIFY(!reply); QVERIFY(input->text().isEmpty());
        }, &owner);
        input = dialog->findChild<QLineEdit*>("passwordEdit"); QVERIFY(input); input->setText("cancel!!");
        dialog->reject(); QCOMPARE(completed, 1); QVERIFY(input->text().isEmpty());
        input->undo(); QVERIFY(input->text().isEmpty()); dialog->reject(); QCOMPARE(completed, 1);
    }
    void promptAuthenticationCarriesNoSecret() { auto authentication = vncPromptAuthentication(); Q_UNUSED(authentication); }
    void opaqueAuthenticationRejectsInvalidBytesWithoutTruncation_data() {
        QTest::addColumn<QByteArray>("password");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("nine-bytes") << QByteArray("123456789");
        QTest::newRow("nul") << QByteArray("a\0b", 3);
        QTest::newRow("control") << QByteArray(1, char(0x1f));
        QTest::newRow("delete") << QByteArray(1, char(0x7f));
        QTest::newRow("high-byte") << QByteArray(1, char(0xff));
    }
    void opaqueAuthenticationRejectsInvalidBytesWithoutTruncation() {
        QFETCH(QByteArray, password);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, vncPasswordAuthentication(Secret(password)));
    }
};
QTEST_MAIN(VncCredentialsTest)
#include "vnc_credentials.moc"
