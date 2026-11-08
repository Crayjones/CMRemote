#include "ProfileEditor.h"
#include "ConfigDescriptions.h"
#include "ConnectionIcons.h"
#include "qttreepropertybrowser_p.h"
#include "qtvariantproperty_p.h"
#include <QComboBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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
const std::vector<Field>& fieldsFor(bool documentRoot) { return documentRoot ? documentFields : fields; }
}

// Opens the inline editor for a clicked Config row. Rows stay plain text until
// then; read-only rows and group headers get no editor.
class ConfigEditorFactory final : public QtAbstractEditorFactory<QtVariantPropertyManager> {
public:
    explicit ConfigEditorFactory(ProfileEditor* editor) : QtAbstractEditorFactory<QtVariantPropertyManager>(editor), editor_(editor) {}
protected:
    void connectPropertyManager(QtVariantPropertyManager*) override {}
    void disconnectPropertyManager(QtVariantPropertyManager*) override {}
    QWidget* createEditor(QtVariantPropertyManager*, QtProperty* property, QWidget* parent) override { return editor_->createCellEditor(property, parent); }
private:
    ProfileEditor* editor_;
};

ProfileEditor::ProfileEditor(QtTreePropertyBrowser* grid)
    : QObject(grid), grid_(grid), manager_(new QtVariantPropertyManager(this)), factory_(new ConfigEditorFactory(this)) {
    setObjectName("profileEditor");
    grid_->setFactoryForManager(manager_, static_cast<QtAbstractEditorFactory<QtVariantPropertyManager>*>(factory_));
    grid_->setHeaderVisible(false); grid_->setRootIsDecorated(true); grid_->setIndentation(14);
    // Category rows span both columns on the header color, like mRemoteNG's grid.
    grid_->setPropertiesWithoutValueMarked(true); grid_->setAlternatingRowColors(false);
    grid_->setResizeMode(QtTreePropertyBrowser::Interactive); grid_->setSplitterPosition(labelWidth_);
    view_ = grid_->findChild<QTreeWidget*>(); view_->header()->setStretchLastSection(false);
    auto* tree = view_.data(); tree->setObjectName("configView"); tree->setUniformRowHeights(true);
    // The grid never scrolls sideways, so the start of every row stays in view.
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(tree->horizontalScrollBar(), &QScrollBar::valueChanged, this, [tree](int value) { if (value) tree->horizontalScrollBar()->setValue(0); });
    connect(tree->header(), &QHeaderView::sectionResized, this, [this](int section, int, int size) {
        if (section == 0 && !fitting_) { labelWidth_ = size; fitColumns(); }
    });
    tree->viewport()->installEventFilter(this);
    // Text and numbers commit on Enter or when the cell loses focus; Escape cancels.
    auto* delegate = tree->itemDelegate();
    connect(delegate, &QAbstractItemDelegate::commitData, this, &ProfileEditor::commitEditor);
    connect(delegate, &QAbstractItemDelegate::closeEditor, this, [this](QWidget* editor) {
        if (auto* line = qobject_cast<QLineEdit*>(editor); line && line->echoMode() == QLineEdit::Password) line->clear();
        if (active_ == editor) { active_.clear(); activeKey_.clear(); }
    });
    connect(grid_, &QtAbstractPropertyBrowser::currentItemChanged, this, &ProfileEditor::showHelp);
    load({}, {});
}
// The grid owns this editor and destroys it after its own browser state.
ProfileEditor::~ProfileEditor() = default;
QTreeWidget* ProfileEditor::view() const { return view_; }
void ProfileEditor::fitColumns() {
    // The value column fills the rest, keeping at least 80 px; the property column gives way first.
    const int available = view()->viewport()->width(), width = qBound(40, available - 80, labelWidth_);
    // Sized here because the hidden header does not re-stretch when the scrollbar appears.
    fitting_ = true; grid_->setSplitterPosition(width); view_->setColumnWidth(1, qMax(0, available - width)); fitting_ = false;
}
bool ProfileEditor::eventFilter(QObject* watched, QEvent* event) {
    if (view_ && watched == view_->viewport() && event->type() == QEvent::Resize) fitColumns();
    if (watched == active_ && event->type() == QEvent::Show) watched->installEventFilter(this); // ahead of the view's delegate
    if (watched == active_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        // Enter commits at once, as Escape (left to the delegate) cancels at once.
        if (key == Qt::Key_Return || key == Qt::Key_Enter) { flush(); return true; }
    }
    return QObject::eventFilter(watched, event);
}

