#include "DocumentStore.h"
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QXmlStreamReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include "remotemanager-core/src/bridge.rs.h"

static Secret master() { return Secret(QByteArray("synthetic document master")); }
struct TestBinding { QString profile; SessionTarget target; };
static TestBinding binding() { return {"e6683f1f-4e06-4200-9a17-89cd30c68b12", {"localhost", 3389, "profile-user"}}; }
static QByteArray unsupported() { return "unsupported connection content"; }
static QByteArray contents(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
static bool replace(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
static QByteArray bytes(const Secret& value) { return QByteArray(reinterpret_cast<const char*>(value.data()), qsizetype(value.size())); }

static QByteArray xmlPasswordCipher(const ConnectionXml& xml, const Secret& pass, const QString& profile = binding().profile) {
    const auto saved = xml.serialize(pass); if (!saved) return {};
    QXmlStreamReader reader(*saved); if (!reader.readNextStartElement()) return {};
    std::optional<Secret> inner;
    if (xml.fullEncryption()) {
        inner = ConnectionCrypto::decrypt(pass, xml.iterations(), reader.readElementText().toLatin1(), ConnectionXml::MaxInner);
        if (!inner) return {};
        reader.clear(); reader.addData(QByteArray::fromRawData(reinterpret_cast<const char*>(inner->data()), qsizetype(inner->size())));
    }
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "Node" && reader.attributes().value("Id") == profile)
            return reader.attributes().value("Password").toLatin1();
    }
    return {};
}
static bool reviewXmlPassword(ConnectionXml& xml, const Secret& pass, const QString& profile = binding().profile) {
    const auto cipher = xmlPasswordCipher(xml, pass, profile); if (cipher.isEmpty()) return false;
    const auto account = binding();
    ConnectionBindings reviewed{xml.fileId().isEmpty() ? QString("f6595425-c35a-45b7-9dfc-84dd83e6b002") : xml.fileId(),
        {{profile, profile, QCryptographicHash::hash(QByteArray::fromBase64(cipher), QCryptographicHash::Sha256), account.target}}};
    return xml.reviewCredentials(pass, reviewed);
}
static QByteArray xmlFixture(bool full = false) {
    const auto pass = master();
    const auto password = ConnectionCrypto::encrypt(pass, 1000, Secret(QByteArray("prior synthetic XML password")), 4096);
    const auto canary = ConnectionCrypto::encrypt(pass, 1000, Secret(QByteArray("ThisIsProtected")), 4096);
    if (!password || !canary) return {};
    const auto body = QByteArray("<Node Type=\"Connection\" Id=\"") + binding().profile.toLatin1() +
        "\" Name=\"fixture\" Protocol=\"RDP\" Hostname=\"localhost\" Port=\"3389\" Username=\"profile-user\" Password=\"" + *password + '"' + "/>";
    const auto encrypted = ConnectionCrypto::encrypt(pass, 1000, Secret(body)); if (!encrypted) return {};
    auto parsed = ConnectionXml::read(QByteArray("<Connections Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"") +
        (full ? "true" : "false") + "\" Protected=\"" + *canary + "\">" + (full ? *encrypted : body) + "</Connections>", &pass);
    if (!parsed || !reviewXmlPassword(*parsed, pass)) return {};
    const auto saved = parsed->serialize(pass); return saved ? *saved : QByteArray();
}

class DocumentTest : public QObject {
    Q_OBJECT
private slots:
    void newXmlUsesCompatibleDefaultsWithoutStartupWrite() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml");
        DocumentStore store(path); const auto read = store.loadXml(); QVERIFY(read.result.ok); QVERIFY(!read.exists); QVERIFY(store.xml());
        QCOMPARE(store.xml()->version(), QString("2.8")); QCOMPARE(store.xml()->iterations(), ConnectionCrypto::NewIterations);
        QVERIFY(!store.xml()->fullEncryption()); QVERIFY(store.xml()->profileIds().isEmpty()); QVERIFY(!QFile::exists(path));
        const Secret defaultMaster(QByteArray("mR3m")); QVERIFY(store.updateXml(defaultMaster).ok); QVERIFY(QFile::exists(path));
        auto parsed = ConnectionXml::read(contents(path), &defaultMaster); QVERIFY(parsed);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList{"confCons.xml"});
        DocumentStore canceled(dir.filePath("canceled.xml")); QVERIFY(!canceled.loadXml(nullptr, [] { return true; }).result.ok);
        QVERIFY(!canceled.xml()); QVERIFY(!QFile::exists(canceled.path()));
    }
    void singleFileReplaceFailurePreservesPriorPassword() {
        for (bool full : {false, true}) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml"); const auto original = xmlFixture(full);
            QVERIFY(!original.isEmpty()); QVERIFY(replace(path, original)); const auto pass = master();
            const Secret replacement(QByteArray("replacement synthetic XML password"));
            DocumentStore store(path); const auto read = store.loadXml(&pass); QVERIFY(read.result.ok && read.exists); QVERIFY(store.xml());
            QCOMPARE(store.xml()->fullEncryption(), full); QCOMPARE(store.xml()->fullEncryption(), full);
            const auto edit = [&](ConnectionXml& candidate) { return candidate.setPassword(pass, binding().profile, replacement) && reviewXmlPassword(candidate, pass); };
            const auto before = store.xml()->password(pass, binding().profile, binding().target); QVERIFY(before); QVERIFY(bytes(*before) == "prior synthetic XML password");
            store.setFault(AtomicFile::Fault::BeforeReplace); const auto failed = store.updateXml(pass, edit); QVERIFY(!failed.ok); QCOMPARE(contents(path), original);
            const auto retained = store.xml()->password(pass, binding().profile, binding().target); QVERIFY(retained); QVERIFY(bytes(*retained) == bytes(*before));
            QVERIFY(!store.updateXml(Secret(QByteArray("wrong master")), edit).ok); QCOMPARE(contents(path), original);
            store.setFault(AtomicFile::Fault::None); const auto saved = store.updateXml(pass, edit); QVERIFY(saved.ok); QVERIFY(saved.warning.isEmpty());
            const auto current = store.xml()->password(pass, binding().profile, binding().target); QVERIFY(current); QVERIFY(bytes(*current) == bytes(replacement));
            DocumentStore portable(path); QVERIFY(portable.loadXml(&pass).result.ok);
            const auto reopened = portable.xml()->password(pass, binding().profile, binding().target); QVERIFY(reopened); QVERIFY(bytes(*reopened) == bytes(replacement));
            for (const auto& secret : {QByteArray("prior synthetic XML password"), QByteArray("replacement synthetic XML password"), QByteArray("synthetic document master")})
                QVERIFY(!contents(path).contains(secret));
            QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList{"confCons.xml"});
