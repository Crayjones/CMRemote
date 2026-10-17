#pragma once
#include <QByteArray>
#include <vector>

// Owned, non-shared secret storage. Callers must also clear any Qt input copies.
class Secret {
public:
    Secret() = default;
    explicit Secret(const QByteArray& bytes);
    explicit Secret(size_t size);
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;
    ~Secret();
    unsigned char* data() { return bytes_.data(); }
    const unsigned char* data() const { return bytes_.data(); }
    size_t size() const { return bytes_.size(); }
    void clear();
    Secret clone() const;
    QByteArray takeBytes();
private:
    std::vector<unsigned char> bytes_;
};
