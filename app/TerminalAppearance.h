#pragma once
#include <QColor>
#include <QByteArray>
#include <QString>
#include <array>
#include <optional>

struct TerminalAppearance {
    enum class Cursor { Block, Underline, Bar };
    QString fontFamily;
    int fontSize = 12;
    Cursor cursor = Cursor::Block;
    bool blink = false, boldBright = false;
    QColor foreground{187, 187, 187}, background{0, 0, 0};
    QColor selectionForeground{0, 0, 0}, selectionBackground{187, 187, 187}, cursorColor{187, 187, 187};
    std::array<QColor, 16> ansi{{
        QColor(0, 0, 0), QColor(187, 0, 0), QColor(0, 187, 0), QColor(187, 187, 0),
        QColor(0, 0, 187), QColor(187, 0, 187), QColor(0, 187, 187), QColor(187, 187, 187),
        QColor(85, 85, 85), QColor(255, 0, 0), QColor(0, 255, 0), QColor(255, 255, 0),
        QColor(0, 0, 255), QColor(255, 0, 255), QColor(0, 255, 255), QColor(255, 255, 255)
    }};
    static TerminalAppearance classic() { return {}; }
    static TerminalAppearance original();
    static TerminalAppearance tomorrowNight();
    bool valid() const;
    QByteArray encode() const;
    static std::optional<TerminalAppearance> decode(const QByteArray& bytes);
    bool operator==(const TerminalAppearance& other) const;
    bool operator!=(const TerminalAppearance& other) const { return !(*this == other); }
};