QWidget* ProfileEditor::createCellEditor(QtProperty* property, QWidget* parent) {
    const auto key = keys_.value(property);
    if (key.isEmpty() || !rows_.value(key).editable || !grid_->isEnabled()) return nullptr;
    const auto& row = rows_[key];
    QWidget* editor = nullptr;
    switch (row.cell) {
    case Cell::Choice: {
        auto* combo = new QComboBox(parent); editor = combo; combo->setObjectName(key + "Combo");
        for (const auto& option : row.options) {
            const auto choice = option.toMap(); combo->addItem(qvariant_cast<QIcon>(choice.value("icon")), choice.value("text").toString(), choice.value("value"));
        }
        combo->setCurrentIndex(combo->findData(row.value));
        connect(combo, &QComboBox::activated, this, [this, combo = QPointer<QComboBox>(combo)] {
            // Popup acceptance must unwind before its editor is closed.
            QTimer::singleShot(0, this, [this, combo] { if (combo && active_ == combo) flush(); });
        });
        break;
    }
    case Cell::Number: {
        auto* spin = new QSpinBox(parent); editor = spin; spin->setObjectName(key + "Spin");
        spin->setRange(row.type == "port" ? 1 : 0, row.type == "port" ? 65535 : 240); spin->setKeyboardTracking(false);
        spin->setValue(row.value.toInt()); spin->selectAll();
        break;
    }
    case Cell::Password: {
        // The saved password never enters the grid: the editor starts empty and
        // hands what is typed straight to the password handler.
        if (!passwordHandler_) return nullptr;
        auto* line = new QLineEdit(parent); editor = line; line->setObjectName(key + "Edit");
        line->setEchoMode(QLineEdit::Password); line->setMaxLength(4096); line->setContextMenuPolicy(Qt::NoContextMenu);
        break;
    }
    case Cell::Text: {
        auto* line = new QLineEdit(parent); editor = line; line->setObjectName(key + "Edit");
        line->setText(row.value.toString()); line->selectAll();
        break;
    }
    case Cell::ReadOnly: return nullptr;
    }
    editor->setMinimumWidth(0); editor->installEventFilter(this); active_ = editor; activeKey_ = key;
    return editor;
}
void ProfileEditor::commitEditor(QWidget* editor) {
    if (!editor || editor != active_) return;
    const auto key = activeKey_; active_.clear(); activeKey_.clear();
    if (auto* line = qobject_cast<QLineEdit*>(editor); line && line->echoMode() == QLineEdit::Password) {
        if (!line->isModified() || line->text().isEmpty() || !passwordHandler_) return;
        auto bytes = line->text().toUtf8(); Secret secret(bytes); bytes.fill('\0'); bytes.clear();
        line->clear(); line->setModified(false);
        passwordEdited({PasswordEdit::Kind::Set, std::move(secret), false}); return;
    }
    QVariant value;
    if (auto* combo = qobject_cast<QComboBox*>(editor)) value = combo->currentData();
    else if (auto* spin = qobject_cast<QSpinBox*>(editor)) { spin->interpretText(); value = spin->value(); }
    else if (auto* line = qobject_cast<QLineEdit*>(editor)) value = line->text();
    else return;
    edited(key, value);
}
void ProfileEditor::closeEditor(QWidget* editor, bool revert) {
    if (auto* tree = view(); editor && tree) emit tree->itemDelegate()->closeEditor(editor, revert ? QAbstractItemDelegate::RevertModelCache : QAbstractItemDelegate::NoHint);
}
void ProfileEditor::flush() {
    QPointer<QWidget> editor = active_; if (!editor) return;
    commitEditor(editor); if (editor) closeEditor(editor, false);
}
void ProfileEditor::cancelEdit() {
    QPointer<QWidget> editor = active_; active_.clear(); activeKey_.clear(); if (editor) closeEditor(editor, true);
}
bool ProfileEditor::editing() const { return active_ != nullptr; }

