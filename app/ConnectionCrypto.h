#pragma once
#include "Secret.h"
#include <functional>
#include <optional>
#include <vector>

// Native mRemoteNG value codec. Blocking; use only on the document worker.
namespace ConnectionCrypto {
constexpr quint32 MinIterations = 1000, MaxIterations = 1000000, NewIterations = 10000;
constexpr qsizetype MaxPayload = 16 * 1024 * 1024;
bool validIterations(quint32 iterations);
bool validNewMaster(const Secret& utf8);
bool validUtf8(const Secret& value);
std::optional<Secret> encodeMaster(const Secret& utf8);
bool validEnvelope(const QByteArray& value, qsizetype limit = MaxPayload);
std::optional<QByteArray> encrypt(const Secret& master, quint32 iterations, const Secret& plain, qsizetype limit = MaxPayload);
std::optional<Secret> decrypt(const Secret& master, quint32 iterations, const QByteArray& value, qsizetype limit = MaxPayload);
}
