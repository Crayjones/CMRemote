#include "TerminalScreen.h"
#include <QThread>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
constexpr size_t scrollbackBytes = 16 * 1024 * 1024;
constexpr size_t scrollbackLines = 10000;
constexpr qsizetype drainBytes = 128 * 1024;
int handledCursor(VTermPos, VTermPos, int, void*) { return 1; }
int ignoredBell(void*) { return 1; }
int handledResize(int, int, void*) { return 1; }
}
TerminalScreen::TerminalScreen(int rows, int columns, QObject* parent) : QObject(parent), rows_(rows), columns_(columns) {
    if (rows < 1 || rows > 256 || columns < 1 || columns > 512) throw std::invalid_argument("Invalid terminal cell dimensions.");
    terminal_ = vterm_new(rows, columns); if (!terminal_) throw std::bad_alloc();
    vterm_set_utf8(terminal_, 1); screen_ = vterm_obtain_screen(terminal_);
    static const VTermScreenCallbacks callbacks = {damage, move, handledCursor, property, ignoredBell, handledResize, push, pop, clear};
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_SCREEN); vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, false); vterm_output_set_callback(terminal_, output, this);
    // No selection callbacks, fallbacks, title accumulation, or local actions.
    setAppearance(appearance_); vterm_screen_reset(screen_, 1); vterm_screen_flush_damage(screen_);
}
bool TerminalScreen::setAppearance(const TerminalAppearance& appearance) {
    Q_ASSERT(QThread::currentThread() == thread()); if (!appearance.valid()) return false;
    appearance_ = appearance; auto* state = vterm_obtain_state(terminal_);
    VTermColor foreground, background;
    vterm_color_rgb(&foreground, appearance.foreground.red(), appearance.foreground.green(), appearance.foreground.blue());
    vterm_color_rgb(&background, appearance.background.red(), appearance.background.green(), appearance.background.blue());
    vterm_screen_set_default_colors(screen_, &foreground, &background);
    for (int index = 0; index < 16; ++index) {
        const auto color = appearance.ansi[size_t(index)]; VTermColor value;
        vterm_color_rgb(&value, color.red(), color.green(), color.blue()); vterm_state_set_palette_color(state, index, &value);
    }
    // Keep original indices so the view can reversibly brighten existing bold cells.
    vterm_state_set_bold_highbright(state, 0); emit appearanceChanged(); return true;
}
TerminalScreen::~TerminalScreen() { vterm_free(terminal_); }
bool TerminalScreen::feed(const QByteArray& bytes) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (bytes.size() > drainBytes) return false;
    const auto consumed = vterm_input_write(terminal_, bytes.constData(), size_t(bytes.size()));
    vterm_screen_flush_damage(screen_); emit changed(); return consumed == size_t(bytes.size());
}
bool TerminalScreen::resizeCells(int rows, int columns) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (rows < 1 || rows > 256 || columns < 1 || columns > 512) return false;
    if (rows == rows_ && columns == columns_) return true;
    ++selectionEpoch_; vterm_set_size(terminal_, rows, columns); rows_ = rows; columns_ = columns;
    vterm_screen_flush_damage(screen_); emit changed(); return true;
}
QPoint TerminalScreen::cursor() const { VTermPos position; vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &position); return QPoint(position.col, position.row); }
TerminalCell TerminalScreen::convert(VTermScreenCell raw) const {
    TerminalCell cell; cell.text.clear(); cell.width = raw.chars[0] == std::numeric_limits<uint32_t>::max() ? 0 : raw.width;
    if (cell.width) {
        for (const auto value : raw.chars) {
            if (value == 0 || value > 0x10ffff) break;
            const char32_t character = value; cell.text += QString::fromUcs4(&character, 1);
        }
        if (cell.text.isEmpty()) cell.text = " ";
    }
    const auto color = [this](VTermColor value, bool bold) {
        if (VTERM_COLOR_IS_DEFAULT_FG(&value)) return appearance_.foreground;
        if (VTERM_COLOR_IS_DEFAULT_BG(&value)) return appearance_.background;
        if (bold && appearance_.boldBright && VTERM_COLOR_IS_INDEXED(&value) && value.indexed.idx < 8) value.indexed.idx += 8;
        // libvterm 0.3.3's extended ramps differ from the xterm palette.
        if (VTERM_COLOR_IS_INDEXED(&value) && value.indexed.idx >= 16) {
            const int index = value.indexed.idx;
            if (index < 232) {
                static constexpr int levels[] = {0, 95, 135, 175, 215, 255}; const int cube = index - 16;
                return QColor(levels[cube / 36], levels[(cube / 6) % 6], levels[cube % 6]);
            }
            const int gray = 8 + 10 * (index - 232); return QColor(gray, gray, gray);
        }
        vterm_screen_convert_color_to_rgb(screen_, &value); return QColor(value.rgb.red, value.rgb.green, value.rgb.blue);
    };
    cell.foreground = color(raw.fg, raw.attrs.bold); cell.background = color(raw.bg, false);
    if (raw.attrs.reverse) std::swap(cell.foreground, cell.background);
    cell.bold = raw.attrs.bold; cell.italic = raw.attrs.italic; cell.underline = raw.attrs.underline != 0;
    cell.strike = raw.attrs.strike; cell.conceal = raw.attrs.conceal; return cell;
}
TerminalCell TerminalScreen::blank() const {
    TerminalCell cell; cell.foreground = appearance_.foreground; cell.background = appearance_.background; return cell;
}
TerminalCell TerminalScreen::cell(int row, int column) const {
    if (row < 0 || row >= rows_ || column < 0 || column >= columns_) return blank();
    VTermScreenCell raw{}; vterm_screen_get_cell(screen_, VTermPos{row, column}, &raw); return convert(raw);
}
TerminalCell TerminalScreen::lineCell(int line, int column) const {
    if (line < 0 || column < 0 || column >= columns_) return blank();
    if (line < historyLines()) {
        const auto& cells = history_[size_t(line)];
        return size_t(column) < cells.size() ? convert(cells[size_t(column)]) : TerminalCell{};
    }
    return cell(line - historyLines(), column);
}
QString TerminalScreen::lineText(int line) const {
    QString text;
    for (int column = 0; column < columns_; ++column) text += lineCell(line, column).text;
    while (text.endsWith(' ')) text.chop(1); return text;
}
void TerminalScreen::output(const char* bytes, size_t length, void* user) {
    auto* self = static_cast<TerminalScreen*>(user);
    if (length > 32768 || (self->encoding_ && size_t(self->encoded_.size()) + length > 32768)) { emit self->encodingFailed(); return; }
    if (self->encoding_) self->encoded_.append(bytes, qsizetype(length));
    else emit self->outgoing(QByteArray(bytes, qsizetype(length)));
}
QByteArray TerminalScreen::finishEncoding() { encoding_ = false; return std::exchange(encoded_, {}); }
QByteArray TerminalScreen::encodeKey(VTermKey key, VTermModifier modifiers) {
    encoding_ = true; vterm_keyboard_key(terminal_, key, modifiers); return finishEncoding();
}
QByteArray TerminalScreen::encodeText(const QString& text, VTermModifier modifiers) {
    if (text.size() > 4096 || text.toUtf8().size() > 4096) return {};
    encoding_ = true;
    for (const auto codepoint : text.toUcs4()) vterm_keyboard_unichar(terminal_, codepoint, modifiers);
    return finishEncoding();
}
QByteArray TerminalScreen::pasteStart() { encoding_ = true; vterm_keyboard_start_paste(terminal_); return finishEncoding(); }
QByteArray TerminalScreen::pasteEnd() { encoding_ = true; vterm_keyboard_end_paste(terminal_); return finishEncoding(); }
int TerminalScreen::damage(VTermRect, void* user) { ++static_cast<TerminalScreen*>(user)->contentRevision_; return 1; }
int TerminalScreen::move(VTermRect, VTermRect, void* user) { ++static_cast<TerminalScreen*>(user)->contentRevision_; return 1; }
int TerminalScreen::property(VTermProp property, VTermValue* value, void* user) {
    auto* self = static_cast<TerminalScreen*>(user);
    if (property == VTERM_PROP_CURSORVISIBLE) self->cursorVisible_ = value->boolean;
    if (property == VTERM_PROP_ALTSCREEN && self->alternate_ != bool(value->boolean)) {
        ++self->selectionEpoch_; self->alternate_ = value->boolean;
    }
    return 1;
}
int TerminalScreen::push(int columns, const VTermScreenCell* cells, void* user) {
    auto* self = static_cast<TerminalScreen*>(user); if (self->alternate_) return 1;
    if (columns < 1 || columns > 512) return 0;
    self->history_.emplace_back(cells, cells + columns); self->historyBytes_ += size_t(columns) * sizeof(VTermScreenCell);
    while (self->history_.size() > scrollbackLines || self->historyBytes_ > scrollbackBytes) {
        self->historyBytes_ -= self->history_.front().size() * sizeof(VTermScreenCell);
        self->history_.pop_front(); ++self->firstLine_;
    }
    return 1;
}
int TerminalScreen::pop(int columns, VTermScreenCell* cells, void* user) {
    auto* self = static_cast<TerminalScreen*>(user);
    if (self->alternate_ || self->history_.empty() || columns < 1 || columns > 512) return 0;
    auto line = std::move(self->history_.back()); self->history_.pop_back(); self->historyBytes_ -= line.size() * sizeof(VTermScreenCell);
    VTermScreenCell blank{}; blank.width = 1;
    vterm_state_get_default_colors(vterm_obtain_state(self->terminal_), &blank.fg, &blank.bg);
    for (int column = 0; column < columns; ++column) cells[column] = size_t(column) < line.size() ? line[size_t(column)] : blank;
    return 1;
}
int TerminalScreen::clear(void* user) {
    auto* self = static_cast<TerminalScreen*>(user); ++self->selectionEpoch_;
    self->firstLine_ += self->history_.size(); self->history_.clear(); self->historyBytes_ = 0; return 1;
}
