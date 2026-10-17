#pragma once
#include "SessionClipboard.h"

class TestClipboardEndpoint final : public ClipboardEndpoint {
public:
    int reads = 0, writes = 0, watches = 0;
    std::optional<QString> text = QString("synthetic existing host text");
    void watch() override { ++watches; }
    std::optional<QString> readText() override { ++reads; return text; }
    void writeText(const QString& value) override { ++writes; text = value; emit changed(); }
    void copy(QString value) { text = std::move(value); emit changed(); }
};
