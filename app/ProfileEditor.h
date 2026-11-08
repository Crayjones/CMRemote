#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QVariant>
#include <functional>
#include <optional>
#include "PasswordEdit.h"
#include "ConnectionDefaults.h"
class ConfigEditorFactory;
class QtBrowserItem;
class QtProperty;
class QtTreePropertyBrowser;
class QtVariantProperty;
class QtVariantPropertyManager;
class QTreeWidget;
class QTreeWidgetItem;

// The Config pane: a two-column property grid (QtTreePropertyBrowser) for one
// connection, folder, connection-file root or the Settings defaults. Rows are
// plain text until clicked; ConfigEditorFactory then opens the inline editor
// (text, masked password, port spin box, or a choice list for enums and Yes/No).
class ProfileEditor final : public QObject {
    Q_OBJECT
public:
    explicit ProfileEditor(QtTreePropertyBrowser* grid);
    ~ProfileEditor() override;
    enum class Mode { Properties, Inheritance, DefaultProperties, DefaultInheritance };
    // The property key ("host", "protocol", …) of a grid row, on its column-0 item.
    static constexpr int KeyRole = Qt::UserRole;
    Mode mode() const { return defaults_ ? (inheritance_ ? Mode::DefaultInheritance : Mode::DefaultProperties) : (inheritance_ ? Mode::Inheritance : Mode::Properties); }
    bool defaultsMode() const { return defaults_; }
    void loadDefaults(const ConnectionDefaults& defaults, bool inheritance);
    ConnectionDefaults defaultsDraft() const { return {draft_.value("values").toObject(), draft_.value("inheritance").toObject()}; }
    void acceptDefaults();
    void load(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles);
    void loadDocument(std::optional<bool> protect, bool fullEncryption, const QString& help, bool editable);
    bool documentRoot() const { return documentRoot_; }
    bool requestedProtection() const { return draft_.value("protection").toBool(); }
    bool requestedFullEncryption() const { return draft_.value("full_encryption").toBool(); }
    QJsonObject draft() const { return draft_; }
    bool dirty() const;
    QString id() const { return profile_.value("id").toString(); }
    void focusName();
    void flush();
    bool editing() const;
    void setCategorized(bool categorized);
    void setInheritance(bool inheritance);
    bool inheritance() const { return inheritance_; }
    void adopt(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles);
    void useInherited();
    void setSshKeyPath(const QString& path);
    void setIcon(const QString& name);
    QString icon() const { return effective_.value("icon").toString(); }
    bool canInherit() const;
    void preview(const QJsonObject& effective);
    void invalid() { valid_ = false; }
    bool valid() const { return valid_; }
    bool categorized() const { return categorized_; }
    void showHelp();
    void setPasswordHandler(std::function<void(PasswordEdit)> handler);
    void setPasswordState(const PasswordState& state, bool editable);
    void finishPasswordEdit();
    void requirePasswordReentry();
    bool passwordPending() const { return passwordPending_; }
    bool canEditPassword() const;
    void editPassword();
    void clearPassword();

    // Rows by property key.
    QTreeWidget* view() const;
    QTreeWidgetItem* row(const QString& key) const;
    QString currentKey() const;
    void setCurrentKey(const QString& key);
    bool rowEditable(const QString& key) const;
    bool rowLocal(const QString& key) const;
    QVariant rowValue(const QString& key) const;
    bool editRow(const QString& key);
    // The open inline editor, if any.
    QWidget* activeEditor() const { return active_; }
    void cancelEdit();
signals:
    void changed();
    void userCommitted();
    void modeChanged();
    void helpChanged(const QString& title, const QString& text);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    friend class ConfigEditorFactory;
    enum class Cell { ReadOnly, Text, Password, Number, Choice };
    struct Row {
        QtVariantProperty* property = nullptr;
        Cell cell = Cell::ReadOnly;
        QString type;            // the field's value type: "port", "minutes", …
        QVariant value;          // the typed value the row shows
        QVariantList options;    // choices: {text, value, icon}
        bool editable = false, local = false, hidden = false;
    };
    QWidget* createCellEditor(QtProperty* property, QWidget* parent);
    bool shownEditable(const QString& key) const;
    void rows(const QString& selected = {});
    void render();
    void syncViewItems();
    QtBrowserItem* browserItem(const QString& key) const;
    void edited(const QString& key, const QVariant& value);
    void passwordEdited(PasswordEdit edit);
    void commitEditor(QWidget* editor);
    void closeEditor(QWidget* editor, bool revert);
    QtTreePropertyBrowser* grid_;
    QPointer<QTreeWidget> view_;
    QtVariantPropertyManager* manager_;
    ConfigEditorFactory* factory_;
    // Property column width the user chose; narrow docks shrink it so both columns fit.
    int labelWidth_ = 140; bool fitting_ = false;
    void fitColumns();
    QHash<QString, Row> rows_;
    QHash<QtProperty*, QString> keys_;
    QHash<QString, QtVariantProperty*> groups_;
    QHash<QString, bool> expanded_;
    QPointer<QWidget> active_; QString activeKey_;
    QJsonObject profile_, draft_, effective_;
    QHash<QString, QJsonObject> profiles_;
    bool valid_ = true, loading_ = false, categorized_ = true, inheritance_ = false;
    bool documentRoot_ = false, documentEditable_ = false, defaults_ = false;
    QString documentHelp_;
    PasswordState passwordState_;
    std::function<void(PasswordEdit)> passwordHandler_;
    bool passwordEditable_ = false, passwordPending_ = false, passwordReentry_ = false;
};
