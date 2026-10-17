#include "TerminalAppearance.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

TerminalAppearance TerminalAppearance::original() {
    TerminalAppearance value; value.foreground = QColor(240, 240, 240); value.cursorColor = value.foreground;
    value.ansi = {{QColor(0, 0, 0), QColor(224, 0, 0), QColor(0, 224, 0), QColor(224, 224, 0),
        QColor(0, 0, 224), QColor(224, 0, 224), QColor(0, 224, 224), QColor(224, 224, 224),
        QColor(128, 128, 128), QColor(255, 64, 64), QColor(64, 255, 64), QColor(255, 255, 64),
        QColor(64, 64, 255), QColor(255, 64, 255), QColor(64, 255, 255), QColor(255, 255, 255)}};
    return value;
}
TerminalAppearance TerminalAppearance::tomorrowNight() {
    TerminalAppearance value; value.foreground = QColor("#eaeaea"); value.background = QColor("#1d1f21");
    value.selectionForeground = value.background; value.selectionBackground = value.foreground; value.cursorColor = value.foreground;
    value.ansi = {{QColor("#1d1f21"), QColor("#cc6666"), QColor("#b4bd68"), QColor("#f0c674"),
        QColor("#81a2be"), QColor("#b294bb"), QColor("#8abeb7"), QColor("#c6c8c6"),
        QColor("#666666"), QColor("#d54e53"), QColor("#b9ca4b"), QColor("#e6c547"),
        QColor("#7aa6da"), QColor("#c397d8"), QColor("#70c0b1"), QColor("#eaeaea")}};
    return value;
}
bool TerminalAppearance::valid() const {
    const auto color = [](const QColor& value) { return value.isValid() && value.alpha() == 255; };
    return fontFamily.size() <= 128 && std::none_of(fontFamily.cbegin(), fontFamily.cend(), [](QChar c) { return c.isNull() || !c.isPrint(); }) &&
        fontSize >= 6 && fontSize <= 48 && (cursor == Cursor::Block || cursor == Cursor::Underline || cursor == Cursor::Bar) &&
        color(foreground) && color(background) && color(selectionForeground) && color(selectionBackground) && color(cursorColor) &&
        std::all_of(ansi.cbegin(), ansi.cend(), color);
}
bool TerminalAppearance::operator==(const TerminalAppearance& other) const {
    return fontFamily == other.fontFamily && fontSize == other.fontSize && cursor == other.cursor && blink == other.blink && boldBright == other.boldBright &&
        foreground == other.foreground && background == other.background && selectionForeground == other.selectionForeground &&
        selectionBackground == other.selectionBackground && cursorColor == other.cursorColor && ansi == other.ansi;
}
QByteArray TerminalAppearance::encode() const {
    if (!valid()) return {};
    QJsonArray colors; for (const auto& color : ansi) colors.append(color.name());
    return QJsonDocument(QJsonObject{{"schema", 1}, {"font_family", fontFamily}, {"font_size", fontSize}, {"cursor", int(cursor)},
        {"blink", blink}, {"bold_bright", boldBright}, {"foreground", foreground.name()}, {"background", background.name()},
        {"selection_foreground", selectionForeground.name()}, {"selection_background", selectionBackground.name()},
        {"cursor_color", cursorColor.name()}, {"ansi", colors}}).toJson(QJsonDocument::Compact);
}
std::optional<TerminalAppearance> TerminalAppearance::decode(const QByteArray& bytes) {
    if (bytes.size() > 64 * 1024) return {};
    const auto document = QJsonDocument::fromJson(bytes); if (!document.isObject()) return {};
    const auto object = document.object();
    const QStringList keys{"schema", "font_family", "font_size", "cursor", "blink", "bold_bright", "foreground", "background", "selection_foreground", "selection_background", "cursor_color", "ansi"};
    if (object.size() != keys.size()) return {};
    for (const auto& key : keys) if (!object.contains(key)) return {};
    if (!object["schema"].isDouble() || object["schema"].toDouble() != 1 || !object["font_family"].isString() ||
        !object["font_size"].isDouble() || object["font_size"].toDouble() != object["font_size"].toInt(-1) ||
        !object["cursor"].isDouble() || object["cursor"].toDouble() != object["cursor"].toInt(-1) ||
        !object["blink"].isBool() || !object["bold_bright"].isBool() || !object["ansi"].isArray() || object["ansi"].toArray().size() != 16) return {};
    const auto color = [](const QJsonValue& value) -> QColor {
        const auto text = value.toString(); if (!value.isString() || text.size() != 7 || !text.startsWith('#')) return {};
        for (int i = 1; i < text.size(); ++i) if (!QString("0123456789abcdefABCDEF").contains(text[i])) return {};
        return QColor(text);
    };
    TerminalAppearance value; value.fontFamily = object["font_family"].toString(); value.fontSize = object["font_size"].toInt();
    value.cursor = static_cast<Cursor>(object["cursor"].toInt()); value.blink = object["blink"].toBool(); value.boldBright = object["bold_bright"].toBool();
    value.foreground = color(object["foreground"]); value.background = color(object["background"]);
    value.selectionForeground = color(object["selection_foreground"]); value.selectionBackground = color(object["selection_background"]); value.cursorColor = color(object["cursor_color"]);
    const auto colors = object["ansi"].toArray(); for (int i = 0; i < 16; ++i) value.ansi[size_t(i)] = color(colors[i]);
    return value.valid() ? std::optional<TerminalAppearance>(value) : std::nullopt;
}
