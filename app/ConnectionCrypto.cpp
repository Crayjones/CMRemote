#include "ConnectionCrypto.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <QChar>
#include <cstring>
#include <memory>

namespace {
constexpr qsizetype SaltSize = 16, NonceSize = 16, TagSize = 16, Overhead = SaltSize + NonceSize + TagSize;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
bool validLimit(qsizetype limit) { return limit >= 0 && limit <= ConnectionCrypto::MaxPayload; }
bool equal(const Secret& value, const char* text) {
    const auto size = std::strlen(text); return value.size() == size && std::memcmp(value.data(), text, size) == 0;
}
std::optional<QByteArray> envelope(const QByteArray& value, qsizetype limit) {
    if (!validLimit(limit)) return std::nullopt;
    if (value.isEmpty()) return QByteArray();
    const auto maxEncoded = ((limit + Overhead + 2) / 3) * 4;
    // Convert.FromBase64String accepts ASCII whitespace. Bound it before copying.
    if (value.size() > maxEncoded + 4096) return std::nullopt;
    QByteArray compact; compact.reserve(qMin(value.size(), maxEncoded));
    for (char ch : value) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        if (compact.size() == maxEncoded) return std::nullopt;
        compact.append(ch);
    }
    if (compact.isEmpty()) return std::nullopt;
    const auto decoded = QByteArray::fromBase64Encoding(compact, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() < Overhead || decoded.decoded.size() > limit + Overhead ||
        decoded.decoded.toBase64() != compact) return std::nullopt;
    return decoded.decoded;
}
}

bool ConnectionCrypto::validIterations(quint32 iterations) { return iterations >= MinIterations && iterations <= MaxIterations; }
bool ConnectionCrypto::validNewMaster(const Secret& utf8) {
    const auto encoded = encodeMaster(utf8);
    if (!encoded || equal(utf8, "mR3m")) return false;
    bool nonWhitespace = false;
    for (size_t i = 0; i < encoded->size(); ++i) {
        const auto ch = encoded->data()[i]; if (ch == 0) return false;
        if (!QChar(ch).isSpace()) nonWhitespace = true;
    }
    return nonWhitespace;
}
std::optional<Secret> ConnectionCrypto::encodeMaster(const Secret& utf8) {
    if (utf8.size() == 0 || utf8.size() > 8192) return std::nullopt;
    // Valid UTF-8 for U+0000..U+00FF needs at most two bytes. Decode directly
    // into owned Secret storage: no QString/QVariant plaintext master copies.
    size_t count = 0;
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto ch = utf8.data()[i];
        if (ch > 0x7f) {
            if ((ch != 0xc2 && ch != 0xc3) || i + 1 == utf8.size()) return std::nullopt;
            const auto next = utf8.data()[++i]; if (next < 0x80 || next > 0xbf) return std::nullopt;
        }
        if (++count > 4096) return std::nullopt;
    }
    Secret result(count); size_t output = 0;
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto ch = utf8.data()[i];
        if (ch <= 0x7f) result.data()[output++] = ch;
        else { const auto next = utf8.data()[++i]; result.data()[output++] = static_cast<unsigned char>(((ch & 3) << 6) | (next & 0x3f)); }
    }
    return result;
}
bool ConnectionCrypto::validUtf8(const Secret& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const auto ch = value.data()[i]; if (ch < 0x80) continue;
        int more; quint32 code, min;
        if (ch >= 0xc2 && ch <= 0xdf) { more = 1; code = ch & 0x1f; min = 0x80; }
        else if (ch >= 0xe0 && ch <= 0xef) { more = 2; code = ch & 0xf; min = 0x800; }
        else if (ch >= 0xf0 && ch <= 0xf4) { more = 3; code = ch & 7; min = 0x10000; }
        else return false;
        if (size_t(more) >= value.size() - i) return false;
        while (more--) { const auto next = value.data()[++i]; if ((next & 0xc0) != 0x80) return false; code = (code << 6) | (next & 0x3f); }
        if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool ConnectionCrypto::validEnvelope(const QByteArray& value, qsizetype limit) { return envelope(value, limit).has_value(); }

namespace ConnectionCrypto::detail {
std::optional<Secret> deriveKey(const Secret& utf8, const QByteArray& salt, quint32 iterations) {
    if (!validIterations(iterations) || salt.size() != SaltSize) return std::nullopt;
    auto pass = encodeMaster(utf8); if (!pass) return std::nullopt;
    Secret key(32);
    if (PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(pass->data()), int(pass->size()),
        reinterpret_cast<const unsigned char*>(salt.constData()), int(salt.size()), int(iterations), EVP_sha1(), int(key.size()), key.data()) != 1)
        return std::nullopt;
    return key;
}
// Internal value primitive for deterministic incoming-vector tests. Public
// encrypt() never accepts caller-supplied entropy and never falls back to it.
std::optional<QByteArray> encryptWithEntropy(const Secret& master, quint32 iterations, const Secret& plain,
                                            const QByteArray& salt, const QByteArray& nonce, qsizetype limit) {
    if (!validLimit(limit) || plain.size() > size_t(limit) || salt.size() != SaltSize || nonce.size() != NonceSize) return std::nullopt;
    auto key = deriveKey(master, salt, iterations); if (!key) return std::nullopt;
    if (plain.size() == 0) return QByteArray();
    CipherContext ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, int(NonceSize), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key->data(), reinterpret_cast<const unsigned char*>(nonce.constData())) != 1)
        return std::nullopt;
    QByteArray cipher(qsizetype(plain.size()) + 16, '\0'), tag(TagSize, '\0'); int size = 0, tail = 0;
    if (EVP_EncryptUpdate(ctx.get(), nullptr, &size, reinterpret_cast<const unsigned char*>(salt.constData()), int(salt.size())) != 1 ||
        EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(cipher.data()), &size, plain.data(), int(plain.size())) != 1 ||
        EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(cipher.data()) + size, &tail) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, int(TagSize), tag.data()) != 1 ||
        size + tail != int(plain.size())) return std::nullopt;
    cipher.resize(size + tail);
    return (salt + nonce + cipher + tag).toBase64();
}
}

