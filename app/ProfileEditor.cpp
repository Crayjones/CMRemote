#include "ProfileEditor.h"
#include "PropertyDelegate.h"
#include "ConfigDescriptions.h"
#include "ConnectionIcons.h"
#include <QHeaderView>
#include <QKeyEvent>
#include <QTreeWidget>
#include <QScrollBar>
#include <algorithm>
#include <vector>

namespace {
int defaultPort(const QString& protocol) { return protocol == "ssh" ? 22 : protocol == "vnc" ? 5900 : 3389; }
struct Field { const char* key; const char* label; const char* category; const char* type; const char* help; bool inherited; const char* protocol = nullptr; };
const std::vector<Field> documentFields{
    {"name", "Name", "Display", "", "", false},
    {"protection", "Password protect", "Display", "choice", "Require a custom password to open this file; without one, saved passwords are only obfuscated.", false},
    {"full_encryption", "Encrypt entire file", "Display", "choice", "Encrypt the whole connection list, not just the passwords.", false}
};
const std::vector<Field> fields = {
    {"inherit_all", "Inherit All", "General", "choice", "Toggle all inheritance options.", true},
    {"name", "Name", "Display", "text", "Connection or folder label. Enter or leaving the cell saves the completed value.", false},
    {"description", "Description", "Display", "text", "Description, at most 4096 UTF-8 bytes. An empty local value differs from inheritance.", true},
    {"icon", "Icon", "Display", "choice", "The icon is saved by name. Unavailable imported icons are preserved and shown with a default icon.", true},
    {"parent", "Parent", "Display", "choice", "Choose the folder this connection inherits its settings from.", false},
    {"host", "Hostname/IP", "Connection", "text", "Remote hostname or IP address. An empty local value is an explicit override.", true},
    {"username", "Username", "Connection", "text", "SSH sends this username literally; Domain is inactive. For RDP with a separate Domain, use an unqualified username. A qualified RDP username or UPN requires an empty Domain.", true, "rdp,ssh"},
    {"password", "Password", "Connection", "password", "Saved password. Type a new one to replace it; saved passwords are never shown.", true},
    {"domain", "Domain", "Connection", "text", "RDP account domain, at most 256 UTF-8 bytes. Empty preserves qualified usernames or UPNs. Preserved but inactive for SSH and VNC; never joined to the SSH username.", true, "rdp"},
    {"ssh_auth", "SSH authentication", "Connection", "choice", "Choose whether to sign in with a password or a private key.", true, "ssh"},
    {"ssh_key_path", "SSH key file", "Connection", "text", "Enter the path to your private key file.", true, "ssh"},
    {"protocol", "Protocol", "Protocol", "choice", "RDP connects through embedded FreeRDP; SSH uses an embedded terminal. VNC uses an embedded desktop with negotiated ZRLE/Tight/TRLE/CopyRect/Raw and aspect-fit scaling; the server controls its resolution. Classic VNC traffic is unencrypted; outside your local network use a VPN or SSH tunnel (a public address is flagged in Notifications). VNC uses the saved Password when the server requires one, asks otherwise, and connects without a password when the server allows it. TLS/vendor authentication, proxies, clipboard and advanced commands are unsupported.", true},
    {"port", "Port", "Protocol", "port", "TCP port, 1–65535. Inherits the parent override or protocol default.", true},
    {"console_session", "Use console session", "Protocol", "bool", "Request the server's console/admin session. The server may ignore this preference.", true, "rdp"},
    {"security_protocol", "Security Protocol", "Protocol", "choice", "Choose the RDP security negotiation; Automatic lets the server choose and NLA requires Use CredSSP.", true, "rdp"},
    {"server_auth", "Server Authentication", "Protocol", "choice", "Choose how the server's identity certificate is checked.", true, "rdp"},
    {"use_credssp", "Use CredSSP", "Protocol", "bool", "Allow Network Level Authentication (CredSSP).", true, "rdp"},
    {"idle_minutes", "Minutes to Idle", "Protocol", "minutes", "Minutes without input before disconnecting, 0–240; 0 disables.", true, "rdp"},
    {"load_balance_info", "Load Balance Info", "Protocol", "text", "Routing token for RD Connection Broker or load balancers, sent verbatim.", true, "rdp"},
    {"sound", "Sounds", "Redirect", "choice", "Choose where remote audio plays.", true, "rdp"},
    {"clipboard", "Clipboard", "Redirect", "bool", "Copy and paste text between this computer and the remote computer. Files and images are not shared.", true, "rdp"},
    {"gateway_usage", "Use RDP Gateway", "Gateway", "choice", "Choose when to use a Remote Desktop Gateway.", true, "rdp"},
    {"gateway_host", "Gateway Hostname", "Gateway", "text", "Remote Desktop Gateway host name or IP address.", true, "rdp"},
    {"gateway_port", "Gateway Port", "Gateway", "port", "Remote Desktop Gateway HTTPS port, 1–65535 (default 443).", true, "rdp"},
    {"gateway_credentials", "Gateway Credentials", "Gateway", "choice", "Choose whether the gateway uses the connection's credentials.", true, "rdp"},
    {"gateway_username", "Gateway Username", "Gateway", "text", "Gateway account user name.", true, "rdp"},
    {"gateway_domain", "Gateway Domain", "Gateway", "text", "Gateway account domain.", true, "rdp"},
    {"resolution", "Resolution", "Appearance", "choice", "Initial remote desktop size. Fit to Panel captures the panel size when connecting. The server may negotiate a different size.", true, "rdp"},
    {"automatic_resize", "Automatic resize", "Appearance", "bool", "This native client uses FreeRDP, not Microsoft's RDC. Request later desktop-size changes when the panel changes size; requires server display-control support. No keeps the initial remote size and scales pointer coordinates to it.", true, "rdp"},
    {"color_depth", "Colors", "Appearance", "choice", "Request 16, 24 or 32-bit color. The server may negotiate a different depth.", true, "rdp"},
    {"cache_bitmaps", "Cache bitmaps", "Appearance", "bool", "Request bitmap caching. The server may ignore this preference.", true, "rdp"},
    {"wallpaper", "Display wallpaper", "Appearance", "bool", "Allow remote wallpaper. The server may ignore this preference.", true, "rdp"},
    {"themes", "Display themes", "Appearance", "bool", "Allow remote themes. The server may ignore this preference.", true, "rdp"},
    {"font_smoothing", "Font smoothing", "Appearance", "bool", "Request font smoothing. The server may ignore this preference.", true, "rdp"},
    {"smart_size", "SmartSize Mode", "Appearance", "", "VNC scales the remote desktop to fit the panel while keeping its aspect ratio. The server controls the desktop resolution. Other SmartSize modes are not supported.", false, "vnc"},
    {"view_only", "View Only", "Appearance", "bool", "VNC only. Yes suppresses all remote keyboard and pointer input; No permits input. Inheritance follows the parent view-only setting. The default is No.", true, "vnc"}
};
QVariant choice(const QString& text, const QVariant& value) { return QVariantMap{{"text", text}, {"value", value}}; }
}
ProfileEditor::ProfileEditor(QTreeWidget* tree) : QObject(tree), tree_(tree), delegate_(new PropertyDelegate(tree)) {
    setObjectName("profileEditor"); tree_->setItemDelegate(delegate_); tree_->setRootIsDecorated(true); tree_->setIndentation(14);
    tree_->setUniformRowHeights(true); tree_->setColumnCount(2); tree_->setHeaderLabels({"Property", "Value"});
    tree_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    tree_->installEventFilter(this);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive); tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed); tree_->header()->setStretchLastSection(false); tree_->setColumnWidth(0, labelWidth_);
    // The grid never scrolls sideways, so the start of every row stays in view.
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(tree_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) { if (value) tree_->horizontalScrollBar()->setValue(0); });
    connect(tree_->header(), &QHeaderView::sectionResized, this, [this](int section, int, int size) {
        if (section == 0 && !fitting_) { labelWidth_ = size; fitColumns(); }
    });
    tree_->viewport()->installEventFilter(this);
    connect(delegate_, &PropertyDelegate::valueCommitted, this, &ProfileEditor::edited);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &ProfileEditor::showHelp); load({}, {});
}
void ProfileEditor::fitColumns() {
    // The value column fills the rest, keeping at least 80 px; the property column gives way first.
    // Sized here because the hidden header does not re-stretch when the scrollbar appears.
    const int available = tree_->viewport()->width(), width = qBound(40, available - 80, labelWidth_);
    fitting_ = true; tree_->setColumnWidth(0, width); tree_->setColumnWidth(1, qMax(0, available - width)); fitting_ = false;
}
bool ProfileEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_->viewport() && event->type() == QEvent::Resize) fitColumns();
    if (watched == tree_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if ((key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            // Enter activates items on Linux; Config always opens the value editor.
            auto* item = tree_->currentItem();
            if (tree_->isEnabled() && item && (item->flags() & Qt::ItemIsEditable) && !delegate_->activeEditor()) tree_->editItem(item, 1);
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}
void ProfileEditor::loadDefaults(const ConnectionDefaults& defaults, bool inheritance) {
    delegate_->cancel(); documentRoot_ = false; defaults_ = true; inheritance_ = inheritance;
    profile_ = defaults.json(); draft_ = profile_; effective_ = defaults.effective(); profiles_.clear(); valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    rows("protocol"); tree_->setEnabled(true); emit changed(); emit modeChanged();
}
void ProfileEditor::acceptDefaults() { profile_ = draft_; render(); emit changed(); }
void ProfileEditor::load(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles) {
    defaults_ = false; documentRoot_ = false; delegate_->cancel(); profile_ = profile; draft_ = profile; draft_.remove("effective"); effective_ = profile.value("effective").toObject(); profiles_ = profiles; valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    rows(); tree_->setEnabled(!id().isEmpty()); emit changed();
}
void ProfileEditor::loadDocument(std::optional<bool> protect, bool fullEncryption, const QString& help, bool editable) {
    delegate_->cancel(); const bool wasDocument = documentRoot_;
    defaults_ = false; documentRoot_ = true; documentEditable_ = editable && protect.has_value(); documentHelp_ = help;
    profile_ = QJsonObject{{"name", "Connections"}, {"protection", protect ? QJsonValue(*protect) : QJsonValue(QJsonValue::Null)}};
    profile_["full_encryption"] = fullEncryption;
    draft_ = profile_; effective_ = {}; profiles_.clear(); valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    if (wasDocument) render(); else rows();
    tree_->setEnabled(true); emit changed();
}
void ProfileEditor::adopt(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles) {
    if (defaults_ || documentRoot_ || profile.value("id").toString() != id() || profile.value("kind") != profile_.value("kind")) { load(profile, profiles); return; }
    profile_ = profile; draft_ = profile; draft_.remove("effective"); effective_ = profile.value("effective").toObject(); profiles_ = profiles; valid_ = true;
    render(); emit changed();
}
void ProfileEditor::rows(const QString& selected) {
    loading_ = true;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* item = tree_->topLevelItem(i); if (item->data(0, PropertyDelegate::CategoryRole).toBool()) expanded_[item->text(0)] = item->isExpanded();
    }
    tree_->clear(); items_.clear(); QHash<QString, QTreeWidgetItem*> categories;
    for (const auto& field : (documentRoot_ ? documentFields : fields)) {
        const QString key(field.key), category(field.category);
        if (documentRoot_ && !draft_.contains(key)) continue;
        QTreeWidgetItem* group = nullptr;
        if (categorized_) {
            if (!categories.contains(category)) {
                group = new QTreeWidgetItem(tree_, {category}); categories.insert(category, group);
                group->setData(0, PropertyDelegate::CategoryRole, true); group->setFirstColumnSpanned(true); group->setExpanded(expanded_.value(category, true));
                group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            } else group = categories.value(category);
        }
        auto* item = new QTreeWidgetItem(QStringList{QString(field.label), QString()});
        if (group) group->addChild(item); else tree_->addTopLevelItem(item);
        item->setData(0, PropertyDelegate::KeyRole, key); item->setData(1, PropertyDelegate::TypeRole, QString(field.type));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (key == "protocol") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("RDP", "rdp"), choice("SSH", "ssh"), choice("VNC", "vnc")});
        items_.insert(key, item);
    }
    if (!categorized_) tree_->sortItems(0, Qt::AscendingOrder);
    render();
    auto* current = items_.value(selected, items_.value("name"));
    if (current && current->isHidden()) current = items_.value("protocol", items_.value("name"));
    tree_->setCurrentItem(current, 1); loading_ = false; showHelp();
}
void ProfileEditor::render() {
    const bool wasLoading = loading_; loading_ = true;
    const auto overrides = draft_.value("overrides").toObject();
    for (const auto& field : (documentRoot_ ? documentFields : fields)) {
        const QString key(field.key); auto* item = items_.value(key); if (!item) continue; QVariant value;
        const bool local = defaults_ ? !defaultsDraft().inherits(key) : key == "password" ? !passwordState_.inherited : !field.inherited || (overrides.contains(key) && !overrides.value(key).isNull());
        // Folders group connections; they have no endpoint of their own.
        const bool available = (!field.protocol || QString(field.protocol).split(',').contains(effective_.value("protocol").toString())) &&
            !(key == "host" && draft_.value("kind") == "folder");
        item->setText(0, inheritance_ && field.inherited && key != "inherit_all" ? "Inherit " + QString(field.label) : QString(field.label));
        const bool defaultHidden = defaults_ && (QStringList{"name", "host", "parent"}.contains(key) || (key == "password" && !inheritance_));
        item->setHidden(defaultHidden || !available || (inheritance_ && !field.inherited) || (!inheritance_ && key == "inherit_all"));
        const bool editable = !item->isHidden() && available && (inheritance_ ? field.inherited : !QString(field.type).isEmpty()) && (!documentRoot_ || documentEditable_) && (key != "password" || (defaults_ ? inheritance_ : (canEditPassword() && (!inheritance_ || !draft_.value("parent").toString().isEmpty()))));
        item->setFlags(Qt::ItemIsSelectable | (available ? Qt::ItemIsEnabled : Qt::NoItemFlags) | (editable ? Qt::ItemIsEditable : Qt::NoItemFlags));
        const bool boolean = QString(field.type) == "bool";
        item->setData(1, PropertyDelegate::TypeRole, inheritance_ ? (field.inherited ? "choice" : "") : (boolean ? "choice" : field.type));
        if (inheritance_ && field.inherited) {
            value = !local; item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Yes", true), choice("No", false)});
        } else if (key == "protection" || key == "full_encryption") {
            value = draft_.value(key).toVariant(); item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("No", false), choice("Yes", true)});
        } else if (key == "password") {
            if (passwordReentry_) value = "Re-enter password or Revert";
            else if (passwordPending_) value = "Pending password change";
            else if (!passwordHandler_) value = "Unavailable";
            else if (passwordState_.present) value = QString(8, QChar(0x2022));
            else value = QString();
        }
        else if (key == "smart_size") value = "Aspect";
        else if (!field.inherited) value = draft_.value(key).toString();
        else value = (local && !defaults_ ? overrides.value(key) : effective_.value(key)).toVariant();
        if (!inheritance_ && boolean) item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("No", false), choice("Yes", true)});
        if (!inheritance_ && key == "protocol") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("RDP", "rdp"), choice("SSH", "ssh"), choice("VNC", "vnc")});
        if (!inheritance_ && key == "ssh_auth") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Password", "password"), choice("Private key", "private_key")});
        if (!inheritance_ && key == "resolution") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Fit to Panel", "fit"), choice("800 × 600", "800x600"), choice("1024 × 768", "1024x768"), choice("1280 × 720", "1280x720"), choice("1920 × 1080", "1920x1080")});
        if (!inheritance_ && key == "color_depth") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("16-bit", 16), choice("24-bit", 24), choice("32-bit", 32)});
        if (!inheritance_ && key == "security_protocol") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Automatic", "auto"), choice("NLA / CredSSP", "nla"), choice("Extended NLA", "ext"), choice("TLS", "tls"), choice("Legacy RDP Security", "rdp")});
        if (!inheritance_ && key == "server_auth") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Always connect, even if authentication fails", "always"), choice("Don't connect if authentication fails", "required"), choice("Accept unverified certificates for this session", "warn")});
        if (!inheritance_ && key == "sound") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Bring to this computer", "local"), choice("Leave at remote computer", "remote"), choice("Do not play", "none")});
        if (!inheritance_ && key == "gateway_usage") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Never", "never"), choice("Always", "always"), choice("Detect", "detect")});
        if (!inheritance_ && key == "gateway_credentials") item->setData(1, PropertyDelegate::OptionsRole, QVariantList{choice("Use the same username and password", "same"), choice("Use a different username and password", "separate")});
        if (!inheritance_ && key == "icon") item->setData(1, PropertyDelegate::OptionsRole, ConnectionIcons::choices(value.toString()));
        item->setIcon(1, key == "icon" && !inheritance_ ? ConnectionIcons::resolve(value.toString(), draft_.value("kind") == "folder") : QIcon());
        item->setData(1, PropertyDelegate::ValueRole, value); item->setData(1, PropertyDelegate::LocalRole, local && !inheritance_);
        QString text = value.toString();
        if (key == "protection" && draft_.value(key).isNull()) text = "Unknown — unlock file";
        else if (key == "protection" || key == "full_encryption" || boolean || (inheritance_ && field.inherited)) text = value.toBool() ? "Yes" : "No";
        else if (key == "protocol") text = text.toUpper();
        else if (!inheritance_ && QStringList{"resolution", "color_depth", "ssh_auth", "icon", "security_protocol", "server_auth", "gateway_usage", "gateway_credentials", "sound"}.contains(key))
            for (const auto& option : item->data(1, PropertyDelegate::OptionsRole).toList()) if (option.toMap().value("value") == value) text = option.toMap().value("text").toString();
        if (key == "parent") {
            QVariantList options{choice("Connections", QString())}; QStringList ids = profiles_.keys();
            std::sort(ids.begin(), ids.end(), [this](const auto& a, const auto& b) { return QString::compare(profiles_[a].value("name").toString(), profiles_[b].value("name").toString(), Qt::CaseInsensitive) < 0; });
            for (const auto& candidate : ids) if (candidate != id() && profiles_[candidate].value("kind") == "folder") options.append(choice(profiles_[candidate].value("name").toString(), candidate));
            item->setData(1, PropertyDelegate::OptionsRole, options);
            text = "Connections"; for (const auto& option : item->data(1, PropertyDelegate::OptionsRole).toList()) if (option.toMap().value("value") == value) text = option.toMap().value("text").toString();
        }
        item->setText(1, text); const auto reference = configDescription(field.key); item->setToolTip(1, reference.isEmpty() ? QString(field.help) : reference);
    }
    if (auto* all = items_.value("inherit_all")) {
        bool inherited = true;
        for (const auto& field : fields) {
            auto* item = items_.value(field.key);
            if (QString(field.key) != "inherit_all" && field.inherited && item && !item->isHidden() && (item->flags() & Qt::ItemIsEditable))
                inherited = inherited && item->data(1, PropertyDelegate::ValueRole).toBool();
        }
        all->setData(1, PropertyDelegate::ValueRole, inherited); all->setText(1, inherited ? "Yes" : "No");
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* group = tree_->topLevelItem(i); if (!group->data(0, PropertyDelegate::CategoryRole).toBool()) continue;
        bool visible = false; for (int j = 0; j < group->childCount(); ++j) visible = visible || !group->child(j)->isHidden();
        group->setHidden(!visible);
    }
    if (auto* selected = tree_->currentItem(); selected && (selected->isHidden() || (selected->parent() && selected->parent()->isHidden())))
        tree_->setCurrentItem(items_.value("protocol", items_.value("name")), 1);
    loading_ = wasLoading; showHelp();
}
void ProfileEditor::edited(const QString& key, const QVariant& value) {
    if (loading_) return;
    if (documentRoot_) {
        if ((key != "protection" && key != "full_encryption") || !draft_.contains(key) || !documentEditable_ || inheritance_ || value.toBool() == draft_.value(key).toBool()) return;
        draft_[key] = value.toBool(); emit changed(); render(); emit userCommitted(); return;
    }
    if (defaults_) {
        auto defaults = defaultsDraft();
        const auto* row = items_.value(key);
        if (!row || row->isHidden() || !(row->flags() & Qt::ItemIsEditable)) return;
        if (key == "inherit_all") {
            for (const auto& field : fields) {
                const QString target(field.key); const auto* item = items_.value(target);
                if (target != key && field.inherited && item && !item->isHidden() && (item->flags() & Qt::ItemIsEditable)) defaults.inheritance[target] = value.toBool();
            }
        } else if (inheritance_) defaults.inheritance[key] = value.toBool();
        else {
            const auto previous = defaults.effective().value(key);
            if (previous == QJsonValue::fromVariant(value)) { render(); return; }
            defaults.values[key] = QJsonValue::fromVariant(value); defaults.inheritance[key] = false;
            if (key == "protocol" && defaults.values.contains("port") && defaults.values.value("port").toInt() == defaultPort(previous.toString()))
                defaults.values["port"] = defaultPort(value.toString());
        }
        if (draft_ == defaults.json()) return;
        draft_ = defaults.json(); effective_ = defaults.effective(); valid_ = ConnectionDefaults::parse(draft_).has_value();
        render(); emit changed(); emit userCommitted(); return;
    }
    if (id().isEmpty()) return;
    if (key == "inherit_all") {
        if (!inheritance_) return;
        auto overrides = draft_.value("overrides").toObject(); const auto before = overrides;
        bool password = false;
        for (const auto& field : fields) {
            const QString target(field.key); const auto* row = items_.value(target);
            if (target == key || !field.inherited || !row || row->isHidden() || !(row->flags() & Qt::ItemIsEditable)) continue;
            if (target == "password") { password = value.toBool() != passwordState_.inherited; continue; }
            overrides[target] = value.toBool() ? QJsonValue(QJsonValue::Null) : effective_.value(target);
        }
        draft_["overrides"] = overrides;
        if (password) passwordEdited({PasswordEdit::Kind::Inheritance, {}, value.toBool()});
        else if (before != overrides) { emit changed(); render(); emit userCommitted(); }
        return;
    }
    if (key == "password") {
        if (inheritance_ && canEditPassword() && value.toBool() != passwordState_.inherited)
            passwordEdited({PasswordEdit::Kind::Inheritance, {}, value.toBool()});
        return;
    }
    const auto before = draft_;
    if (!inheritance_ && key == "name") draft_[key] = value.toString();
    else if (!inheritance_ && key == "parent") draft_[key] = value.toString().isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value.toString());
    else {
        const auto field = std::find_if(std::begin(fields), std::end(fields), [&key](const auto& field) { return key == field.key && field.inherited; });
        if (field == std::end(fields)) return;
        auto overrides = draft_.value("overrides").toObject();
        if (inheritance_) overrides[key] = value.toBool() ? QJsonValue(QJsonValue::Null) : effective_.value(key);
        else {
            const auto current = !overrides.contains(key) || overrides.value(key).isNull() ? effective_.value(key) : overrides.value(key);
            if (current == QJsonValue::fromVariant(value)) { render(); return; }
            overrides[key] = QJsonValue::fromVariant(value);
            // A port still at the old protocol's default follows the new one (RDP 3389 → SSH 22).
            // A port that only comes from the protocol default already follows it.
            if (key == "protocol" && effective_.value("port").toInt() == defaultPort(current.toString()) &&
                effective_.value("sources").toObject().value("port").toString() != "default")
                overrides["port"] = defaultPort(value.toString());
        }
        draft_["overrides"] = overrides;
    }
    if (before != draft_) { emit changed(); render(); emit userCommitted(); }
}
void ProfileEditor::preview(const QJsonObject& effective) { effective_ = effective; valid_ = true; render(); }
bool ProfileEditor::dirty() const { auto committed = profile_; committed.remove("effective"); return passwordPending_ || draft_ != committed; }
void ProfileEditor::flush() { delegate_->flush(); }
bool ProfileEditor::editing() const { return delegate_->activeEditor() != nullptr; }
void ProfileEditor::focusName() { auto* item = items_.value("name"); if (!item || id().isEmpty()) return; if (item->parent()) item->parent()->setExpanded(true); tree_->scrollToItem(item); tree_->setCurrentItem(item, 1); tree_->editItem(item, 1); }
void ProfileEditor::setCategorized(bool categorized) {
    flush(); if (categorized == categorized_) return;
    const auto key = tree_->currentItem() ? tree_->currentItem()->data(0, PropertyDelegate::KeyRole).toString() : QString();
    const auto scroll = tree_->verticalScrollBar()->value(); categorized_ = categorized; rows(key); tree_->verticalScrollBar()->setValue(scroll); emit modeChanged();
}
void ProfileEditor::setInheritance(bool inheritance) {
    flush(); if (inheritance == inheritance_) return;
    inheritance_ = inheritance; render(); emit modeChanged();
}
bool ProfileEditor::canInherit() const {
    if (defaults_ || documentRoot_ || !tree_->currentItem() || !(tree_->currentItem()->flags() & Qt::ItemIsEnabled)) return false;
    const auto key = tree_->currentItem()->data(0, PropertyDelegate::KeyRole).toString();
    if (key == "inherit_all") return false;
    if (key == "password") return canEditPassword() && !draft_.value("parent").isNull() && !draft_.value("parent").toString().isEmpty();
    return std::any_of(std::begin(fields), std::end(fields), [&key](const auto& field) { return key == field.key && field.inherited; });
}
void ProfileEditor::useInherited() {
    flush(); if (!canInherit()) return;
    const auto key = tree_->currentItem()->data(0, PropertyDelegate::KeyRole).toString();
    if (key == "password") { if (!passwordState_.inherited) passwordEdited({PasswordEdit::Kind::Inheritance, {}, true}); return; }
    auto overrides = draft_.value("overrides").toObject(); if (!overrides.contains(key) || overrides.value(key).isNull()) return;
    overrides[key] = QJsonValue(QJsonValue::Null); draft_["overrides"] = overrides;
    emit changed(); render(); emit userCommitted();
}
void ProfileEditor::setIcon(const QString& name) {
    if (documentRoot_ || (!defaults_ && id().isEmpty())) return;
    setInheritance(false); edited("icon", name);
}
void ProfileEditor::setSshKeyPath(const QString& path) {
    if (documentRoot_ || id().isEmpty() || (draft_.value("kind") != "folder" && effective_.value("protocol") != "ssh")) return;
    setInheritance(false); edited("ssh_key_path", path);
}
void ProfileEditor::setPasswordHandler(std::function<void(PasswordEdit)> handler) {
    delegate_->cancel(); passwordHandler_ = std::move(handler);
    if (passwordHandler_) delegate_->setPasswordHandler([this](PasswordEdit edit) { passwordEdited(std::move(edit)); });
    else delegate_->setPasswordHandler({});
    render();
}
void ProfileEditor::setPasswordState(const PasswordState& state, bool editable) {
    passwordState_ = state; passwordEditable_ = editable; render();
}
void ProfileEditor::finishPasswordEdit() {
    passwordPending_ = false; passwordReentry_ = false; render(); emit changed();
}
void ProfileEditor::requirePasswordReentry() {
    delegate_->cancel(); passwordReentry_ = passwordPending_; render(); emit changed();
}
bool ProfileEditor::canEditPassword() const {
    const bool privateKey = draft_.value("kind") == "connection" && effective_.value("protocol") == "ssh" && effective_.value("ssh_auth") == "private_key";
    return !documentRoot_ && !id().isEmpty() && !privateKey && passwordEditable_ && bool(passwordHandler_);
}
void ProfileEditor::passwordEdited(PasswordEdit edit) {
    if (!canEditPassword()) return;
    passwordPending_ = true; passwordReentry_ = false;
    passwordHandler_(std::move(edit));
    render(); emit changed(); emit userCommitted();
}
void ProfileEditor::editPassword() {
    if (!canEditPassword()) return;
    if (inheritance_) setInheritance(false);
    auto* item = items_.value("password"); if (!item) return;
    if (item->parent()) item->parent()->setExpanded(true);
    tree_->scrollToItem(item); tree_->setCurrentItem(item, 1); tree_->editItem(item, 1);
}
void ProfileEditor::clearPassword() {
    delegate_->cancel(); if (canEditPassword()) passwordEdited({PasswordEdit::Kind::Clear, {}, false});
}
void ProfileEditor::showHelp() {
    const auto* item = tree_->currentItem(); if (!item) { emit helpChanged("Connections", "Select a connection or folder to edit its settings."); return; }
    const auto key = item->data(0, PropertyDelegate::KeyRole).toString();
    for (const auto& field : (documentRoot_ ? documentFields : fields)) if (key == field.key) {
        // One short description per property, like mRemoteNG's grid.
        const auto reference = documentRoot_ ? QString() : configDescription(key);
        QString help = reference.isEmpty() ? QString(field.help) : reference;
        if (inheritance_ && !documentRoot_) help.prepend("Description of inherited property: ");
        // The root's Name row shows which file is open; other rows keep their one-line description.
        if (documentRoot_ && key == "name") help = documentHelp_;
        emit helpChanged(item->text(0), help); return;
    }
    emit helpChanged(item->text(0), "Select a property value to edit. Enter or leaving the cell saves it; Escape cancels the cell.");
}
