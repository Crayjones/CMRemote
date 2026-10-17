#pragma once
#include "TerminalAppearance.h"
#include <QObject>
#include <QByteArray>
#include <QColor>
#include <QPoint>
#include <QSize>
#include <vterm.h>
#include <deque>
#include <vector>

struct TerminalCell {
    QString text = " ";
    QColor foreground{240, 240, 240}, background{0, 0, 0};
    int width = 1;
    bool bold = false, italic = false, underline = false, strike = false, conceal = false;
};
class TerminalScreen final : public QObject {
    Q_OBJECT
public:
    explicit TerminalScreen(int rows = 24, int columns = 80, QObject* parent = nullptr);
    ~TerminalScreen() override;
    bool feed(const QByteArray& bytes);
    bool setAppearance(const TerminalAppearance& appearance);
    bool resizeCells(int rows, int columns);
    QSize cells() const { return QSize(columns_, rows_); }
    QPoint cursor() const;
    bool cursorVisible() const { return cursorVisible_; }
    bool alternate() const { return alternate_; }
    int historyLines() const { return alternate_ ? 0 : int(history_.size()); }
    size_t historyBytes() const { return historyBytes_; }
    quint64 firstLine() const { return firstLine_; }
    quint64 contentRevision() const { return contentRevision_; }
    quint64 selectionEpoch() const { return selectionEpoch_; }
    TerminalCell cell(int row, int column) const;
    TerminalCell lineCell(int line, int column) const;
    QString lineText(int line) const;
    QByteArray encodeKey(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE);
    QByteArray encodeText(const QString& text, VTermModifier modifiers = VTERM_MOD_NONE);
    QByteArray pasteStart();
    QByteArray pasteEnd();
signals:
    void changed();
    void appearanceChanged();
    void outgoing(QByteArray bytes);
    void encodingFailed();
private:
    TerminalCell convert(VTermScreenCell cell) const;
    TerminalCell blank() const;
    QByteArray finishEncoding();
    static void output(const char* bytes, size_t length, void* user);
    static int damage(VTermRect rect, void* user);
    static int move(VTermRect destination, VTermRect source, void* user);
    static int property(VTermProp property, VTermValue* value, void* user);
    static int push(int columns, const VTermScreenCell* cells, void* user);
    static int pop(int columns, VTermScreenCell* cells, void* user);
    static int clear(void* user);
    TerminalAppearance appearance_;
    VTerm* terminal_ = nullptr;
    VTermScreen* screen_ = nullptr;
    int rows_, columns_;
    bool cursorVisible_ = true, alternate_ = false, encoding_ = false;
    QByteArray encoded_;
    std::deque<std::vector<VTermScreenCell>> history_;
    size_t historyBytes_ = 0;
    quint64 firstLine_ = 0, contentRevision_ = 0, selectionEpoch_ = 0;
};
