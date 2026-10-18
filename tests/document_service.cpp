#include "DocumentService.h"
#include "DocumentContext.h"
#include <QCryptographicHash>
#include <QSemaphore>
#include <QXmlStreamReader>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
const QString Profile = "e6683f1f-4e06-4200-9a17-89cd30c68b12";
Secret masterA() { return Secret(QByteArray("synthetic file master A")); }
Secret masterB() { return Secret(QByteArray("synthetic file master B")); }
QByteArray contents(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& value) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(value) == value.size(); }
QByteArray fixture(const Secret& master, const QString& host, bool full = false, const QByteArray& password = {}) {
    auto xml = ConnectionXml::create(); const Secret initial(QByteArray("mR3m"));
    if (!xml || !xml->unlock(initial) ||
        (ConnectionCrypto::validNewMaster(master) && !xml->changeMaster(initial, master))) return {};
    const QJsonObject profile{{"id", Profile}, {"name", "Same UUID"}, {"kind", "connection"}, {"parent", QJsonValue()},
        {"overrides", QJsonObject{{"host", host}, {"username", "synthetic-user"}, {"protocol", "rdp"}, {"port", 3389}}}};
    const auto json = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", QJsonArray{profile}}}).toJson(QJsonDocument::Compact);
    if (!xml->patch(master, json)) return {};
    if (!password.isEmpty()) {
        if (!xml->setPassword(master, Profile, Secret(password))) return {};
        const auto readable = xml->serialize(master); if (!readable) return {};
        QXmlStreamReader reader(*readable); QByteArray cipher;
        while (!reader.atEnd()) { reader.readNext(); if (reader.isStartElement() && reader.name() == "Node") cipher = reader.attributes().value("Password").toLatin1(); }
        const auto target = normalizeRdpTarget({host, 3389, "synthetic-user"}); if (!target || cipher.isEmpty()) return {};
        ConnectionBindings reviewed{"f6595425-c35a-45b7-9dfc-84dd83e6b002", {{Profile, Profile,
            QCryptographicHash::hash(QByteArray::fromBase64(cipher), QCryptographicHash::Sha256), *target}}};
        if (!xml->reviewCredentials(master, reviewed)) return {};
    }
    if (!xml->setFullEncryption(master, full)) return {};
    const auto saved = xml->serialize(master); return saved ? *saved : QByteArray();
}
struct Await {
    std::optional<DocumentReply> reply;
    DocumentService::Completion completion() { return [this](DocumentReply value) { reply = std::move(value); }; }
};
QByteArray renamed(const QByteArray& projection, const QString& name) {
    auto root = QJsonDocument::fromJson(projection).object(); auto profiles = root["profiles"].toArray();
    auto profile = profiles[0].toObject(); profile["name"] = name; profiles[0] = profile; root["profiles"] = profiles;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
struct Barrier {
    QSemaphore entered, release;
    bool after = false;
    AtomicFile::CommitGate gate() {
        return [this](const auto& commit) {
            bool result = false; if (after) result = commit();
            entered.release(); release.acquire(); return after ? result : commit();
        };
    }
};
bool finish(Await& result) {
    QElapsedTimer timer; timer.start();
    while (!result.reply && timer.elapsed() < 10000) { QCoreApplication::processEvents(); QTest::qWait(1); }
    return result.reply.has_value();
}
}
class DocumentServiceTest : public QObject {
    Q_OBJECT
private slots:
    void twoFilesHaveIndependentRootsMastersAndRevisions() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        const auto sourceA = fixture(masterA(), "127.0.0.1"), sourceB = fixture(masterB(), "localhost", true);
        QVERIFY(!sourceA.isEmpty() && !sourceB.isEmpty()); QVERIFY(write(a, sourceA)); QVERIFY(write(b, sourceB));
        DocumentService service; Await openedA, openedB;
        const auto idA = service.open(a, openedA.completion()), idB = service.open(b, openedB.completion());
        QVERIFY(idA.valid() && idB.valid() && idA != idB); QVERIFY(finish(openedA) && finish(openedB));
        QVERIFY(openedA.reply->result.ok && openedB.reply->result.ok);
        QVERIFY(openedA.reply->status.metadataAvailable && !openedA.reply->status.unlocked);
        QVERIFY(!openedB.reply->status.metadataAvailable && !openedB.reply->status.unlocked); QVERIFY(openedB.reply->projection.isEmpty());
        const auto revisionB = openedB.reply->status.revision;
        Await wrong; service.unlock(idB, masterA(), wrong.completion()); QVERIFY(finish(wrong)); QVERIFY(!wrong.reply->result.ok);
        QVERIFY(!service.status(idB)->metadataAvailable); QCOMPARE(service.status(idB)->revision, revisionB);
        Await unlockedA, unlockedB;
        service.unlock(idA, masterA(), unlockedA.completion()); service.unlock(idB, masterB(), unlockedB.completion());
        QVERIFY(finish(unlockedA) && finish(unlockedB)); QVERIFY(unlockedA.reply->result.ok && unlockedB.reply->result.ok);
        QVERIFY(service.status(idA)->unlocked && service.status(idB)->unlocked);
        auto changed = QJsonDocument::fromJson(unlockedA.reply->projection).object(); auto profiles = changed["profiles"].toArray();
        auto p = profiles[0].toObject(); p["name"] = "Only A"; profiles[0] = p; changed["profiles"] = profiles;
        Await saved; service.save(idA, QJsonDocument(changed).toJson(QJsonDocument::Compact), "owner-A", saved.completion());
        QVERIFY(finish(saved)); QVERIFY(saved.reply->result.ok && saved.reply->committed); QCOMPARE(saved.reply->reservation, QString("owner-A"));
        QVERIFY(contents(a) != sourceA); QCOMPARE(contents(b), sourceB); QCOMPARE(service.status(idB)->revision, revisionB);
        service.lock(idA); QVERIFY(!service.status(idA)->unlocked); QVERIFY(service.status(idB)->unlocked);
        QCOMPARE(contents(b), sourceB); QCOMPARE(service.status(idB)->revision, revisionB);
    }
    void samePhysicalFileCannotOpenTwice() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("source.xml");
        const auto source = fixture(masterA(), "localhost"); QVERIFY(!source.isEmpty()); QVERIFY(write(path, source));
        DocumentService service; Await first; const auto id = service.open(path, first.completion()); QVERIFY(finish(first)); QVERIFY(first.reply->result.ok);
        Await again; service.open(dir.path() + "/./source.xml", again.completion()); QVERIFY(finish(again));
        QVERIFY(!again.reply->result.ok); QVERIFY(again.reply->alreadyOpen); QCOMPARE(*again.reply->alreadyOpen, id);
