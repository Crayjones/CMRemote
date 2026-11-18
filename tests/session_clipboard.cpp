#include "SessionClipboard.h"
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>
#include <QtEndian>

class FakeClipboard final : public ClipboardEndpoint {
public:
    int reads = 0, writes = 0, watches = 0, fileWrites = 0;
    std::optional<QString> text = QString("existing host text");
    QStringList files;
    void watch() override { ++watches; }
    QStringList readFiles() override { return files; }
    // Like Finder: a file copy also carries the names as text.
    void writeFiles(const QStringList& paths) override { ++fileWrites; files = paths; text = paths.join('\n'); emit changed(); }
    std::optional<QString> readText() override { ++reads; return text; }
    void writeText(const QString& value) override { ++writes; text = value; emit changed(); }
    void copy(std::optional<QString> value) { text = std::move(value); emit changed(); }
};
// FILEDESCRIPTORW entries as a server sends them: FD_ATTRIBUTES | FD_FILESIZE.
struct Entry { QString name; bool directory = false; std::optional<quint64> size; };
static QByteArray descriptors(const std::vector<Entry>& entries, std::optional<quint32> count = {}) {
    QByteArray bytes(4 + qsizetype(entries.size()) * 592, '\0'); auto* d = reinterpret_cast<uchar*>(bytes.data());
    qToLittleEndian<quint32>(count.value_or(quint32(entries.size())), d);
    for (size_t i = 0; i < entries.size(); ++i) {
        auto* e = d + 4 + i * 592; const auto& entry = entries[i];
        qToLittleEndian<quint32>(0x4 | (entry.size ? 0x40 : 0), e); qToLittleEndian<quint32>(entry.directory ? 0x10 : 0x80, e + 36);
        if (entry.size) { qToLittleEndian<quint32>(quint32(*entry.size >> 32), e + 64); qToLittleEndian<quint32>(quint32(*entry.size), e + 68); }
        for (qsizetype c = 0; c < entry.name.size() && c < 260; ++c) qToLittleEndian<quint16>(entry.name[c].unicode(), e + 72 + c * 2);
    }
    return bytes;
}
static std::optional<RemoteFileList> parse(const QByteArray& bytes) {
    return parseRemoteFileList(reinterpret_cast<const uchar*>(bytes.constData()), bytes.size());
}
static void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(data);
}
static QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>"); }
// Serves a download from host files, as the remote computer would from its own clipboard.
static bool serve(RemoteFileDownload& download, const LocalFileList& source, int limit = -1) {
    for (int served = 0; limit < 0 || served < limit; ++served) {
        const auto request = download.next(); if (!request) return download.finished();
        const auto bytes = readLocalFileContents(source, request->index, request->flags, request->position, request->size);
        if (!download.response(request->stream, bytes ? reinterpret_cast<const uchar*>(bytes->constData()) : nullptr, bytes ? bytes->size() : 0, bytes.has_value()))
            return false;
    }
    return true;
}
class ClipboardTest : public QObject {
    Q_OBJECT
    static ClipboardStamp latest(QSignalSpy& spy) { return qvariant_cast<ClipboardControl>(spy.last()[0]).stamp; }
private slots:
    void initTestCase() { qRegisterMetaType<ClipboardControl>(); qRegisterMetaType<ClipboardStamp>(); }
    void clipboardDefaultsOffAndNeverTouchesHost() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        session.setFocused(true); host->copy("new host text");
        session.receive({"remote text", {}}); session.reset();
        QVERIFY(!session.enabled()); QCOMPARE(host->reads, 0); QCOMPARE(host->writes, 0); QCOMPARE(host->watches, 0);
    }
    void clipboardSynchronizesExistingTextOnFocus() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy local(&session, &SessionClipboard::localText);
        session.setEnabled(true); QCOMPARE(host->reads, 0);
        session.setFocused(true); QCOMPARE(local.count(), 1); QCOMPARE(local.last()[0].toString(), *host->text);
    }
    void clipboardRoutesOnlyFocusedSession() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard first(host), second(host);
        QSignalSpy a(&first, &SessionClipboard::localText), b(&second, &SessionClipboard::localText);
        first.setEnabled(true); second.setEnabled(true); first.setFocused(true); a.clear();
        host->copy("first copy"); QCOMPARE(a.count(), 1); QCOMPARE(b.count(), 0);
        first.setFocused(false); second.setFocused(true); a.clear(); b.clear();
        host->copy("second copy"); QCOMPARE(a.count(), 0); QCOMPARE(b.count(), 1);
        host->copy(std::nullopt); QCOMPARE(b.count(), 1);
    }
    void clipboardCopyCompletesAfterHostFocus() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.setFocused(false); session.receive({"late remote copy", stamp});
        QCOMPARE(host->writes, 1); QCOMPARE(*host->text, QString("late remote copy"));
    }
    void clipboardRemoteReplyCannotOverwriteNewerHostCopy() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.setFocused(false); host->copy("newer host copy"); session.receive({"obsolete remote copy", stamp});
        QCOMPARE(host->writes, 0); QCOMPARE(*host->text, QString("newer host copy"));
    }
    void clipboardDoesNotEchoDelayedChanges() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy control(&session, &SessionClipboard::configure), local(&session, &SessionClipboard::localText);
        session.setEnabled(true); session.setFocused(true); local.clear();
        session.receive({"remote copy", latest(control)}); emit host->changed(); emit host->changed();
        QCOMPARE(local.count(), 0); QCOMPARE(host->writes, 1);
        session.setFocused(false); session.setFocused(true); QCOMPARE(local.count(), 0);
        QVERIFY(host->echo(&session, "remote copy")); session.reset();
        QVERIFY(!host->echo(&session, "remote copy")); QCOMPARE(*host->text, QString("remote copy"));
        session.setEnabled(true); session.setFocused(true); local.clear();
        host->copy("different host copy"); QCOMPARE(local.count(), 1);
    }
    void clipboardDisableAndReconnectRejectPendingTransfers() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host), replacement(host);
        QSignalSpy control(&session, &SessionClipboard::configure);
        session.setEnabled(true); session.setFocused(true); const auto stamp = latest(control);
        session.reset(); const auto reads = host->reads; host->copy("host copy"); session.receive({"old", stamp});
        QCOMPARE(host->reads, reads); QCOMPARE(host->writes, 0);
        session.setEnabled(true); session.setFocused(true); session.receive({"old", stamp}); QCOMPARE(host->writes, 0);
        const auto current = latest(control); replacement.setEnabled(true); replacement.setFocused(true);
        session.receive({"other tab", current}); QCOMPARE(host->writes, 0);
    }
    void clipboardWireTextIsBoundedAndNormalized() {
        const QString text = QString::fromUtf8("empty?\nline\r\nemoji: \xf0\x9f\x98\x80\rfinal");
        auto bytes = encodeClipboardText(text); QVERIFY(bytes);
        auto decoded = decodeClipboardText(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size()); QVERIFY(decoded);
        QCOMPARE(*decoded, QString::fromUtf8("empty?\nline\nemoji: \xf0\x9f\x98\x80\nfinal"));
        auto empty = encodeClipboardText({}); QVERIFY(empty); QCOMPARE(empty->size(), 2);
        QVERIFY(encodeClipboardText(QString(524287, 'x'))); QVERIFY(!encodeClipboardText(QString(524288, 'x')));
        QVERIFY(!encodeClipboardText(QString(262144, '\n')));
        QVERIFY(!encodeClipboardText(QString(QChar(0xd800)))); QVERIFY(!encodeClipboardText(QString(QChar(0xdc00))));
        QVERIFY(!encodeClipboardText(QString("a") + QChar(0) + "b"));
        for (QByteArray bad : {QByteArray("x", 1), QByteArray("xx", 2), QByteArray("x\0\0\0y\0\0\0", 8), QByteArray("\0\xd8\0\0", 4), QByteArray(1024 * 1024 + 2, '\0')})
            QVERIFY(!decodeClipboardText(reinterpret_cast<const uchar*>(bad.constData()), bad.size()));
    }
    void clipboardRequestsAreSerializedAndRevisionChecked() {
        ClipboardProtocol protocol; const ClipboardStamp first{1, 2, 3};
        protocol.control({true, true, first}); protocol.ready(); protocol.offer(true);
        QVERIFY(protocol.request()); protocol.offer(true); QVERIFY(!protocol.request());
        auto bytes = encodeClipboardText("old");
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        QVERIFY(protocol.request()); auto received = protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true);
        QVERIFY(received); QCOMPARE(received->stamp, first); QVERIFY(!protocol.request());
        protocol.offer(true); QVERIFY(protocol.request()); protocol.control({true, false, first});
        QVERIFY(protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.control({true, true, first}); protocol.offer(true); QVERIFY(protocol.request());
        protocol.control({true, true, {1, 2, 4}}); QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.offer(true); QVERIFY(protocol.request()); QVERIFY(!protocol.response(nullptr, 0, false));
        protocol.offer(true); QVERIFY(protocol.request()); protocol.control({false, false, {2, 2, 4}});
        protocol.control({true, true, {3, 2, 4}}); protocol.offer(true); QVERIFY(!protocol.request());
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true)); QVERIFY(protocol.request());
        protocol.reset(); protocol.control({true, true, first}); protocol.ready(); protocol.offer(true);
        QCOMPARE(protocol.responseWait(), qint64(-1)); QVERIFY(protocol.request(0)); QCOMPARE(protocol.responseWait(), qint64(0));
        protocol.offer(true); QVERIFY(!protocol.request());
        protocol.reset(); QCOMPARE(protocol.responseWait(), qint64(-1));
    }
    void clipboardRemoteOwnershipDoesNotWithdrawImportedText() {
        ClipboardProtocol protocol; const ClipboardStamp first{1, 1, 1};
        protocol.control({true, true, first}); protocol.ready(); protocol.local("host copy", first);
        QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{true, false}));
        protocol.offer(true); QVERIFY(protocol.request()); auto bytes = encodeClipboardText("remote copy");
        QVERIFY(protocol.response(reinterpret_cast<const uchar*>(bytes->constData()), bytes->size(), true));
        protocol.control({true, true, {1, 1, 2}});
        QVERIFY(!protocol.localReply()); QVERIFY(!protocol.advertisement());
        protocol.local("new host copy", {1, 1, 2}); QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{true, false}));
    }
    void clipboardDisabledRejectsDataRequests() {
        ClipboardProtocol protocol; protocol.ready(); protocol.offer(true);
        QVERIFY(!protocol.request()); QVERIFY(!protocol.localReply()); QVERIFY(!protocol.advertisement());
        protocol.control({true, true, {1, 1, 1}}); protocol.local("host", {1, 1, 1}); QVERIFY(protocol.localReply());
        QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{true, false}));
        protocol.control({true, false, {1, 1, 1}}); QVERIFY(!protocol.localReply());
        protocol.control({true, true, {1, 1, 1}}); QVERIFY(protocol.localReply());
        protocol.control({false, false, {2, 1, 1}}); QVERIFY(!protocol.localReply());
        QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{}));
        protocol.offer(true); QVERIFY(!protocol.request());
    }
    void clipboardSynchronizationOffersHostFiles() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy files(&session, &SessionClipboard::localFiles), text(&session, &SessionClipboard::localText);
        host->files = {"/host/a.txt", "/host/folder"};
        session.setFocused(true); host->copy("a.txt"); QCOMPARE(files.count(), 0);
        session.setEnabled(true); session.setFocused(false); session.setFocused(true);
        QVERIFY(files.count() >= 1); QCOMPARE(files.last()[0].toStringList(), host->files);
        QCOMPARE(qvariant_cast<ClipboardStamp>(files.last()[1]), qvariant_cast<ClipboardStamp>(text.last()[1]));
        host->files.clear(); host->copy("plain text"); QVERIFY(files.last()[0].toStringList().isEmpty());
    }
    void clipboardPutsRemoteFilesOnHostWithoutEchoingThem() {
        auto host = std::make_shared<FakeClipboard>(); SessionClipboard session(host);
        QSignalSpy controls(&session, &SessionClipboard::configure), files(&session, &SessionClipboard::localFiles), text(&session, &SessionClipboard::localText);
        session.receiveFiles({"/staged/a.txt"}, {}); QCOMPARE(host->fileWrites, 0); // sharing off
        session.setEnabled(true); session.setFocused(true); auto stamp = latest(controls);
        session.receiveFiles({"/staged/a.txt"}, {stamp.permission, stamp.route, stamp.host - 1}); QCOMPARE(host->fileWrites, 0); // stale
        const auto filesBefore = files.count(), textBefore = text.count();
        session.receiveFiles({"/staged/a.txt", "/staged/b.bin"}, stamp);
        QCOMPARE(host->fileWrites, 1); QCOMPARE(host->files, QStringList({"/staged/a.txt", "/staged/b.bin"}));
        host->copy(host->text); session.setFocused(false); session.setFocused(true); // delayed change notices and refocus
        QCOMPARE(files.count(), filesBefore); QCOMPARE(text.count(), textBefore);
        host->files = {"/host/new.txt"}; host->copy("new.txt"); // a real host copy is offered again
        QCOMPARE(files.last()[0].toStringList(), QStringList({"/host/new.txt"})); QCOMPARE(text.last()[0].toString(), QString("new.txt"));
        stamp = latest(controls); session.setEnabled(false); session.receiveFiles({"/staged/c.txt"}, stamp); QCOMPARE(host->fileWrites, 1);
    }
    void localFileListExpandsFoldersParentsFirst() {
        QTemporaryDir root; QVERIFY(root.isValid());
        write(root.filePath("folder/b.txt"), "bee"); write(root.filePath("folder/sub/c.bin"), QByteArray(70000, 'c'));
        write(root.filePath("a.txt"), "alpha"); QDir(root.path()).mkpath("folder/empty");
        QVERIFY(QFile::link(root.filePath("a.txt"), root.filePath("folder/link.txt")));
        // Folders arrive as Finder sends them: with a trailing slash.
        QString error; const auto list = buildLocalFileList({root.filePath("a.txt"), root.filePath("folder") + "/"}, &error);
        QVERIFY2(list, qPrintable(error)); QVERIFY(error.isEmpty());
        const auto parsed = parse(list->descriptor); QVERIFY(parsed);
        QStringList names; for (const auto& file : parsed->files) names << file.path + (file.directory ? "/" : "");
        QCOMPARE(names, QStringList({"a.txt", "folder/", "folder/b.txt", "folder/empty/", "folder/sub/", "folder/sub/c.bin"}));
        QCOMPARE(parsed->bytes, quint64(5 + 3 + 70000)); QCOMPARE(int(list->files.size()), 6);
        const auto size = readLocalFileContents(*list, 5, ClipboardFileContentsSize, 0, 8);
        QVERIFY(size); QCOMPARE(qFromLittleEndian<quint64>(size->constData()), quint64(70000));
        QCOMPARE(readLocalFileContents(*list, 0, ClipboardFileContentsRange, 1, 3), std::optional<QByteArray>("lph"));
        QCOMPARE(readLocalFileContents(*list, 0, ClipboardFileContentsRange, 5, 3), std::optional<QByteArray>(QByteArray()));
        QVERIFY(!readLocalFileContents(*list, 1, ClipboardFileContentsRange, 0, 3)); // a folder has no contents
        QVERIFY(!readLocalFileContents(*list, 6, ClipboardFileContentsRange, 0, 3));
        QVERIFY(!readLocalFileContents(*list, 0, ClipboardFileContentsRange, 0, 8 * 1024 * 1024 + 1));
        QVERIFY(!buildLocalFileList({root.filePath("missing")}, &error)); QVERIFY(error.contains("missing"));
        write(root.filePath("other/a.txt"), "x");
        QVERIFY(!buildLocalFileList({root.filePath("a.txt"), root.filePath("other/a.txt")}, &error)); QVERIFY(error.contains("two items"));
        QVERIFY(!buildLocalFileList({}, &error)); QVERIFY(error.isEmpty());
    }
    void remoteFileListRejectsUnsafeNames() {
        const auto ok = parse(descriptors({{"Report", true}, {"Report\\q1.pdf", false, 10}, {"notes.txt", false, 2}}));
        QVERIFY(ok); QCOMPARE(ok->files.size(), size_t(3)); QCOMPARE(ok->files[1].path, QString("Report/q1.pdf")); QCOMPARE(ok->bytes, quint64(12));
        QVERIFY(ok->files[0].directory); QVERIFY(!ok->files[0].size);
        for (const auto* name : {"..\\evil", "a\\..\\b", "\\root", "C:\\x", "a\\\\b", "dot.", "space ", "a\\.\\b", "", "pipe|x", "/abs"})
            QVERIFY2(!parse(descriptors({{QString::fromUtf8(name), false, 1}})), name);
        QVERIFY(!parse(descriptors({{"a.txt", false, 1}, {"A.TXT", false, 1}})));
        QVERIFY(!parse(descriptors({{"a", false, 1}, {"a\\b", false, 1}})));
        QVERIFY(!parse(descriptors({{"a\\b", false, 1}, {"a", false, 1}})));
        QVERIFY(!parse(descriptors({{QString(260, 'x'), false, 1}}))); // no terminator
        QVERIFY(!parse(descriptors({{"a", false, 1}}, 2)));
        QVERIFY(!parse(descriptors({}, 10001)));
        QVERIFY(!parse(descriptors({{QString(QChar(0xd800)), false, 1}})));
        QVERIFY(!parseRemoteFileList(nullptr, 0));
    }
    void remoteFileDownloadWritesFilesWithoutReplacingAny() {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        write(source.filePath("a.txt"), "first"); write(source.filePath("dir/big.bin"), QByteArray(1300000, 'z'));
        write(source.filePath("dir/zero"), {});
        const auto list = buildLocalFileList({source.filePath("a.txt"), source.filePath("dir")}); QVERIFY(list);
        auto files = parse(list->descriptor); QVERIFY(files);
        files->files[0].size.reset(); files->bytes -= 5; // the server sent no size: one FILECONTENTS_SIZE first
        write(target.filePath("a.txt"), "existing"); write(target.filePath("dir/keep"), "existing");
        RemoteFileDownload download(*files, target.path()); QVERIFY(download.start());
        QVERIFY(serve(download, *list)); QVERIFY(download.finished()); QVERIFY(!download.failed());
        QCOMPARE(read(target.filePath("a.txt")), QByteArray("existing")); QCOMPARE(read(target.filePath("a (2).txt")), QByteArray("first"));
        QCOMPARE(read(target.filePath("dir (2)/big.bin")), QByteArray(1300000, 'z')); QCOMPARE(read(target.filePath("dir (2)/zero")), QByteArray());
        QCOMPARE(download.done(), quint64(1300005)); QCOMPARE(download.total(), quint64(1300005));
        QVERIFY(download.summary().contains("3 files"));
    }
    void remoteFileDownloadOnWorkerThread() {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        write(source.filePath("report.txt"), "remote-file-fixture\n"); write(source.filePath("blocks.bin"), QByteArray(1500000, 'b'));
        write(target.filePath("report.txt"), "keep");
        const auto list = buildLocalFileList({source.filePath("report.txt"), source.filePath("blocks.bin")}); QVERIFY(list);
        bool ok = false;
        std::unique_ptr<QThread> thread(QThread::create([&] {
            RemoteFileDownload download(*parse(list->descriptor), target.path());
            ok = download.start() && serve(download, *list) && !download.failed();
        }));
        thread->start(); QVERIFY(thread->wait(30000)); QVERIFY(ok);
        QCOMPARE(read(target.filePath("report (2).txt")), QByteArray("remote-file-fixture\n"));
    }
    void remoteFileDownloadStopsCleanly() {
        QTemporaryDir source, target; QVERIFY(source.isValid() && target.isValid());
        write(source.filePath("one.bin"), QByteArray(10, '1')); write(source.filePath("two.bin"), QByteArray(2000000, '2'));
        const auto list = buildLocalFileList({source.filePath("one.bin"), source.filePath("two.bin")}); QVERIFY(list);
        {
            RemoteFileDownload download(*parse(list->descriptor), target.path()); QVERIFY(download.start());
            QVERIFY(serve(download, *list, 2)); QVERIFY(!download.finished());
            const auto pending = download.next(); QVERIFY(pending); QVERIFY(!download.next()); // one request at a time
            QVERIFY(download.responseWait() > 0);
            download.cancel("canceled"); QVERIFY(download.failed()); QVERIFY(!download.next());
            QVERIFY(!download.response(pending->stream, nullptr, 0, true));
            QCOMPARE(download.summary(), QString("canceled 1 file copied before the transfer stopped."));
        }
        QCOMPARE(QDir(target.path()).entryList(QDir::Files | QDir::Hidden), QStringList({"one.bin"}));
        RemoteFileDownload failing(*parse(list->descriptor), target.path()); QVERIFY(failing.start());
        auto request = failing.next(); QVERIFY(request);
        QVERIFY(failing.response(request->stream + 1000, nullptr, 0, false)); // another stream: ignored
        QVERIFY(!failing.response(request->stream, nullptr, 0, false)); QVERIFY(failing.error().contains("one.bin"));
        RemoteFileDownload oversized(*parse(list->descriptor), target.path()); QVERIFY(oversized.start());
        request = oversized.next(); const QByteArray tooMuch(request->size + 1, 'x');
        QVERIFY(!oversized.response(request->stream, reinterpret_cast<const uchar*>(tooMuch.constData()), tooMuch.size(), true));
        QCOMPARE(QDir(target.path()).entryList(QDir::Files | QDir::Hidden), QStringList({"one.bin"}));
        RemoteFileDownload missing(*parse(list->descriptor), target.filePath("gone")); QVERIFY(!missing.start());
    }
    void clipboardProtocolOffersHostFilesAndHonorsLocks() {
        QTemporaryDir root; QVERIFY(root.isValid()); write(root.filePath("a.txt"), "alpha");
        const auto list = buildLocalFileList({root.filePath("a.txt")}); QVERIFY(list);
        ClipboardProtocol protocol; const ClipboardStamp stamp{1, 1, 1};
        protocol.serverCapabilities(0x10); protocol.ready(); protocol.localFiles(list, stamp); QVERIFY(!protocol.localFileList()); // not authorized yet
        protocol.control({true, true, stamp}); protocol.localFiles(list, {1, 1, 2}); QVERIFY(!protocol.localFileList());
        protocol.localFiles(list, stamp); protocol.local("a.txt", stamp);
        QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{true, true}));
        QCOMPARE(*protocol.localFileList(), list->descriptor);
        protocol.localFiles(buildLocalFileList({root.filePath("a.txt")}), stamp); QVERIFY(!protocol.advertisement()); // unchanged list
        protocol.control({true, false, stamp}); QVERIFY(!protocol.localFileList());
        QCOMPARE(protocol.fileContents({}, 0, ClipboardFileContentsRange, 0, 5), std::optional<QByteArray>("alpha")); // a paste outlives focus
        protocol.lock(7); protocol.offer(true);
        QVERIFY(!protocol.fileContents({}, 0, ClipboardFileContentsRange, 0, 5));
        QCOMPARE(protocol.fileContents(7, 0, ClipboardFileContentsRange, 0, 5), std::optional<QByteArray>("alpha"));
        protocol.unlock(7); QVERIFY(!protocol.fileContents(7, 0, ClipboardFileContentsRange, 0, 5));
        protocol.control({true, true, stamp}); protocol.localFiles(list, stamp); protocol.lock(8);
        QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{false, true}));
        protocol.localFiles(nullptr, stamp); QCOMPARE(protocol.advertisement(), std::optional(ClipboardAdvertisement{}));
        QVERIFY(protocol.fileContents(8, 0, ClipboardFileContentsRange, 0, 5));
        protocol.control({false, false, {2, 1, 1}}); QVERIFY(!protocol.fileContents(8, 0, ClipboardFileContentsRange, 0, 5));
    }
    void clipboardProtocolLocksServerFileLists() {
        ClipboardProtocol protocol; const ClipboardStamp stamp{1, 1, 1};
        const auto list = descriptors({{"a.txt", false, 4}});
        const auto answer = [&] { return protocol.response(reinterpret_cast<const uchar*>(list.constData()), list.size(), true); };
        protocol.control({true, true, stamp}); protocol.ready(); protocol.serverCapabilities(0x10);
        protocol.offer(false, 0xC123);
        QVERIFY(!protocol.request()); // the lock goes first
        const auto first = protocol.takeLock(); QVERIFY(first); QVERIFY(!protocol.takeLock());
        QVERIFY(protocol.request()); QVERIFY(!answer());
        auto update = protocol.takeRemoteFiles(); QVERIFY(update && update->files); QCOMPARE(update->stamp, stamp);
        QCOMPARE(protocol.remoteFilesLock(update->id), first);
        protocol.pin(first); protocol.offer(false, 0xC124); QVERIFY(protocol.takeUnlocks().empty()); // a download still reads it
        QVERIFY(!protocol.remoteFilesLock(update->id));
        const auto second = protocol.takeLock(); QVERIFY(second); QVERIFY(*second != *first);
        protocol.unpin(); QCOMPARE(protocol.takeUnlocks(), std::vector<quint32>{*first});
        protocol.offer(true); QCOMPARE(protocol.takeUnlocks(), std::vector<quint32>{*second}); QVERIFY(!protocol.takeLock()); // text only
        protocol.offer(false, 0xC125); protocol.offer(true); QVERIFY(protocol.takeUnlocks().empty()); // never sent, never unlocked
        protocol.offer(false, 0xC126); protocol.control({true, false, stamp}); QVERIFY(!protocol.takeLock()); // only the focused route locks
        protocol.control({true, true, stamp}); QVERIFY(protocol.takeLock());
        protocol.control({false, false, {2, 1, 1}}); QCOMPARE(protocol.takeUnlocks().size(), size_t(1));
        ClipboardProtocol plain; plain.control({true, true, stamp}); plain.ready(); plain.offer(false, 0xC123);
        QVERIFY(!plain.takeLock()); QVERIFY(plain.request()); // servers without lock support
    }
    void clipboardProtocolServesServersWithoutLocks() {
        // xrdp: no CB_CAN_LOCK_CLIPDATA, and every FileContents request carries clipDataId 0.
        QTemporaryDir root; QVERIFY(root.isValid()); write(root.filePath("a.txt"), "alpha");
        ClipboardProtocol protocol; const ClipboardStamp stamp{1, 1, 1};
        protocol.serverCapabilities(0x02 | 0x04 | 0x08); protocol.ready(); protocol.control({true, true, stamp});
        protocol.localFiles(buildLocalFileList({root.filePath("a.txt")}), stamp);
        QCOMPARE(protocol.fileContents(0, 0, ClipboardFileContentsRange, 0, 5), std::optional<QByteArray>("alpha"));
        protocol.offer(true); QVERIFY(!protocol.fileContents(0, 0, ClipboardFileContentsRange, 0, 5)); // nothing kept once replaced
        protocol.control({true, true, stamp}); protocol.localFiles(buildLocalFileList({root.filePath("a.txt")}), stamp);
        protocol.control({false, false, {2, 1, 1}}); QVERIFY(!protocol.fileContents(0, 0, ClipboardFileContentsRange, 0, 5));
    }
    void clipboardProtocolRequestsRemoteFileListsAfterText() {
        ClipboardProtocol protocol; const ClipboardStamp stamp{1, 1, 1};
        protocol.control({true, true, stamp}); protocol.ready();
        protocol.offer(true, 0xC123);
        auto request = protocol.request(); QVERIFY(request); QCOMPARE(request->format, ClipboardUnicodeTextFormat);
        QVERIFY(!protocol.request());
        const auto text = encodeClipboardText("names");
        QVERIFY(protocol.response(reinterpret_cast<const uchar*>(text->constData()), text->size(), true));
        request = protocol.request(); QVERIFY(request); QCOMPARE(request->format, quint32(0xC123)); QVERIFY(!protocol.request());
        const auto list = descriptors({{"a.txt", false, 4}});
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(list.constData()), list.size(), true)); // no text delivery
        auto update = protocol.takeRemoteFiles(); QVERIFY(update); QVERIFY(update->files); QCOMPARE(update->files->bytes, quint64(4));
        QVERIFY(!protocol.takeRemoteFiles()); QVERIFY(protocol.remoteFiles(update->id));
        const auto first = update->id;
        protocol.offer(true); update = protocol.takeRemoteFiles(); QVERIFY(update); QVERIFY(!update->files);
        QVERIFY(update->id != first); QVERIFY(!protocol.remoteFiles(first));
        protocol.offer(false, 0xC124); protocol.control({true, false, stamp}); QVERIFY(!protocol.request()); // only the focused route asks
        protocol.control({true, true, stamp}); request = protocol.request(); QVERIFY(request); QCOMPARE(request->format, quint32(0xC124));
        const auto unsafe = descriptors({{"..\\x", false, 1}});
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(unsafe.constData()), unsafe.size(), true)); QVERIFY(!protocol.takeRemoteFiles());
        protocol.offer(false, 0xC125); QVERIFY(protocol.request());
        QVERIFY(!protocol.response(reinterpret_cast<const uchar*>(list.constData()), list.size(), true)); update = protocol.takeRemoteFiles();
        QVERIFY(update && update->files);
        protocol.reset(); const auto after = protocol.takeRemoteFiles(); QVERIFY(after); QVERIFY(!after->files); QVERIFY(after->id > update->id);
        protocol.offer(false, ClipboardUnicodeTextFormat); protocol.control({true, true, stamp}); protocol.ready(); QVERIFY(!protocol.request());
    }
};
QTEST_GUILESS_MAIN(ClipboardTest)
#include "session_clipboard.moc"
