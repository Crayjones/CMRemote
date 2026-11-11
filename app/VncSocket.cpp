#include "VncSocket.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
const VncSocketHandle Invalid = VncSocketHandle(INVALID_SOCKET);
bool nonBlocking(VncSocketHandle socket) { u_long on = 1; return ioctlsocket(SOCKET(socket), FIONBIO, &on) == 0; }
bool inProgress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
const VncSocketHandle Invalid = -1;
bool nonBlocking(int socket) {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(socket, F_SETFD, FD_CLOEXEC) == 0;
}
bool inProgress() { return errno == EINPROGRESS || errno == EINTR; }
#endif
// Waits for a socket to become writable (connected) or for the wake channel.
VncWait waitWritable(VncSocketHandle socket, VncWake& wake, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD fds[2] = {{SOCKET(socket), POLLWRNORM, 0}, {SOCKET(wake.handle()), POLLRDNORM, 0}};
    const int result = WSAPoll(fds, 2, timeoutMs);
#else
    pollfd fds[2] = {{socket, POLLOUT, 0}, {wake.handle(), POLLIN, 0}};
    int result;
    do result = poll(fds, 2, timeoutMs); while (result < 0 && errno == EINTR);
#endif
    if (result < 0) return VncWait::Error;
    if (result == 0) return VncWait::Timeout;
    if (fds[1].revents) return VncWait::Woken;
    return VncWait::Readable;
}
bool connected(VncSocketHandle socket) {
    int error = 0;
#ifdef _WIN32
    int length = sizeof(error);
    return getsockopt(SOCKET(socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
#else
    socklen_t length = sizeof(error);
    return getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
#endif
}
}

VncSocketHandle invalidVncSocket() { return Invalid; }

VncWake::VncWake() : read_(Invalid), write_(Invalid) {
#ifdef _WIN32
    // select/poll on Windows only accept sockets: a loopback datagram socket sends to itself.
    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) return;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
        ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || !nonBlocking(VncSocketHandle(socket))) {
        closesocket(socket); return;
    }
    read_ = write_ = VncSocketHandle(socket);
#else
    int fds[2];
    if (pipe(fds) != 0) return;
    if (!nonBlocking(fds[0]) || !nonBlocking(fds[1])) { close(fds[0]); close(fds[1]); return; }
    read_ = fds[0]; write_ = fds[1];
#endif
}
VncWake::~VncWake() {
#ifdef _WIN32
    if (read_ != Invalid) closesocket(SOCKET(read_));
#else
    if (read_ != Invalid) close(read_);
    if (write_ != Invalid) close(write_);
#endif
}
bool VncWake::valid() const { return read_ != Invalid && write_ != Invalid; }
void VncWake::signal() {
    const char byte = 1;
    // A full pipe already guarantees a pending wake; the result is irrelevant.
#ifdef _WIN32
    (void)send(SOCKET(write_), &byte, 1, 0);
#else
    (void)!write(write_, &byte, 1);
#endif
}
void VncWake::drain() {
    char buffer[64];
#ifdef _WIN32
    while (recv(SOCKET(read_), buffer, sizeof(buffer), 0) > 0) {}
#else
    while (read(read_, buffer, sizeof(buffer)) > 0) {}
#endif
}

void prepareVncTransportThread() {
#ifdef _WIN32
    static std::once_flag started;
    std::call_once(started, [] { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); });
#else
    // Blocked for this thread only: SIGPIPE stays pending and the write reports EPIPE.
    sigset_t pipe; sigemptyset(&pipe); sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
#endif
}

VncSocketHandle connectVncSocket(const QStringList& addresses, quint16 port, int perAddressMs, VncWake& wake,
                                 const std::atomic<bool>& cancelled) {
    for (const auto& text : addresses) {
        if (cancelled.load()) return Invalid;
        QHostAddress address(text);
        sockaddr_storage storage{}; socklen_t length = 0;
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            auto* v4 = reinterpret_cast<sockaddr_in*>(&storage);
            v4->sin_family = AF_INET; v4->sin_port = htons(port); v4->sin_addr.s_addr = htonl(address.toIPv4Address());
            length = sizeof(sockaddr_in);
        } else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            auto* v6 = reinterpret_cast<sockaddr_in6*>(&storage);
            v6->sin6_family = AF_INET6; v6->sin6_port = htons(port);
            const auto bytes = address.toIPv6Address(); std::memcpy(&v6->sin6_addr, bytes.c, 16);
            v6->sin6_scope_id = address.scopeId().toUInt();
            length = sizeof(sockaddr_in6);
        } else continue;
#ifdef _WIN32
        const VncSocketHandle socket = VncSocketHandle(::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP));
#else
        const VncSocketHandle socket = ::socket(storage.ss_family, SOCK_STREAM, IPPROTO_TCP);
#endif
        if (socket == Invalid) continue;
        bool ok = nonBlocking(socket);
#ifdef SO_NOSIGPIPE
        int on = 1; ok = ok && setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#endif
        if (ok && ::connect(socket, reinterpret_cast<sockaddr*>(&storage), length) != 0) {
            ok = inProgress();
            QElapsedTimer elapsed; elapsed.start();
            while (ok) {
                const auto remaining = perAddressMs - int(elapsed.elapsed());
                if (remaining <= 0 || cancelled.load()) { ok = false; break; }
                const auto wait = waitWritable(socket, wake, remaining);
                if (wait == VncWait::Woken) { wake.drain(); continue; }
                ok = wait == VncWait::Readable && connected(socket);
                break;
            }
        }
        if (ok) {
            int on = 1;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
            if (!cancelled.load()) return socket;
        }
        closeVncSocket(socket);
    }
    return Invalid;
}

VncWait waitVncSocket(VncSocketHandle socket, VncWake& wake, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD fds[2] = {{SOCKET(socket), POLLRDNORM, 0}, {SOCKET(wake.handle()), POLLRDNORM, 0}};
    const int result = WSAPoll(fds, 2, timeoutMs);
#else
    pollfd fds[2] = {{socket, POLLIN, 0}, {wake.handle(), POLLIN, 0}};
    int result;
    do result = poll(fds, 2, timeoutMs); while (result < 0 && errno == EINTR);
#endif
    if (result < 0) return VncWait::Error;
    if (result == 0) return VncWait::Timeout;
    // Data, EOF and errors are all handled by the next protocol read.
    if (fds[0].revents) return VncWait::Readable;
    return VncWait::Woken;
}

void shutdownVncSocket(VncSocketHandle socket) {
    if (socket == Invalid) return;
#ifdef _WIN32
    shutdown(SOCKET(socket), SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}
void closeVncSocket(VncSocketHandle socket) {
    if (socket == Invalid) return;
#ifdef _WIN32
    closesocket(SOCKET(socket));
#else
    close(socket);
#endif
}