#ifdef Q_OS_UNIX
        const auto hard = dir.filePath("hard.xml"); QCOMPARE(::link(QFile::encodeName(path).constData(), QFile::encodeName(hard).constData()), 0);
        Await alias; service.open(hard, alias.completion()); QVERIFY(finish(alias)); QVERIFY(!alias.reply->result.ok);
        QVERIFY(alias.reply->alreadyOpen); QCOMPARE(*alias.reply->alreadyOpen, id);
        const auto symbolic = dir.filePath("symlink.xml"); QCOMPARE(::symlink(QFile::encodeName(path).constData(), QFile::encodeName(symbolic).constData()), 0);
        Await unsafe; service.open(symbolic, unsafe.completion()); QVERIFY(finish(unsafe)); QVERIFY(!unsafe.reply->result.ok);
#endif
        const auto copy = dir.filePath("copy.xml"); QVERIFY(write(copy, source)); Await copied;
        const auto copyId = service.open(copy, copied.completion()); QVERIFY(finish(copied)); QVERIFY(copied.reply->result.ok); QVERIFY(copyId != id);
        QCOMPARE(contents(path), source); QCOMPARE(contents(copy), source);
        Await newExisting; service.open(path, newExisting.completion(), true); QVERIFY(finish(newExisting)); QVERIFY(!newExisting.reply->result.ok);
        Await created; const auto newPath = dir.filePath("new.xml"); const auto newId = service.open(newPath, created.completion(), true);
        QVERIFY(finish(created)); QVERIFY(created.reply->result.ok); QVERIFY(!QFile::exists(newPath)); QVERIFY(service.status(newId)->unlocked);
    }
    void duplicateProfileIdsAreScopedByDocument() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(write(a, fixture(masterA(), "127.0.0.1", false, "synthetic password A")));
        QVERIFY(write(b, fixture(masterB(), "localhost", false, "synthetic password B")));
        DocumentService service; Await openA, openB; const auto idA = service.open(a, openA.completion()), idB = service.open(b, openB.completion());
        QVERIFY(finish(openA) && finish(openB)); DocumentContext contextA(idA), contextB(idB);
        QVERIFY(contextA.adopt(*openA.reply).ok && contextB.adopt(*openB.reply).ok);
        const ProfileReference refA{idA, Profile}, refB{idB, Profile}; QVERIFY(refA != refB);
        QVERIFY(contextA.select(refA)); QVERIFY(!contextA.select(refB)); QVERIFY(!contextA.select(ProfileReference{idA, {}}));
        QVERIFY(std::holds_alternative<ProfileReference>(contextA.selection()));
        QVERIFY(contextA.select(DocumentRootReference{idA})); QVERIFY(!contextA.select(DocumentRootReference{idB}));
        QVERIFY(contextB.select(refB)); contextA.expanded().insert(Profile); QVERIFY(contextB.expanded().isEmpty());
        Await unlockedA, unlockedB; service.unlock(idA, masterA(), unlockedA.completion()); service.unlock(idB, masterB(), unlockedB.completion());
        QVERIFY(finish(unlockedA) && finish(unlockedB)); QVERIFY(contextA.adopt(*unlockedA.reply).ok && contextB.adopt(*unlockedB.reply).ok);
        const auto profile = Profile.toStdString(); auto sessionA = contextA.core().open_session(profile, true), sessionB = contextB.core().open_session(profile, true);
        QCOMPARE(QString::fromUtf8(sessionA.host.data(), qsizetype(sessionA.host.size())), QString("127.0.0.1"));
        QCOMPARE(QString::fromUtf8(sessionB.host.data(), qsizetype(sessionB.host.size())), QString("localhost"));
        QVERIFY(!contextA.canClose() && !contextB.canClose()); contextA.core().session_state(sessionA.id, "disconnected"); QVERIFY(!contextA.canClose());
        Await passwordA, passwordB;
        service.lookup(refA, {"127.0.0.1", 3389, "synthetic-user"}, passwordA.completion());
        service.lookup(refB, {"localhost", 3389, "synthetic-user"}, passwordB.completion());
        QVERIFY(finish(passwordA) && finish(passwordB)); QVERIFY(passwordA.reply->password && passwordB.reply->password);
        QCOMPARE(passwordA.reply->password->takeBytes(), QByteArray("synthetic password A"));
        QCOMPARE(passwordB.reply->password->takeBytes(), QByteArray("synthetic password B"));
        const auto beforeB = contextB.projection(); const auto draft = QJsonDocument::fromJson(renamed(contextA.projection(), "Only A")).object()["profiles"].toArray()[0].toObject();
        auto candidate = contextA.prepareProfile(rm::ProfileChange::Update, QJsonDocument(draft).toJson(QJsonDocument::Compact));
        Await saved; service.save(candidate.document, candidate.projection, candidate.reservation, saved.completion()); QVERIFY(finish(saved));
        QVERIFY(!contextB.adopt(*saved.reply).ok); QVERIFY(contextA.adopt(*saved.reply).ok); QVERIFY(!contextA.adopt(*saved.reply).ok);
        QCOMPARE(contextB.projection(), beforeB); QCOMPARE(contextA.profiles()[Profile]["name"].toString(), QString("Only A"));
        QCOMPARE(contextA.core().session_count(), size_t(1)); QCOMPARE(contextB.core().session_count(), size_t(1));
        contextA.core().close_session(sessionA.id); contextB.core().close_session(sessionB.id); QVERIFY(contextA.canClose() && contextB.canClose());
        service.lock(idA); QVERIFY(contextA.updateStatus(*service.status(idA))); QVERIFY(!contextA.canWrite()); QVERIFY(contextB.canWrite());
    }
    void oneDocumentFaultOrLockDoesNotChangeAnother() {
        for (bool after : {false, true}) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
            const auto sourceA = fixture(masterA(), "localhost"), sourceB = fixture(masterB(), "127.0.0.1");
            QVERIFY(write(a, sourceA) && write(b, sourceB)); Barrier barrier; barrier.after = after; DocumentService service;
            struct Unblock { Barrier& barrier; ~Unblock() { barrier.release.release(); } } unblock{barrier};
            Await openA, openB; const auto idA = service.open(a, openA.completion()), idB = service.open(b, openB.completion());
            QVERIFY(finish(openA) && finish(openB)); Await unlockA, unlockB;
            service.unlock(idA, masterA(), unlockA.completion()); service.unlock(idB, masterB(), unlockB.completion()); QVERIFY(finish(unlockA) && finish(unlockB));
            DocumentContext contextA(idA); QVERIFY(contextA.adopt(*unlockA.reply).ok);
            Await fault; service.setFault(idA, AtomicFile::Fault::BeforeReplace, fault.completion()); QVERIFY(finish(fault));
            Await failed; service.save(idA, renamed(unlockA.reply->projection, "Failed"), "failed-A", failed.completion()); QVERIFY(finish(failed));
            QVERIFY(!failed.reply->committed && !failed.reply->result.ok); QCOMPARE(contents(a), sourceA); QCOMPARE(contents(b), sourceB);
            QVERIFY(service.status(idB)->unlocked); const auto revisionB = service.status(idB)->revision;
            Await clearFault; service.setFault(idA, AtomicFile::Fault::None, clearFault.completion()); QVERIFY(finish(clearFault));
            Await gate; service.setCommitGate(idA, barrier.gate(), gate.completion()); QVERIFY(finish(gate));
            auto draft = QJsonDocument::fromJson(renamed(contextA.projection(), "Saved A")).object()["profiles"].toArray()[0].toObject();
            auto candidate = contextA.prepareProfile(rm::ProfileChange::Update, QJsonDocument(draft).toJson(QJsonDocument::Compact));
            Await savedA, savedB; service.save(idA, candidate.projection, candidate.reservation, savedA.completion());
            QTRY_VERIFY_WITH_TIMEOUT(barrier.entered.available() == 1, 10000);
            service.save(idB, renamed(unlockB.reply->projection, "Saved B"), "B", savedB.completion());
            service.lock(idA); QVERIFY(contextA.updateStatus(*service.status(idA))); barrier.release.release();
            QVERIFY(finish(savedA) && finish(savedB)); QVERIFY(savedA.reply->canceled); QCOMPARE(savedA.reply->committed, after);
            const auto settled = contextA.adopt(*savedA.reply); QCOMPARE(settled.ok, after); QVERIFY(!contextA.pending()); QVERIFY(!contextA.canWrite());
            QVERIFY(savedB.reply->committed && savedB.reply->result.ok && !savedB.reply->canceled); QVERIFY(service.status(idB)->unlocked);
            QVERIFY(service.status(idB)->revision != revisionB); QVERIFY(contents(b) != sourceB);
            if (after) { QVERIFY(contents(a) != sourceA); QCOMPARE(contextA.profiles()[Profile]["name"].toString(), QString("Saved A")); }
            else { QCOMPARE(contents(a), sourceA); QCOMPARE(contextA.profiles()[Profile]["name"].toString(), QString("Same UUID")); }
        }
    }
    void closeAndSaveAsReconcileOwningCommit() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto source = dir.filePath("source.xml"), destination = dir.filePath("destination.xml"), other = dir.filePath("other.xml");
        const auto sourceBytes = fixture(masterA(), "localhost"), otherBytes = fixture(masterB(), "127.0.0.1");
        QVERIFY(write(source, sourceBytes) && write(other, otherBytes)); const QByteArray prior("owned synthetic destination"); QVERIFY(write(destination, prior));
        Barrier barrier; barrier.after = true; DocumentService service;
        struct Unblock { Barrier& barrier; ~Unblock() { barrier.release.release(); } } unblock{barrier};
        Await opened, otherOpened; const auto id = service.open(source, opened.completion()), otherId = service.open(other, otherOpened.completion());
        QVERIFY(finish(opened) && finish(otherOpened)); Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked));
        DocumentContext context(id); QVERIFY(context.adopt(*unlocked.reply).ok);
        Await alias; service.inspectDestination(id, other, alias.completion()); QVERIFY(finish(alias)); QVERIFY(!alias.reply->result.ok);
        QVERIFY(alias.reply->alreadyOpen); QCOMPARE(*alias.reply->alreadyOpen, otherId);
        Await inspected; service.inspectDestination(id, destination, inspected.completion()); QVERIFY(finish(inspected)); QVERIFY(inspected.reply->destination && inspected.reply->destination->exists);
        auto candidate = context.prepareDocument(); Await unconfirmed;
        service.saveAs(id, inspected.reply->destination->ticket, false, candidate.reservation, unconfirmed.completion()); QVERIFY(finish(unconfirmed));
        QVERIFY(!unconfirmed.reply->committed); QVERIFY(!context.adopt(*unconfirmed.reply).ok); QCOMPARE(contents(destination), prior); QCOMPARE(context.status().path, source);
        const QByteArray changed("externally changed synthetic destination"); QVERIFY(write(destination, changed)); candidate = context.prepareDocument(); Await conflict;
        service.saveAs(id, inspected.reply->destination->ticket, true, candidate.reservation, conflict.completion()); QVERIFY(finish(conflict)); QVERIFY(!conflict.reply->committed);
        QVERIFY(!context.adopt(*conflict.reply).ok); QCOMPARE(contents(destination), changed); QCOMPARE(context.status().path, source);
        Await inspectedAgain; service.inspectDestination(id, destination, inspectedAgain.completion()); QVERIFY(finish(inspectedAgain)); QVERIFY(inspectedAgain.reply->destination);
        Await fault; service.setFault(id, AtomicFile::Fault::BeforeReplace, fault.completion()); QVERIFY(finish(fault)); candidate = context.prepareDocument(); Await failed;
        service.saveAs(id, inspectedAgain.reply->destination->ticket, true, candidate.reservation, failed.completion()); QVERIFY(finish(failed));
        QVERIFY(!context.adopt(*failed.reply).ok); QCOMPARE(contents(destination), changed); QCOMPARE(contents(source), sourceBytes); QCOMPARE(context.status().path, source);
        Await warningFault; service.setFault(id, AtomicFile::Fault::DirectorySync, warningFault.completion()); QVERIFY(finish(warningFault));
        Await gate; service.setCommitGate(id, barrier.gate(), gate.completion()); QVERIFY(finish(gate));
        const auto profile = Profile.toStdString(); const auto session = context.core().open_session(profile, true); QVERIFY(!context.canClose());
        candidate = context.prepareDocument(); Await saved, closePending; service.saveAs(id, inspectedAgain.reply->destination->ticket, true, candidate.reservation, saved.completion());
        QTRY_VERIFY_WITH_TIMEOUT(barrier.entered.available() == 1, 10000);
        service.close(id, closePending.completion()); QVERIFY(finish(closePending)); QVERIFY(!closePending.reply->closed && !closePending.reply->result.ok);
        service.lock(id); barrier.release.release(); QVERIFY(finish(saved)); QVERIFY(saved.reply->committed && saved.reply->canceled && saved.reply->result.ok);
        QVERIFY(!saved.reply->result.warning.isEmpty()); QVERIFY(context.adopt(*saved.reply).ok); QCOMPARE(context.id(), id); QCOMPARE(context.status().path, destination);
        QVERIFY(!context.status().unlocked); QCOMPARE(context.core().session_count(), size_t(1)); QCOMPARE(contents(source), sourceBytes); QCOMPARE(contents(other), otherBytes);
        QVERIFY(contents(destination) != changed); QVERIFY(!context.canClose());
        service.acknowledgeWarning(id); context.acknowledgeWarning(); Await closedWithNoTabs;
        context.core().close_session(session.id); QVERIFY(context.canClose()); service.close(id, closedWithNoTabs.completion()); QVERIFY(finish(closedWithNoTabs));
        QVERIFY(closedWithNoTabs.reply->closed && closedWithNoTabs.reply->result.ok); QVERIFY(!service.status(id)); QVERIFY(service.status(otherId));
        Await original; const auto originalId = service.open(source, original.completion()); QVERIFY(finish(original)); QVERIFY(original.reply->result.ok); QVERIFY(originalId != id);
        QCOMPARE(contents(source), sourceBytes);
    }
    void documentWarningBlocksOwningContinuation() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        DocumentService service; Await openA, openB; const auto idA = service.open(a, openA.completion(), true), idB = service.open(b, openB.completion(), true);
        QVERIFY(finish(openA) && finish(openB)); Await fault; service.setFault(idA, AtomicFile::Fault::DirectorySync, fault.completion()); QVERIFY(finish(fault));
        DocumentContext context(idA); QVERIFY(context.adopt(*openA.reply).ok); auto candidate = context.prepareDocument(); Await saved;
        service.save(idA, candidate.projection, candidate.reservation, saved.completion()); QVERIFY(finish(saved)); QVERIFY(saved.reply->committed && !saved.reply->result.warning.isEmpty());
        QVERIFY(context.adopt(*saved.reply).ok); QVERIFY(!context.canWrite() && !context.canClose());
        Await blocked, closed, otherSaved; service.save(idA, context.projection(), "blocked", blocked.completion()); service.close(idA, closed.completion());
        service.save(idB, openB.reply->projection, "other", otherSaved.completion()); QVERIFY(finish(blocked) && finish(closed) && finish(otherSaved));
        QVERIFY(!blocked.reply->result.ok && !closed.reply->closed); QVERIFY(otherSaved.reply->committed && otherSaved.reply->result.warning.isEmpty());
        service.acknowledgeWarning(idA); context.acknowledgeWarning(); QVERIFY(context.canWrite() && context.canClose());
        Await clearFault; service.setFault(idA, AtomicFile::Fault::None, clearFault.completion()); QVERIFY(finish(clearFault)); candidate = context.prepareDocument(); Await next;
        service.save(idA, candidate.projection, candidate.reservation, next.completion()); QVERIFY(finish(next)); QVERIFY(context.adopt(*next.reply).ok);
        QVERIFY(service.status(idA)->warning.isEmpty());
    }
    void unlockedFilesNeverRelockAndRevisionGates() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        const auto sourceA = fixture(masterA(), "localhost"), sourceB = fixture(masterB(), "127.0.0.1"); QVERIFY(write(a, sourceA) && write(b, sourceB));
        DocumentService service; Await openA, openB; const auto idA = service.open(a, openA.completion()), idB = service.open(b, openB.completion()); QVERIFY(finish(openA) && finish(openB));
        Await unlockA, unlockB; service.unlock(idA, masterA(), unlockA.completion()); service.unlock(idB, masterB(), unlockB.completion()); QVERIFY(finish(unlockA) && finish(unlockB));
        const auto statusB = *service.status(idB);
        // Unlocked files never relock on their own; only an explicit Lock clears the master.
        QTest::qWait(200); QVERIFY(service.status(idA)->unlocked && service.status(idB)->unlocked);
        service.lock(idA); QVERIFY(!service.status(idA)->unlocked); QVERIFY(service.status(idB)->unlocked);
        QCOMPARE(service.status(idB)->generation, statusB.generation); QCOMPARE(service.status(idB)->revision, statusB.revision);
        const auto revisionA = service.status(idA)->revision; QVERIFY(write(a, fixture(masterA(), "127.0.0.1")));
        Await changed; service.unlock(idA, masterA(), changed.completion()); QVERIFY(finish(changed)); QVERIFY(!changed.reply->result.ok);
        QVERIFY(!service.status(idA)->unlocked); QCOMPARE(service.status(idA)->revision, revisionA); QCOMPARE(contents(b), sourceB);
    }
    void defaultKeyPasswordsAreWritableAndReusable() {
        for (bool full : {false, true}) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("default.xml");
            const Secret initial(QByteArray("mR3m"));
            const auto source = fixture(initial, "localhost", full, "synthetic default password"); QVERIFY(!source.isEmpty()); QVERIFY(write(path, source));
            DocumentService service; Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened));
            QVERIFY(opened.reply->result.ok && opened.reply->status.unlocked && opened.reply->status.metadataAvailable);
            QCOMPARE(opened.reply->status.fullEncryption, full);
            DocumentContext context(id); QVERIFY(context.adopt(*opened.reply).ok && context.canWrite());
            Await lookup; service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, lookup.completion()); QVERIFY(finish(lookup));
            QVERIFY(lookup.reply->password); QCOMPARE(lookup.reply->password->takeBytes(), QByteArray("synthetic default password"));
            const auto candidate = context.prepareDocument(); Await saved;
            service.save(id, candidate.projection, candidate.reservation, saved.completion()); QVERIFY(finish(saved)); QVERIFY(saved.reply->committed);
            QVERIFY(context.adopt(*saved.reply).ok); service.lock(id); QVERIFY(!service.status(id)->unlocked);
            Await unlocked; service.unlock(id, Secret(QByteArray("mR3m")), unlocked.completion()); QVERIFY(finish(unlocked)); QVERIFY(unlocked.reply->status.unlocked);
            auto parsed = ConnectionXml::read(contents(path), &initial); QVERIFY(parsed); QCOMPARE(parsed->fullEncryption(), full);
            QVERIFY(parsed->password(initial, Profile, {"localhost", 3389, "synthetic-user"}));
        }
    }
    void defaultKeyImportsNeedReviewNotConversion() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("import.xml"); Secret initial(QByteArray("mR3m"));
        const auto canary = ConnectionCrypto::encrypt(initial, 1000, Secret(QByteArray("ThisIsNotProtected")), 4096);
        const auto password = ConnectionCrypto::encrypt(initial, 1000, Secret(QByteArray("synthetic imported password")), 4096); QVERIFY(canary && password);
        const auto source = QByteArray("<Connections Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"false\" Protected=\"") + *canary +
            "\"><Node Type=\"Connection\" Id=\"" + Profile.toLatin1() + "\" Name=\"Import\" Hostname=\"localhost\" Protocol=\"RDP\" Port=\"3389\" Username=\"synthetic-user\" Password=\"" + *password + "\"/></Connections>";
        QVERIFY(write(path, source)); DocumentService service; Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened)); QVERIFY(opened.reply->result.ok);
        QVERIFY(opened.reply->status.unlocked && opened.reply->status.metadataAvailable && opened.reply->status.reviewRequired); QCOMPARE(contents(path), source);
        Await lookup; service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, lookup.completion());
        QVERIFY(finish(lookup)); QVERIFY(!lookup.reply->password); QCOMPARE(contents(path), source);
        Await reviewed; service.reviewPasswords(id, {{Profile, {"localhost", 3389, "synthetic-user"}}}, reviewed.completion());
        QVERIFY(finish(reviewed)); QVERIFY(reviewed.reply->result.ok); QCOMPARE(contents(path), source);
        Await reused; service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, reused.completion());
        QVERIFY(finish(reused)); QVERIFY(reused.reply->password); QCOMPARE(reused.reply->password->takeBytes(), QByteArray("synthetic imported password"));
        Await saved; service.save(id, opened.reply->projection, "reviewed-default", saved.completion()); QVERIFY(finish(saved)); QVERIFY(saved.reply->committed);
        auto parsed = ConnectionXml::read(contents(path), &initial); QVERIFY(parsed); QVERIFY(!parsed->credentialReviewRequired());
        QVERIFY(parsed->password(initial, Profile, {"localhost", 3389, "synthetic-user"}));
    }
    void resetMasterFailurePreservesOwningState() {
        for (bool committed : {false, true}) {
            QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
            const auto source = fixture(masterA(), "localhost", true, "synthetic reset password"), other = fixture(masterB(), "127.0.0.1");
            QVERIFY(write(a, source) && write(b, other)); DocumentService service; Await opened, openedOther;
            const auto id = service.open(a, opened.completion()), otherId = service.open(b, openedOther.completion());
            QVERIFY(finish(opened) && finish(openedOther)); QVERIFY(!opened.reply->status.passwordProtected.has_value());
            Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked));
            DocumentContext context(id); QVERIFY(context.adopt(*unlocked.reply).ok); QVERIFY(context.status().passwordProtected == true);
            const auto revision = context.status().revision, otherRevision = service.status(otherId)->revision;
            Await fault; service.setFault(id, committed ? AtomicFile::Fault::DirectorySync : AtomicFile::Fault::BeforeReplace, fault.completion()); QVERIFY(finish(fault));
            const auto candidate = context.prepareDocument(); Await reset;
            service.resetMaster(id, candidate.reservation, reset.completion()); QVERIFY(finish(reset)); QCOMPARE(reset.reply->committed, committed);
            QCOMPARE(context.adopt(*reset.reply).ok, committed); QVERIFY(!context.pending()); QVERIFY(context.status().fullEncryption);
            QCOMPARE(contents(b), other); QCOMPARE(service.status(otherId)->revision, otherRevision);
            if (!committed) {
                QCOMPARE(contents(a), source); QCOMPARE(context.status().revision, revision); QVERIFY(context.status().passwordProtected == true && context.canWrite());
                Await lookup; service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, lookup.completion()); QVERIFY(finish(lookup)); QVERIFY(lookup.reply->password);
            } else {
                QVERIFY(!reset.reply->result.warning.isEmpty()); QVERIFY(!context.canWrite()); QVERIFY(context.status().passwordProtected == false);
                service.acknowledgeWarning(id); context.acknowledgeWarning(); QVERIFY(context.canWrite());
                service.lock(id); Await wrong; service.unlock(id, masterA(), wrong.completion()); QVERIFY(finish(wrong)); QVERIFY(!wrong.reply->result.ok);
                Await defaultUnlock; service.unlock(id, Secret(QByteArray("mR3m")), defaultUnlock.completion()); QVERIFY(finish(defaultUnlock)); QVERIFY(defaultUnlock.reply->status.unlocked);
                Await lookup; service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, lookup.completion()); QVERIFY(finish(lookup)); QVERIFY(lookup.reply->password);
                QCOMPARE(lookup.reply->password->takeBytes(), QByteArray("synthetic reset password"));
            }
        }
    }
    void resetMasterCancelReconcilesOwningCommit() {
        for (bool after : {false, true}) {
            QTemporaryDir dir; const auto path = dir.filePath("reset.xml"); const auto source = fixture(masterA(), "localhost", true, "synthetic reset cancellation");
            QVERIFY(write(path, source)); Barrier barrier; barrier.after = after; DocumentService service;
            struct Unblock { Barrier& barrier; ~Unblock() { barrier.release.release(); } } unblock{barrier};
            Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened));
            Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked)); DocumentContext context(id); QVERIFY(context.adopt(*unlocked.reply).ok);
            Await gate; service.setCommitGate(id, barrier.gate(), gate.completion()); QVERIFY(finish(gate));
            const auto candidate = context.prepareDocument(); Await reset; service.resetMaster(id, candidate.reservation, reset.completion());
            QTRY_VERIFY_WITH_TIMEOUT(barrier.entered.available() == 1, 10000); service.lock(id); barrier.release.release();
            QVERIFY(finish(reset)); QVERIFY(reset.reply->canceled); QCOMPARE(reset.reply->committed, after); QCOMPARE(context.adopt(*reset.reply).ok, after);
            QVERIFY(!context.pending() && !context.status().unlocked); QVERIFY(context.status().passwordProtected == !after);
            if (!after) QCOMPARE(contents(path), source);
            const auto key = after ? Secret(QByteArray("mR3m")) : masterA(); auto parsed = ConnectionXml::read(contents(path), &key); QVERIFY(parsed);
            QVERIFY(parsed->password(key, Profile, {"localhost", 3389, "synthetic-user"}));
        }
    }
    void unsupportedNodeCannotLaunchAsRdp() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("unsupported.xml"); auto source = fixture(masterA(), "localhost");
        QVERIFY(source.contains("Protocol=\"RDP\"")); source.replace("Protocol=\"RDP\"", "Protocol=\"ICA\""); QVERIFY(write(path, source));
        DocumentService service; Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened)); QVERIFY(opened.reply->result.ok);
        DocumentContext context(id); QVERIFY(context.adopt(*opened.reply).ok); QCOMPARE(context.profiles()[Profile]["effective"].toObject()["protocol"].toString(), QString("unsupported"));
        bool rejected = false; try { context.core().open_session(Profile.toStdString(), true); } catch (const std::exception&) { rejected = true; }
        QVERIFY(rejected); QCOMPARE(context.core().session_count(), size_t(0)); QCOMPARE(contents(path), source);
    }
    void canceledUnlockAndQueuedPasswordNeverExposeSecrets() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("full.xml");
        const auto source = fixture(masterA(), "localhost", true, "synthetic pending lookup password"); QVERIFY(!source.isEmpty()); QVERIFY(write(path, source));
        DocumentService service; Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened)); QVERIFY(opened.reply->result.ok);
        DocumentContext context(id); QVERIFY(context.adopt(*opened.reply).ok); QVERIFY(context.profiles().isEmpty());
        bool cancelNext = true;
        connect(&service, &DocumentService::operationThread, &service, [&](DocumentId owner, QThread*) {
            if (owner == id && cancelNext) { cancelNext = false; service.lock(id); }
        });
        Await canceled; service.unlock(id, masterA(), canceled.completion()); QVERIFY(finish(canceled));
        QVERIFY(canceled.reply->canceled && canceled.reply->projection.isEmpty()); QVERIFY(!canceled.reply->status.metadataAvailable && !canceled.reply->status.unlocked);
        QVERIFY(!context.adopt(*canceled.reply).ok); QVERIFY(context.profiles().isEmpty()); QVERIFY(!service.status(id)->metadataAvailable);
        Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked)); QVERIFY(context.adopt(*unlocked.reply).ok);
        QVERIFY(context.profiles().contains(Profile)); cancelNext = true; Await lookup;
        service.lookup({id, Profile}, {"localhost", 3389, "synthetic-user"}, lookup.completion()); QVERIFY(finish(lookup));
        QVERIFY(lookup.reply->canceled && !lookup.reply->password && !lookup.reply->result.ok); QVERIFY(!service.status(id)->unlocked);
        QCOMPARE(contents(path), source);
    }
    void saveAsRechecksSourceAndDestinationAliases() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto source = dir.filePath("source.xml"), other = dir.filePath("other.xml"), destination = dir.filePath("new.xml");
        const auto sourceBytes = fixture(masterA(), "localhost"), otherBytes = sourceBytes; QVERIFY(write(source, sourceBytes) && write(other, otherBytes));
        DocumentService service; Await opened, openedOther; const auto id = service.open(source, opened.completion()); service.open(other, openedOther.completion());
        QVERIFY(finish(opened) && finish(openedOther)); Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked));
        Await inspected; service.inspectDestination(id, destination, inspected.completion()); QVERIFY(finish(inspected)); QVERIFY(inspected.reply->destination);
        const auto changedSource = fixture(masterA(), "127.0.0.1"); QVERIFY(write(source, changedSource)); Await conflict;
        service.saveAs(id, inspected.reply->destination->ticket, false, "source-conflict", conflict.completion()); QVERIFY(finish(conflict)); QVERIFY(!conflict.reply->committed);
        QCOMPARE(service.status(id)->path, source); QCOMPARE(contents(source), changedSource); QVERIFY(!QFile::exists(destination)); QCOMPARE(contents(other), otherBytes);
        QVERIFY(write(source, sourceBytes));
