#include "Secret.h"
#include <cstring>
#ifdef RM_SECRET_OPENSSL
#include <openssl/crypto.h>
#endif

Secret::Secret(const QByteArray& bytes) : bytes_(size_t(bytes.size())) { if (!bytes_.empty()) std::memcpy(data(), bytes.constData(), size()); }
Secret::Secret(size_t size) : bytes_(size) {}
Secret::Secret(Secret&& other) noexcept : bytes_(std::move(other.bytes_)) {}
Secret& Secret::operator=(Secret&& other) noexcept { if (this != &other) { clear(); bytes_ = std::move(other.bytes_); } return *this; }
Secret::~Secret() { clear(); }
void Secret::clear() {
#ifdef RM_SECRET_OPENSSL
    if (!bytes_.empty()) OPENSSL_cleanse(data(), size());
#else
    // Qt-only preview links shared editor types without a crypto/storage backend.
    // Volatile stores keep clearing observable even in an optimized build.
    volatile unsigned char* bytes = data();
    for (size_t i = 0; i < size(); ++i) bytes[i] = 0;
#endif
    std::vector<unsigned char>().swap(bytes_);
}
Secret Secret::clone() const { Secret copy(size()); if (size()) std::memcpy(copy.data(), data(), size()); return copy; }
QByteArray Secret::takeBytes() { QByteArray copy(reinterpret_cast<const char*>(data()), qsizetype(size())); clear(); return copy; }
