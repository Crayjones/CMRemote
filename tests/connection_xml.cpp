#include "ConnectionCrypto.h"
#include "ConnectionXml.h"
#include <QTest>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include "remotemanager-core/src/bridge.rs.h"

// Internal deterministic seams, deliberately absent from the production header.
// Production encrypt() always obtains fresh checked salt/nonce bytes.
namespace ConnectionCrypto::detail {
std::optional<QByteArray> encryptWithEntropy(const Secret&, quint32, const Secret&, const QByteArray&, const QByteArray&, qsizetype);
std::optional<Secret> deriveKey(const Secret&, const QByteArray&, quint32);
}

namespace {
QByteArray bytes(const Secret& value) { return QByteArray::fromRawData(reinterpret_cast<const char*>(value.data()), qsizetype(value.size())); }
struct Vector { const char* master; quint32 iterations; const char* plain; const char* cipher; };
// Incoming Protected canaries from the original mRemoteNG test resources:
// confCons_v2_6.xml, *_5k-iterations.xml, *_fullencryption.xml,
// *_passwordis_Password.xml and *_passwordis_Password_fullencryption.xml.
// These vectors test the original format without executing the .NET application.
const Vector Original[] = {
    {"mR3m", 1000, "ThisIsNotProtected", "8LmIO3+MWBY0zTmfjfOEdCGxhTAwnlohb1veTGNZFt6lAYvY2UOzWyjVzkx6V93smpbP0ZOuexN15u7rvwJEjawC"},
    {"mR3m", 5000, "ThisIsNotProtected", "Z1IOT8h7neJ5V7es5Iv63A2WsDG6QWl10F/Rb9ljKxvCseEITty1BfMNgiaVPfm7w61uabQKqu2waDCXUpLo1OZW"},
    {"mR3m", 1000, "ThisIsNotProtected", "qrOUhRlBLCnjjmfU1r8uwhgIBwZ+D1+trBgPmcQyEnW0xf2adLeOfUhZHnto2NOsMneXDibsS8PH0B1moOyvua22"},
    {"Password", 1000, "ThisIsProtected", "e/T6ajrPtNNlHreSeD4QBqToTuiqtNACKiPJv7vU+l6TWCu9JNsmL+Y8lJ4aTl5YVcstXpQjxsZ9i8+YV4Gs"},
    {"Password", 1000, "ThisIsProtected", "d7pvbk1N6aswOmTgjCKkkavQKM2xTpknkdnMgzi+bOPgwr9WirGy82kaQldQ06af7MrZgJwvc9hKAcGxBfYt"}
};
const QByteArray Id("e6683f1f-4e06-4200-9a17-89cd30c68b12");
QByteArray attribute(const QByteArray& xml, const QString& name, const QString& id = {}) {
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext(); if (!reader.isStartElement()) continue;
        if ((id.isEmpty() && reader.name() == "Connections") || (reader.name() == "Node" && reader.attributes().value("Id") == id))
            return reader.attributes().value(name).toLatin1();
    }
    return {};
}
QByteArray node() { return "<Node xmlns=\"\" Type=\"Connection\" Id=\"" + Id + "\" Name=\"fixture\" Protocol=\"RDP\" Hostname=\"localhost\" Port=\"3389\" Password=\"\"/>"; }
QByteArray xml(const QByteArray& body, const QByteArray& version = "2.8", const QByteArray& extra = {}, bool full = false) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Connections Name=\"Connections\" Export=\"false\" ConfVersion=\"" + version +
        "\" EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"" + (full ? "true" : "false") +
        "\" Protected=\"" + Original[3].cipher + "\" " + extra + ">" + body + "</Connections>";
}
}