#ifdef Q_OS_UNIX
        // A late source hard link can retain the exact inspected bytes/hash.
        // The commit gate must still reject two loaded writers to that inode.
        QVERIFY(QFile::remove(source)); QCOMPARE(::link(QFile::encodeName(other).constData(), QFile::encodeName(source).constData()), 0);
        Await sourceAlias; service.saveAs(id, inspected.reply->destination->ticket, false, "late-source-alias", sourceAlias.completion());
        QVERIFY(finish(sourceAlias)); QVERIFY(!sourceAlias.reply->committed); QVERIFY(!QFile::exists(destination)); QCOMPARE(contents(other), otherBytes);
        QVERIFY(QFile::remove(source)); QVERIFY(write(source, sourceBytes));
        QCOMPARE(::link(QFile::encodeName(other).constData(), QFile::encodeName(destination).constData()), 0);
        Await alias; service.saveAs(id, inspected.reply->destination->ticket, false, "late-alias", alias.completion()); QVERIFY(finish(alias));
        QVERIFY(!alias.reply->committed && alias.reply->alreadyOpen); QCOMPARE(contents(other), otherBytes); QCOMPARE(contents(source), sourceBytes);
#endif
    }
    void invalidTerminalProjectionKeepsCommitReceiptForRecovery() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("source.xml"); QVERIFY(write(path, fixture(masterA(), "localhost")));
        DocumentService service; Await opened; const auto id = service.open(path, opened.completion()); QVERIFY(finish(opened));
        Await unlocked; service.unlock(id, masterA(), unlocked.completion()); QVERIFY(finish(unlocked)); DocumentContext context(id); QVERIFY(context.adopt(*unlocked.reply).ok);
        const auto before = context.projection(); const auto candidate = context.prepareDocument(); Await saved;
        service.save(id, candidate.projection, candidate.reservation, saved.completion()); QVERIFY(finish(saved)); QVERIFY(saved.reply->committed);
        // Simulate a faulty terminal dispatcher, not a different on-disk file.
        DocumentReply invalid; invalid.status = saved.reply->status; invalid.result = saved.reply->result;
        invalid.committed = true; invalid.reservation = candidate.reservation; invalid.projection = renamed(saved.reply->projection, "Foreign normalized name");
        QVERIFY(!context.adopt(invalid).ok); QVERIFY(context.pending() && context.lastCommitted()); QVERIFY(!context.canWrite()); QCOMPARE(context.projection(), before);
        QVERIFY(context.canClose()); Await closed; service.close(id, closed.completion()); QVERIFY(finish(closed)); QVERIFY(closed.reply->closed);
        const auto master = masterA(); auto parsed = ConnectionXml::read(contents(path), &master); QVERIFY(parsed);
        QCOMPARE(QJsonDocument::fromJson(*parsed->projection()).object()["profiles"].toArray()[0].toObject()["name"].toString(), QString("Same UUID"));
    }
    void storageAndKdfStayOffGuiThread() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto a = dir.filePath("a.xml"), b = dir.filePath("b.xml");
        QVERIFY(write(a, fixture(masterA(), "localhost"))); QVERIFY(write(b, fixture(masterB(), "127.0.0.1")));
        DocumentService service; QList<QThread*> threads;
        connect(&service, &DocumentService::operationThread, &service, [&](DocumentId, QThread* performed) { threads.append(performed); });
        Await openA, openB; const auto idA = service.open(a, openA.completion()), idB = service.open(b, openB.completion());
        QVERIFY(finish(openA) && finish(openB)); QVERIFY(openA.reply->result.ok && openB.reply->result.ok);
        Await unlockA, unlockB; service.unlock(idA, masterA(), unlockA.completion()); service.unlock(idB, masterB(), unlockB.completion());
        QVERIFY(finish(unlockA) && finish(unlockB)); QVERIFY(unlockA.reply->result.ok && unlockB.reply->result.ok);
        Await saved; service.save(idA, unlockA.reply->projection, "thread-check", saved.completion()); QVERIFY(finish(saved)); QVERIFY(saved.reply->result.ok);
        QCOMPARE(threads.size(), 5); for (auto* performed : threads) { QVERIFY(performed != QThread::currentThread()); QCOMPARE(performed, threads.front()); }
    }
};
QTEST_GUILESS_MAIN(DocumentServiceTest)
#include "document_service.moc"
