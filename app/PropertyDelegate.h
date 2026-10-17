#pragma once
#include <QPointer>
#include <QStyledItemDelegate>
#include "PasswordEdit.h"

class PropertyDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum Role { KeyRole = Qt::UserRole, TypeRole, ValueRole, OptionsRole, LocalRole, CategoryRole };
    explicit PropertyDelegate(QObject* parent = nullptr);
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setEditorData(QWidget* editor, const QModelIndex& index) const override;
    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QWidget* activeEditor() const { return active_; }
    void flush();
    void cancel();
    void setPasswordHandler(std::function<void(PasswordEdit)> handler) { passwordHandler_ = std::move(handler); }
signals:
    void valueCommitted(const QString& key, const QVariant& value);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    mutable QPointer<QWidget> active_;
    std::function<void(PasswordEdit)> passwordHandler_;
};
