#pragma once
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>

// Platform sockets for the LibVNCClient transport thread. LibVNCClient performs all
// reads/writes itself; these helpers only connect, wait and wake.
#ifdef _WIN32
using VncSocketHandle = std::uintptr_t;
#else
using VncSocketHandle = int;
#endif
VncSocketHandle invalidVncSocket();

// Makes a GUI-side signal visible to a thread blocked in waitVncSocket.
class VncWake final {
public:
    VncWake();
    ~VncWake();
    VncWake(const VncWake&) = delete;
    VncWake& operator=(const VncWake&) = delete;
    bool valid() const;
    void signal();
    void drain();
    VncSocketHandle handle() const { return read_; }
private:
    VncSocketHandle read_, write_;
};

// Called once by each transport thread: socket writes after a peer reset or a local
// shutdown fail with EPIPE instead of raising SIGPIPE.
void prepareVncTransportThread();
// Tries each resolved address (at most `perAddressMs` each) until one accepts. The
// socket is returned non-blocking, with TCP_NODELAY. `cancelled` aborts the attempt.
VncSocketHandle connectVncSocket(const QStringList& addresses, quint16 port, int perAddressMs, VncWake& wake,
                                 const std::atomic<bool>& cancelled);
enum class VncWait { Readable, Woken, Timeout, Error };
VncWait waitVncSocket(VncSocketHandle socket, VncWake& wake, int timeoutMs);
// Unblocks any thread reading or writing this socket; the owner still closes it.
void shutdownVncSocket(VncSocketHandle socket);
void closeVncSocket(VncSocketHandle socket);