std::optional<QByteArray> ConnectionCrypto::encrypt(const Secret& master, quint32 iterations, const Secret& plain, qsizetype limit) {
    if (!validLimit(limit) || plain.size() > size_t(limit) || !validIterations(iterations) || !encodeMaster(master)) return std::nullopt;
    if (plain.size() == 0) return QByteArray();
    QByteArray salt(SaltSize, '\0'), nonce(NonceSize, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), int(salt.size())) != 1 ||
        RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), int(nonce.size())) != 1) return std::nullopt;
    return detail::encryptWithEntropy(master, iterations, plain, salt, nonce, limit);
}
std::optional<Secret> ConnectionCrypto::decrypt(const Secret& master, quint32 iterations, const QByteArray& value, qsizetype limit) {
    if (!validIterations(iterations) || !encodeMaster(master)) return std::nullopt;
    auto raw = envelope(value, limit); if (!raw) return std::nullopt;
    if (raw->isEmpty()) return Secret();
    const auto salt = raw->left(SaltSize), nonce = raw->mid(SaltSize, NonceSize);
    auto key = detail::deriveKey(master, salt, iterations); if (!key) return std::nullopt;
    CipherContext ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, int(NonceSize), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key->data(), reinterpret_cast<const unsigned char*>(nonce.constData())) != 1)
        return std::nullopt;
    const auto cipherSize = raw->size() - Overhead;
    Secret plain{size_t(cipherSize)}; int size = 0, tail = 0; unsigned char final[16] = {};
    if (EVP_DecryptUpdate(ctx.get(), nullptr, &size, reinterpret_cast<const unsigned char*>(salt.constData()), int(salt.size())) != 1 ||
        EVP_DecryptUpdate(ctx.get(), plain.data(), &size, reinterpret_cast<const unsigned char*>(raw->constData() + SaltSize + NonceSize), int(cipherSize)) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, int(TagSize), raw->data() + raw->size() - TagSize) != 1 ||
        EVP_DecryptFinal_ex(ctx.get(), final, &tail) != 1 || tail != 0 || size != cipherSize) return std::nullopt;
    // Failed authentication above destroys/cleanses the provisional plaintext.
    return plain;
}
