#include "PropertyDelegate.h"
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>

PropertyDelegate::PropertyDelegate(QObject* parent) : QStyledItemDelegate(parent) {
    connect(this, &QAbstractItemDelegate::closeEditor, this, [this](QWidget* editor) {
        if (editor->objectName() == "passwordEdit") if (auto* line = qobject_cast<QLineEdit*>(editor)) line->clear();
        if (active_ == editor) active_.clear();
    });
}
QWidget* PropertyDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const {
    if (index.column() != 1 || index.data(TypeRole).toString().isEmpty()) return nullptr;
    QWidget* editor;
    const auto type = index.data(TypeRole).toString();
    if (type == "choice") {
        auto* combo = new QComboBox(parent); editor = combo;
        connect(combo, &QComboBox::activated, const_cast<PropertyDelegate*>(this), [this, combo = QPointer<QComboBox>(combo)] {
            // Popup acceptance must unwind before its editor is closed.
            QTimer::singleShot(0, const_cast<PropertyDelegate*>(this), [this, combo] {
                if (combo && active_ == combo) const_cast<PropertyDelegate*>(this)->flush();
            });
        });
    } else if (type == "port" || type == "minutes") {
        auto* spin = new QSpinBox(parent); spin->setRange(type == "port" ? 1 : 0, type == "port" ? 65535 : 240); spin->setKeyboardTracking(false); editor = spin;
    }
    else if (type == "password") {
        if (!passwordHandler_) return nullptr;
        auto* line = new QLineEdit(parent); line->setEchoMode(QLineEdit::Password); line->setMaxLength(4096);
        line->setContextMenuPolicy(Qt::NoContextMenu); editor = line;
    } else editor = new QLineEdit(parent);
    const auto key = index.siblingAtColumn(0).data(KeyRole).toString();
    editor->setObjectName(type == "port" || type == "minutes" ? key + "Spin" : type == "choice" ? key + "Combo" : key + "Edit");
    editor->setMinimumWidth(0); active_ = editor; return editor;
}
void PropertyDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const {
    if (editor->property("propertyInitialized").toBool()) return;
    if (index.siblingAtColumn(0).data(KeyRole).toString() == "password" && index.data(TypeRole) == "password") {
        if (auto* line = qobject_cast<QLineEdit*>(editor)) { line->clear(); line->setModified(false); }
        editor->setProperty("propertyInitialized", true); return;
    }
    const auto value = index.data(ValueRole);
    if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        for (const auto& option : index.data(OptionsRole).toList()) {
            const auto choice = option.toMap(); combo->addItem(qvariant_cast<QIcon>(choice.value("icon")), choice.value("text").toString(), choice.value("value"));
        }
        combo->setCurrentIndex(combo->findData(value));
    } else if (auto* spin = qobject_cast<QSpinBox*>(editor)) { spin->setValue(value.toInt()); spin->selectAll(); }
    else if (auto* line = qobject_cast<QLineEdit*>(editor)) { line->setText(value.toString()); line->selectAll(); }
    editor->setProperty("propertyInitialized", true);
}
void PropertyDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const {
    const auto key = index.siblingAtColumn(0).data(KeyRole).toString();
    if (key == "password" && index.data(TypeRole) == "password") {
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (!line || !line->isModified() || line->text().isEmpty() || !passwordHandler_) return;
        auto bytes = line->text().toUtf8(); Secret secret(bytes); bytes.fill('\0'); bytes.clear();
        line->clear(); line->setModified(false);
        passwordHandler_({PasswordEdit::Kind::Set, std::move(secret), false}); return;
    }
    QVariant value;
    if (auto* combo = qobject_cast<QComboBox*>(editor)) value = combo->currentData();
    else if (auto* spin = qobject_cast<QSpinBox*>(editor)) { spin->interpretText(); value = spin->value(); }
    else value = qobject_cast<QLineEdit*>(editor)->text();
    model->setData(index, value, ValueRole);
    emit const_cast<PropertyDelegate*>(this)->valueCommitted(key, value);
}
void PropertyDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const {
    editor->setGeometry(option.rect.adjusted(1, 1, -1, -1));
}
void PropertyDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QStyleOptionViewItem cell(option); initStyleOption(&cell, index);
    if (index.siblingAtColumn(0).data(CategoryRole).toBool()) { cell.backgroundBrush = option.palette.color(QPalette::Window); cell.font.setBold(true); }
    else if (index.column() == 1) cell.font.setBold(index.data(LocalRole).toBool());
    const auto* style = cell.widget ? cell.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, cell.widget);
    // Grid lines: a tenth of the way from the cell background to the text color.
    const auto base = option.palette.color(QPalette::Base), text = option.palette.color(QPalette::Text);
    painter->save(); painter->setPen(QColor::fromRgbF(base.redF() * 0.9f + text.redF() * 0.1f, base.greenF() * 0.9f + text.greenF() * 0.1f, base.blueF() * 0.9f + text.blueF() * 0.1f));
    painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());
    painter->drawLine(option.rect.topRight(), option.rect.bottomRight()); painter->restore();
}
QSize PropertyDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const { return {80, option.fontMetrics.height() + 6}; }
bool PropertyDelegate::eventFilter(QObject* watched, QEvent* event) {
    if (watched == active_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape) { cancel(); return true; }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { flush(); return true; }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}
void PropertyDelegate::flush() {
    QPointer<QWidget> editor = active_; active_.clear();
    if (editor) emit commitData(editor);
    if (editor) emit closeEditor(editor, QAbstractItemDelegate::NoHint);
}
void PropertyDelegate::cancel() {
    QPointer<QWidget> editor = active_; active_.clear(); if (editor) emit closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
}
