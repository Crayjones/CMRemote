#include "OpenH264.h"
#include "UpdateCheck.h"
#include "remotemanager-core/src/bridge.rs.h"
#include "rust/cxx.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QSaveFile>

#ifdef RM_WITH_RDP
// Added to FreeRDP by scripts/bootstrap.py (patch_freerdp).
extern "C" void freerdp_openh264_set_library(const char* path);
#endif

namespace OpenH264 {
std::optional<Binary> platformBinary() {
    // Checksums are of Cisco's decompressed 2.6.0 binaries; each matched Cisco's signed MD5 list.
    const auto binary = [](const char* name, const char* installed, const char* sha256) {
        return Binary{QUrl(QStringLiteral("http://ciscobinary.openh264.org/") + name + ".bz2"), installed, sha256};
    };
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return binary("libopenh264-2.6.0-mac-arm64.dylib", "libopenh264.dylib", "052e98bfcf7a9167d22f3bbb3f5988ef79065591f36af8b52924b22b13624551");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_X86_64)
    return binary("libopenh264-2.6.0-mac-x64.dylib", "libopenh264.dylib", "e3dc8bc01fe69363f61fd3c02fd27798537a585eadd38cd808f303d1ee505a19");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return binary("libopenh264-2.6.0-linux64.8.so", "libopenh264.so.8", "2f0cde7c6a6abcf5cae76942894ea42897fa677bce4ed6c91a24dd1b041d5f04");
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
    return binary("libopenh264-2.6.0-linux-arm64.8.so", "libopenh264.so.8", "12e7b33623667cdab0e575170c147b1b36eadb77d0d2aa7ceb5afd3e58902140");
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    // FreeRDP's Windows fallback finds the preloaded module by this name.
    return binary("openh264-2.6.0-win64.dll", "openh264.dll", "2076cb5675ec6c1a4c70e7a2a322552f547b6eeed649d6dfcd9e02a543b24691");
#else
    return std::nullopt;
#endif
}
void useLibrary(const QString& path) {
#ifdef RM_WITH_RDP
    freerdp_openh264_set_library(path.isEmpty() ? nullptr : QFile::encodeName(path).constData());
#else
    Q_UNUSED(path);
#endif
}
}

namespace {
constexpr qint64 MaxDownload = 8 * 1024 * 1024, MaxLibrary = 16 * 1024 * 1024;
QByteArray sha256(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
}

OpenH264Codec::OpenH264Codec(QString directory, QObject* parent, std::optional<OpenH264::Binary> binary)
    : QObject(parent), directory_(QDir(directory).filePath("openh264")), binary_(std::move(binary)) {}
OpenH264Codec::~OpenH264Codec() { if (reply_) reply_->abort(); }

QString OpenH264Codec::libraryPath() const { return binary_ ? QDir(directory_).filePath(binary_->fileName) : QString(); }

QString OpenH264Codec::statusText() const {
    switch (state_) {
    case State::Off: return "Off. Remote desktops use other graphics formats.";
    case State::Unsupported: return "Cisco does not publish OpenH264 for this platform.";
    case State::Downloading: return "Downloading OpenH264 from Cisco…";
    case State::Ready: return QString("OpenH264 %1 is installed and used for new sessions.").arg(OpenH264::Version);
    case State::Failed: return "Download failed: " + error_ + " Turn it off and on again to retry.";
    }
    return {};
}

void OpenH264Codec::setState(State state, QString error) {
    state_ = state; error_ = std::move(error);
    emit stateChanged();
}

void OpenH264Codec::setEnabled(bool enabled) {
    if (!enabled) {
        enabled_ = false;
        if (reply_) reply_->abort();
        OpenH264::useLibrary({});
        setState(State::Off);
        return;
    }
    if (enabled_ && state_ != State::Failed) return;
    enabled_ = true;
    if (!binary_) { setState(State::Unsupported); return; }
    if (activate()) return;
    download();
}

// Uses the installed library only when it still matches Cisco's pinned checksum.
bool OpenH264Codec::activate() {
    QFile file(libraryPath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxLibrary || sha256(file.readAll()) != binary_->sha256) return false;
    file.close();
    if (!module_) {
        auto* module = new QLibrary(libraryPath(), this);
        // Loading here proves it runs on this machine; on Windows FreeRDP then finds this module.
        if (!module->load()) { setState(State::Failed, module->errorString() + "."); delete module; return false; }
        module_ = module;
    }
    OpenH264::useLibrary(libraryPath());
    setState(State::Ready);
    return true;
}

void OpenH264Codec::download() {
    if (reply_) return;
    if (!network_) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request(binary_->url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "CMRemote/" + Updates::currentVersion());
    request.setTransferTimeout(60000);
    reply_ = network_->get(request);
    setState(State::Downloading);
    connect(reply_, &QNetworkReply::downloadProgress, reply_, [reply = reply_](qint64 received, qint64) {
        if (received > MaxDownload) reply->abort();
    });
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
        reply->deleteLater();
        if (!enabled_) return;
        if (reply->error() != QNetworkReply::NoError) { setState(State::Failed, reply->errorString() + "."); return; }
        QByteArray library;
        try {
            const auto compressed = reply->readAll();
            const auto bytes = rm::decompress_bzip2(rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t*>(compressed.constData()), size_t(compressed.size())), size_t(MaxLibrary));
            library = QByteArray(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
        } catch (const rust::Error& error) { setState(State::Failed, QString::fromUtf8(error.what()) + "."); return; }
        if (sha256(library) != binary_->sha256) { setState(State::Failed, "the file does not match Cisco's published OpenH264 " + QString(OpenH264::Version) + "."); return; }
        QSaveFile file(libraryPath());
        if (!QDir().mkpath(directory_) || !file.open(QIODevice::WriteOnly) || file.write(library) != library.size() || !file.commit()) {
            setState(State::Failed, "cannot save it to " + QDir::toNativeSeparators(directory_) + "."); return;
        }
        if (!activate() && state_ != State::Failed) setState(State::Failed, "the saved file could not be verified.");
    });
}