class ConnectionXmlTest : public QObject {
    Q_OBJECT
private slots:
    void nativeIconInheritanceRoundTrip() {
        const Secret master(QByteArray("Password"));
        const QString folder = "11111111-1111-4111-8111-111111111111", child = QString::fromLatin1(Id);
        const auto encrypted = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic icon-test password"))); QVERIFY(encrypted);
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder.toLatin1() + "\" Name=\"Folder\" Icon=\"SSH\"><Node Type=\"Connection\" Id=\"" + Id +
            "\" Name=\"Child\" Icon=\"Unavailable imported icon\" InheritIcon=\"true\" Hostname=\"localhost\" Password=\"" + *encrypted + "\" UnsupportedOption=\"keep\"/></Node>";
        auto document = ConnectionXml::read(xml(body), &master); QVERIFY(document);
        const auto initial = document->serialize(master); QVERIFY(initial);
        const auto projection = document->projection(); QVERIFY(projection);
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        auto snapshot = core->snapshot(); auto profiles = QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray();
        QCOMPARE(profiles[1].toObject().value("effective").toObject().value("icon").toString(), QString("SSH"));
        QVERIFY(document->patch(master, *projection)); QCOMPARE(document->serialize(master), initial);
        auto request = QJsonDocument::fromJson(*projection).object(); auto nodes = request.value("profiles").toArray(); auto edited = nodes[1].toObject(); auto overrides = edited.value("overrides").toObject();
        overrides["icon"] = "../../unknown-local-name"; edited["overrides"] = overrides; nodes[1] = edited; request["profiles"] = nodes;
        QVERIFY(document->patch(master, QJsonDocument(request).toJson(QJsonDocument::Compact))); const auto changed = document->serialize(master); QVERIFY(changed);
        QCOMPARE(attribute(*changed, "Icon", child), QByteArray("../../unknown-local-name")); QCOMPARE(attribute(*changed, "InheritIcon", child), QByteArray("false"));
        QCOMPARE(attribute(*changed, "Password", child), *encrypted); QCOMPARE(attribute(*changed, "UnsupportedOption", child), QByteArray("keep"));
        auto restart = ConnectionXml::read(*changed, &master); QVERIFY(restart); QCOMPARE(restart->projection(), document->projection());
        overrides["icon"] = QJsonValue(QJsonValue::Null); edited["overrides"] = overrides; nodes[1] = edited; request["profiles"] = nodes;
        QVERIFY(restart->patch(master, QJsonDocument(request).toJson(QJsonDocument::Compact)));
        QCOMPARE(attribute(*restart->serialize(master), "InheritIcon", child), QByteArray("true"));
        auto absent = ConnectionXml::read(xml(node()), &master); QVERIFY(absent); const auto without = absent->serialize(master); QVERIFY(without);
        QVERIFY(absent->patch(master, *absent->projection())); QCOMPARE(absent->serialize(master), without); QVERIFY(!without->contains("InheritIcon"));
    }
    void siblingOrderAndFolderPasswordRoundTrip() {
        auto xml = ConnectionXml::create(); QVERIFY(xml); const Secret master(QByteArray("mR3m"));
        const QString folder = "11111111-1111-4111-8111-111111111111", a = "22222222-2222-4222-8222-222222222222", b = "33333333-3333-4333-8333-333333333333";
        auto node = [](const QString& id, const char* kind, const QString& name, const QJsonValue& parent) {
            return QJsonObject{{"id", id}, {"kind", kind}, {"name", name}, {"parent", parent}, {"overrides", QJsonObject{}}};
        };
        auto ids = [&](const QByteArray& json) { QStringList result; for (const auto& value : QJsonDocument::fromJson(json).object().value("profiles").toArray()) result << value.toObject().value("id").toString(); return result; };
        auto document = [](const QJsonArray& profiles) { return QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", profiles}}).toJson(QJsonDocument::Compact); };
        auto first = xml->patch(master, document({node(folder, "folder", "Folder", QJsonValue()), node(a, "connection", "A", folder), node(b, "connection", "B", folder)}));
        QVERIFY(first); QCOMPARE(ids(*first), QStringList({folder, a, b}));
        // Siblings follow the requested order, not the previous file order.
        const auto committed = QJsonDocument::fromJson(*first).object().value("profiles").toArray();
        auto swapped = xml->patch(master, document({committed.at(0), committed.at(2), committed.at(1)}));
        QVERIFY(swapped); QCOMPARE(ids(*swapped), QStringList({folder, b, a}));
        auto reread = ConnectionXml::read(*xml->serialize(master), &master); QVERIFY(reread); QCOMPARE(ids(*reread->projection()), QStringList({folder, b, a}));
        QVERIFY(!xml->folderPassword(master, folder));
        QVERIFY(xml->setPassword(master, folder, Secret(QByteArray("synthetic folder password"))));
        const auto password = xml->folderPassword(master, folder); QVERIFY(password); QCOMPARE(QByteArray(reinterpret_cast<const char*>(password->data()), qsizetype(password->size())), QByteArray("synthetic folder password"));
        QVERIFY(!xml->folderPassword(master, a));
    }
    void defaultKeyPasswordsAndFullXmlRoundTrip() {
        const Secret master(QByteArray("mR3m")), secret(QByteArray("synthetic default credential"));
        auto document = ConnectionXml::read(xml(node()).replace(Original[3].cipher, Original[0].cipher), &master); QVERIFY(document);
        QVERIFY(document->setPassword(master, QString::fromLatin1(Id), secret));
        const auto target = document->committedAccount(QString::fromLatin1(Id)); QVERIFY(target);
        QVERIFY(document->authorizePasswords(master, QString::fromLatin1(Id), {{QString::fromLatin1(Id), *target}}));
        for (bool full : {false, true}) {
            QVERIFY(document->setFullEncryption(master, full));
            const auto output = document->serialize(master); QVERIFY(output); QVERIFY(!output->contains(bytes(secret)));
            auto reopened = ConnectionXml::read(*output, &master); QVERIFY(reopened);
            QCOMPARE(reopened->fullEncryption(), full);
            const auto saved = reopened->password(master, QString::fromLatin1(Id), *target);
            QVERIFY(saved && bytes(*saved) == bytes(secret));
        }
    }
    void originalXmlFixturesDecrypt() {
        for (const auto& v : Original) {
            const auto decoded = ConnectionCrypto::decrypt(Secret(QByteArray(v.master)), v.iterations, v.cipher);
            QVERIFY(decoded); QVERIFY(bytes(*decoded) == v.plain);
        }
        // Optional read-only acceptance against the separately supplied original
        // checkout. The committed vector contract above never depends on it.
        const auto fixtures = qEnvironmentVariable("RM_ORIGINAL_XML_FIXTURES"); if (fixtures.isEmpty()) return;
        for (const auto& name : {"confCons_v2_6.xml", "confCons_v2_6_5k-iterations.xml", "confCons_v2_6_fullencryption.xml",
                                "confCons_v2_6_passwordis_Password.xml", "confCons_v2_6_passwordis_Password_fullencryption.xml"}) {
            QFile file(QDir(fixtures).filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.size() <= ConnectionXml::MaxFile);
            const auto original = file.read(ConnectionXml::MaxFile + 1); QVERIFY(file.error() == QFileDevice::NoError);
            const Secret master(QByteArray(name).contains("passwordis_Password") ? QByteArray("Password") : QByteArray("mR3m"));
            auto parsed = ConnectionXml::read(original, &master); QVERIFY(parsed); QVERIFY(!parsed->needsUnlock());
            QCOMPARE(parsed->profileIds().size(), 18); QVERIFY(parsed->requiresIdRepair());
            const auto projection = parsed->projection(); QVERIFY(projection);
            const auto profiles = QJsonDocument::fromJson(*projection).object().value("profiles").toArray();
            for (const auto& value : profiles) {
                const auto profile = value.toObject(); const auto overrides = profile.value("overrides").toObject();
                for (const auto& key : {"Password", "VNCProxyPassword", "RDGatewayPassword", "RDGatewayAccessToken", "password"}) {
                    QVERIFY(!profile.contains(key)); QVERIFY(!overrides.contains(key));
                }
            }
            auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
            const auto snapshot = core->snapshot(); QCOMPARE(QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray().size(), 18);
        }
    }
    void xmlWriterMatchesOriginalCipherVectors() {
        for (const auto& v : Original) {
            const auto raw = QByteArray::fromBase64(v.cipher);
            const auto encoded = ConnectionCrypto::detail::encryptWithEntropy(Secret(QByteArray(v.master)), v.iterations,
                Secret(QByteArray(v.plain)), raw.left(16), raw.mid(16, 16), ConnectionCrypto::MaxPayload);
            QVERIFY(encoded); QCOMPARE(*encoded, QByteArray(v.cipher));
        }
        const auto pass = Secret(QByteArray("synthetic XML master"));
        const auto plain = Secret(QByteArray("synthetic secret"));
        const auto first = ConnectionCrypto::encrypt(pass, 1000, plain), second = ConnectionCrypto::encrypt(pass, 1000, plain);
        QVERIFY(first && second && *first != *second);
        const auto a = QByteArray::fromBase64(*first), b = QByteArray::fromBase64(*second);
        QVERIFY(a.left(16) != b.left(16)); QVERIFY(a.mid(16, 16) != b.mid(16, 16));
        const auto decoded = ConnectionCrypto::decrypt(pass, 1000, *first); QVERIFY(decoded); QVERIFY(bytes(*decoded) == bytes(plain));
        QVERIFY(!ConnectionCrypto::detail::encryptWithEntropy(pass, 1000, plain, QByteArray(15, 'a'), QByteArray(16, 'b'), 4096));
        QVERIFY(!ConnectionCrypto::detail::encryptWithEntropy(pass, 1000, plain, QByteArray(16, 'a'), QByteArray(12, 'b'), 4096));
        const Secret defaultMaster(QByteArray("mR3m"));
        const auto defaultCipher = ConnectionCrypto::encrypt(defaultMaster, 1000, plain); QVERIFY(defaultCipher);
        const auto defaultPlain = ConnectionCrypto::decrypt(defaultMaster, 1000, *defaultCipher); QVERIFY(defaultPlain);
        QVERIFY(bytes(*defaultPlain) == bytes(plain));
    }
    void masterEncodingAndKdfBounds() {
        const Secret latin1(QString::fromUtf8("päss£").toUtf8());
        const auto encoded = ConnectionCrypto::encodeMaster(latin1); QVERIFY(encoded);
        QVERIFY(bytes(*encoded) == QByteArray::fromHex("70e47373a3"));
        const auto key = ConnectionCrypto::detail::deriveKey(latin1, QByteArray::fromHex("000102030405060708090a0b0c0d0e0f"), 1000);
        QVERIFY(key); QVERIFY(bytes(*key) == QByteArray::fromHex("2a2686e66b1592b491255e3a514b2ee2d567de638d41f237692cc073db17fd8e"));
        QVERIFY(!ConnectionCrypto::encodeMaster(Secret(QString::fromUtf8("Ā").toUtf8())));
        QVERIFY(!ConnectionCrypto::encodeMaster(Secret(QString::fromUtf8("\U0001f512").toUtf8())));
        QVERIFY(!ConnectionCrypto::encodeMaster(Secret(QByteArray::fromHex("c080"))));
        QVERIFY(!ConnectionCrypto::encodeMaster(Secret(QByteArray(4097, 'a'))));
        QVERIFY(!ConnectionCrypto::encodeMaster(Secret()));
        const Secret pass(QByteArray("synthetic XML master"));
        QVERIFY(ConnectionCrypto::validNewMaster(pass));
        QVERIFY(ConnectionCrypto::validNewMaster(Secret(QByteArray(4096, '~'))));
        QVERIFY(ConnectionCrypto::validNewMaster(Secret(QByteArray("short"))));
        QVERIFY(ConnectionCrypto::validNewMaster(latin1));
        QVERIFY(ConnectionCrypto::validNewMaster(Secret(QString(4096, QChar(0xe4)).toUtf8())));
        for (const auto& invalid : {QByteArray(), QByteArray("mR3m"), QByteArray(4097, 'a'), QByteArray(" \t\n"),
                                   QString(4097, QChar(0xe4)).toUtf8(), QString::fromUtf8("Ā").toUtf8(), QByteArray("a\0b", 3)})
            QVERIFY(!ConnectionCrypto::validNewMaster(Secret(invalid)));
        QVERIFY(ConnectionCrypto::validIterations(1000)); QVERIFY(ConnectionCrypto::validIterations(1000000));
        for (quint32 invalid : {0U, 999U, 1000001U, 0xffffffffU}) {
            QVERIFY(!ConnectionCrypto::validIterations(invalid));
            QVERIFY(!ConnectionCrypto::encrypt(pass, invalid, Secret(QByteArray("x"))));
            QVERIFY(!ConnectionCrypto::decrypt(pass, invalid, Original[3].cipher));
        }
        const auto empty = ConnectionCrypto::encrypt(pass, 1000, Secret()); QVERIFY(empty && empty->isEmpty());
        const auto opened = ConnectionCrypto::decrypt(pass, 1000, {}); QVERIFY(opened && opened->size() == 0);
        QVERIFY(!ConnectionCrypto::encrypt(pass, 1000, Secret(QByteArray("12345")), 4));
    }
    void xmlNamespaceAndVersionRoundTrip() {
        const Secret master(QByteArray("Password"));
        for (const QByteArray version : {QByteArray("2.6"), QByteArray("2.7"), QByteArray("2.8")}) {
            for (const QByteArray ns : {QByteArray(), QByteArray("xmlns=\"http://mremoteng.org\"")}) {
                const auto original = xml(node(), version, ns);
                auto parsed = ConnectionXml::read(original, &master); QVERIFY(parsed);
                QCOMPARE(parsed->version(), QString::fromLatin1(version)); QCOMPARE(parsed->iterations(), quint32(1000));
                QCOMPARE(parsed->profileIds(), QStringList{QString::fromLatin1(Id)});
                const auto saved = parsed->serialize(master); QVERIFY(saved);
                auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened);
                QCOMPARE(reopened->version(), parsed->version()); QCOMPARE(reopened->profileIds(), parsed->profileIds());
                QXmlStreamReader reader(*saved); QVERIFY(reader.readNextStartElement());
                QCOMPARE(reader.namespaceUri().toString(), ns.isEmpty() ? QString() : QString("http://mremoteng.org"));
                QVERIFY(reader.readNextStartElement()); QVERIFY(reader.namespaceUri().isEmpty());
            }
        }
    }
    void passwordOnlyAndFullInnerXmlPortable() {
        const Secret master(QByteArray("Password"));
        const auto secret = ConnectionCrypto::encrypt(master, 1000, Secret(QString::fromUtf8("synthetic pässword").toUtf8()), 4096); QVERIFY(secret);
        const auto inner = QByteArray("<Node xmlns=\"\" Id=\"") + Id + "\" Type=\"Connection\" Name=\"fixture\" Password=\"" + *secret +
            "\" VNCProxyPassword=\"" + *secret + "\" RDGatewayPassword=\"" + *secret + "\" RDGatewayAccessToken=\"" + *secret + "\"/>";
        auto readable = ConnectionXml::read(xml(inner)); QVERIFY(readable); QVERIFY(!readable->needsUnlock());
        QVERIFY(!readable->serialize(master)); QVERIFY(readable->unlock(master));
        const auto saved = readable->serialize(master); QVERIFY(saved); QCOMPARE(saved->count(*secret), 4);
        QVERIFY(!readable->serialize(master, [] { return true; }));
        QVERIFY(!saved->contains("synthetic"));
        const auto encryptedInner = ConnectionCrypto::encrypt(master, 1000, Secret(inner)); QVERIFY(encryptedInner);
        auto full = ConnectionXml::read(xml(*encryptedInner, "2.8", {}, true)); QVERIFY(full);
        QVERIFY(full->needsUnlock()); QVERIFY(full->profileIds().isEmpty());
        QVERIFY(!full->unlock(master, [] { return true; })); QVERIFY(full->profileIds().isEmpty());
        QVERIFY(!full->unlock(Secret(QByteArray("wrong")))); QVERIFY(full->profileIds().isEmpty());
        QVERIFY(full->unlock(master)); QVERIFY(!full->needsUnlock()); QCOMPARE(full->profileIds(), readable->profileIds());
        const auto protectedSaved = full->serialize(master); QVERIFY(protectedSaved);
        QVERIFY(!protectedSaved->contains(Id)); QVERIFY(!protectedSaved->contains("<Node"));
        auto reopened = ConnectionXml::read(*protectedSaved, &master); QVERIFY(reopened); QCOMPARE(reopened->profileIds(), full->profileIds());
        auto damaged = QByteArray::fromBase64(*encryptedInner); damaged.back() = char(damaged.back() ^ 1);
        QVERIFY(!ConnectionXml::read(xml(damaged.toBase64(), "2.8", {}, true), &master));
        auto brokenSecret = QByteArray::fromBase64(*secret); brokenSecret.back() = char(brokenSecret.back() ^ 1);
        QVERIFY(!ConnectionXml::read(xml(QByteArray(inner).replace(*secret, brokenSecret.toBase64())), &master));
    }
    void unknownAttributesAndOpaqueNodesSurviveEdit() {
        const Secret master(QByteArray("Password"));
        const auto body = QByteArray("<!--before--><Node xmlns=\"\" Id=\"") + Id +
            "\" Type=\"Connection\" Name=\"old\" Protocol=\"ICA\" UnknownOption=\"keep\" q:extra=\"opaque\"><PluginData>left<q:Item q:value=\"x\"><![CDATA[α<&]]></q:Item>right</PluginData><?passive keep?></Node><!--after-->";
        const auto source = xml(body, "2.8", "xmlns=\"http://mremoteng.org\" xmlns:q=\"urn:fixture\" q:root=\"kept\"");
        auto parsed = ConnectionXml::read(source, &master); QVERIFY(parsed);
        QVERIFY(parsed->rename(QString::fromLatin1(Id), "changed"));
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        for (const auto marker : {"Protocol=\"ICA\"", "UnknownOption=\"keep\"", "q:extra=\"opaque\"", "q:root=\"kept\"", "<?passive keep?>"}) QVERIFY(saved->contains(marker));
        QVERIFY(saved->indexOf("<!--before-->") < saved->indexOf("<Node")); QVERIFY(saved->indexOf("<!--after-->") > saved->indexOf("</Node>"));
        QVERIFY(saved->contains("Name=\"changed\"")); QVERIFY(!saved->contains("Name=\"old\""));
        QXmlStreamReader reader(*saved); QString plugin;
        while (!reader.atEnd()) { reader.readNext(); if (reader.isStartElement() && reader.name() == "PluginData") plugin = reader.readElementText(QXmlStreamReader::IncludeChildElements); }
        QVERIFY(!reader.hasError()); QCOMPARE(plugin, QString::fromUtf8("leftα<&right"));
    }
    void missingXmlIdsRequireExplicitStableRepair() {
        const Secret master(QByteArray("Password"));
        const auto original = xml("<Node Type=\"Container\" Name=\"folder\"><Node Type=\"Connection\" Name=\"child\"/></Node>");
        auto parsed = ConnectionXml::read(original, &master); QVERIFY(parsed); QVERIFY(parsed->requiresIdRepair());
        const auto ids = parsed->profileIds(); QCOMPARE(ids.size(), 2); QCOMPARE(parsed->profileIds(), ids);
        QVERIFY(!parsed->rename(ids.first(), "not allowed")); QVERIFY(!parsed->serialize(master));
        QVERIFY(parsed->authorizeIdRepair()); QVERIFY(parsed->rename(ids.first(), "allowed"));
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        auto repaired = ConnectionXml::read(*saved, &master); QVERIFY(repaired); QVERIFY(!repaired->requiresIdRepair());
        QCOMPARE(repaired->profileIds(), ids); QVERIFY(!original.contains("Id="));
    }
    void nativeInheritanceAndAppOnlyPreferencesRoundTrip() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const QByteArray safe(" UseCredSsp=\"true\" RDPAuthenticationLevel=\"WarnOnFailedAuth\" RedirectSound=\"DoNotPlay\"");
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder +
            "\" Name=\"folder\" Protocol=\"RDP\" Hostname=\"folder.test\" Port=\"3391\" Username=\"folder-user\" Domain=\"TEAM\" Descr=\"folder description\" "
            "ConnectToConsole=\"true\" Colors=\"Colors24Bit\" Resolution=\"FitToWindow\" RmResolution=\"1280x720\" InheritUsername=\"true\"" + safe +
            "><Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"child\" Hostname=\"folder.test\" Port=\"3389\" Username=\"unused\" Domain=\"unused\" "
            "InheritProtocol=\"true\" InheritPort=\"true\" InheritUsername=\"true\" InheritDomain=\"true\" InheritDescription=\"true\" "
            "InheritUseConsoleSession=\"true\" InheritColors=\"true\" InheritResolution=\"true\" RmHostInherited=\"true\" RmResolutionInherited=\"true\"" + safe + "/></Node>";
        auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed); const auto projection = parsed->projection(); QVERIFY(projection);
        const auto document = QJsonDocument::fromJson(*projection).object(); QCOMPARE(document.value("schema_version").toInt(), 2);
        const auto profiles = document.value("profiles").toArray(); QCOMPARE(profiles.size(), 2);
        const auto parent = profiles[0].toObject(), child = profiles[1].toObject(), overrides = child.value("overrides").toObject();
        QCOMPARE(child.value("parent").toString(), QString::fromLatin1(folder));
        for (const auto& field : {"host", "port", "protocol", "username", "domain", "description", "console_session", "color_depth", "resolution"}) QVERIFY(overrides.value(field).isNull());
        QCOMPARE(parent.value("overrides").toObject().value("username").toString(), QString("folder-user")); // root inheritance inactive
        for (const auto& field : {"automatic_resize", "cache_bitmaps", "wallpaper", "themes", "font_smoothing"}) QCOMPARE(overrides.value(field).toBool(true), false);
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        const auto snapshot = core->snapshot(); const auto effective = QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray()[1].toObject().value("effective").toObject();
        QCOMPARE(effective.value("host").toString(), QString("folder.test")); QCOMPARE(effective.value("port").toInt(), 3391);
        QCOMPARE(effective.value("username").toString(), QString("folder-user")); QCOMPARE(effective.value("domain").toString(), QString("TEAM"));
        QCOMPARE(effective.value("description").toString(), QString("folder description")); QVERIFY(effective.value("console_session").toBool());
        QCOMPARE(effective.value("resolution").toString(), QString("1280x720")); QCOMPARE(effective.value("color_depth").toInt(), 24);
        QVERIFY(effective.value("unsupported").toArray().isEmpty()); const auto session = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true); core->close_session(session.id);
        const auto saved = parsed->serialize(master); QVERIFY(saved); auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), projection);
        auto native = body; native.replace(" RmHostInherited=\"true\"", ""); native.replace(" RmResolutionInherited=\"true\"", ""); native.replace(" RmResolution=\"1280x720\"", "");
        auto flattened = ConnectionXml::read(xml(native), &master); QVERIFY(flattened); const auto flat = flattened->projection(); QVERIFY(flat);
        const auto flatChild = QJsonDocument::fromJson(*flat).object().value("profiles").toArray()[1].toObject().value("overrides").toObject();
        QCOMPARE(flatChild.value("host").toString(), QString("folder.test")); QVERIFY(!flatChild.value("host").isNull());
        const auto flatParent = QJsonDocument::fromJson(*flat).object().value("profiles").toArray()[0].toObject().value("overrides").toObject();
        QCOMPARE(flatParent.value("resolution").toString(), QString("fit")); // lost app preference adopts native sizing
        QVERIFY(!parsed->projection([] { return true; }));
    }
    void projectionPatchCreatesCompatibleDefaultsAndPreservesNoOpAttributes() {
        const Secret defaultMaster(QByteArray("mR3m")); auto parsed = ConnectionXml::create(); QVERIFY(parsed);
        const QJsonObject profile{{"id", QString::fromLatin1(Id)}, {"name", "new"}, {"kind", "connection"}, {"parent", QJsonValue(QJsonValue::Null)},
            {"overrides", QJsonObject{{"protocol", "rdp"}, {"host", "localhost"}, {"port", 3389}, {"username", ""}}}};
        const auto request = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
        const auto normalized = parsed->patch(defaultMaster, request); QVERIFY(normalized); QVERIFY(!normalized->contains("Password"));
        auto omittedParent = profile; omittedParent.remove("parent"); auto sparse = ConnectionXml::create(); QVERIFY(sparse);
        const auto sparseRequest = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{omittedParent}}}).toJson(QJsonDocument::Compact);
        QCOMPARE(sparse->patch(defaultMaster, sparseRequest), normalized); // missing optional parent means document root
        const auto saved = parsed->serialize(defaultMaster); QVERIFY(saved);
        QCOMPARE(attribute(*saved, "Protocol", QString::fromLatin1(Id)), QByteArray("RDP"));
        QCOMPARE(attribute(*saved, "Resolution", QString::fromLatin1(Id)), QByteArray("FitToWindow"));
        QCOMPARE(attribute(*saved, "RmResolution", QString::fromLatin1(Id)), QByteArray("1024x768"));
        QCOMPARE(attribute(*saved, "Colors", QString::fromLatin1(Id)), QByteArray("Colors32Bit"));
        QVERIFY(!saved->contains("RmFormatVersion")); QVERIFY(!saved->contains("RmCredentialBindings"));
        auto reopened = ConnectionXml::read(*saved, &defaultMaster); QVERIFY(reopened); QCOMPARE(reopened->projection(), normalized);
        auto core = rm::detached_core(); core->load_document(rust::Str(normalized->constData(), size_t(normalized->size())));
        const auto snapshot = core->snapshot(); const auto effective = QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray()[0].toObject().value("effective").toObject();
        QVERIFY(effective.value("unsupported").toArray().isEmpty()); QVERIFY(effective.value("automatic_resize").toBool()); QVERIFY(effective.value("wallpaper").toBool());
        const auto session = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true); core->close_session(session.id);
        const auto canonical = core->stored_document(); const auto noOp = parsed->patch(defaultMaster, QByteArray(canonical.data(), qsizetype(canonical.size()))); QVERIFY(noOp); QCOMPARE(parsed->serialize(defaultMaster), saved);
        auto edited = QJsonDocument::fromJson(*normalized).object(); auto profiles = edited.value("profiles").toArray(); auto local = profiles[0].toObject(); auto overrides = local.value("overrides").toObject();
        overrides.insert("resolution", "1920x1080"); overrides.insert("color_depth", 16); local.insert("overrides", overrides); profiles[0] = local; edited.insert("profiles", profiles);
        const auto changed = parsed->patch(defaultMaster, QJsonDocument(edited).toJson(QJsonDocument::Compact)); QVERIFY(changed);
        const auto output = parsed->serialize(defaultMaster); QVERIFY(output); QCOMPARE(attribute(*output, "Resolution", QString::fromLatin1(Id)), QByteArray("FitToWindow"));
        QCOMPARE(attribute(*output, "RmResolution", QString::fromLatin1(Id)), QByteArray("1920x1080")); QCOMPARE(attribute(*output, "Colors", QString::fromLatin1(Id)), QByteArray("Colors16Bit"));
        QVERIFY(!parsed->patch(defaultMaster, QByteArray("{\"schema_version\":2,\"profiles\":[],\"Password\":\"forbidden\"}")));
        QVERIFY(!parsed->patch(defaultMaster, QByteArray(ConnectionXml::MaxInner + 1, ' '))); QCOMPARE(parsed->serialize(defaultMaster), output);
    }
    void projectionPatchPreservesOpaqueXmlAndInvalidatesMovedCredentials() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7"), destination("b1c3f965-9c10-4cf0-b688-a530cde15a17");
        const auto password = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic retained password")), 4096); QVERIFY(password);
        const auto source = xml(QByteArray("<!--outer--><Node Type=\"Container\" Id=\"") + folder + "\" Name=\"source\" Username=\"first\" xmlns:q=\"urn:first\">" +
            "<Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"child\" Hostname=\"localhost\" Port=\"3389\" Protocol=\"RDP\" InheritUsername=\"true\" "
            "Password=\"" + *password + "\" VNCProxyPassword=\"" + *password + "\" RDGatewayPassword=\"" + *password + "\" RDGatewayAccessToken=\"" + *password +
            "\" UnknownOption=\"untouched\" q:extra=\"keep\"><!--inner--><PluginData><q:Item q:value=\"opaque\"/></PluginData></Node></Node>" +
            "<Node Type=\"Container\" Id=\"" + destination + "\" Name=\"destination\" Username=\"second\" xmlns:q=\"urn:second\"/><!--tail-->");
        auto parsed = ConnectionXml::read(source, &master); QVERIFY(parsed);
        const SessionTarget first{"localhost", 3389, "first"}, second{"localhost", 3389, "second"};
        ConnectionBindings reviewed{"f6595425-c35a-45b7-9dfc-84dd83e6b002", {{QString::fromLatin1(Id), QString::fromLatin1(Id), QCryptographicHash::hash(QByteArray::fromBase64(*password), QCryptographicHash::Sha256), first}}};
        QVERIFY(parsed->reviewCredentials(master, reviewed)); QVERIFY(parsed->password(master, QString::fromLatin1(Id), first));
        const auto original = parsed->serialize(master); QVERIFY(original); const auto projection = parsed->projection(); QVERIFY(projection);
        auto document = QJsonDocument::fromJson(*projection).object(); auto profiles = document.value("profiles").toArray(); auto child = profiles[1].toObject();
        child.insert("name", "renamed"); child.insert("parent", QString::fromLatin1(destination)); profiles[1] = child; document.insert("profiles", profiles);
        const auto request = QJsonDocument(document).toJson(QJsonDocument::Compact);
        QVERIFY(!parsed->patch(Secret(QByteArray("wrong")), request)); QVERIFY(!parsed->patch(master, request, [] { return true; })); QCOMPARE(parsed->serialize(master), original);
        const auto normalized = parsed->patch(master, request); QVERIFY(normalized); QVERIFY(!parsed->password(master, QString::fromLatin1(Id), first)); QVERIFY(!parsed->password(master, QString::fromLatin1(Id), second));
        const auto saved = parsed->serialize(master); QVERIFY(saved); QCOMPARE(saved->count(*password), 4);
        for (const auto& marker : {"UnknownOption=\"untouched\"", "q:extra=\"keep\"", "<!--outer-->", "<!--inner-->", "<!--tail-->"}) QVERIFY(saved->contains(marker));
        QXmlStreamReader reader(*saved); bool checked = false;
        while (!reader.atEnd()) { reader.readNext(); if (reader.isStartElement() && reader.name() == "Item") { QCOMPARE(reader.namespaceUri().toString(), QString("urn:first")); checked = true; } }
        QVERIFY(checked); QVERIFY(!reader.hasError()); auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), normalized);
        auto movedBack = QJsonDocument::fromJson(*normalized).object(); auto backProfiles = movedBack.value("profiles").toArray(); auto backChild = backProfiles[1].toObject();
        // Locate by identity: topology projection order changes after the move.
        int index = -1; for (int i = 0; i < backProfiles.size(); ++i) if (backProfiles[i].toObject().value("id").toString() == QString::fromLatin1(Id)) index = i;
        QVERIFY(index >= 0); backChild = backProfiles[index].toObject(); backChild.insert("parent", QString::fromLatin1(folder)); backProfiles[index] = backChild; movedBack.insert("profiles", backProfiles);
        QVERIFY(reopened->patch(master, QJsonDocument(movedBack).toJson(QJsonDocument::Compact))); QVERIFY(!reopened->password(master, QString::fromLatin1(Id), first));
        auto deletion = QJsonDocument::fromJson(*reopened->projection()).object(); auto remaining = deletion.value("profiles").toArray();
        for (int i = remaining.size() - 1; i >= 0; --i) if (remaining[i].toObject().value("id").toString() == QString::fromLatin1(Id)) remaining.removeAt(i);
        deletion.insert("profiles", remaining); QVERIFY(reopened->patch(master, QJsonDocument(deletion).toJson(QJsonDocument::Compact)));
        const auto deleted = reopened->serialize(master); QVERIFY(deleted); QVERIFY(!deleted->contains(*password)); QVERIFY(!deleted->contains("PluginData"));
    }
    void nativeProjectionKeepsSecretsAndOpaqueValuesOutOfCore() {
        const Secret master(QByteArray("Password")); const auto password = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic projection password")), 4096); QVERIFY(password);
        const auto source = xml(QByteArray("<Node Type=\"Connection\" Id=\"") + Id + "\" Name=\"unsupported\" Protocol=\"ICA\" Password=\"" + *password +
            "\" VNCProxyPassword=\"" + *password + "\" RDGatewayPassword=\"" + *password + "\" RDGatewayAccessToken=\"" + *password +
            "\" UnknownOption=\"opaque synthetic marker\"><PluginData>private plugin marker</PluginData></Node>");
        auto parsed = ConnectionXml::read(source, &master); QVERIFY(parsed); const auto projection = parsed->projection(); QVERIFY(projection);
        for (const auto& marker : {"Password", "Protected", "RmCredentialBindings", "synthetic projection password", "opaque synthetic marker", "private plugin marker"}) QVERIFY(!projection->contains(marker));
        QVERIFY(!projection->contains(*password)); const auto profile = QJsonDocument::fromJson(*projection).object().value("profiles").toArray()[0].toObject();
        QCOMPARE(profile.value("overrides").toObject().value("protocol").toString(), QString("unsupported"));
        const auto reasons = profile.value("native").toObject().value("unsupported").toArray(); QVERIFY(reasons.contains("protocol")); QVERIFY(reasons.contains("opaque"));
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        QVERIFY_THROWS_EXCEPTION(std::exception, core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true)); QCOMPARE(core->session_count(), size_t(0));
        const auto saved = parsed->serialize(master); QVERIFY(saved); QVERIFY(saved->contains("Protocol=\"ICA\"")); QVERIFY(saved->contains("private plugin marker")); QCOMPARE(saved->count(*password), 4);
        for (const QByteArray protocol : {QByteArray("SSH2"), QByteArray("VNC")}) {
            auto mapped = ConnectionXml::read(xml(QByteArray(node()).replace("RDP", protocol)), &master); QVERIFY(mapped); const auto json = mapped->projection(); QVERIFY(json);
            const auto name = QJsonDocument::fromJson(*json).object().value("profiles").toArray()[0].toObject().value("overrides").toObject().value("protocol").toString();
            QCOMPARE(name, protocol == "SSH2" ? QString("ssh") : QString("vnc"));
        }
        const auto encrypted = ConnectionCrypto::encrypt(master, 1000, Secret(node())); QVERIFY(encrypted);
        auto locked = ConnectionXml::read(xml(*encrypted, "2.8", {}, true)); QVERIFY(locked); QVERIFY(!locked->projection()); QVERIFY(locked->unlock(master)); QVERIFY(locked->projection());
    }
    void nativeSshProtocolAliasesRoundTrip() {
        const Secret master(QByteArray("Password"));
        for (const QByteArray alias : {QByteArray("SSH2"), QByteArray("3"), QByteArray("SSH1"), QByteArray("2")}) {
            const auto source = xml(QByteArray(node()).replace("Protocol=\"RDP\"", "Protocol=\"" + alias + '"').replace("3389", "22"));
            auto parsed = ConnectionXml::read(source, &master); QVERIFY(parsed);
            const auto projection = parsed->projection(); QVERIFY(projection);
            const auto profile = QJsonDocument::fromJson(*projection).object().value("profiles").toArray()[0].toObject();
            const bool supported = alias == "SSH2" || alias == "3";
            QCOMPARE(profile.value("overrides").toObject().value("protocol").toString(), supported ? QString("ssh") : QString("unsupported"));
            QCOMPARE(profile.value("native").toObject().value("unsupported").toArray().contains("protocol"), !supported);
            const auto saved = parsed->serialize(master); QVERIFY(saved);
            QCOMPARE(attribute(*saved, "Protocol", QString::fromLatin1(Id)), alias);
            auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), projection);
            if (!supported) {
                auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
                QVERIFY_THROWS_EXCEPTION(std::exception, core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true));
                QCOMPARE(core->session_count(), size_t(0));
            }
        }
    }
    void nativeSshInheritanceRoundTrip() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder + "\" Name=\"SSH group\" Protocol=\"3\" Hostname=\"localhost\" Port=\"2222\" Username=\"literal-user\" Domain=\"passive\">"
            "<Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"child\" Protocol=\"SSH1\" Hostname=\"unused.test\" Port=\"0\" Username=\"unused\" Domain=\"unused\" "
            "InheritProtocol=\"true\" RmHostInherited=\"true\" InheritPort=\"true\" InheritUsername=\"true\" InheritDomain=\"true\"/></Node>";
        auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed);
        const auto projection = parsed->projection(); QVERIFY(projection);
        const auto child = QJsonDocument::fromJson(*projection).object().value("profiles").toArray()[1].toObject();
        const auto overrides = child.value("overrides").toObject();
        for (const auto& field : {"protocol", "host", "port", "username", "domain"}) QVERIFY(overrides.value(field).isNull());
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        const auto info = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true);
        QCOMPARE(QString::fromUtf8(info.protocol.data(), qsizetype(info.protocol.size())), QString("ssh"));
        QCOMPARE(QString::fromUtf8(info.host.data(), qsizetype(info.host.size())), QString("localhost"));
        QCOMPARE(info.port, quint16(2222));
        QCOMPARE(QString::fromUtf8(info.username.data(), qsizetype(info.username.size())), QString("literal-user"));
        QVERIFY(!info.graphical && info.keyboard && info.resize && !info.mouse && !info.clipboard);
        core->close_session(info.id);
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        QCOMPARE(attribute(*saved, "Protocol", QString::fromLatin1(Id)), QByteArray("SSH1"));
        QCOMPARE(attribute(*saved, "Port", QString::fromLatin1(Id)), QByteArray("0"));
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), projection);
    }
    void projectionPatchExportsNativeSsh2() {
        const Secret master(QByteArray("mR3m")); auto parsed = ConnectionXml::create(); QVERIFY(parsed);
        const auto input = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{QJsonObject{
            {"id", QString::fromLatin1(Id)}, {"name", "SSH fixture"}, {"kind", "connection"}, {"parent", QJsonValue(QJsonValue::Null)},
            {"overrides", QJsonObject{{"protocol", "ssh"}, {"host", "localhost"}, {"username", "literal-user"}}}}}}}).toJson(QJsonDocument::Compact);
        const auto projection = parsed->patch(master, input); QVERIFY(projection);
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        QCOMPARE(attribute(*saved, "Protocol", QString::fromLatin1(Id)), QByteArray("SSH2"));
        QCOMPARE(attribute(*saved, "Port", QString::fromLatin1(Id)), QByteArray("22"));
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), projection);
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        const auto info = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true); QCOMPARE(info.port, quint16(22));
        core->close_session(info.id);
    }
    void nativeSshMetadataSurvivesProjectionPatch() {
        const Secret master(QByteArray("Password"));
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic SSH password")), 4096); QVERIFY(cipher);
        const auto body = QByteArray(node()).replace("RDP", "3").replace("3389", "22").replace("Password=\"\"", "Password=\"" + *cipher + '"')
            .replace("/>", " PuttySession=\"passive-session\" Domain=\"passive-domain\" SSHOptions=\"\" OpeningCommand=\"\" SSHTunnelConnectionName=\"\"/>");
        auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed); const auto projection = parsed->projection(); QVERIFY(projection);
        QVERIFY(!projection->contains(*cipher)); QVERIFY(!projection->contains("passive-session"));
        auto request = QJsonDocument::fromJson(*projection).object(); auto profiles = request.value("profiles").toArray(); auto profile = profiles[0].toObject();
        profile.insert("name", "renamed SSH"); profiles[0] = profile; request.insert("profiles", profiles);
        const auto changed = parsed->patch(master, QJsonDocument(request).toJson(QJsonDocument::Compact)); QVERIFY(changed);
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        for (const auto& field : {"Protocol", "Password", "PuttySession", "Domain", "SSHOptions", "OpeningCommand", "SSHTunnelConnectionName"})
            QCOMPARE(attribute(*saved, field, QString::fromLatin1(Id)), attribute(xml(body), field, QString::fromLatin1(Id)));
        QCOMPARE(attribute(*saved, "Name", QString::fromLatin1(Id)), QByteArray("renamed SSH"));
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), changed);
    }
    void sshAuthAndKeyPathInheritAndRoundTrip() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto parent = QByteArray("<Node Type=\"Container\" Id=\"") + folder + "\" Name=\"SSH group\" Protocol=\"SSH2\" Hostname=\"localhost\" Port=\"22\" "
            "RmSshAuth=\"private_key\" RmSshKeyPath=\"/owned/fixture/id_ed25519\" RmSshAuthInherited=\"true\" RmSshKeyPathInherited=\"true\">";
        const auto child = QByteArray(node()).replace("RDP", "SSH2").replace("3389", "22");
        for (bool inherited : {false, true}) {
            auto body = child;
            if (inherited) body.replace("/>", " RmSshAuthInherited=\"true\" RmSshKeyPathInherited=\"true\"/>");
            auto parsed = ConnectionXml::read(xml(parent + body + "</Node>"), &master); QVERIFY(parsed);
            const auto projection = parsed->projection(); QVERIFY(projection);
            const auto overrides = QJsonDocument::fromJson(*projection).object().value("profiles").toArray()[1].toObject().value("overrides").toObject();
            if (inherited) { QVERIFY(overrides.value("ssh_auth").isNull()); QVERIFY(overrides.value("ssh_key_path").isNull()); }
            else { QCOMPARE(overrides.value("ssh_auth").toString(), QString("password")); QCOMPARE(overrides.value("ssh_key_path").toString(), QString()); }
            auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
            const auto snapshot = core->snapshot(); const auto effective = QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray()[1].toObject().value("effective").toObject();
            QCOMPARE(effective.value("ssh_auth").toString(), inherited ? QString("private_key") : QString("password"));
            QCOMPARE(effective.value("ssh_key_path").toString(), inherited ? QString("/owned/fixture/id_ed25519") : QString());
            QCOMPARE(effective.value("sources").toObject().value("ssh_auth").toString(), QString::fromLatin1(inherited ? folder : Id));
            auto request = QJsonDocument::fromJson(*projection).object(); auto profiles = request.value("profiles").toArray(); auto profile = profiles[1].toObject(); auto local = profile.value("overrides").toObject();
            local.insert("ssh_auth", "password"); local.insert("ssh_key_path", ""); profile.insert("overrides", local); profile.insert("name", "SSH override"); profiles[1] = profile; request.insert("profiles", profiles);
            const auto changed = parsed->patch(master, QJsonDocument(request).toJson(QJsonDocument::Compact)); QVERIFY(changed);
            const auto saved = parsed->serialize(master); QVERIFY(saved);
            if (inherited) {
                QCOMPARE(attribute(*saved, "RmSshAuth", QString::fromLatin1(Id)), QByteArray("password"));
                QCOMPARE(attribute(*saved, "RmSshAuthInherited", QString::fromLatin1(Id)), QByteArray("false"));
                QCOMPARE(attribute(*saved, "RmSshKeyPathInherited", QString::fromLatin1(Id)), QByteArray("false"));
            }
            QCOMPARE(attribute(*saved, "RmSshKeyPath", QString::fromLatin1(Id)), QByteArray());
            QCOMPARE(attribute(*saved, "RmSshKeyPath", QString::fromLatin1(folder)), QByteArray("/owned/fixture/id_ed25519"));
            auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), changed);
        }
    }
    void sshIgnoresInactiveRdpSettingsButBlocksCriticalOptions() {
        const Secret master(QByteArray("Password"));
        const auto base = QByteArray(node()).replace("RDP", "SSH2").replace("3389", "22").replace("/>",
            " Resolution=\"SmartSize\" Colors=\"Colors15Bit\" RmResolution=\"invalid\" InheritColors=\"invalid\" "
            "UseCredSsp=\"false\" RDPAuthenticationLevel=\"NoAuth\" RedirectSound=\"BringToThisComputer\" "
            "RedirectPrinters=\"true\" RDGatewayUsageMethod=\"Always\" RDPMinutesToIdleTimeout=\"5\" PuttySession=\"passive\"/>");
        struct Case { const char *attribute, *reason; };
        const Case cases[] = {{"", ""}, {"OpeningCommand=\"unsupported\"", "command"}, {"SSHOptions=\"unsupported\"", "command"},
            {"SSHTunnelConnectionName=\"unsupported\"", "command"}, {"ExternalCredentialProvider=\"unsupported\"", "provider"},
            {"UnknownOption=\"opaque\"", "opaque"}};
        for (const auto& test : cases) {
            auto body = base; if (*test.attribute) body.replace("/>", QByteArray(" ") + test.attribute + "/>");
            auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed); const auto projection = parsed->projection(); QVERIFY(projection);
            const auto reasons = QJsonDocument::fromJson(*projection).object().value("profiles").toArray()[0].toObject().value("native").toObject().value("unsupported").toArray();
            if (*test.reason) QCOMPARE(reasons, QJsonArray{QString::fromLatin1(test.reason)});
            else QVERIFY2(reasons.isEmpty(), QJsonDocument(reasons).toJson().constData());
            auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
            if (*test.reason) {
                QVERIFY_THROWS_EXCEPTION(std::exception, core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true));
                QCOMPARE(core->session_count(), size_t(0));
            } else {
                const auto info = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true); core->close_session(info.id);
            }
            const auto saved = parsed->serialize(master); QVERIFY(saved);
            QCOMPARE(attribute(*saved, "Colors", QString::fromLatin1(Id)), QByteArray("Colors15Bit"));
            QCOMPARE(attribute(*saved, "InheritColors", QString::fromLatin1(Id)), QByteArray("invalid"));
        }
    }
    void nativeCriticalOptionsRemainPreservedAndNonConnectable() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto base = QByteArray("<Node Type=\"Connection\" Id=\"") + Id + "\" Name=\"child\" Protocol=\"RDP\" Hostname=\"localhost\" Port=\"3389\" Username=\"user\" Domain=\"TEAM\" "
            "Colors=\"Colors32Bit\" Resolution=\"FitToWindow\" UseCredSsp=\"true\" RDPAuthenticationLevel=\"WarnOnFailedAuth\" RedirectSound=\"DoNotPlay\"/>";
        const auto parent = QByteArray(base).replace("Type=\"Connection\"", "Type=\"Container\"").replace(Id, folder).replace("/>", ">");
        struct Case { const char *attribute, *before, *value, *reason; };
        const Case cases[] = {
            {"Type", "Connection", "Unrecognized", "node_type"}, {"Protocol", "RDP", "ICA", "protocol"}, {"Port", "3389", "0", "port"},
            {"Resolution", "FitToWindow", "SmartSize", "resolution"}, {"Colors", "Colors32Bit", "Colors15Bit", "colors"},
            {"InheritUsername", nullptr, "invalid", "inheritance"}, {"RmResolution", nullptr, "invalid", "preferences"},
            // Mapped RDP security/gateway fields (V105) only disable launch for values they cannot represent.
            {"RDGatewayUseConnectionCredentials", nullptr, "SmartCard", "gateway"}, {"ExternalCredentialProvider", nullptr, "VaultOpenbao", "provider"},
            {"UserViaAPI", nullptr, "synthetic-opaque-marker", "provider"}, {"PreExtApp", nullptr, "synthetic-opaque-marker", "command"},
            {"RDPAuthenticationLevel", "WarnOnFailedAuth", "Sometimes", "authentication"},
            {"UseRestrictedAdmin", nullptr, "true", "authentication"}, {"LoadBalanceInfo", nullptr, "&#9;synthetic-opaque-marker", "authentication"},
            {"RedirectPrinters", nullptr, "true", "redirection"}, {"RedirectDiskDrives", nullptr, "All", "redirection"},
            {"RedirectSound", "DoNotPlay", "PlaySomewhereElse", "redirection"}, {"EnableDesktopComposition", nullptr, "true", "display_options"},
            {"RDPMinutesToIdleTimeout", nullptr, "500", "idle_timeout"}, {"RDPAlertIdleTimeout", nullptr, "true", "idle_timeout"}, {"UnknownOption", nullptr, "synthetic-opaque-marker", "opaque"}
        };
        for (const auto& test : cases) {
            const auto replacement = QByteArray(test.attribute) + "=\"" + test.value + '"'; auto child = base;
            if (test.before) child.replace(QByteArray(test.attribute) + "=\"" + test.before + '"', replacement);
            else child.replace("/>", ' ' + replacement + "/>");
            auto parsed = ConnectionXml::read(xml(parent + child + "</Node>"), &master); QVERIFY(parsed);
            const auto before = parsed->serialize(master); QVERIFY(before); const auto projection = parsed->projection(); QVERIFY(projection);
            QVERIFY(!projection->contains("synthetic-opaque-marker"));
            const auto profiles = QJsonDocument::fromJson(*projection).object().value("profiles").toArray();
            QVERIFY2(profiles[1].toObject().value("native").toObject().value("unsupported").toArray().contains(QString::fromLatin1(test.reason)), test.attribute);
            auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
            QVERIFY_THROWS_EXCEPTION(std::exception, core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true)); QCOMPARE(core->session_count(), size_t(0));
            const auto saved = parsed->serialize(master); QCOMPARE(saved, before); QVERIFY(saved->contains(replacement));
            auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->projection(), projection);
        }
        auto absent = ConnectionXml::read(xml(node()), &master); QVERIFY(absent); const auto missing = absent->projection(); QVERIFY(missing);
        const auto reasons = QJsonDocument::fromJson(*missing).object().value("profiles").toArray()[0].toObject().value("native").toObject().value("unsupported").toArray();
        for (const auto& code : {"resolution", "colors"}) QVERIFY(reasons.contains(QString::fromLatin1(code)));
        QVERIFY(!reasons.contains(QString("redirection"))); // absent RedirectSound = Bring to this computer
        // Absent UseCredSsp/RDPAuthenticationLevel use secure defaults (V105), not a launch block.
        QVERIFY(!reasons.contains(QString("authentication")));
    }
    void nativeCredentialAccountMatchesProjectionSourcesAndAliases() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic inherited account password")), 4096); QVERIFY(cipher);
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder + "\" Name=\"folder\" Hostname=\"LOCALHOST\" Port=\"3391\" Protocol=\" 0 \" Username=\"user\" Domain=\"TEAM\" Password=\"" + *cipher +
            "\" Colors=\"Colors32Bit\" Resolution=\"FitToWindow\" UseCredSsp=\"true\" RDPAuthenticationLevel=\"WarnOnFailedAuth\" RedirectSound=\"DoNotPlay\">"
            "<Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"child\" Hostname=\"stale.test\" Port=\"3389\" Protocol=\"VNC\" Username=\"unused\" Domain=\"unused\" "
            "RmHostInherited=\"true\" InheritPort=\" true \" InheritProtocol=\"true\" InheritUsername=\"true\" InheritDomain=\"true\" InheritPassword=\" true \" "
            "InheritColors=\"true\" InheritResolution=\"true\" InheritUseCredSsp=\"true\" InheritRDPAuthenticationLevel=\"true\" InheritRedirectSound=\"true\"/></Node>";
        auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed); const auto projection = parsed->projection(); QVERIFY(projection);
        auto core = rm::detached_core(); core->load_document(rust::Str(projection->constData(), size_t(projection->size())));
        const auto snapshot = core->snapshot();
        const auto profiles = QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray();
        QVERIFY(profiles[1].toObject().value("native").toObject().value("unsupported").toArray().isEmpty());
        const auto info = core->open_session(rust::Str(Id.constData(), size_t(Id.size())), true);
        const SessionTarget projected{QString::fromUtf8(info.host.data(), qsizetype(info.host.size())), info.port,
            QString::fromUtf8(info.username.data(), qsizetype(info.username.size())), QString::fromUtf8(info.domain.data(), qsizetype(info.domain.size()))};
        QCOMPARE(projected.host, QString("LOCALHOST")); QCOMPARE(projected.port, quint16(3391)); QCOMPARE(projected.username, QString("user")); QCOMPARE(projected.domain, QString("TEAM"));
        const auto normalized = normalizeRdpTarget(projected); QVERIFY(normalized); const auto target = *normalized;
        core->close_session(info.id);
        ConnectionBindings reviewed{"f6595425-c35a-45b7-9dfc-84dd83e6b002", {{QString::fromLatin1(Id), QString::fromLatin1(folder),
            QCryptographicHash::hash(QByteArray::fromBase64(*cipher), QCryptographicHash::Sha256), target}}};
        QVERIFY(parsed->reviewCredentials(master, reviewed)); const auto password = parsed->password(master, QString::fromLatin1(Id), target);
        QVERIFY(password); QVERIFY(bytes(*password) == "synthetic inherited account password");
        const auto saved = parsed->serialize(master); QVERIFY(saved); auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened);
        QVERIFY(reopened->password(master, QString::fromLatin1(Id), target));
        for (const auto& port : {QByteArray(), QByteArray("0"), QByteArray("bad"), QByteArray("65536")}) {
            auto invalid = ConnectionXml::read(xml(QByteArray(body).replace("Port=\"3391\"", port.isEmpty() ? QByteArray() : "Port=\"" + port + '"')), &master);
            QVERIFY(invalid); QVERIFY(!invalid->reviewCredentials(master, reviewed));
        }
    }
    void xmlProtectionAndPasswordInheritanceKeepExplicitAssociation() {
        const Secret master(QByteArray("Password")); const QByteArray folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto parentPassword = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic parent password")), 4096),
            childPassword = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic child password")), 4096); QVERIFY(parentPassword && childPassword);
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder + "\" Name=\"folder\" Password=\"" + *parentPassword +
            "\"><Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"child\" Hostname=\"localhost\" Port=\"3389\" Username=\"user\" Password=\"" + *childPassword + "\"/></Node>";
        auto parsed = ConnectionXml::read(xml(body), &master); QVERIFY(parsed); const SessionTarget target{"localhost", 3389, "user"};
        ConnectionBindings reviewed{"f6595425-c35a-45b7-9dfc-84dd83e6b002", {{QString::fromLatin1(Id), QString::fromLatin1(Id), QCryptographicHash::hash(QByteArray::fromBase64(*childPassword), QCryptographicHash::Sha256), target}}};
        QVERIFY(parsed->reviewCredentials(master, reviewed)); const auto original = parsed->serialize(master); QVERIFY(original);
        const Secret wrong(QByteArray("wrong")); QVERIFY(!parsed->setPasswordInheritance(wrong, QString::fromLatin1(Id), true)); QVERIFY(!parsed->setFullEncryption(wrong, true));
        QVERIFY(!parsed->setPasswordInheritance(master, QString::fromLatin1(Id), true, [] { return true; })); QVERIFY(!parsed->setFullEncryption(master, true, [] { return true; })); QCOMPARE(parsed->serialize(master), original);
        QVERIFY(parsed->setPasswordInheritance(master, QString::fromLatin1(Id), true)); QVERIFY(!parsed->password(master, QString::fromLatin1(Id), target));
        const auto inherited = parsed->serialize(master); QVERIFY(inherited); QVERIFY(inherited->contains(*childPassword)); QVERIFY(inherited->contains(*parentPassword));
        QVERIFY(parsed->setPasswordInheritance(master, QString::fromLatin1(Id), false)); QVERIFY(!parsed->password(master, QString::fromLatin1(Id), target)); // no revival of local authorization
        QVERIFY(parsed->setPasswordInheritance(master, QString::fromLatin1(Id), true)); reviewed.records[0].owner = QString::fromLatin1(folder);
        reviewed.records[0].passwordHash = QCryptographicHash::hash(QByteArray::fromBase64(*parentPassword), QCryptographicHash::Sha256); QVERIFY(parsed->reviewCredentials(master, reviewed));
        const auto password = parsed->password(master, QString::fromLatin1(Id), target); QVERIFY(password); QVERIFY(bytes(*password) == "synthetic parent password");
        QVERIFY(parsed->setFullEncryption(master, true)); QVERIFY(parsed->fullEncryption()); const auto encrypted = parsed->serialize(master); QVERIFY(encrypted);
        QVERIFY(!encrypted->contains("<Node")); QVERIFY(!encrypted->contains(Id)); QVERIFY(!encrypted->contains(*childPassword));
        auto reopened = ConnectionXml::read(*encrypted, &master); QVERIFY(reopened); QVERIFY(reopened->password(master, QString::fromLatin1(Id), target));
        QVERIFY(reopened->setFullEncryption(master, false)); const auto readable = reopened->serialize(master); QVERIFY(readable);
        QCOMPARE(attribute(*readable, "Password", QString::fromLatin1(Id)), *childPassword); QCOMPARE(attribute(*readable, "Password", QString::fromLatin1(folder)), *parentPassword);
        QVERIFY(reopened->password(master, QString::fromLatin1(Id), target));
        const Secret defaultMaster(QByteArray("mR3m")); auto empty = ConnectionXml::create(); QVERIFY(empty); QVERIFY(empty->setFullEncryption(defaultMaster, true)); QVERIFY(empty->setFullEncryption(defaultMaster, false));
    }
    void emptyFullEncryptedXmlRetainsAuthenticatedInnerPayload() {
        const Secret defaultMaster(QByteArray("mR3m")), master(QByteArray("synthetic empty XML master"));
        auto parsed = ConnectionXml::create(); QVERIFY(parsed); QVERIFY(parsed->changeMaster(defaultMaster, master));
        const auto projection = parsed->projection(); QVERIFY(projection); QVERIFY(parsed->setFullEncryption(master, true));
        const auto encrypted = parsed->serialize(master); QVERIFY(encrypted); auto reopened = ConnectionXml::read(*encrypted, &master); QVERIFY(reopened);
        QCOMPARE(reopened->projection(), projection); QVERIFY(reopened->profileIds().isEmpty());
        QXmlStreamReader reader(*encrypted); QVERIFY(reader.readNextStartElement());
        const auto inner = reader.readElementText().toLatin1(); QVERIFY(!inner.isEmpty()); QVERIFY(ConnectionCrypto::validEnvelope(inner, ConnectionXml::MaxInner));
        auto locked = ConnectionXml::read(*encrypted); QVERIFY(locked); QVERIFY(!locked->projection());
        QVERIFY(!locked->unlock(Secret(QByteArray("wrong")))); QVERIFY(!locked->projection()); QVERIFY(locked->unlock(master)); QCOMPARE(locked->projection(), projection);
        QVERIFY(!ConnectionXml::read(QByteArray(*encrypted).replace(inner, ""), &master));
        QVERIFY(reopened->setFullEncryption(master, false)); const auto readable = reopened->serialize(master); QVERIFY(readable);
        auto plain = ConnectionXml::read(*readable, &master); QVERIFY(plain); QCOMPARE(plain->projection(), projection);
    }
    void xmlPassiveNodeCountsAndPatchDepthAreBounded() {
        const Secret master(QByteArray("Password")); QByteArray comments;
        for (int i = 0; i < 100001; ++i) comments += "<!--x-->";
        QVERIFY(!ConnectionXml::read(xml(comments), &master));
        const Secret defaultMaster(QByteArray("mR3m")); auto parsed = ConnectionXml::create(); QVERIFY(parsed); const auto before = parsed->serialize(defaultMaster); QVERIFY(before);
        QJsonArray profiles; QString parent;
        for (int i = 0; i < 128; ++i) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            profiles.push_back(QJsonObject{{"id", id}, {"name", "bounded"}, {"kind", "folder"}, {"parent", parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent)},
                {"overrides", QJsonObject{{"protocol", "rdp"}, {"host", "localhost"}, {"port", 3389}, {"username", ""}}}}); parent = id;
        }
        const auto encode = [&] { return QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", profiles}}).toJson(QJsonDocument::Compact); };
        QVERIFY(!parsed->patch(defaultMaster, encode())); QCOMPARE(parsed->serialize(defaultMaster), before); // Connections root also consumes one level
        profiles.removeLast(); const auto allowed = parsed->patch(defaultMaster, encode()); QVERIFY(allowed);
        const auto output = parsed->serialize(defaultMaster); QVERIFY(output); auto reopened = ConnectionXml::read(*output, &defaultMaster); QVERIFY(reopened); QCOMPARE(reopened->projection(), allowed);
    }
    void xmlTamperingAndParserLimitsReject() {
        const Secret master(QByteArray("Password"));
        for (const auto& bad : {xml(node(), "2.5"), xml(node(), "2.9"), xml(node(), "2.8", "xmlns=\"urn:wrong\""),
            xml(node()).replace("AES", "DES"), xml(node()).replace("GCM", "CBC"), xml(node()).replace("KdfIterations=\"1000\"", "KdfIterations=\"0\""),
            xml(node()).replace("BlockCipherMode=\"GCM\"", ""), xml(node() + node()), xml(node()).replace(Id, "invalid"),
            xml(node()).replace("Name=\"fixture\"", "Name=\"fixture\" Name=\"duplicate\""), xml(node()).replace("</Connections>", ""),
            xml(node()).replace("?><Connections", "?><!DOCTYPE Connections [<!ENTITY external SYSTEM \"file:///no-access\">]><Connections"),
            xml(node()).replace("?><Connections", "?><!DOCTYPE Connections [<!ENTITY expansion \"unsafe\">]><Connections").replace("fixture", "&expansion;"),
            xml(QByteArray(129, '<'))}) QVERIFY(!ConnectionXml::read(bad, &master));
        QByteArray nested; for (int i = 0; i < 129; ++i) nested += "<opaque>"; for (int i = 0; i < 129; ++i) nested += "</opaque>";
        QVERIFY(!ConnectionXml::read(xml(nested), &master));
        QVERIFY(!ConnectionXml::read(QByteArray(ConnectionXml::MaxFile + 1, ' ')));
        const Secret wrong(QByteArray("incorrect")); QVERIFY(!ConnectionXml::read(xml(node()), &wrong));
        const auto one = QByteArray("<Node Type=\"Connection\" Password=\"") + Original[3].cipher + "\"/>";
        QByteArray many; for (int i = 0; i < 1025; ++i) many += one;
        QVERIFY(!ConnectionXml::read(xml(many), &master));
    }
    void credentialBindingsRejectSwapsAndRequireImportConsent() {
        const Secret master(QByteArray("Password"));
        const QString profile = QString::fromLatin1(Id), fileId = "f6595425-c35a-45b7-9dfc-84dd83e6b002";
        SessionTarget target; target.host = "localhost"; target.port = 3389;
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic connection password")), 4096); QVERIFY(cipher);
        const auto replacement = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("different synthetic password")), 4096); QVERIFY(replacement);
        const auto body = QByteArray(node()).replace("Password=\"\"", "Password=\"" + *cipher + '"');
        const auto original = xml(body);
        ConnectionBindings reviewed{fileId, {{profile, profile,
            QCryptographicHash::hash(QByteArray::fromBase64(*cipher), QCryptographicHash::Sha256), target}}};
        auto native = ConnectionXml::read(original, &master); QVERIFY(native);
        QVERIFY(native->credentialReviewRequired()); QVERIFY(native->fileId().isEmpty());
        QVERIFY(!native->password(master, profile, target));
        const auto unreviewedSave = native->serialize(master); QVERIFY(unreviewedSave); QVERIFY(!unreviewedSave->contains("RmCredentialBindings"));
        QVERIFY(!native->reviewCredentials(master, reviewed, [] { return true; })); QVERIFY(native->credentialReviewRequired());
        auto wrongTarget = reviewed; wrongTarget.records.front().target.host = "127.0.0.1";
        QVERIFY(!native->reviewCredentials(master, wrongTarget)); QVERIFY(native->credentialReviewRequired());
        QVERIFY(native->reviewCredentials(master, reviewed)); QVERIFY(!native->credentialReviewRequired()); QCOMPARE(native->fileId(), fileId);
        const auto password = native->password(master, profile, target); QVERIFY(password); QVERIFY(bytes(*password) == "synthetic connection password");
        QVERIFY(!native->password(master, profile, target, [] { return true; }));
        QVERIFY(!native->password(Secret(QByteArray("wrong")), profile, target));
        auto changed = target; changed.username = "changed"; QVERIFY(!native->password(master, profile, changed));
        const auto saved = native->serialize(master); QVERIFY(saved); QVERIFY(saved->contains("RmCredentialBindings="));
        QXmlStreamReader originalReader(original);
        while (!originalReader.atEnd()) {
            originalReader.readNext(); if (!originalReader.isStartElement()) continue;
            for (const auto& attribute : originalReader.attributes()) QVERIFY(!attribute.name().startsWith(QLatin1String("Rm")));
        }
        QVERIFY(!originalReader.hasError());
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QVERIFY(!reopened->credentialReviewRequired());
        QVERIFY(reopened->password(master, profile, target));
        auto swapped = ConnectionXml::read(QByteArray(*saved).replace(*cipher, *replacement), &master); QVERIFY(swapped);
        QVERIFY(!swapped->password(master, profile, target));
        auto endpoint = ConnectionXml::read(QByteArray(*saved).replace("Hostname=\"localhost\"", "Hostname=\"127.0.0.1\""), &master); QVERIFY(endpoint);
        QVERIFY(!endpoint->password(master, profile, target));
        changed = target; changed.host = "127.0.0.1"; QVERIFY(!endpoint->password(master, profile, changed));
        const auto fullBody = ConnectionCrypto::encrypt(master, 1000, Secret(body)); QVERIFY(fullBody);
        const auto payload = ConnectionMetadata::encodeBindings(reviewed); QVERIFY(payload);
        const auto bindingCipher = ConnectionCrypto::encrypt(master, 1000, *payload, ConnectionMetadata::MaxPayload); QVERIFY(bindingCipher);
        const auto extensions = QByteArray("RmFormatVersion=\"1\" RmFileId=\"") + fileId.toLatin1() + "\" RmCredentialBindings=\"" + *bindingCipher + '"';
        auto full = ConnectionXml::read(xml(*fullBody, "2.8", extensions, true)); QVERIFY(full); QVERIFY(full->profileIds().isEmpty());
        QVERIFY(full->unlock(master)); QVERIFY(full->password(master, profile, target));
        // Native default-key credentials require endpoint/account review, not rekeying.
        const Secret defaultMaster(QByteArray("mR3m"));
        auto insecureXml = xml(QByteArray(node()).replace("Password=\"\"", QByteArray("Password=\"") + Original[0].cipher + '"'));
        insecureXml.replace(Original[3].cipher, Original[0].cipher);
        auto insecure = ConnectionXml::read(insecureXml, &defaultMaster); QVERIFY(insecure);
        auto insecureReview = reviewed; insecureReview.records.front().passwordHash = QCryptographicHash::hash(QByteArray::fromBase64(Original[0].cipher), QCryptographicHash::Sha256);
        QVERIFY(insecure->credentialReviewRequired()); QVERIFY(!insecure->password(defaultMaster, profile, target));
        QVERIFY(insecure->serialize(defaultMaster));
        QVERIFY(insecure->reviewCredentials(defaultMaster, insecureReview)); QVERIFY(!insecure->credentialReviewRequired());
        const auto reviewedPassword = insecure->password(defaultMaster, profile, target);
        QVERIFY(reviewedPassword); QVERIFY(bytes(*reviewedPassword) == "ThisIsNotProtected");
        const auto savedDefault = insecure->serialize(defaultMaster); QVERIFY(savedDefault);
        auto reopenedDefault = ConnectionXml::read(*savedDefault, &defaultMaster); QVERIFY(reopenedDefault);
        QVERIFY(reopenedDefault->password(defaultMaster, profile, target));
    }
    void sshPasswordReuseRequiresOwningAccountSourceAndReview() {
        const Secret master(QByteArray("Password")), secret(QByteArray("synthetic SSH inherited password"));
        const QString consumer = QString::fromLatin1(Id), folder = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, secret, 4096); QVERIFY(cipher);
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder.toLatin1() +
            "\" Name=\"SSH folder\" Protocol=\"SSH2\" Port=\"22\" Username=\"TEAM\\literal-user\" Domain=\"passive /@ domain\" Password=\"" + *cipher +
            "\"><Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"SSH child\" Hostname=\"LOCALHOST\" Protocol=\"SSH2\" Port=\"22\" InheritUsername=\"true\" InheritPassword=\"true\"/></Node>";
        auto document = ConnectionXml::read(xml(body), &master); QVERIFY(document);
        const auto target = document->committedAccount(consumer); QVERIFY(target);
        QCOMPARE(target->protocol, QString("ssh")); QCOMPARE(target->username, QString("TEAM\\literal-user")); QVERIFY(target->domain.isEmpty());
        QVERIFY(!document->password(master, consumer, *target)); QVERIFY(!document->passwordStates().value(consumer).authorized);
        const auto untouched = document->serialize(master); QVERIFY(untouched); QVERIFY(!untouched->contains("RmFormatVersion"));
        const auto hash = QCryptographicHash::hash(QByteArray::fromBase64(*cipher), QCryptographicHash::Sha256);
        ConnectionBindings wrongOwner{"f6595425-c35a-45b7-9dfc-84dd83e6b002", {{consumer, consumer, hash, *target}}};
        QVERIFY(!document->reviewCredentials(master, wrongOwner));
        QVERIFY(document->reviewPasswords(master, {{consumer, *target}}));
        const auto saved = document->serialize(master); QVERIFY(saved); QCOMPARE(attribute(*saved, "RmFormatVersion"), QByteArray("2"));
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened);
        const auto password = reopened->password(master, consumer, *target); QVERIFY(password); QCOMPARE(bytes(*password), bytes(secret));
        QVERIFY(reopened->passwordStates().value(consumer).authorized); QVERIFY(!reopened->password(master, folder, *target));
        auto otherAccount = *target; otherAccount.protocol = "rdp"; QVERIFY(!reopened->password(master, consumer, otherAccount));
        auto changedOwner = ConnectionXml::read(QByteArray(*saved).replace("InheritPassword=\"true\"", "InheritPassword=\"false\" Password=\"" + *cipher + '"'), &master); QVERIFY(changedOwner);
        QVERIFY(!changedOwner->password(master, consumer, *target));
        auto changedUsername = ConnectionXml::read(QByteArray(*saved).replace("TEAM\\literal-user", "other-user"), &master); QVERIFY(changedUsername);
        QVERIFY(!changedUsername->password(master, consumer, *target));
        auto changedDomain = ConnectionXml::read(QByteArray(*saved).replace("passive /@ domain", "different passive domain"), &master); QVERIFY(changedDomain);
        QVERIFY(changedDomain->password(master, consumer, *target));
        QVERIFY(!ConnectionXml::read(QByteArray(*saved).replace("RmFormatVersion=\"2\"", "RmFormatVersion=\"1\""), &master));
    }
    void sshPrivateKeyNeverUsesSavedPasswordAsPassphrase() {
        const Secret master(QByteArray("Password")), secret(QByteArray("synthetic password is not a passphrase"));
        const QString consumer = QString::fromLatin1(Id);
        auto document = ConnectionXml::read(xml(QByteArray(node()).replace("Protocol=\"RDP\"", "Protocol=\"SSH2\"").replace("Port=\"3389\"", "Port=\"22\"")), &master); QVERIFY(document);
        QVERIFY(document->setPassword(master, consumer, secret));
        const auto target = document->committedAccount(consumer); QVERIFY(target);
        QVERIFY(document->authorizePasswords(master, consumer, {{consumer, *target}}));
        const auto initial = document->serialize(master); QVERIFY(initial);
        const auto cipher = attribute(*initial, "Password", consumer); QVERIFY(!cipher.isEmpty());
        auto projection = QJsonDocument::fromJson(*document->projection()).object(); auto profiles = projection.value("profiles").toArray();
        auto profile = profiles[0].toObject(); auto overrides = profile.value("overrides").toObject();
        overrides.insert("ssh_auth", "private_key"); overrides.insert("ssh_key_path", "/owned/id_ed25519");
        profile.insert("overrides", overrides); profiles[0] = profile; projection.insert("profiles", profiles);
        auto patched = document->patch(master, QJsonDocument(projection).toJson(QJsonDocument::Compact)); QVERIFY(patched);
        const auto persisted = document->serialize(master); QVERIFY(persisted);
        QCOMPARE(attribute(*persisted, "Password", consumer), cipher);
        QVERIFY(!document->committedAccount(consumer)); QVERIFY(!document->password(master, consumer, *target));
        QVERIFY(!document->passwordStates().value(consumer).authorized);
        overrides.insert("ssh_auth", "password"); profile.insert("overrides", overrides); profiles[0] = profile; projection.insert("profiles", profiles);
        patched = document->patch(master, QJsonDocument(projection).toJson(QJsonDocument::Compact)); QVERIFY(patched);
        const auto restored = document->serialize(master); QVERIFY(restored);
        QCOMPARE(attribute(*restored, "Password", consumer), cipher);
        QVERIFY(document->committedAccount(consumer)); QVERIFY(!document->password(master, consumer, *target));
        QVERIFY(document->reviewPasswords(master, {{consumer, *target}}));
        QVERIFY(document->password(master, consumer, *target));
        QCOMPARE(attribute(*document->serialize(master), "RmFormatVersion"), QByteArray("2"));
    }
    void sshPasswordsSurviveDefaultCustomFullAndRekeyWithoutSidecar() {
        const QString consumer = QString::fromLatin1(Id);
        const Secret secret(QByteArray("synthetic portable SSH credential")), replacement(QByteArray("replacement SSH document master"));
        for (const QByteArray& masterText : {QByteArray("mR3m"), QByteArray("Password")}) for (bool full : {false, true}) {
            const Secret master(masterText);
            auto source = xml(QByteArray(node()).replace("Protocol=\"RDP\"", "Protocol=\"SSH2\"").replace("Port=\"3389\"", "Port=\"22\""));
            if (masterText == "mR3m") source.replace(Original[3].cipher, Original[0].cipher);
            auto document = ConnectionXml::read(source, &master); QVERIFY(document);
            QVERIFY(document->setPassword(master, consumer, secret)); const auto target = document->committedAccount(consumer); QVERIFY(target);
            QVERIFY(document->authorizePasswords(master, consumer, {{consumer, *target}})); QVERIFY(document->setFullEncryption(master, full));
            const auto saved = document->serialize(master); QVERIFY(saved); QVERIFY(!saved->contains(bytes(secret)));
            auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened); QCOMPARE(reopened->fullEncryption(), full);
            const auto password = reopened->password(master, consumer, *target); QVERIFY(password); QCOMPARE(bytes(*password), bytes(secret));
            const auto file = reopened->fileId(); QVERIFY(reopened->changeMaster(master, replacement));
            const auto rekeyed = reopened->serialize(replacement); QVERIFY(rekeyed); QCOMPARE(attribute(*rekeyed, "RmFormatVersion"), QByteArray("2"));
            auto relocated = ConnectionXml::read(*rekeyed, &replacement); QVERIFY(relocated); QCOMPARE(relocated->fileId(), file);
            const auto portable = relocated->password(replacement, consumer, *target); QVERIFY(portable); QCOMPARE(bytes(*portable), bytes(secret));
            QVERIFY(!ConnectionXml::read(*rekeyed, &master));
        }
    }
    void customMasterResetPreservesAllSecretsAndFullMode() {
        const Secret master(QByteArray("Password")), defaultMaster(QByteArray("mR3m")), secret(QByteArray("synthetic reset credential"));
        const QString profile = QString::fromLatin1(Id), other = "1897a1fb-b702-45e9-bcae-884f59cc66c7";
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, secret, 4096); QVERIFY(cipher);
        const auto body = QByteArray(node()).replace("Password=\"\"", "Password=\"" + *cipher + "\" VNCProxyPassword=\"" + *cipher +
            "\" RDGatewayPassword=\"" + *cipher + "\" RDGatewayAccessToken=\"" + *cipher + '"') +
            QByteArray(node()).replace(Id, other.toLatin1()).replace("Password=\"\"", "Password=\"" + *cipher + '"');
        for (bool full : {false, true}) {
            auto document = ConnectionXml::read(xml(body, "2.7"), &master); QVERIFY(document);
            const auto target = document->committedAccount(profile); QVERIFY(target);
            QVERIFY(document->reviewPasswords(master, {{profile, *target}, {other, *target}}));
            QVERIFY(document->setFullEncryption(master, full));
            const auto old = document->serialize(master); QVERIFY(old);
            QVERIFY(document->passwordProtected() == true);
            auto canceled = document->clone(); QVERIFY(!canceled.resetMaster(master, [] { return true; }));
            QVERIFY(canceled.serialize(master)); QVERIFY(canceled.passwordProtected() == true);
            QVERIFY(!document->resetMaster(Secret(QByteArray("wrong"))));
            QVERIFY(document->resetMaster(master)); QVERIFY(document->passwordProtected() == false);
            const auto saved = document->serialize(defaultMaster); QVERIFY(saved); QVERIFY(!document->serialize(master));
            auto reopened = ConnectionXml::read(*saved, &defaultMaster); QVERIFY(reopened);
            QCOMPARE(reopened->fullEncryption(), full); QCOMPARE(reopened->version(), QString("2.7")); QCOMPARE(reopened->iterations(), quint32(1000));
            QCOMPARE(reopened->fileId(), document->fileId()); QVERIFY(reopened->passwordProtected() == false);
            const auto password = reopened->password(defaultMaster, profile, *target); QVERIFY(password); QVERIFY(bytes(*password) == bytes(secret));
            const auto canary = ConnectionCrypto::decrypt(defaultMaster, 1000, attribute(*saved, "Protected"), 4096); QVERIFY(canary);
            QVERIFY(bytes(*canary) == "ThisIsNotProtected");
            QByteArray readable = *saved;
            if (full) {
                QXmlStreamReader reader(*saved); QVERIFY(reader.readNextStartElement());
                const auto inner = ConnectionCrypto::decrypt(defaultMaster, 1000, reader.readElementText().toLatin1(), ConnectionXml::MaxInner); QVERIFY(inner);
                readable = QByteArray(reinterpret_cast<const char*>(inner->data()), qsizetype(inner->size()));
            }
            for (const auto& name : {"Password", "VNCProxyPassword", "RDGatewayPassword", "RDGatewayAccessToken"}) {
                const auto value = attribute(readable, name, profile); QVERIFY(value != *cipher);
                const auto plain = ConnectionCrypto::decrypt(defaultMaster, 1000, value, 4096); QVERIFY(plain); QVERIFY(bytes(*plain) == bytes(secret));
            }
            auto retained = ConnectionXml::read(*old, &master); QVERIFY(retained); QVERIFY(retained->password(master, profile, *target));
        }
    }
    void masterChangeReencryptsAllSecretKinds() {
        const Secret master(QByteArray("Password")), replacement(QByteArray("new synthetic file master"));
        const QString profile = QString::fromLatin1(Id), other = "1897a1fb-b702-45e9-bcae-884f59cc66c7", fileId = "f6595425-c35a-45b7-9dfc-84dd83e6b002";
        SessionTarget target; target.host = "localhost"; target.port = 3389;
        const auto password = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic rekey password")), 4096); QVERIFY(password);
        const auto oldHash = QCryptographicHash::hash(QByteArray::fromBase64(*password), QCryptographicHash::Sha256);
        const auto body = QByteArray(node()).replace("Password=\"\"", "Password=\"" + *password + "\" VNCProxyPassword=\"" + *password +
            "\" RDGatewayPassword=\"" + *password + "\" RDGatewayAccessToken=\"" + *password + '"') +
            QByteArray(node()).replace(Id, other.toLatin1()).replace("Password=\"\"", "Password=\"" + *password + '"');
        auto staleTarget = target; staleTarget.username = "stale-user";
        const auto bindings = ConnectionMetadata::encodeBindings({fileId, {{profile, profile, oldHash, target}, {other, other, oldHash, staleTarget}}}); QVERIFY(bindings);
        const auto bindingCipher = ConnectionCrypto::encrypt(master, 1000, *bindings, ConnectionMetadata::MaxPayload); QVERIFY(bindingCipher);
        const auto extensions = QByteArray("RmFormatVersion=\"1\" RmFileId=\"") + fileId.toLatin1() + "\" RmCredentialBindings=\"" + *bindingCipher + '"';
        for (bool full : {false, true}) {
            const auto encryptedBody = ConnectionCrypto::encrypt(master, 1000, Secret(body)); QVERIFY(encryptedBody);
            const auto original = xml(full ? *encryptedBody : body, "2.7", extensions, full);
            auto parsed = ConnectionXml::read(original, &master); QVERIFY(parsed);
            QVERIFY(parsed->password(master, profile, target)); QVERIFY(!parsed->password(master, other, staleTarget));
            QVERIFY(!parsed->changeMaster(master, Secret(QByteArray(" \t"))));
            QVERIFY(!parsed->changeMaster(Secret(QByteArray("wrong")), replacement));
            int totalChecks = 0;
            QVERIFY(parsed->changeMaster(master, replacement, [&] { ++totalChecks; return false; })); QVERIFY(totalChecks > 2);
            const auto saved = parsed->serialize(replacement); QVERIFY(saved); QVERIFY(!parsed->serialize(master));
            QVERIFY(!ConnectionXml::read(*saved, &master)); auto reopened = ConnectionXml::read(*saved, &replacement); QVERIFY(reopened);
            QCOMPARE(reopened->version(), QString("2.7")); QCOMPARE(reopened->iterations(), quint32(1000)); QCOMPARE(reopened->fullEncryption(), full);
            const auto value = reopened->password(replacement, profile, target); QVERIFY(value); QVERIFY(bytes(*value) == "synthetic rekey password");
            QVERIFY(!reopened->password(replacement, other, staleTarget));
            auto retainedCopy = ConnectionXml::read(original, &master); QVERIFY(retainedCopy); QVERIFY(retainedCopy->password(master, profile, target));
            QVERIFY(attribute(*saved, "Protected") != attribute(original, "Protected"));
            QVERIFY(attribute(*saved, "RmCredentialBindings") != *bindingCipher);
            const auto newBindingsPayload = ConnectionCrypto::decrypt(replacement, 1000, attribute(*saved, "RmCredentialBindings"), ConnectionMetadata::MaxPayload); QVERIFY(newBindingsPayload);
            const auto newBindings = ConnectionMetadata::decodeBindings(*newBindingsPayload, fileId); QVERIFY(newBindings);
            for (const auto& record : newBindings->records) {
                if (record.consumer == other) QCOMPARE(record.passwordHash, oldHash);
                else QVERIFY(record.passwordHash != oldHash);
            }
            std::optional<Secret> inner;
            if (full) {
                QXmlStreamReader reader(*saved); QVERIFY(reader.readNextStartElement());
                const auto cipher = reader.readElementText().toLatin1(); QVERIFY(cipher != *encryptedBody);
                inner = ConnectionCrypto::decrypt(replacement, 1000, cipher, ConnectionXml::MaxInner); QVERIFY(inner);
            }
            const auto readable = full ? bytes(*inner) : *saved;
            for (const auto& name : {"Password", "VNCProxyPassword", "RDGatewayPassword", "RDGatewayAccessToken"}) {
                const auto cipher = attribute(readable, name, profile); QVERIFY(!cipher.isEmpty() && cipher != *password);
                const auto raw = QByteArray::fromBase64(cipher), old = QByteArray::fromBase64(*password);
                QVERIFY(raw.left(16) != old.left(16)); QVERIFY(raw.mid(16, 16) != old.mid(16, 16));
                const auto plain = ConnectionCrypto::decrypt(replacement, 1000, cipher, 4096); QVERIFY(plain); QVERIFY(bytes(*plain) == "synthetic rekey password");
                QVERIFY(!ConnectionCrypto::decrypt(master, 1000, cipher, 4096));
            }
            auto canceled = ConnectionXml::read(original, &master); QVERIFY(canceled); int checks = 0;
            QVERIFY(!canceled->changeMaster(master, replacement, [&] { return ++checks >= totalChecks - 1; }));
            QVERIFY(canceled->serialize(master)); QVERIFY(!canceled->serialize(replacement)); QVERIFY(canceled->password(master, profile, target));
        }
        for (const auto& unknown : {xml(QByteArray(body).replace("Name=\"fixture\"", "Name=\"fixture\" UnknownSecret=\"opaque\"")),
             xml(body + "<PluginData>unknown secret semantics</PluginData>"), xml(body, "2.8", "q:Secret=\"opaque\" xmlns:q=\"urn:fixture\"")}) {
            auto parsed = ConnectionXml::read(unknown, &master); QVERIFY(parsed); const auto before = parsed->serialize(master); QVERIFY(before);
            QVERIFY(!parsed->changeMaster(master, replacement)); const auto after = parsed->serialize(master); QVERIFY(after); QCOMPARE(*after, *before);
        }
    }
    void inheritedBindingsRequireActualOwnerAndCommittedAccount() {
        const Secret master(QByteArray("Password"));
        const QString profile = QString::fromLatin1(Id), folder = "1897a1fb-b702-45e9-bcae-884f59cc66c7", fileId = "f6595425-c35a-45b7-9dfc-84dd83e6b002";
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("synthetic inherited password")), 4096); QVERIFY(cipher);
        const auto body = QByteArray("<Node Type=\"Container\" Id=\"") + folder.toLatin1() + "\" Username=\"folder-user\" Domain=\"EXAMPLE\" Port=\"3390\" Protocol=\"RDP\" Password=\"" + *cipher +
            "\" InheritUsername=\"true\" InheritDomain=\"true\" InheritPort=\"true\" InheritPassword=\"true\"><Node Type=\"Connection\" Id=\"" + Id +
            "\" Hostname=\"localhost\" Username=\"local-user\" Domain=\"LOCAL\" Port=\"3389\" Protocol=\"VNC\" InheritUsername=\"true\" InheritDomain=\"true\" InheritPort=\"true\" InheritProtocol=\"true\" InheritPassword=\"true\"/></Node>";
        SessionTarget target; target.host = "localhost"; target.port = 3390; target.username = "folder-user"; target.domain = "EXAMPLE";
        ConnectionBindings reviewed{fileId, {{profile, folder,
            QCryptographicHash::hash(QByteArray::fromBase64(*cipher), QCryptographicHash::Sha256), target}}};
        auto parsed = ConnectionXml::read(xml(body, "2.8", "Username=\"root-user\" Domain=\"ROOT\" Port=\"1\""), &master); QVERIFY(parsed);
        auto wrongOwner = reviewed; wrongOwner.records.front().owner = profile; QVERIFY(!parsed->reviewCredentials(master, wrongOwner));
        QVERIFY(parsed->reviewCredentials(master, reviewed)); const auto value = parsed->password(master, profile, target);
        QVERIFY(value); QVERIFY(bytes(*value) == "synthetic inherited password");
        QVERIFY(!parsed->password(master, folder, target));
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        auto changedOwner = ConnectionXml::read(QByteArray(*saved).replace("InheritPassword=\"true\"/>", "InheritPassword=\"false\" Password=\"" + *cipher + "\"/>"), &master); QVERIFY(changedOwner);
        QVERIFY(!changedOwner->password(master, profile, target));
        auto changedAccount = ConnectionXml::read(QByteArray(*saved).replace("InheritUsername=\"true\"", "InheritUsername=\"false\""), &master); QVERIFY(changedAccount);
        QVERIFY(!changedAccount->password(master, profile, target));
        auto local = target; local.username = "local-user"; QVERIFY(!changedAccount->password(master, profile, local));
        auto malformed = ConnectionXml::read(QByteArray(*saved).replace("InheritPassword=\"true\"/>", "InheritPassword=\"invalid\"/>"), &master); QVERIFY(malformed);
        QVERIFY(!malformed->password(master, profile, target));
        auto inheritedRekey = ConnectionXml::read(xml(body), &master); QVERIFY(inheritedRekey); QVERIFY(inheritedRekey->reviewCredentials(master, reviewed));
        const Secret replacement(QByteArray("replacement inherited file master"));
        QVERIFY(inheritedRekey->changeMaster(master, replacement));
        const auto rekeyed = inheritedRekey->password(replacement, profile, target); QVERIFY(rekeyed); QVERIFY(bytes(*rekeyed) == bytes(*value));
        QVERIFY(!inheritedRekey->password(master, profile, target));
    }
    void privateXmlExtensionsRejectPartialInvalidAndUnauthenticatedPayloads() {
        const Secret master(QByteArray("Password"));
        const QByteArray fileId("f6595425-c35a-45b7-9dfc-84dd83e6b002"), otherId("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        const auto payload = ConnectionMetadata::encodeBindings({QString::fromLatin1(fileId), {}}); QVERIFY(payload);
        const auto cipher = ConnectionCrypto::encrypt(master, 1000, *payload, ConnectionMetadata::MaxPayload); QVERIFY(cipher);
        const auto extensions = QByteArray("RmFormatVersion=\"1\" RmFileId=\"") + fileId + "\" RmCredentialBindings=\"" + *cipher + '"';
        auto valid = ConnectionXml::read(xml(node(), "2.8", extensions), &master); QVERIFY(valid);
        const auto saved = valid->serialize(master); QVERIFY(saved); QVERIFY(saved->contains(*cipher));
        for (const auto& extra : {QByteArray("RmFormatVersion=\"1\""), QByteArray("RmFileId=\"") + fileId + '"',
             QByteArray("RmCredentialBindings=\"") + *cipher + '"', QByteArray("RmUnknownSecret=\"opaque\""),
             QByteArray(extensions).replace("RmFormatVersion=\"1\"", "RmFormatVersion=\"4\""),
             QByteArray(extensions).replace(fileId, "invalid"), QByteArray(extensions).replace(*cipher, ""),
             QByteArray(extensions).replace(*cipher, "!!!"), extensions + " RmUnknownSecret=\"\""})
            QVERIFY(!ConnectionXml::read(xml(node(), "2.8", extra)));
        const auto mismatch = xml(node(), "2.8", QByteArray(extensions).replace("RmFormatVersion=\"1\"", "RmFormatVersion=\"2\""));
        QVERIFY(!ConnectionXml::read(mismatch, &master));
        auto lockedMismatch = ConnectionXml::read(mismatch); QVERIFY(lockedMismatch);
        QVERIFY(!lockedMismatch->unlock(master)); QVERIFY(!lockedMismatch->serialize(master));
        QVERIFY(!lockedMismatch->passwordProtected().has_value());
        QVERIFY(!ConnectionXml::read(xml(node(), "2.8", QByteArray(extensions).replace(fileId, otherId)), &master));
        auto damaged = QByteArray::fromBase64(*cipher); damaged.back() = char(damaged.back() ^ 1);
        const auto badExtensions = QByteArray(extensions).replace(*cipher, damaged.toBase64());
        QVERIFY(!ConnectionXml::read(xml(node(), "2.8", badExtensions), &master));
        const auto encryptedBody = ConnectionCrypto::encrypt(master, 1000, Secret(node())); QVERIFY(encryptedBody);
        auto full = ConnectionXml::read(xml(*encryptedBody, "2.8", badExtensions, true)); QVERIFY(full);
        QVERIFY(!full->unlock(master)); QVERIFY(full->needsUnlock()); QVERIFY(full->profileIds().isEmpty());
        const auto malformed = ConnectionCrypto::encrypt(master, 1000, Secret(QByteArray("invalid binary")), ConnectionMetadata::MaxPayload); QVERIFY(malformed);
        QVERIFY(!ConnectionXml::read(xml(node(), "2.8", QByteArray(extensions).replace(*cipher, *malformed)), &master));
        const auto oversized = QByteArray(ConnectionMetadata::MaxPayload + 49, '\0').toBase64();
        QVERIFY(!ConnectionXml::read(xml(node(), "2.8", QByteArray(extensions).replace(*cipher, oversized))));
        // Removing every private attribute reproduces a legitimate native import.
        auto stripped = ConnectionXml::read(xml(node()), &master); QVERIFY(stripped); QVERIFY(stripped->credentialReviewRequired());
    }
    void xmlValueTamperingAndLimitsReject() {
        const Secret pass(QByteArray("Password")); const QByteArray value(Original[3].cipher);
        QVERIFY(ConnectionCrypto::validEnvelope(value));
        auto raw = QByteArray::fromBase64(value);
        for (qsizetype i = 0; i < raw.size(); ++i) {
            auto damaged = raw; damaged[i] = char(damaged[i] ^ 1);
            QVERIFY(!ConnectionCrypto::decrypt(pass, 1000, damaged.toBase64()));
        }
        QVERIFY(!ConnectionCrypto::decrypt(Secret(QByteArray("incorrect")), 1000, value));
        QVERIFY(!ConnectionCrypto::decrypt(pass, 5000, value));
        for (const auto& bad : {QByteArray("!!!"), value + "=", QByteArray(47, 'a').toBase64(), value.left(value.size() - 1)}) {
            QVERIFY(!ConnectionCrypto::validEnvelope(bad)); QVERIFY(!ConnectionCrypto::decrypt(pass, 1000, bad));
        }
        QVERIFY(!ConnectionCrypto::validEnvelope(value, 14));
        QVERIFY(!ConnectionCrypto::decrypt(pass, 1000, value, 14));
        QVERIFY(!ConnectionCrypto::validEnvelope(value, -1));
        QVERIFY(!ConnectionCrypto::validEnvelope(value, ConnectionCrypto::MaxPayload + 1));
        QVERIFY(ConnectionCrypto::validEnvelope({}));
        const auto spaced = value.left(20) + " \r\n\t" + value.mid(20);
        const auto decoded = ConnectionCrypto::decrypt(pass, 1000, spaced); QVERIFY(decoded); QVERIFY(bytes(*decoded) == "ThisIsProtected");
        QVERIFY(!ConnectionCrypto::validEnvelope(QByteArray(ConnectionCrypto::MaxPayload * 2, 'A')));
    }
};
QTEST_GUILESS_MAIN(ConnectionXmlTest)
#include "connection_xml.moc"