QtBrowserItem* ProfileEditor::browserItem(const QString& key) const {
    const auto found = rows_.constFind(key);
    return found != rows_.cend() && found->property ? grid_->items(found->property).value(0) : nullptr;
}
QTreeWidgetItem* ProfileEditor::row(const QString& key) const {
    if (key.isEmpty()) return nullptr;
    for (QTreeWidgetItemIterator it(view()); *it; ++it) if ((*it)->data(0, KeyRole).toString() == key) return *it;
    return nullptr;
}
QString ProfileEditor::currentKey() const {
    auto* item = grid_->currentItem(); return item ? keys_.value(item->property()) : QString();
}
void ProfileEditor::setCurrentKey(const QString& key) {
    auto* item = browserItem(key); if (!item) return;
    if (auto* parent = item->parent()) grid_->setExpanded(parent, true);
    grid_->setCurrentItem(item);
    if (auto* cell = row(key)) view()->scrollToItem(cell);
}
bool ProfileEditor::rowEditable(const QString& key) const { return rows_.value(key).editable; }
bool ProfileEditor::rowLocal(const QString& key) const { return rows_.value(key).local; }
QVariant ProfileEditor::rowValue(const QString& key) const { return rows_.value(key).value; }
bool ProfileEditor::shownEditable(const QString& key) const { const auto row = rows_.value(key); return row.property && !row.hidden && row.editable; }
bool ProfileEditor::editRow(const QString& key) {
    if (!rowEditable(key)) return false;
    setCurrentKey(key); if (auto* item = browserItem(key)) grid_->editItem(item);
    return active_ != nullptr;
}
// QtTreePropertyBrowser marks every row editable and leaves it to the factory;
// the view items carry the property key and only the rows that really open an editor are editable.
void ProfileEditor::syncViewItems() {
    std::function<void(QtBrowserItem*, QTreeWidgetItem*)> visit = [&](QtBrowserItem* item, QTreeWidgetItem* cell) {
        const auto key = keys_.value(item->property());
        cell->setData(0, KeyRole, key);
        auto flags = cell->flags(); flags.setFlag(Qt::ItemIsEditable, !key.isEmpty() && rows_.value(key).editable);
        if (flags != cell->flags()) cell->setFlags(flags);
        const auto children = item->children();
        for (int i = 0; i < children.size() && i < cell->childCount(); ++i) visit(children[i], cell->child(i));
    };
    const auto top = grid_->topLevelItems(); auto* tree = view();
    for (int i = 0; i < top.size() && i < tree->topLevelItemCount(); ++i) visit(top[i], tree->topLevelItem(i));
}