#ifdef Q_OS_UNIX
            const auto permissions = QFile::permissions(path); QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
#endif
        }
    }
    void xmlCommitWarningAndLateCancelAdoptOwningCandidate() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); const auto original = xmlFixture(); QVERIFY(replace(path, original));
        const auto pass = master(); const Secret replacement(QByteArray("late committed XML password")); DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok);
        const auto edit = [&](ConnectionXml& candidate) { return candidate.setPassword(pass, binding().profile, replacement) && reviewXmlPassword(candidate, pass); };
        bool canceled = false; store.setFault(AtomicFile::Fault::DirectorySync);
        store.setCommitGate([&](const auto& commit) { const auto committed = commit(); canceled = true; return committed; });
        const auto result = store.updateXml(pass, edit, nullptr, [&] { return canceled; }); QVERIFY(result.ok); QVERIFY(!result.warning.isEmpty()); QVERIFY(canceled);
        const auto current = store.xml()->password(pass, binding().profile, binding().target); QVERIFY(current); QVERIFY(bytes(*current) == bytes(replacement));
        auto reopened = ConnectionXml::read(contents(path), &pass); QVERIFY(reopened);
        const auto saved = reopened->password(pass, binding().profile, binding().target); QVERIFY(saved); QVERIFY(bytes(*saved) == bytes(replacement));
    }
    void xmlCommittedProjectionFinalizesOwningCore() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml");
        const Secret defaultMaster(QByteArray("mR3m")); DocumentStore store(path); QVERIFY(store.loadXml().result.ok);
        const auto initial = store.xml()->projection(); QVERIFY(initial);
        auto first = rm::detached_core(), second = rm::detached_core();
        first->load_document(rust::Str(initial->constData(), size_t(initial->size()))); second->load_document(rust::Str(initial->constData(), size_t(initial->size())));
        const auto before = first->snapshot(), other = second->snapshot();
        const auto request = QByteArray("{\"name\":\"created\",\"kind\":\"connection\",\"parent\":null,\"overrides\":{\"host\":\"localhost\",\"port\":3389,\"protocol\":\"rdp\",\"username\":\"\"}}");
        auto failed = first->prepare_profile(rm::ProfileChange::Create, rust::Str(request.constData(), size_t(request.size())));
        store.setFault(AtomicFile::Fault::BeforeReplace);
        QVERIFY(!store.updateXml(defaultMaster, [&](ConnectionXml& candidate) { return bool(candidate.patch(defaultMaster, QByteArray(failed.json.data(), qsizetype(failed.json.size())))); }).ok);
        QVERIFY(!QFile::exists(path)); QCOMPARE(first->snapshot(), before); QCOMPARE(store.xml()->projection(), initial);
        first->finish_profile(failed.token, false);
        auto prepared = first->prepare_profile(rm::ProfileChange::Create, rust::Str(request.constData(), size_t(request.size())));
        bool canceled = false; store.setFault(AtomicFile::Fault::DirectorySync);
        store.setCommitGate([&](const auto& commit) { const auto committed = commit(); canceled = true; return committed; });
        const auto saved = store.updateXml(defaultMaster, [&](ConnectionXml& candidate) { return bool(candidate.patch(defaultMaster, QByteArray(prepared.json.data(), qsizetype(prepared.json.size())))); }, nullptr, [&] { return canceled; });
        QVERIFY(saved.ok); QVERIFY(!saved.warning.isEmpty()); QVERIFY(canceled); QCOMPARE(first->snapshot(), before);
        const auto normalized = store.xml()->projection(); QVERIFY(normalized);
        const auto normalizedProfile = QJsonDocument::fromJson(*normalized).object().value("profiles").toArray()[0].toObject(); QVERIFY(normalizedProfile.value("native").isObject());
        first->finish_document(prepared.token, rust::Str(normalized->constData(), size_t(normalized->size())));
        const auto committed = first->stored_document();
        QCOMPARE(QJsonDocument::fromJson(QByteArray(committed.data(), qsizetype(committed.size()))), QJsonDocument::fromJson(*normalized));
        QCOMPARE(second->snapshot(), other); auto reopened = ConnectionXml::read(contents(path), &defaultMaster); QVERIFY(reopened); QCOMPARE(reopened->projection(), normalized);
        QVERIFY_THROWS_EXCEPTION(std::exception, first->finish_document(prepared.token, rust::Str(normalized->constData(), size_t(normalized->size()))));
        const auto session = first->open_session(prepared.id, true); QCOMPARE(first->session_count(), size_t(1));
        const auto protection = first->prepare_document();
        first->finish_document(protection.token, rust::Str(normalized->constData(), size_t(normalized->size()))); // file-only terminal adoption works with an open saved tab
        QCOMPARE(first->session_count(), size_t(1)); first->close_session(session.id);
    }
    void xmlPatchInheritanceAndProtectionAdoptOnlyCommittedCandidate() {
        const auto pass = master(); const QString folder("1897a1fb-b702-45e9-bcae-884f59cc66c7");
        for (bool full : {false, true}) for (int operation = 0; operation < 3; ++operation) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("confCons.xml");
            auto fixture = ConnectionXml::read(xmlFixture(full), &pass); QVERIFY(fixture);
            const auto initial = fixture->projection(); QVERIFY(initial); auto json = QJsonDocument::fromJson(*initial).object();
            auto profiles = json.value("profiles").toArray(); auto child = profiles[0].toObject(); child.insert("parent", folder);
            profiles = QJsonArray{QJsonObject{{"id", folder}, {"name", "folder"}, {"kind", "folder"}, {"parent", QJsonValue(QJsonValue::Null)}, {"overrides", QJsonObject{}}}, child};
            json.insert("profiles", profiles); QVERIFY(fixture->patch(pass, QJsonDocument(json).toJson(QJsonDocument::Compact)));
            QVERIFY(fixture->setPassword(pass, folder, Secret(QByteArray("synthetic folder password"))));
            const auto original = fixture->serialize(pass); QVERIFY(original); QVERIFY(replace(path, *original));
            DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok); const auto before = store.xml()->projection(); QVERIFY(before);
            const auto cipher = xmlPasswordCipher(*store.xml(), pass); QVERIFY(!cipher.isEmpty());
            const auto parentCipher = xmlPasswordCipher(*store.xml(), pass, folder); QVERIFY(!parentCipher.isEmpty());
            QVERIFY(store.xml()->password(pass, binding().profile, binding().target));
            std::optional<QByteArray> normalized;
            const auto edit = [&](ConnectionXml& candidate) {
                if (operation == 0) {
                    auto changed = QJsonDocument::fromJson(*before).object(); auto entries = changed.value("profiles").toArray();
                    auto profile = entries[1].toObject(); auto overrides = profile.value("overrides").toObject(); overrides.insert("username", "changed-user");
                    profile.insert("overrides", overrides); entries[1] = profile; changed.insert("profiles", entries);
                    normalized = candidate.patch(pass, QJsonDocument(changed).toJson(QJsonDocument::Compact)); return normalized.has_value();
                }
                const bool ok = operation == 1 ? candidate.setPasswordInheritance(pass, binding().profile, true) : candidate.setFullEncryption(pass, !full);
                if (ok) normalized = candidate.projection(); return ok && normalized.has_value();
            };
            store.setFault(AtomicFile::Fault::BeforeReplace); QVERIFY(!store.updateXml(pass, edit).ok);
            QCOMPARE(contents(path), *original); QCOMPARE(store.xml()->projection(), before); QCOMPARE(store.xml()->fullEncryption(), full);
            QCOMPARE(xmlPasswordCipher(*store.xml(), pass), cipher); QVERIFY(store.xml()->password(pass, binding().profile, binding().target));
            bool canceled = false; store.setFault(AtomicFile::Fault::None);
            store.setCommitGate([&](const auto& commit) { canceled = true; return commit(); });
            QVERIFY(!store.updateXml(pass, edit, nullptr, [&] { return canceled; }).ok); QCOMPARE(contents(path), *original);
            QCOMPARE(store.xml()->projection(), before); QVERIFY(store.xml()->password(pass, binding().profile, binding().target));
            canceled = false; store.setFault(AtomicFile::Fault::DirectorySync);
            store.setCommitGate([&](const auto& commit) { const auto committed = commit(); canceled = true; return committed; });
            const auto result = store.updateXml(pass, edit, nullptr, [&] { return canceled; }); QVERIFY(result.ok); QVERIFY(!result.warning.isEmpty()); QVERIFY(canceled);
            QCOMPARE(store.xml()->projection(), normalized); QCOMPARE(store.xml()->fullEncryption(), operation == 2 ? !full : full);
            QCOMPARE(xmlPasswordCipher(*store.xml(), pass), cipher); QCOMPARE(xmlPasswordCipher(*store.xml(), pass, folder), parentCipher);
            QCOMPARE(bool(store.xml()->password(pass, binding().profile, binding().target)), operation == 2);
            auto reopened = ConnectionXml::read(contents(path), &pass); QVERIFY(reopened); QCOMPARE(reopened->projection(), normalized);
            QCOMPARE(reopened->fullEncryption(), store.xml()->fullEncryption()); QCOMPARE(xmlPasswordCipher(*reopened, pass), cipher);
            QCOMPARE(bool(reopened->password(pass, binding().profile, binding().target)), operation == 2);
            for (const auto& secret : {QByteArray("prior synthetic XML password"), QByteArray("synthetic folder password"), QByteArray("synthetic document master")}) {
                QVERIFY(!contents(path).contains(secret)); QVERIFY(!normalized->contains(secret));
            }
            QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList{"confCons.xml"});
        }
    }
    void xmlRevisionAndCanceledGateNeverPublishCandidate() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); const auto original = xmlFixture(); QVERIFY(replace(path, original));
        const auto pass = master(); const Secret replacement(QByteArray("uncommitted XML password")); DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok);
        const auto edit = [&](ConnectionXml& candidate) { return candidate.setPassword(pass, binding().profile, replacement) && reviewXmlPassword(candidate, pass); };
        bool canceled = false; store.setCommitGate([&](const auto& commit) { canceled = true; return commit(); });
        QVERIFY(!store.updateXml(pass, edit, nullptr, [&] { return canceled; }).ok); QCOMPARE(contents(path), original);
        const auto retained = store.xml()->password(pass, binding().profile, binding().target); QVERIFY(retained); QVERIFY(bytes(*retained) == "prior synthetic XML password");
        const auto foreign = original + ' ';
        store.setCommitGate([&](const auto& commit) { if (!replace(path, foreign)) return false; return commit(); });
        QVERIFY(!store.updateXml(pass, edit).ok); QCOMPARE(contents(path), foreign);
        int edits = 0; QVERIFY(!store.updateXml(pass, [&](ConnectionXml&) { ++edits; return true; }).ok); QCOMPARE(edits, 0);
        // A failed reload must not bless a new revision while keeping old metadata.
        const Secret wrong(QByteArray("wrong master")); QVERIFY(!store.loadXml(&wrong).result.ok); QVERIFY(store.xml());
        QVERIFY(!store.updateXml(pass, [&](ConnectionXml&) { ++edits; return true; }).ok); QCOMPARE(edits, 0); QCOMPARE(contents(path), foreign);
    }
    void xmlRekeyPublishesOnlyAtReplacement() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); const auto original = xmlFixture(true); QVERIFY(replace(path, original));
        const auto pass = master(); const Secret replacement(QByteArray("replacement synthetic XML master")); DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok);
        const auto edit = [&](ConnectionXml& candidate) { return candidate.changeMaster(pass, replacement); };
        store.setFault(AtomicFile::Fault::BeforeReplace); QVERIFY(!store.updateXml(pass, edit, &replacement).ok); QCOMPARE(contents(path), original);
        QVERIFY(store.xml()->password(pass, binding().profile, binding().target)); QVERIFY(!store.xml()->password(replacement, binding().profile, binding().target));
        store.setFault(AtomicFile::Fault::DirectorySync); const auto result = store.updateXml(pass, edit, &replacement); QVERIFY(result.ok); QVERIFY(!result.warning.isEmpty());
        QVERIFY(!store.xml()->password(pass, binding().profile, binding().target)); QVERIFY(store.xml()->password(replacement, binding().profile, binding().target));
        auto reopened = ConnectionXml::read(contents(path), &replacement); QVERIFY(reopened); QVERIFY(reopened->password(replacement, binding().profile, binding().target));
        QVERIFY(!ConnectionXml::read(contents(path), &pass)); QVERIFY(ConnectionXml::read(original, &pass));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot), QStringList{"confCons.xml"});
    }
    void xmlPasswordEditRejectsInvalidInputAndClearIsExplicit() {
        const auto pass = master(); auto parsed = ConnectionXml::read(xmlFixture(), &pass); QVERIFY(parsed); const auto original = parsed->serialize(pass); QVERIFY(original);
        const auto valid = Secret(QByteArray("candidate XML password"));
        QVERIFY(!parsed->setPassword(pass, binding().profile, Secret(QByteArray(4097, 'a'))));
        QVERIFY(!parsed->setPassword(pass, binding().profile, Secret(QByteArray::fromHex("c080"))));
        QVERIFY(!parsed->setPassword(pass, "missing", valid));
        QVERIFY(!parsed->setPassword(Secret(QByteArray("wrong")), binding().profile, valid));
        QVERIFY(!parsed->setPassword(pass, binding().profile, valid, [] { return true; }));
        const auto unchanged = parsed->serialize(pass); QVERIFY(unchanged); QCOMPARE(*unchanged, *original);
        auto candidate = parsed->clone(); QVERIFY(candidate.setPassword(pass, binding().profile, valid));
        QVERIFY(!candidate.password(pass, binding().profile, binding().target)); // stale ciphertext binding cannot authorize replacement
        const auto retained = parsed->password(pass, binding().profile, binding().target); QVERIFY(retained); QVERIFY(bytes(*retained) == "prior synthetic XML password");
        QVERIFY(reviewXmlPassword(candidate, pass)); QVERIFY(candidate.password(pass, binding().profile, binding().target));
        QVERIFY(candidate.setPassword(pass, binding().profile, Secret())); QVERIFY(!candidate.password(pass, binding().profile, binding().target));
        const auto cleared = candidate.serialize(pass); QVERIFY(cleared); QVERIFY(cleared->contains("Password=\"\"")); QVERIFY(cleared->contains("InheritPassword=\"false\""));
        QVERIFY(parsed->password(pass, binding().profile, binding().target));
        const Secret defaultMaster(QByteArray("mR3m")); auto empty = ConnectionXml::create(); QVERIFY(empty);
        const auto newXml = empty->serialize(defaultMaster); QVERIFY(newXml);
        auto body = *newXml; // insert one password-free native node into the empty factory root
        const auto end = body.lastIndexOf("/>"); QVERIFY(end >= 0);
        body = body.left(end) + "><Node xmlns=\"\" Type=\"Connection\" Id=\"" + binding().profile.toLatin1() + "\" Name=\"empty\" Password=\"\"/></Connections>";
        auto defaultXml = ConnectionXml::read(body, &defaultMaster); QVERIFY(defaultXml);
        QVERIFY(defaultXml->setPassword(defaultMaster, binding().profile, valid));
        const auto encrypted = defaultXml->serialize(defaultMaster); QVERIFY(encrypted && !encrypted->contains("candidate XML password"));
        QVERIFY(defaultXml->setPassword(defaultMaster, binding().profile, Secret()));
    }
    void xmlRepairConsentKeepsIdentityAcrossFailedReplacement() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml");
        const auto original = QByteArray(xmlFixture()).replace(" Id=\"" + binding().profile.toLatin1() + '"', ""); QVERIFY(replace(path, original));
        const auto pass = master(); DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok); QVERIFY(store.xml()->requiresIdRepair());
        const auto ids = store.xml()->profileIds(); QCOMPARE(ids.size(), 1); QVERIFY(!store.updateXml(pass).ok); QCOMPARE(contents(path), original);
        const auto repair = [&](ConnectionXml& candidate) { return candidate.authorizeIdRepair() && reviewXmlPassword(candidate, pass, ids.front()); };
        store.setFault(AtomicFile::Fault::BeforeReplace); QVERIFY(!store.updateXml(pass, repair).ok); QCOMPARE(contents(path), original);
        QCOMPARE(store.xml()->profileIds(), ids); QVERIFY(store.xml()->requiresIdRepair());
        store.setFault(AtomicFile::Fault::None); QVERIFY(store.updateXml(pass, repair).ok); QCOMPARE(store.xml()->profileIds(), ids); QVERIFY(!store.xml()->requiresIdRepair());
        auto reopened = ConnectionXml::read(contents(path), &pass); QVERIFY(reopened); QCOMPARE(reopened->profileIds(), ids);
        QVERIFY(reopened->password(pass, ids.front(), binding().target));
    }
    void xmlLoadNeverFallsBackOrChangesExistingBytes() {
        QTemporaryDir dir; const auto path = dir.filePath("confCons.xml"); const auto pass = master();
        for (const auto& bad : {unsupported(), QByteArray("<Connections/>"), QByteArray(ConnectionXml::MaxFile + 1, ' ')}) {
            QVERIFY(replace(path, bad)); DocumentStore store(path); QVERIFY(!store.loadXml(&pass).result.ok); QVERIFY(!store.xml());
            QVERIFY(!store.updateXml(pass).ok); QCOMPARE(contents(path), bad);
        }
        const auto original = xmlFixture(); QVERIFY(replace(path, original)); DocumentStore store(path); QVERIFY(store.loadXml(&pass).result.ok);
        const Secret wrong(QByteArray("wrong master")); QVERIFY(!store.loadXml(&wrong).result.ok); QVERIFY(store.xml()->password(pass, binding().profile, binding().target));
        QVERIFY(!store.loadXml(&pass, [] { return true; }).result.ok); QVERIFY(store.xml()->password(pass, binding().profile, binding().target)); QCOMPARE(contents(path), original);
        const auto copy = dir.filePath("portable.xml"); QVERIFY(QFile::copy(path, copy)); DocumentStore portable(copy); QVERIFY(portable.loadXml(&pass).result.ok);
        const auto password = portable.xml()->password(pass, binding().profile, binding().target); QVERIFY(password); QVERIFY(bytes(*password) == "prior synthetic XML password");
#ifdef Q_OS_UNIX
        const auto alias = dir.filePath("alias.xml"); QVERIFY(QFile::link(path, alias)); DocumentStore unsafe(alias); QVERIFY(!unsafe.loadXml(&pass).result.ok); QCOMPARE(contents(path), original);
#endif
    }
};
QTEST_GUILESS_MAIN(DocumentTest)
#include "document_store.moc"
