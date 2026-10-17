#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include "PasswordEdit.h"
#include "ConnectionDefaults.h"
class PropertyDelegate;
class QTreeWidget;
class QTreeWidgetItem;

class ProfileEditor final : public QObject {
    Q_OBJECT
public:
    explicit ProfileEditor(QTreeWidget* tree);
    enum class Mode { Properties, Inheritance, DefaultProperties, DefaultInheritance };
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
signals:
    void changed();
    void userCommitted();
    void modeChanged();
    void helpChanged(const QString& title, const QString& text);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void rows(const QString& selected = {});
    void render();
    void edited(const QString& key, const QVariant& value);
    void passwordEdited(PasswordEdit edit);
    QTreeWidget* tree_;
    // Property column width the user chose; narrow docks shrink it so both columns fit.
    int labelWidth_ = 140; bool fitting_ = false;
    void fitColumns();
    PropertyDelegate* delegate_;
    QHash<QString, QTreeWidgetItem*> items_;
    QHash<QString, bool> expanded_;
    QJsonObject profile_, draft_, effective_;
    QHash<QString, QJsonObject> profiles_;
    bool valid_ = true, loading_ = false, categorized_ = true, inheritance_ = false;
    bool documentRoot_ = false, documentEditable_ = false, defaults_ = false;
    QString documentHelp_;
    PasswordState passwordState_;
    std::function<void(PasswordEdit)> passwordHandler_;
    bool passwordEditable_ = false, passwordPending_ = false, passwordReentry_ = false;
};