void ProfileEditor::loadDefaults(const ConnectionDefaults& defaults, bool inheritance) {
    cancelEdit(); documentRoot_ = false; defaults_ = true; inheritance_ = inheritance;
    profile_ = defaults.json(); draft_ = profile_; effective_ = defaults.effective(); profiles_.clear(); valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    rows("protocol"); grid_->setEnabled(true); emit changed(); emit modeChanged();
}
void ProfileEditor::acceptDefaults() { profile_ = draft_; render(); emit changed(); }
void ProfileEditor::load(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles) {
    defaults_ = false; documentRoot_ = false; cancelEdit(); profile_ = profile; draft_ = profile; draft_.remove("effective"); effective_ = profile.value("effective").toObject(); profiles_ = profiles; valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    rows(); grid_->setEnabled(!id().isEmpty()); emit changed();
}
void ProfileEditor::loadDocument(std::optional<bool> protect, bool fullEncryption, const QString& help, bool editable) {
    cancelEdit(); const bool wasDocument = documentRoot_;
    defaults_ = false; documentRoot_ = true; documentEditable_ = editable && protect.has_value(); documentHelp_ = help;
    profile_ = QJsonObject{{"name", "Connections"}, {"protection", protect ? QJsonValue(*protect) : QJsonValue(QJsonValue::Null)}};
    profile_["full_encryption"] = fullEncryption;
    draft_ = profile_; effective_ = {}; profiles_.clear(); valid_ = true;
    passwordState_ = {}; passwordEditable_ = false; passwordPending_ = false; passwordReentry_ = false;
    if (wasDocument && !inheritance_) render(); else { inheritance_ = false; rows(); }
    grid_->setEnabled(true); emit changed();
}
void ProfileEditor::adopt(const QJsonObject& profile, const QHash<QString, QJsonObject>& profiles) {
    if (defaults_ || documentRoot_ || profile.value("id").toString() != id() || profile.value("kind") != profile_.value("kind")) { load(profile, profiles); return; }
    profile_ = profile; draft_ = profile; draft_.remove("effective"); effective_ = profile.value("effective").toObject(); profiles_ = profiles; valid_ = true;
    render(); emit changed();
}
void ProfileEditor::rows(const QString& selected) {
    const bool wasLoading = loading_; loading_ = true;
    cancelEdit();
    for (auto it = groups_.cbegin(); it != groups_.cend(); ++it)
        if (auto* item = grid_->items(it.value()).value(0)) expanded_[it.key()] = grid_->isExpanded(item);
    grid_->clear(); manager_->clear(); rows_.clear(); keys_.clear(); groups_.clear();
    // The row type follows the mode: inheritance shows every inherited setting as Yes/No.
    const auto cell = [this](const Field& field) {
        const QString type(field.type);
        if (inheritance_) return field.inherited ? Cell::Choice : Cell::ReadOnly;
        if (type == "choice" || type == "bool") return Cell::Choice;
        if (type == "port" || type == "minutes") return Cell::Number;
        if (type == "password") return Cell::Password;
        return type.isEmpty() ? Cell::ReadOnly : Cell::Text;
    };
    std::vector<const Field*> order;
    for (const auto& field : fieldsFor(documentRoot_)) if (!documentRoot_ || draft_.contains(field.key)) order.push_back(&field);
    if (!categorized_) std::stable_sort(order.begin(), order.end(), [](const Field* a, const Field* b) { return QString::compare(a->label, b->label, Qt::CaseInsensitive) < 0; });
    QStringList categories;
    for (const auto* field : order) {
        Row row; row.cell = cell(*field); row.type = field->type;
        const int type = row.cell == Cell::Choice ? QtVariantPropertyManager::enumTypeId() : row.cell == Cell::Number ? int(QMetaType::Int) : int(QMetaType::QString);
        row.property = manager_->addProperty(type, field->label);
        if (row.cell == Cell::Number) {
            manager_->setAttribute(row.property, "minimum", row.type == "port" ? 1 : 0);
            manager_->setAttribute(row.property, "maximum", row.type == "port" ? 65535 : 240);
        }
        keys_.insert(row.property, field->key); rows_.insert(field->key, row);
        if (categorized_) {
            const QString category(field->category);
            if (!groups_.contains(category)) { groups_.insert(category, manager_->addProperty(QtVariantPropertyManager::groupTypeId(), category)); categories << category; }
            groups_[category]->addSubProperty(row.property);
        } else grid_->addProperty(row.property);
    }
    for (const auto& category : std::as_const(categories)) {
        auto* item = grid_->addProperty(groups_[category]);
        grid_->setExpanded(item, expanded_.value(category, true));
    }
    render();
    auto key = rows_.contains(selected) ? selected : QString("name");
    if (!rows_.contains(key) || rows_[key].hidden) key = rows_.contains("protocol") ? "protocol" : "name";
    setCurrentKey(key); loading_ = wasLoading; showHelp();
}
void ProfileEditor::render() {
    const bool wasLoading = loading_; loading_ = true;
    const auto overrides = draft_.value("overrides").toObject();
    for (const auto& field : fieldsFor(documentRoot_)) {
        const QString key(field.key); auto found = rows_.find(key); if (found == rows_.end()) continue;
        auto& row = *found; auto* property = row.property; QVariant value; QVariantList options;
        const bool local = defaults_ ? !defaultsDraft().inherits(key) : key == "password" ? !passwordState_.inherited : !field.inherited || (overrides.contains(key) && !overrides.value(key).isNull());
        // Folders group connections; they have no endpoint of their own.
        const bool available = (!field.protocol || QString(field.protocol).split(',').contains(effective_.value("protocol").toString())) &&
            !(key == "host" && draft_.value("kind") == "folder");
        const bool defaultHidden = defaults_ && (QStringList{"name", "host", "parent"}.contains(key) || (key == "password" && !inheritance_));
        row.hidden = defaultHidden || !available || (inheritance_ && !field.inherited) || (!inheritance_ && key == "inherit_all");
        row.editable = !row.hidden && available && (inheritance_ ? field.inherited : !QString(field.type).isEmpty()) && (!documentRoot_ || documentEditable_) && (key != "password" || (defaults_ ? inheritance_ : (canEditPassword() && (!inheritance_ || !draft_.value("parent").toString().isEmpty()))));
        const bool boolean = QString(field.type) == "bool";
        if (inheritance_ && field.inherited) {
            value = !local; options = {choice("Yes", true), choice("No", false)};
        } else if (key == "protection" || key == "full_encryption") {
            value = draft_.value(key).toVariant(); options = {choice("No", false), choice("Yes", true)};
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
        if (!inheritance_) {
            if (boolean) options = {choice("No", false), choice("Yes", true)};
            if (key == "protocol") options = {choice("RDP", "rdp"), choice("SSH", "ssh"), choice("VNC", "vnc")};
            if (key == "ssh_auth") options = {choice("Password", "password"), choice("Private key", "private_key")};
            if (key == "resolution") options = {choice("Fit to Panel", "fit"), choice("800 × 600", "800x600"), choice("1024 × 768", "1024x768"), choice("1280 × 720", "1280x720"), choice("1920 × 1080", "1920x1080")};
            if (key == "color_depth") options = {choice("16-bit", 16), choice("24-bit", 24), choice("32-bit", 32)};
            if (key == "security_protocol") options = {choice("Automatic", "auto"), choice("NLA / CredSSP", "nla"), choice("Extended NLA", "ext"), choice("TLS", "tls"), choice("Legacy RDP Security", "rdp")};
            if (key == "server_auth") options = {choice("Always connect, even if authentication fails", "always"), choice("Don't connect if authentication fails", "required"), choice("Accept unverified certificates for this session", "warn")};
            if (key == "sound") options = {choice("Bring to this computer", "local"), choice("Leave at remote computer", "remote"), choice("Do not play", "none")};
            if (key == "gateway_usage") options = {choice("Never", "never"), choice("Always", "always"), choice("Detect", "detect")};
            if (key == "gateway_credentials") options = {choice("Use the same username and password", "same"), choice("Use a different username and password", "separate")};
            if (key == "icon") options = ConnectionIcons::choices(value.toString());
            if (key == "parent") {
                options = {choice("Connections", QString())}; QStringList ids = profiles_.keys();
                std::sort(ids.begin(), ids.end(), [this](const auto& a, const auto& b) { return QString::compare(profiles_[a].value("name").toString(), profiles_[b].value("name").toString(), Qt::CaseInsensitive) < 0; });
                for (const auto& candidate : ids) if (candidate != id() && profiles_[candidate].value("kind") == "folder") options.append(choice(profiles_[candidate].value("name").toString(), candidate));
            }
        }
        row.value = value; row.options = options; row.local = local && !inheritance_;
        property->setPropertyName(inheritance_ && field.inherited && key != "inherit_all" ? "Inherit " + QString(field.label) : QString(field.label));
        if (row.cell == Cell::Choice) {
            QStringList names; QMap<int, QIcon> icons; int index = -1;
            for (const auto& option : options) {
                const auto entry = option.toMap(); const auto icon = qvariant_cast<QIcon>(entry.value("icon"));
                if (!icon.isNull()) icons.insert(int(names.size()), icon);
                if (index < 0 && entry.value("value") == value) index = int(names.size());
                names << entry.value("text").toString();
            }
            // A value outside the choices still shows as text: an unknown password protection, say.
            if (index < 0) { index = int(names.size()); names << (key == "protection" && draft_.value(key).isNull() ? "Unknown — unlock file" : key == "parent" ? "Connections" : value.toString()); }
            manager_->setAttribute(property, "enumNames", names); manager_->setAttribute(property, "enumIcons", QVariant::fromValue(icons));
            manager_->setValue(property, index);
        } else if (row.cell == Cell::Number) manager_->setValue(property, value.toInt());
        else manager_->setValue(property, value.toString());
        property->setEnabled(available); property->setModified(row.local);
        const auto reference = configDescription(field.key); const auto tip = reference.isEmpty() ? QString(field.help) : reference;
        property->setValueToolTip(tip); property->setDescriptionToolTip(tip);
        if (auto* item = grid_->items(property).value(0)) grid_->setItemVisible(item, !row.hidden);
    }
    if (rows_.contains("inherit_all")) {
        bool inherited = true;
        for (const auto& field : fields)
            if (QString(field.key) != "inherit_all" && field.inherited && shownEditable(field.key)) inherited = inherited && rows_[field.key].value.toBool();
        auto& all = rows_["inherit_all"]; all.value = inherited;
        if (all.cell == Cell::Choice) manager_->setValue(all.property, inherited ? 0 : 1);
    }
    // A category shows while any of its settings does: RDP's Redirect and Gateway go away for SSH.
    for (auto* group : std::as_const(groups_)) {
        bool visible = false;
        for (auto* child : group->subProperties()) visible = visible || !rows_.value(keys_.value(child)).hidden;
        if (auto* item = grid_->items(group).value(0)) grid_->setItemVisible(item, visible);
    }
    syncViewItems();
    if (const auto key = currentKey(); !key.isEmpty() && rows_.value(key).hidden) setCurrentKey(rows_.contains("protocol") ? "protocol" : "name");
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
        if (!shownEditable(key)) return;
        if (key == "inherit_all") {
            for (const auto& field : fields) {
                const QString target(field.key);
                if (target != key && field.inherited && shownEditable(target)) defaults.inheritance[target] = value.toBool();
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
            const QString target(field.key);
            if (target == key || !field.inherited || !shownEditable(target)) continue;
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
void ProfileEditor::focusName() { if (!id().isEmpty()) editRow("name"); }
void ProfileEditor::setCategorized(bool categorized) {
    flush(); if (categorized == categorized_) return;
    const auto key = currentKey();
    const auto scroll = view()->verticalScrollBar()->value(); categorized_ = categorized; rows(key); view()->verticalScrollBar()->setValue(scroll); emit modeChanged();
}
void ProfileEditor::setInheritance(bool inheritance) {
    flush(); if (inheritance == inheritance_) return;
    // Inheritance shows the same settings as Yes/No choices, so the rows are rebuilt.
    inheritance_ = inheritance; rows(currentKey()); emit modeChanged();
}
bool ProfileEditor::canInherit() const {
    const auto key = currentKey();
    if (defaults_ || documentRoot_ || key.isEmpty() || !rows_.value(key).property->isEnabled()) return false;
    if (key == "inherit_all") return false;
    if (key == "password") return canEditPassword() && !draft_.value("parent").isNull() && !draft_.value("parent").toString().isEmpty();
    return std::any_of(std::begin(fields), std::end(fields), [&key](const auto& field) { return key == field.key && field.inherited; });
}
void ProfileEditor::useInherited() {
    flush(); if (!canInherit()) return;
    const auto key = currentKey();
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
    cancelEdit(); passwordHandler_ = std::move(handler); render();
}
void ProfileEditor::setPasswordState(const PasswordState& state, bool editable) {
    passwordState_ = state; passwordEditable_ = editable; render();
}
void ProfileEditor::finishPasswordEdit() {
    passwordPending_ = false; passwordReentry_ = false; render(); emit changed();
}
void ProfileEditor::requirePasswordReentry() {
    cancelEdit(); passwordReentry_ = passwordPending_; render(); emit changed();
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
    editRow("password");
}
void ProfileEditor::clearPassword() {
    cancelEdit(); if (canEditPassword()) passwordEdited({PasswordEdit::Kind::Clear, {}, false});
}
void ProfileEditor::showHelp() {
    const auto* item = grid_->currentItem(); if (!item) { emit helpChanged("Connections", "Select a connection or folder to edit its settings."); return; }
    const auto key = keys_.value(item->property());
    for (const auto& field : (documentRoot_ ? documentFields : fields)) if (key == field.key) {
        // One short description per property, like mRemoteNG's grid.
        const auto reference = documentRoot_ ? QString() : configDescription(key);
        QString help = reference.isEmpty() ? QString(field.help) : reference;
        if (inheritance_ && !documentRoot_) help.prepend("Description of inherited property: ");
        // The root's Name row shows which file is open; other rows keep their one-line description.
        if (documentRoot_ && key == "name") help = documentHelp_;
        emit helpChanged(item->property()->propertyName(), help); return;
    }
    emit helpChanged(item->property()->propertyName(), "Select a property value to edit. Enter or leaving the cell saves it; Escape cancels the cell.");
}
