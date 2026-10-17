#include "ConnectionXml.h"
#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "remotemanager-core/src/bridge.rs.h"
#include <QSet>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <openssl/crypto.h>
#include <algorithm>
#include <functional>
#include <vector>
#include <map>
#include <QMap>

namespace {
const QString Namespace = "http://mremoteng.org";
const QStringList SecretAttributes{"Password", "VNCProxyPassword", "RDGatewayPassword", "RDGatewayAccessToken"};
struct Attribute { QString name, uri, value; };
struct Declaration { QString prefix, uri; };
struct Node {
    enum Kind { Element, Text, Comment, Instruction } kind = Element;
    QString name, uri, value, id;
    std::vector<Attribute> attributes;
    std::vector<Declaration> declarations;
    std::vector<Node> children;
    bool hasAttribute(const QString& name) const {
        for (const auto& a : attributes) if (a.uri.isEmpty() && a.name == name) return true;
        return false;
    }
    QString attribute(const QString& name) const {
        for (const auto& a : attributes) if (a.uri.isEmpty() && a.name == name) return a.value;
        return {};
    }
    void setAttribute(const QString& name, const QString& value) {
        for (auto& a : attributes) if (a.uri.isEmpty() && a.name == name) { a.value = value; return; }
        attributes.push_back({name, {}, value});
    }
    bool profile() const { return kind == Element && name == "Node" && uri.isEmpty(); }
};
struct Tree { Node root; std::vector<Node> before, after; };
bool stopped(const ConnectionXml::Canceled& canceled) { return canceled && canceled(); }
Node passive(QXmlStreamReader& reader) {
    Node n;
    if (reader.isCharacters()) { n.kind = Node::Text; n.value = reader.text().toString(); }
    else if (reader.isComment()) { n.kind = Node::Comment; n.value = reader.text().toString(); }
    else { n.kind = Node::Instruction; n.name = reader.processingInstructionTarget().toString(); n.value = reader.processingInstructionData().toString(); }
    return n;
}
bool element(QXmlStreamReader& reader, Node& n, int depth, quint32& count, const ConnectionXml::Canceled& canceled) {
    if (depth > 128 || ++count > 100000 || stopped(canceled)) return false;
    n.name = reader.qualifiedName().toString(); n.uri = reader.namespaceUri().toString();
    for (const auto& a : reader.attributes()) n.attributes.push_back({a.qualifiedName().toString(), a.namespaceUri().toString(), a.value().toString()});
    for (const auto& d : reader.namespaceDeclarations()) n.declarations.push_back({d.prefix().toString(), d.namespaceUri().toString()});
    while (!reader.atEnd()) {
        reader.readNext(); if (stopped(canceled) || reader.isDTD() || reader.isEntityReference()) return false;
        if (reader.isEndElement()) return true;
        if (reader.isStartElement()) {
            Node child; if (!element(reader, child, depth + 1, count, canceled)) return false;
            n.children.push_back(std::move(child));
        } else if (reader.isCharacters() || reader.isComment() || reader.isProcessingInstruction()) {
            if (++count > 100000) return false;
            n.children.push_back(passive(reader));
        }
    }
    return false;
}
std::optional<Tree> parse(const QByteArray& bytes, const ConnectionXml::Canceled& canceled) {
    if (bytes.isEmpty() || bytes.size() > ConnectionXml::MaxFile || stopped(canceled)) return std::nullopt;
    QXmlStreamReader reader(bytes); reader.setEntityExpansionLimit(1);
    Tree tree; bool seen = false; quint32 count = 0;
    while (!reader.atEnd()) {
        reader.readNext(); if (stopped(canceled) || reader.isDTD() || reader.isEntityReference()) return std::nullopt;
        if (reader.isStartElement()) {
            if (seen || reader.name() != "Connections" || (!reader.namespaceUri().isEmpty() && reader.namespaceUri() != Namespace) ||
                !element(reader, tree.root, 1, count, canceled)) return std::nullopt;
            seen = true;
        } else if (reader.isComment() || reader.isProcessingInstruction()) {
            if (++count > 100000) return std::nullopt;
            (seen ? tree.after : tree.before).push_back(passive(reader));
        }
    }
    if (!seen || reader.hasError() || stopped(canceled)) return std::nullopt;
    return tree;
}
bool bounded(const Node& node, int depth, quint32& count, const ConnectionXml::Canceled& canceled) {
    if (++count > 100000 || (node.kind == Node::Element && depth > 128) || stopped(canceled)) return false;
    for (const auto& child : node.children) if (!bounded(child, depth + 1, count, canceled)) return false;
    return true;
}
bool bounded(const Tree& tree, const ConnectionXml::Canceled& canceled) {
    quint32 count = 0;
    for (const auto& node : tree.before) if (!bounded(node, 1, count, canceled)) return false;
    if (!bounded(tree.root, 1, count, canceled)) return false;
    for (const auto& node : tree.after) if (!bounded(node, 1, count, canceled)) return false;
    return true;
}
void write(QXmlStreamWriter& writer, const Node& n) {
    switch (n.kind) {
    case Node::Text: writer.writeCharacters(n.value); return;
    case Node::Comment: writer.writeComment(n.value); return;
    case Node::Instruction: writer.writeProcessingInstruction(n.name, n.value); return;
    case Node::Element: break;
    }
    writer.writeStartElement(n.name);
    for (const auto& d : n.declarations) {
        if (d.prefix.isEmpty()) writer.writeDefaultNamespace(d.uri);
        else writer.writeNamespace(d.uri, d.prefix);
    }
    for (const auto& a : n.attributes) writer.writeAttribute(a.name, a.value);
    for (const auto& child : n.children) write(writer, child);
    writer.writeEndElement();
}
std::optional<QByteArray> encode(const Tree& tree) {
    QByteArray out; QXmlStreamWriter writer(&out); writer.setAutoFormatting(false); writer.writeStartDocument();
    for (const auto& n : tree.before) write(writer, n);
    write(writer, tree.root);
    for (const auto& n : tree.after) write(writer, n);
    writer.writeEndDocument();
    if (writer.hasError() || out.size() > ConnectionXml::MaxFile) return std::nullopt;
    return out;
}
QByteArray view(const Secret& secret) { return QByteArray::fromRawData(reinterpret_cast<const char*>(secret.data()), qsizetype(secret.size())); }
bool boolean(const QString& value, bool& result) {
    if (value.compare("true", Qt::CaseInsensitive) == 0) { result = true; return true; }
    if (value.compare("false", Qt::CaseInsensitive) == 0) { result = false; return true; }
    return false;
}
bool header(const Node& root, QString& version, quint32& iterations, bool& full) {
    version = root.attribute("ConfVersion"); if (version != "2.6" && version != "2.7" && version != "2.8") return false;
    const auto number = root.attribute("KdfIterations");
    if (number.isEmpty() || number.size() > 7) return false;
    for (auto c : number) if (c < QChar('0') || c > QChar('9')) return false;
    bool ok = false; iterations = number.toUInt(&ok);
    if (!ok || !ConnectionCrypto::validIterations(iterations) || root.attribute("EncryptionEngine") != "AES" || root.attribute("BlockCipherMode") != "GCM" ||
        !boolean(root.attribute("FullFileEncryption"), full) || root.attribute("Protected").isEmpty() ||
        !ConnectionCrypto::validEnvelope(root.attribute("Protected").toLatin1(), 4096)) return false;
    bool privateFormat = false;
    const QSet<QString> extensions{"RmFormatVersion", "RmFileId", "RmCredentialBindings"};
    for (const auto& attribute : root.attributes) {
        // Unknown private extensions may contain encrypted values. Refuse them
        // rather than save/rekey a file whose secrets we cannot authenticate.
        if (attribute.uri.isEmpty() && attribute.name.startsWith("Rm")) {
            if (!extensions.contains(attribute.name)) return false;
            privateFormat = true;
        }
    }
    if (!privateFormat) return true;
    const auto fileId = root.attribute("RmFileId"); const QUuid file(fileId);
    return (root.attribute("RmFormatVersion") == "1" || root.attribute("RmFormatVersion") == "2" || root.attribute("RmFormatVersion") == "3") && !file.isNull() && file.toString(QUuid::WithoutBraces) == fileId.toLower() &&
        !root.attribute("RmCredentialBindings").isEmpty() &&
        ConnectionCrypto::validEnvelope(root.attribute("RmCredentialBindings").toLatin1(), ConnectionMetadata::MaxPayload);
}
bool identities(Node& parent, QSet<QString>& ids, bool& missing, const ConnectionXml::Canceled& canceled) {
    for (auto& node : parent.children) {
        if (stopped(canceled)) return false;
        if (!node.profile()) continue;
        const auto stored = node.attribute("Id");
        if (stored.isEmpty()) { node.id = QUuid::createUuid().toString(QUuid::WithoutBraces); missing = true; }
        else {
            const QUuid id(stored);
            if (id.isNull() || id.toString(QUuid::WithoutBraces) != stored.toLower()) return false;
            node.id = stored;
        }
        const auto canonical = node.id.toLower(); if (ids.contains(canonical)) return false; ids.insert(canonical);
        if (node.attribute("Type") == "Connection") for (const auto& child : node.children) if (child.profile()) return false;
        if (!identities(node, ids, missing, canceled)) return false;
    }
    return true;
}
bool secrets(const Node& parent, quint32& count, const Secret* master, quint32 iterations, const ConnectionXml::Canceled& canceled) {
    for (const auto& node : parent.children) {
        if (stopped(canceled)) return false;
        if (!node.profile()) continue;
        for (const auto& name : SecretAttributes) {
            const auto value = node.attribute(name); if (value.isEmpty()) continue;
            if (++count > 1024 || !ConnectionCrypto::validEnvelope(value.toLatin1(), 4096)) return false;
            if (master) {
                if (stopped(canceled)) return false;
                auto plain = ConnectionCrypto::decrypt(*master, iterations, value.toLatin1(), 4096);
                if (!plain || !ConnectionCrypto::validUtf8(*plain)) return false;
            }
        }
        if (!secrets(node, count, master, iterations, canceled)) return false;
    }
    return true;
}
void collect(const Node& parent, QStringList& ids) {
    for (const auto& node : parent.children) if (node.profile()) { ids.push_back(node.id); collect(node, ids); }
}
void repair(Node& parent) {
    for (auto& node : parent.children) if (node.profile()) {
        if (node.attribute("Id").isEmpty()) node.setAttribute("Id", node.id);
        repair(node);
    }
}
Node* find(Node& parent, const QString& id) {
    for (auto& node : parent.children) if (node.profile()) {
        if (node.id.compare(id, Qt::CaseInsensitive) == 0) return &node;
        if (auto* result = find(node, id)) return result;
    }
    return nullptr;
}
bool metadata(const Node& root, const Secret& master, quint32 iterations, std::optional<ConnectionBindings>& bindings,
              const ConnectionXml::Canceled& canceled) {
    bindings.reset(); if (!root.hasAttribute("RmFormatVersion")) return !stopped(canceled);
    if (stopped(canceled)) return false;
    auto payload = ConnectionCrypto::decrypt(master, iterations, root.attribute("RmCredentialBindings").toLatin1(), ConnectionMetadata::MaxPayload);
    if (!payload) return false;
    bindings = ConnectionMetadata::decodeBindings(*payload, root.attribute("RmFileId"), canceled);
    if (!bindings || root.attribute("RmFormatVersion") != QString::number(bindings->version)) return false;
    return !stopped(canceled);
}
bool path(const Node& parent, const QString& id, std::vector<const Node*>& result) {
    for (const auto& node : parent.children) if (node.profile()) {
        result.push_back(&node);
        if (node.id.compare(id, Qt::CaseInsensitive) == 0 || path(node, id, result)) return true;
        result.pop_back();
    }
    return false;
}
const Node* passwordOwner(const std::vector<const Node*>& path) {
    for (size_t i = path.size(); i > 0; --i) {
        const auto& node = *path[i - 1]; bool inherited = false;
        if (i > 1 && node.hasAttribute("InheritPassword") && !boolean(node.attribute("InheritPassword").trimmed(), inherited)) return nullptr;
        if (!inherited || i == 1) return node.attribute("Password").isEmpty() ? nullptr : &node;
    }
    return nullptr;
}
std::optional<SessionTarget> account(const std::vector<const Node*>& path);
bool association(const Node& root, const ConnectionBindings& bindings, const QString& consumer, const SessionTarget& target) {
    std::vector<const Node*> nodes; if (!path(root, consumer, nodes)) return false;
    const auto committed = account(nodes); const auto* owner = passwordOwner(nodes);
    return committed && committed->sameAccount(target) && owner && ConnectionMetadata::matches(bindings, root.attribute("RmFileId"),
        consumer, owner->id, owner->attribute("Password").toLatin1(), target);
}
bool canary(const Node& root, const Secret& master, quint32 iterations, const ConnectionXml::Canceled& canceled) {
    if (stopped(canceled)) return false;
    auto plain = ConnectionCrypto::decrypt(master, iterations, root.attribute("Protected").toLatin1(), 4096); if (!plain) return false;
    return view(*plain) == "ThisIsNotProtected" || view(*plain) == "ThisIsProtected";
}
bool rekeyable(const Node& node, bool root, const ConnectionXml::Canceled& canceled) {
    if (stopped(canceled)) return false;
    if (node.kind == Node::Comment) return true;
    if (node.kind == Node::Text) return node.value.trimmed().isEmpty();
    if (node.kind != Node::Element || (!root && !node.profile())) return false;
    static const QSet<QString> rootNames{"Name", "Export", "ConfVersion", "EncryptionEngine", "BlockCipherMode", "KdfIterations",
        "FullFileEncryption", "Protected", "RmFormatVersion", "RmFileId", "RmCredentialBindings"};
    // Plain native fields from XmlConnectionNodeSerializer28.SetElementAttributes.
    // Unknown attributes/opaque content are conservatively refused on rekey;
    // ordinary edits still preserve them without interpreting their contents.
    static const QSet<QString> nodeNames = [] {
        const auto names = QString("Name VmId UseVmId UseEnhancedMode Type Expanded IsRootGroup Descr Icon Panel TabColor ConnectionFrameColor Id "
            "Username Domain Password Hostname Protocol RdpVersion SSHTunnelConnectionName OpeningCommand SSHOptions PuttySession Port ConnectToConsole "
            "UseCredSsp RenderingEngine RDPAuthenticationLevel RDPMinutesToIdleTimeout RDPAlertIdleTimeout LoadBalanceInfo Colors Resolution AutomaticResize "
            "DisplayWallpaper DisplayThemes EnableFontSmoothing EnableDesktopComposition DisableFullWindowDrag DisableMenuAnimations DisableCursorShadow "
            "DisableCursorBlinking CacheBitmaps RedirectDiskDrives RedirectDiskDrivesCustom RedirectPorts RedirectPrinters RedirectClipboard RedirectSmartCards "
            "RedirectSound SoundQuality RedirectAudioCapture RedirectKeys Connected PreExtApp PostExtApp MacAddress UserField EnvironmentTags Favorite ExtApp "
            "StartProgram StartProgramWorkDir VNCCompression VNCEncoding VNCAuthMode VNCProxyType VNCProxyIP VNCProxyPort VNCProxyUsername VNCProxyPassword "
            "VNCColors VNCSmartSizeMode VNCViewOnly RDGatewayUsageMethod RDGatewayHostname RDGatewayUseConnectionCredentials RDGatewayExternalCredentialProvider "
            "RDGatewayUserViaAPI RDGatewayUsername RDGatewayPassword RDGatewayAccessToken RDGatewayDomain UseRCG UseRestrictedAdmin UseRedirectionServerName "
            "UserViaAPI EC2InstanceId EC2Region ExternalCredentialProvider ExternalAddressProvider VaultOpenbaoMount VaultOpenbaoRole VaultOpenbaoSecretEngine").split(' ');
        QSet<QString> result(names.begin(), names.end());
        for (const auto& name : names) result.insert("Inherit" + name);
        result.insert("InheritDescription"); result.insert("InheritUseConsoleSession");
        for (const auto& name : {"RmHostInherited", "RmResolution", "RmResolutionInherited", "RmSshAuth", "RmSshKeyPath", "RmSshAuthInherited", "RmSshKeyPathInherited",
                                 "RmRdpSecurity", "RmRdpSecurityInherited", "RmGatewayPort", "RmGatewayPortInherited"}) result.insert(name);
        return result;
    }();
    for (const auto& a : node.attributes) {
        if (!a.uri.isEmpty() || !(root ? rootNames : nodeNames).contains(a.name)) return false;
        if (!root && (a.name.startsWith("Inherit") || (a.name.startsWith("Rm") && a.name.endsWith("Inherited")))) {
            bool value; if (!boolean(a.value.trimmed(), value)) return false;
        }
        if (!root && a.name == "RmResolution" && !QStringList{"fit", "800x600", "1024x768", "1280x720", "1920x1080"}.contains(a.value)) return false;
    }
    for (const auto& child : node.children) if (!rekeyable(child, false, canceled)) return false;
    return true;
}
struct Field {
    const char *key, *attribute, *inherit;
    enum Kind { String, Boolean, Protocol, Port, Colors, Resolution, SshAuth, KeyPath,
        SecurityProtocol, ServerAuth, Minutes, GatewayUsage, GatewayPort, GatewayCredentials, Sound } kind;
};
const Field Fields[] = {
    {"protocol", "Protocol", "InheritProtocol", Field::Protocol}, {"host", "Hostname", "RmHostInherited", Field::String},
    {"port", "Port", "InheritPort", Field::Port}, {"username", "Username", "InheritUsername", Field::String},
    {"description", "Descr", "InheritDescription", Field::String}, {"icon", "Icon", "InheritIcon", Field::String},
    {"domain", "Domain", "InheritDomain", Field::String},
    {"console_session", "ConnectToConsole", "InheritUseConsoleSession", Field::Boolean},
    {"resolution", "Resolution", "InheritResolution", Field::Resolution}, {"automatic_resize", "AutomaticResize", "InheritAutomaticResize", Field::Boolean},
    {"color_depth", "Colors", "InheritColors", Field::Colors}, {"cache_bitmaps", "CacheBitmaps", "InheritCacheBitmaps", Field::Boolean},
    {"wallpaper", "DisplayWallpaper", "InheritDisplayWallpaper", Field::Boolean}, {"themes", "DisplayThemes", "InheritDisplayThemes", Field::Boolean},
    {"font_smoothing", "EnableFontSmoothing", "InheritEnableFontSmoothing", Field::Boolean},
    {"ssh_auth", "RmSshAuth", "RmSshAuthInherited", Field::SshAuth}, {"ssh_key_path", "RmSshKeyPath", "RmSshKeyPathInherited", Field::KeyPath},
    {"view_only", "VNCViewOnly", "InheritVNCViewOnly", Field::Boolean},
    // RDP security/gateway (I.rdpsecurity). Native mRemoteNG attributes where they exist.
    {"security_protocol", "RmRdpSecurity", "RmRdpSecurityInherited", Field::SecurityProtocol},
    {"server_auth", "RDPAuthenticationLevel", "InheritRDPAuthenticationLevel", Field::ServerAuth},
    {"use_credssp", "UseCredSsp", "InheritUseCredSsp", Field::Boolean},
    {"idle_minutes", "RDPMinutesToIdleTimeout", "InheritRDPMinutesToIdleTimeout", Field::Minutes},
    {"load_balance_info", "LoadBalanceInfo", "InheritLoadBalanceInfo", Field::String},
    {"gateway_usage", "RDGatewayUsageMethod", "InheritRDGatewayUsageMethod", Field::GatewayUsage},
    {"gateway_host", "RDGatewayHostname", "InheritRDGatewayHostname", Field::String},
    {"gateway_port", "RmGatewayPort", "RmGatewayPortInherited", Field::GatewayPort},
    {"gateway_username", "RDGatewayUsername", "InheritRDGatewayUsername", Field::String},
    {"gateway_domain", "RDGatewayDomain", "InheritRDGatewayDomain", Field::String},
    {"gateway_credentials", "RDGatewayUseConnectionCredentials", "InheritRDGatewayUseConnectionCredentials", Field::GatewayCredentials},
    {"sound", "RedirectSound", "InheritRedirectSound", Field::Sound},
    {"clipboard", "RedirectClipboard", "InheritRedirectClipboard", Field::Boolean}
};
// Native enum names and numeric aliases → model names; empty = mRemoteNG default.
struct NativeChoice { const char* model; QStringList native; };
const std::vector<NativeChoice>& nativeChoices(Field::Kind kind) {
    static const std::vector<NativeChoice> security{{"auto", {"", "auto"}}, {"nla", {"nla"}}, {"ext", {"ext"}}, {"tls", {"tls"}}, {"rdp", {"rdp"}}};
    // Absent level is a secure migration to warn (V105), not mRemoteNG's NoAuth.
    static const std::vector<NativeChoice> serverAuth{{"warn", {"", "WarnOnFailedAuth", "2"}}, {"always", {"NoAuth", "0"}}, {"required", {"AuthRequired", "1"}}};
    static const std::vector<NativeChoice> usage{{"never", {"", "Never", "0"}}, {"always", {"Always", "1"}}, {"detect", {"Detect", "2"}}};
    static const std::vector<NativeChoice> credentials{{"same", {"", "Yes", "1"}}, {"separate", {"No", "0"}}};
    static const std::vector<NativeChoice> sound{{"local", {"", "BringToThisComputer", "0"}}, {"remote", {"LeaveAtRemoteComputer", "1"}}, {"none", {"DoNotPlay", "2"}}};
    static const std::vector<NativeChoice> none;
    switch (kind) {
    case Field::SecurityProtocol: return security;
    case Field::ServerAuth: return serverAuth;
    case Field::GatewayUsage: return usage;
    case Field::GatewayCredentials: return credentials;
    case Field::Sound: return sound;
    default: return none;
    }
}
const char* choiceReason(Field::Kind kind) {
    return kind == Field::ServerAuth ? "authentication" : kind == Field::SecurityProtocol ? "preferences" : kind == Field::Sound ? "redirection" : "gateway";
}
void reason(QJsonArray& reasons, const char* code) { const auto value = QString::fromLatin1(code); if (!reasons.contains(value)) reasons.push_back(value); }
bool nativeBoolean(const QString& value) { bool result = false; boolean(value.trimmed(), result); return result; }
QString inheritanceFlag(const Node& node, const Field& field) {
    if (field.kind == Field::Resolution && node.hasAttribute("RmResolutionInherited")) return "RmResolutionInherited";
    if (QString::fromLatin1(field.attribute) == "Descr" && !node.hasAttribute(field.inherit) && node.hasAttribute("InheritDescr")) return "InheritDescr";
    if (QString::fromLatin1(field.attribute) == "ConnectToConsole" && !node.hasAttribute(field.inherit) && node.hasAttribute("InheritConnectToConsole")) return "InheritConnectToConsole";
    return QString::fromLatin1(field.inherit);
}
const Node& source(const std::vector<const Node*>& nodes, const Field& field, QJsonArray& reasons) {
    for (size_t i = nodes.size(); i > 0; --i) {
        const auto& node = *nodes[i - 1]; const auto flag = inheritanceFlag(node, field); bool inherited = false;
        if (i > 1 && node.hasAttribute(flag) && !boolean(node.attribute(flag).trimmed(), inherited)) reason(reasons, "inheritance");
        if (!inherited || i == 1) return node;
    }
    return *nodes.front();
}
QJsonValue fieldValue(const Node& node, const Field& field, QJsonArray& reasons) {
    const auto value = node.attribute(field.attribute), trimmed = value.trimmed();
    switch (field.kind) {
    case Field::String: return value;
    case Field::SshAuth:
        if (value.isEmpty() || value == "password") return "password";
        if (value == "private_key") return value;
        reason(reasons, "preferences"); return "password";
    case Field::KeyPath:
        if (value.toUtf8().size() > 4096) { reason(reasons, "preferences"); return ""; }
        for (auto c : value) if (c.category() == QChar::Other_Control) { reason(reasons, "preferences"); return ""; }
        return value;
    case Field::Boolean:
        if (QString::fromLatin1(field.attribute) == "VNCViewOnly" && node.hasAttribute(field.attribute)) {
            bool viewOnly = false;
            if (!boolean(trimmed, viewOnly)) reason(reasons, "preferences"); // never silently enable input for a malformed view-only policy
            return viewOnly;
        }
        // mRemoteNG defaults UseCredSsp to true; other absent/invalid RDP bools are false.
        if (QString::fromLatin1(field.attribute) == "UseCredSsp" && trimmed.isEmpty()) return true;
        return nativeBoolean(value); // original absent/invalid RDP bool default = false
    case Field::Protocol:
        if (trimmed.isEmpty() || trimmed == "RDP" || trimmed == "0") return "rdp";
        if (trimmed == "SSH2" || trimmed == "3") return "ssh";
        if (trimmed == "VNC" || trimmed == "1") return "vnc";
        reason(reasons, "protocol"); return "unsupported";
    case Field::Port: {
        bool ok = false; const auto port = trimmed.toInt(&ok);
        if (ok && port > 0 && port <= 65535) return port;
        // A zero/invalid native port is not a validated endpoint. Preserve it in
        // XML and disable launch rather than guessing an enabled configuration.
        reason(reasons, "port"); return 3389;
    }
    case Field::Colors:
        if (trimmed == "Colors16Bit" || trimmed == "16") return 16;
        if (trimmed == "Colors24Bit" || trimmed == "24") return 24;
        if (trimmed == "Colors32Bit" || trimmed == "32") return 32;
        reason(reasons, "colors"); return 32;
    case Field::SecurityProtocol: case Field::ServerAuth: case Field::GatewayUsage: case Field::GatewayCredentials: case Field::Sound: {
        const auto& choices = nativeChoices(field.kind);
        for (const auto& option : choices) if (option.native.contains(trimmed)) return QString::fromLatin1(option.model);
        // SmartCard/ExternalCredentialProvider/AccessToken gateway logons and
        // unknown values stay launch-disabled rather than becoming a weaker policy.
        reason(reasons, choiceReason(field.kind)); return QString::fromLatin1(choices.front().model);
    }
    case Field::Minutes: {
        if (trimmed.isEmpty()) return 0;
        bool ok = false; const auto minutes = trimmed.toInt(&ok);
        if (ok && minutes >= 0 && minutes <= 240) return minutes;
        reason(reasons, "idle_timeout"); return 0;
    }
    case Field::GatewayPort: {
        if (trimmed.isEmpty()) return 443;
        bool ok = false; const auto port = trimmed.toInt(&ok);
        if (ok && port > 0 && port <= 65535) return port;
        reason(reasons, "gateway"); return 443;
    }
    case Field::Resolution:
        if (node.hasAttribute("RmResolution")) {
            const auto preferred = node.attribute("RmResolution");
            if ((trimmed == "FitToWindow" || trimmed == "1") && QStringList{"fit", "800x600", "1024x768", "1280x720", "1920x1080"}.contains(preferred)) return preferred;
            reason(reasons, "preferences");
        }
        if (trimmed == "FitToWindow" || trimmed == "1") return "fit";
        reason(reasons, "resolution"); return "fit";
    }
    return {};
}
std::optional<SessionTarget> account(const std::vector<const Node*>& path) {
    if (path.empty() || path.back()->attribute("Type") != "Connection") return std::nullopt;
    QJsonArray reasons;
    const auto value = [&](const char* key) {
        for (const auto& field : Fields) if (QString::fromLatin1(field.key) == key) return fieldValue(source(path, field, reasons), field, reasons);
        return QJsonValue();
    };
    SessionTarget target; target.protocol = value("protocol").toString(); target.host = value("host").toString();
    target.port = quint16(value("port").toInt()); target.username = value("username").toString();
    if (target.protocol == "ssh") {
        target.sshAuth = value("ssh_auth").toString(); target.sshKeyPath = value("ssh_key_path").toString();
        if (!reasons.isEmpty() || target.sshAuth != "password") return std::nullopt;
        return normalizeSshTarget(target);
    }
    // A VNC account is host and port only; the password is the VNC password.
    if (target.protocol == "vnc") {
        if (!reasons.isEmpty()) return std::nullopt;
        target.username.clear(); return normalizeVncTarget(target);
    }
    if (target.protocol != "rdp") return std::nullopt;
    target.domain = value("domain").toString();
    if (!reasons.isEmpty()) return std::nullopt;
    return normalizeRdpTarget(target);
}
QString criticalValue(const std::vector<const Node*>& nodes, const char* attribute, QJsonArray& reasons) {
    const Field field{"", attribute, "", Field::String};
    auto inherited = field; const auto flag = "Inherit" + QString::fromLatin1(attribute); const auto utf8 = flag.toUtf8(); inherited.inherit = utf8.constData();
    return source(nodes, inherited, reasons).attribute(attribute).trimmed();
}
bool inactiveSshAttribute(QString name) {
    if (name.startsWith("Inherit")) name.remove(0, 7);
    static const QSet<QString> names{
        "Domain", "ConnectToConsole", "UseConsoleSession", "Resolution", "RmResolution", "RmResolutionInherited", "Colors",
        "AutomaticResize", "CacheBitmaps", "DisplayWallpaper", "DisplayThemes", "EnableFontSmoothing", "RenderingEngine",
        "RDGatewayUsageMethod", "RDGatewayHostname", "RDGatewayUseConnectionCredentials", "RDGatewayExternalCredentialProvider",
        "RDGatewayUserViaAPI", "RDGatewayUsername", "RDGatewayPassword", "RDGatewayAccessToken", "RDGatewayDomain",
        "UseCredSsp", "RDPAuthenticationLevel", "UseRestrictedAdmin", "UseRCG", "UseRedirectionServerName", "UseVmId", "UseEnhancedMode",
        "LoadBalanceInfo", "RdpVersion", "RedirectPrinters", "RedirectPorts", "RedirectSmartCards", "RedirectAudioCapture", "RedirectKeys",
        "RedirectDiskDrives", "RedirectDiskDrivesCustom", "RedirectSound", "SoundQuality", "RedirectClipboard", "EnableDesktopComposition",
        "DisableFullWindowDrag", "DisableMenuAnimations", "DisableCursorShadow", "DisableCursorBlinking", "RDPMinutesToIdleTimeout", "RDPAlertIdleTimeout",
        "RmRdpSecurity", "RmRdpSecurityInherited", "RmGatewayPort", "RmGatewayPortInherited"
    };
    return names.contains(name);
}
bool inactiveAttribute(const QString& protocol, QString name) {
    if ((protocol == "ssh" || protocol == "vnc") && inactiveSshAttribute(name)) return true;
    if (name.startsWith("Inherit")) name.remove(0, 7);
    if (protocol != "vnc") return name == "VNCViewOnly";
    static const QSet<QString> names{"Username", "PuttySession", "RmSshAuth", "RmSshKeyPath", "RmSshAuthInherited", "RmSshKeyPathInherited"};
    return names.contains(name);
}
void criticalOptions(const std::vector<const Node*>& nodes, const QString& protocol, QJsonArray& reasons) {
    const auto value = [&](const char* name) { return criticalValue(nodes, name, reasons); };
    const auto empty = [&](const char* name, const char* code) { if (!value(name).isEmpty()) reason(reasons, code); };
    const auto off = [&](const char* name, const char* code) {
        const auto text = value(name); if (!text.isEmpty() && text.compare("false", Qt::CaseInsensitive) != 0) reason(reasons, code);
    };
    const auto choice = [&](const char* name, const QStringList& safe, const char* code) { if (!safe.contains(value(name))) reason(reasons, code); };
    for (const auto& name : {"ExternalCredentialProvider", "ExternalAddressProvider"}) choice(name, {"", "None", "0"}, "provider");
    for (const auto& name : {"UserViaAPI", "EC2InstanceId", "EC2Region", "VaultOpenbaoMount", "VaultOpenbaoRole"}) empty(name, "provider");
    if (protocol == "rdp") {
        choice("RDGatewayExternalCredentialProvider", {"", "None", "0"}, "provider");
        empty("RDGatewayUserViaAPI", "provider");
    }
    for (const auto& name : {"PreExtApp", "PostExtApp", "ExtApp", "StartProgram", "StartProgramWorkDir", "OpeningCommand", "SSHOptions", "SSHTunnelConnectionName"}) empty(name, "command");
    if (protocol == "rdp") {
        // Security protocol, server authentication, CredSSP, load balancing and
        // gateway are mapped fields (I.rdpsecurity). Other alternate logon modes
        // cannot silently become a supported policy.
        for (const auto& name : {"UseRestrictedAdmin", "UseRCG", "UseRedirectionServerName", "UseVmId", "UseEnhancedMode"}) off(name, "authentication");
        choice("RdpVersion", {"", "Highest", "1000"}, "authentication");
        for (const auto& name : {"RedirectPrinters", "RedirectPorts", "RedirectSmartCards", "RedirectAudioCapture", "RedirectKeys"}) off(name, "redirection");
        choice("RedirectDiskDrives", {"", "None", "0", "false", "False"}, "redirection");
        for (const auto& name : {"EnableDesktopComposition", "DisableFullWindowDrag", "DisableMenuAnimations", "DisableCursorShadow", "DisableCursorBlinking"}) off(name, "display_options");
        off("RDPAlertIdleTimeout", "idle_timeout");
    }
    if (protocol == "vnc") {
        // Native enum names and numeric aliases, with absence using the stock
        // defaults. Alternate authentication/proxy policies are never downgraded
        // to classic VNC over a direct, unencrypted TCP connection.
        choice("VNCAuthMode", {"", "AuthVNC", "0"}, "authentication");
        choice("VNCProxyType", {"", "ProxyNone", "0"}, "gateway");
        for (const auto& name : {"VNCProxyIP", "VNCProxyUsername", "VNCProxyPassword"}) empty(name, "gateway");
        choice("VNCProxyPort", {"", "0"}, "gateway");
        choice("VNCCompression", {"", "CompNone", "99"}, "preferences");
        // EncHextile is the legacy stock preference, not a requirement to
        // advertise Hextile. Keep it passive while negotiating Raw/ZRLE/CopyRect.
        choice("VNCEncoding", {"", "EncHextile", "3", "EncRaw", "0", "EncZRLE", "7"}, "preferences");
        choice("VNCColors", {"", "ColNormal", "0"}, "preferences");
        choice("VNCSmartSizeMode", {"", "SmartSAspect", "2"}, "preferences"); // fit with aspect ratio only
    }
    for (const auto* node : nodes) {
        Node shape; shape.name = node->name; shape.uri = node->uri;
        for (const auto& attribute : node->attributes) {
            if (!attribute.uri.isEmpty() || !inactiveAttribute(protocol, attribute.name)) shape.attributes.push_back(attribute);
        }
        if (!rekeyable(shape, false, {})) reason(reasons, "opaque");
        for (const auto& child : node->children) {
            if ((child.kind == Node::Element && !child.profile()) || child.kind == Node::Instruction ||
                (child.kind == Node::Text && !child.value.trimmed().isEmpty())) reason(reasons, "opaque");
        }
    }
}
// Mirrors model::validate_rdp_security_overrides for local values. Native
// values the model cannot hold (gateway "host:port", qualified gateway user
// plus domain, control characters) are preserved in XML, projected as the
// empty default and launch-disabled instead of failing the whole file.
void sanitizeRdpSecurity(QJsonObject& overrides, QJsonArray& reasons, bool active) {
    const auto text = [&](const char* key) { return overrides.value(key).toString(); };
    const auto controls = [](const QString& value) { for (auto c : value) if (c.category() == QChar::Other_Control) return true; return false; };
    const auto reset = [&](const char* key, const char* code) {
        if (overrides.value(key).isString() && !text(key).isEmpty()) { overrides.insert(key, ""); if (active) reason(reasons, code); }
    };
    const auto lb = text("load_balance_info");
    if (lb.toUtf8().size() > 4096 || controls(lb)) reset("load_balance_info", "authentication");
    const auto host = text("gateway_host");
    bool badHost = host.toUtf8().size() > 4096 || controls(host);
    for (auto c : host) badHost = badHost || c.isSpace() || QString("/\\@#?[]").contains(c);
    if (host.contains(':') && QHostAddress(host).protocol() != QAbstractSocket::IPv6Protocol) badHost = true;
    if (badHost) reset("gateway_host", "gateway");
    const auto user = text("gateway_username"), domain = text("gateway_domain");
    bool badDomain = domain.toUtf8().size() > 256;
    for (auto c : domain) badDomain = badDomain || c.isSpace() || c.category() == QChar::Other_Control || QString("\\/@:").contains(c);
    if (user.toUtf8().size() > 4096 || controls(user)) reset("gateway_username", "gateway");
    if (badDomain || (!domain.isEmpty() && (user.contains('\\') || user.contains('@')))) reset("gateway_domain", "gateway");
}
bool project(const Node& parent, std::vector<const Node*>& path, QJsonArray& profiles, qsizetype& size, const ConnectionXml::Canceled& canceled) {
    for (const auto& node : parent.children) if (node.profile()) {
        if (stopped(canceled)) return false;
        path.push_back(&node); QJsonObject overrides; QJsonArray unsupported;
        if (node.attribute("Type") != "Connection" && node.attribute("Type") != "Container") reason(unsupported, "node_type");
        const auto protocol = fieldValue(source(path, Fields[0], unsupported), Fields[0], unsupported).toString();
        for (const auto& field : Fields) {
            QJsonArray ignored;
            auto& reasons = inactiveAttribute(protocol, field.attribute) ? ignored : unsupported;
            const auto& owner = source(path, field, reasons);
            auto localReasons = QJsonArray(); const auto local = fieldValue(node, field, localReasons);
            fieldValue(owner, field, reasons);
            overrides.insert(field.key, &owner == &node ? local : QJsonValue(QJsonValue::Null));
        }
        sanitizeRdpSecurity(overrides, unsupported, protocol == "rdp");
        criticalOptions(path, protocol, unsupported);
        const auto parentId = path.size() > 1 ? QJsonValue(path[path.size() - 2]->id.toLower()) : QJsonValue(QJsonValue::Null);
        const QJsonObject profile{{"id", node.id.toLower()}, {"name", node.attribute("Name")}, {"kind", node.attribute("Type") == "Container" ? "folder" : "connection"},
            {"parent", parentId}, {"overrides", overrides}, {"native", QJsonObject{{"unsupported", unsupported}}}};
        size += QJsonDocument(profile).toJson(QJsonDocument::Compact).size() + 1;
        if (size > ConnectionXml::MaxInner) return false;
        profiles.push_back(profile);
        if (!project(node, path, profiles, size, canceled)) return false;
        path.pop_back();
    }
    return !stopped(canceled);
}
using Profiles = QMap<QString, QJsonObject>;
using Namespaces = QMap<QString, QString>;
struct DetachedNode { Node node; QString parent; Namespaces namespaces; };
using DetachedNodes = std::map<QString, DetachedNode>;
Namespaces namespaceContext(const Node& node, Namespaces inherited) {
    for (const auto& declaration : node.declarations) inherited.insert(declaration.prefix, declaration.uri);
    return inherited;
}
bool detach(Node& parent, const QString& parentId, const Namespaces& namespaces, DetachedNodes& nodes, const ConnectionXml::Canceled& canceled) {
    for (auto& child : parent.children) if (child.profile()) {
        if (stopped(canceled)) return false;
        auto node = std::move(child); const auto id = node.id.toLower(); const auto context = namespaceContext(node, namespaces);
        if (!detach(node, id, context, nodes, canceled)) return false;
        child = Node(); child.name = "Node"; child.id = id;
        nodes.emplace(id, DetachedNode{std::move(node), parentId, context});
    }
    return !stopped(canceled);
}
QJsonValue defaultValue(const Field& field, const QJsonObject& overrides) {
    switch (field.kind) {
    case Field::String: case Field::KeyPath: return "";
    case Field::SshAuth: return "password";
    case Field::Boolean: return QStringList{"automatic_resize", "wallpaper", "themes", "font_smoothing", "use_credssp"}.contains(field.key);
    case Field::Protocol: return "rdp";
    case Field::SecurityProtocol: case Field::ServerAuth: case Field::GatewayUsage: case Field::GatewayCredentials: case Field::Sound:
        return QString::fromLatin1(nativeChoices(field.kind).front().model);
    case Field::Minutes: return 0;
    case Field::GatewayPort: return 443;
    case Field::Port: {
        const auto protocol = overrides.value("protocol").toString("rdp"); return protocol == "ssh" ? 22 : protocol == "vnc" ? 5900 : 3389;
    }
    case Field::Colors: return 32;
    case Field::Resolution: return overrides.value("protocol") == "vnc" ? "fit" : "1024x768";
    }
    return {};
}
void patchField(Node& node, const Field& field, const QJsonValue& requested, bool root, const QJsonObject& overrides) {
    const bool inherited = requested.isNull() || requested.isUndefined();
    const auto flag = inheritanceFlag(node, field); node.setAttribute(flag, inherited ? "true" : "false");
    if (field.kind == Field::Resolution) {
        node.setAttribute("InheritResolution", inherited ? "true" : "false");
        node.setAttribute("RmResolutionInherited", inherited ? "true" : "false");
    }
    if (inherited && !root) return;
    const auto value = inherited ? defaultValue(field, overrides) : requested;
    QString native;
    switch (field.kind) {
    case Field::String: case Field::SshAuth: case Field::KeyPath: native = value.toString(); break;
    case Field::Boolean: native = value.toBool() ? "true" : "false"; break;
    case Field::Protocol: {
        const auto protocol = value.toString(); native = protocol == "ssh" ? "SSH2" : protocol == "vnc" ? "VNC" : "RDP"; break;
    }
    case Field::Port: native = QString::number(value.toInt()); break;
    case Field::Colors: native = "Colors" + QString::number(value.toInt()) + "Bit"; break;
    case Field::Resolution: native = "FitToWindow"; node.setAttribute("RmResolution", value.toString()); break;
    case Field::SecurityProtocol: native = value.toString(); break;
    case Field::ServerAuth: case Field::GatewayUsage: case Field::GatewayCredentials: case Field::Sound:
        // Write the mRemoteNG enum name (second native alias after the empty default).
        for (const auto& option : nativeChoices(field.kind)) if (option.model == value.toString()) {
            for (const auto& name : option.native) if (!name.isEmpty() && !name.front().isDigit()) { native = name; break; }
        }
        break;
    case Field::Minutes: case Field::GatewayPort: native = QString::number(value.toInt()); break;
    }
    node.setAttribute(field.attribute, native);
}
bool patchNodes(DetachedNodes& nodes, const Profiles& before, const Profiles& requested, const ConnectionXml::Canceled& canceled) {
    for (auto i = requested.begin(); i != requested.end(); ++i) {
        if (stopped(canceled)) return false;
        const auto profile = i.value(); const auto old = before.value(i.key()); const bool fresh = old.isEmpty();
        if ((!fresh && (old.value("kind") != profile.value("kind") || old.value("native") != profile.value("native"))) ||
            (fresh && !profile.value("native").isNull() && !profile.value("native").isUndefined())) return false;
        if (fresh) {
            Node node; node.name = "Node"; node.id = i.key(); node.declarations.push_back({{}, {}});
            node.setAttribute("Id", node.id); node.setAttribute("Type", profile.value("kind") == "folder" ? "Container" : "Connection");
            node.setAttribute("Password", ""); node.setAttribute("InheritPassword", "false");
            node.setAttribute("UseCredSsp", "true"); node.setAttribute("RDPAuthenticationLevel", "WarnOnFailedAuth");
            nodes.emplace(i.key(), DetachedNode{std::move(node), profile.value("parent").toString(), {{"", ""}}});
        }
        auto& node = nodes.at(i.key()).node;
        if (fresh || old.value("name") != profile.value("name")) {
            const auto name = profile.value("name").toString(); for (auto c : name) if (c.category() == QChar::Other_Control) return false;
            node.setAttribute("Name", name);
        }
        const auto overrides = profile.value("overrides").toObject(), previous = old.value("overrides").toObject();
        for (const auto& field : Fields) {
            const auto value = overrides.value(field.key), prior = previous.value(field.key);
            const bool bothInherited = (value.isNull() || value.isUndefined()) && (prior.isNull() || prior.isUndefined());
            if (fresh || (!bothInherited && value != prior)) {
                // Unsupported is a preservation sentinel, never a native RDP write.
                if (field.kind == Field::Protocol && value == "unsupported") return false;
                patchField(node, field, value, profile.value("parent").toString().isEmpty(), overrides);
            }
        }
    }
    return !stopped(canceled);
}
bool compose(Node& parent, const QString& id, const Profiles& requested, const QMap<QString, QStringList>& children,
             DetachedNodes& nodes, QSet<QString>& placed, const ConnectionXml::Canceled& canceled) {
    const auto attach = [&](const QString& childId, Node& child) {
        if (stopped(canceled) || placed.contains(childId)) return false;
        auto& entry = nodes.at(childId); child = std::move(entry.node); placed.insert(childId);
        if (entry.parent != id) {
            // Keep original in-scope prefixes when crossing a namespace boundary.
            for (auto i = entry.namespaces.begin(); i != entry.namespaces.end(); ++i) {
                bool present = false; for (const auto& d : child.declarations) if (d.prefix == i.key()) { present = true; break; }
                if (!present) child.declarations.push_back({i.key(), i.value()});
            }
        }
        return compose(child, childId, requested, children, nodes, placed, canceled);
    };
    auto i = parent.children.begin();
    while (i != parent.children.end()) {
        if (!i->profile()) { ++i; continue; }
        const auto childId = i->id.toLower();
        if (!requested.contains(childId) || requested.value(childId).value("parent").toString() != id) { i = parent.children.erase(i); continue; }
        ++i;
    }
    // Remaining profile slots take the requested sibling order; non-profile
    // children keep their positions and extra profiles are appended.
    const auto order = children.value(id); qsizetype next = 0;
    for (auto& child : parent.children) if (child.profile()) {
        if (next >= order.size()) return false;
        if (!attach(order.at(next++), child)) return false;
    }
    for (; next < order.size(); ++next) {
        Node child; if (!attach(order.at(next), child)) return false; parent.children.push_back(std::move(child));
    }
    return !stopped(canceled);
}
bool materializeHosts(Node& parent, std::vector<const Node*>& path, const ConnectionXml::Canceled& canceled) {
    for (auto& node : parent.children) if (node.profile()) {
        if (stopped(canceled)) return false;
        path.push_back(&node); QJsonArray ignored; const auto& owner = source(path, Fields[1], ignored);
        if (&owner != &node) node.setAttribute("Hostname", owner.attribute("Hostname"));
        if (!materializeHosts(node, path, canceled)) return false;
        path.pop_back();
    }
    return !stopped(canceled);
}
bool sameAssociation(const Node& before, const Node& after, const QString& consumer) {
    std::vector<const Node*> oldPath, newPath;
    if (!path(before, consumer, oldPath) || !path(after, consumer, newPath)) return false;
    const auto oldAccount = account(oldPath), newAccount = account(newPath);
    const auto *oldOwner = passwordOwner(oldPath), *newOwner = passwordOwner(newPath);
    return oldAccount && newAccount && oldAccount->sameAccount(*newAccount) && oldOwner && newOwner &&
        oldOwner->id.compare(newOwner->id, Qt::CaseInsensitive) == 0 && oldOwner->attribute("Password") == newOwner->attribute("Password");
}
bool invalidateBindings(const Node& before, Node& after, const Secret& master, quint32 iterations,
                        std::optional<ConnectionBindings>& bindings, const ConnectionXml::Canceled& canceled) {
    if (!metadata(before, master, iterations, bindings, canceled)) return false;
    if (!bindings) return !stopped(canceled);
    const auto priorCount = bindings->records.size();
    auto i = bindings->records.begin();
    while (i != bindings->records.end()) {
        if (stopped(canceled)) return false;
        if (!sameAssociation(before, after, i->consumer)) i = bindings->records.erase(i);
        else ++i;
    }
    if (bindings->records.size() != priorCount) {
        const auto payload = ConnectionMetadata::encodeBindings(*bindings, canceled); if (!payload) return false;
        const auto cipher = ConnectionCrypto::encrypt(master, iterations, *payload, ConnectionMetadata::MaxPayload);
        if (!cipher || stopped(canceled)) return false;
        after.setAttribute("RmCredentialBindings", QString::fromLatin1(*cipher));
    }
    return !stopped(canceled);
}
bool reencrypt(Node& parent, const Secret& previous, const Secret& replacement, quint32 iterations, const ConnectionXml::Canceled& canceled) {
    for (auto& node : parent.children) {
        if (stopped(canceled)) return false;
        if (!node.profile()) continue;
        for (const auto& name : SecretAttributes) {
            const auto value = node.attribute(name); if (value.isEmpty()) continue;
            if (stopped(canceled)) return false;
            auto plain = ConnectionCrypto::decrypt(previous, iterations, value.toLatin1(), 4096);
            if (!plain || !ConnectionCrypto::validUtf8(*plain)) return false;
            const auto cipher = ConnectionCrypto::encrypt(replacement, iterations, *plain, 4096); if (!cipher || stopped(canceled)) return false;
            node.setAttribute(name, QString::fromLatin1(*cipher));
        }
        if (!reencrypt(node, previous, replacement, iterations, canceled)) return false;
    }
    return !stopped(canceled);
}
QString encryptedText(const Node& root, bool& ok) {
    QString text;
    for (const auto& child : root.children) {
        if (child.kind == Node::Element) { ok = false; return {}; }
        if (child.kind == Node::Text) text += child.value;
    }
    ok = !text.isEmpty() && ConnectionCrypto::validEnvelope(text.toLatin1(), ConnectionXml::MaxInner);
    return text;
}
}

struct ConnectionXml::Impl {
    Tree tree;
    QString version, encrypted;
    quint32 iterations = 0;
    bool full = false, ready = false, authenticated = false, missing = false;
    std::optional<ConnectionBindings> bindings;
    std::optional<bool> passwordProtected;
};
ConnectionXml::ConnectionXml(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ConnectionXml::ConnectionXml(ConnectionXml&&) noexcept = default;
ConnectionXml& ConnectionXml::operator=(ConnectionXml&&) noexcept = default;
ConnectionXml::~ConnectionXml() = default;
std::optional<ConnectionXml> ConnectionXml::read(const QByteArray& bytes, const Secret* master, const Canceled& canceled) {
    auto tree = parse(bytes, canceled); if (!tree) return std::nullopt;
    auto impl = std::make_unique<Impl>(); impl->tree = std::move(*tree);
    if (!header(impl->tree.root, impl->version, impl->iterations, impl->full)) return std::nullopt;
    if (impl->full) {
        bool ok = false; impl->encrypted = encryptedText(impl->tree.root, ok); if (!ok) return std::nullopt;
    } else {
        QSet<QString> ids; quint32 count = 0;
        if (!identities(impl->tree.root, ids, impl->missing, canceled) || !secrets(impl->tree.root, count, nullptr, impl->iterations, canceled)) return std::nullopt;
        impl->ready = true;
    }
    ConnectionXml result(std::move(impl)); if (master && !result.unlock(*master, canceled)) return std::nullopt;
    return result;
}
std::optional<ConnectionXml> ConnectionXml::create(const Canceled& canceled) {
    if (stopped(canceled)) return std::nullopt;
    const Secret master(QByteArray("mR3m"));
    const auto protectedValue = ConnectionCrypto::encrypt(master, ConnectionCrypto::NewIterations, Secret(QByteArray("ThisIsNotProtected")), 4096);
    if (!protectedValue || stopped(canceled)) return std::nullopt;
    const auto bytes = QByteArray("<Connections xmlns=\"http://mremoteng.org\" Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"") +
        QByteArray::number(ConnectionCrypto::NewIterations) + "\" FullFileEncryption=\"false\" Protected=\"" + *protectedValue + "\"/>";
    return read(bytes, &master, canceled);
}
ConnectionXml ConnectionXml::clone() const { return ConnectionXml(std::make_unique<Impl>(*impl_)); }
QString ConnectionXml::version() const { return impl_->version; }
quint32 ConnectionXml::iterations() const { return impl_->iterations; }
bool ConnectionXml::fullEncryption() const { return impl_->full; }
std::optional<bool> ConnectionXml::passwordProtected() const { return impl_->passwordProtected; }
bool ConnectionXml::needsUnlock() const { return !impl_->ready; }
QStringList ConnectionXml::profileIds() const { QStringList ids; if (impl_->ready) collect(impl_->tree.root, ids); return ids; }
bool ConnectionXml::requiresIdRepair() const { return impl_->missing; }
std::optional<QByteArray> ConnectionXml::projection(const Canceled& canceled) const {
    if (!impl_->ready || stopped(canceled)) return std::nullopt;
    QJsonArray profiles; std::vector<const Node*> path; qsizetype size = 40;
    if (!project(impl_->tree.root, path, profiles, size, canceled)) return std::nullopt;
    const auto json = QJsonDocument(QJsonObject{{"schema_version", 2}, {"profiles", profiles}}).toJson(QJsonDocument::Compact);
    if (json.size() > MaxInner || stopped(canceled)) return std::nullopt;
    try { rm::validate_document(rust::Str(json.constData(), size_t(json.size()))); }
    catch (const std::exception&) { return std::nullopt; }
    return stopped(canceled) ? std::nullopt : std::optional<QByteArray>(json);
}
std::optional<QByteArray> ConnectionXml::patch(const Secret& master, const QByteArray& json, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || json.isEmpty() || json.size() > MaxInner || stopped(canceled) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    try { rm::validate_document(rust::Str(json.constData(), size_t(json.size()))); }
    catch (const std::exception&) { return std::nullopt; }
    const auto previous = projection(canceled); if (!previous) return std::nullopt;
    Profiles before, requested; QMap<QString, QStringList> children;
    for (const auto& value : QJsonDocument::fromJson(*previous).object().value("profiles").toArray()) {
        const auto profile = value.toObject(); before.insert(profile.value("id").toString(), profile);
    }
    for (const auto& value : QJsonDocument::fromJson(json).object().value("profiles").toArray()) {
        if (stopped(canceled)) return std::nullopt;
        const auto profile = value.toObject(); const auto id = profile.value("id").toString();
        if (QUuid(id).toString(QUuid::WithoutBraces) != id) return std::nullopt;
        requested.insert(id, profile); children[profile.value("parent").toString()].push_back(id);
    }
    auto candidate = std::make_unique<Impl>(*impl_); DetachedNodes nodes;
    const auto namespaces = namespaceContext(candidate->tree.root, {{"", ""}});
    if (!detach(candidate->tree.root, {}, namespaces, nodes, canceled) || !patchNodes(nodes, before, requested, canceled)) return std::nullopt;
    QSet<QString> placed;
    if (!compose(candidate->tree.root, {}, requested, children, nodes, placed, canceled) || placed.size() != requested.size()) return std::nullopt;
    std::vector<const Node*> path;
    if (!materializeHosts(candidate->tree.root, path, canceled)) return std::nullopt;
    if (!invalidateBindings(impl_->tree.root, candidate->tree.root, master, impl_->iterations, candidate->bindings, canceled)) return std::nullopt;
    candidate->encrypted.clear();
    ConnectionXml prepared(std::move(candidate)); const auto normalized = prepared.projection(canceled);
    if (!normalized || !prepared.serialize(master, canceled) || stopped(canceled)) return std::nullopt;
    impl_ = std::move(prepared.impl_); return normalized;
}
bool ConnectionXml::unlock(const Secret& master, const Canceled& canceled) {
    if (!canary(impl_->tree.root, master, impl_->iterations, canceled)) return false;
    std::optional<ConnectionBindings> bindings;
    if (!metadata(impl_->tree.root, master, impl_->iterations, bindings, canceled)) return false;
    if (impl_->full && !impl_->ready) {
        auto inner = ConnectionCrypto::decrypt(master, impl_->iterations, impl_->encrypted.toLatin1(), MaxInner);
        if (!inner || !ConnectionCrypto::validUtf8(*inner) || stopped(canceled)) return false;
        // Recreate the root namespace context, not a second document schema.
        auto shell = impl_->tree; shell.before.clear(); shell.after.clear(); shell.root.children.clear();
        auto encoded = encode(shell); if (!encoded) return false;
        const auto end = encoded->lastIndexOf("/>"); if (end < 0) return false;
        auto wrapped = encoded->left(end) + '>' + view(*inner) + "</" + shell.root.name.toUtf8() + '>';
        auto candidate = parse(wrapped, canceled); OPENSSL_cleanse(wrapped.data(), size_t(wrapped.size()));
        if (!candidate) return false;
        QSet<QString> ids; bool missing = false; quint32 count = 0;
        // Validate every envelope/count before beginning per-value KDF work.
        if (!identities(candidate->root, ids, missing, canceled) || !secrets(candidate->root, count, nullptr, impl_->iterations, canceled)) return false;
        candidate->before = impl_->tree.before; candidate->after = impl_->tree.after;
        if (!bounded(*candidate, canceled)) return false;
        count = 0; if (!secrets(candidate->root, count, &master, impl_->iterations, canceled) || stopped(canceled)) return false;
        impl_->tree.root.children = std::move(candidate->root.children); impl_->missing = missing; impl_->ready = true;
    } else {
        quint32 count = 0; if (!secrets(impl_->tree.root, count, &master, impl_->iterations, canceled) || stopped(canceled)) return false;
    }
    impl_->bindings = std::move(bindings); impl_->authenticated = true;
    impl_->passwordProtected = view(master) != "mR3m"; return true;
}
bool ConnectionXml::authorizeIdRepair() {
    if (!impl_->ready || !impl_->authenticated) return false;
    repair(impl_->tree.root); impl_->missing = false; return true;
}
bool ConnectionXml::rename(const QString& id, const QString& name) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || name.isEmpty() || name.toUtf8().size() > 256) return false;
    for (auto ch : name) if (ch.category() == QChar::Other_Control) return false;
    auto* node = find(impl_->tree.root, id); if (!node) return false;
    node->setAttribute("Name", name); return true;
}
bool ConnectionXml::setPassword(const Secret& master, const QString& owner, const Secret& value, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || value.size() > 4096 || !ConnectionCrypto::validUtf8(value) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return false;
    auto* node = find(impl_->tree.root, owner);
    if (!node || (node->attribute("Type") != "Connection" && node->attribute("Type") != "Container")) return false;
    quint32 count = 0;
    if (!secrets(impl_->tree.root, count, nullptr, impl_->iterations, canceled) ||
        (count == 1024 && node->attribute("Password").isEmpty() && value.size() != 0)) return false;
    const auto cipher = ConnectionCrypto::encrypt(master, impl_->iterations, value, 4096); if (!cipher || stopped(canceled)) return false;
    node->setAttribute("Password", QString::fromLatin1(*cipher)); node->setAttribute("InheritPassword", "false");
    // Existing bindings cannot authorize the changed ciphertext/source. The
    // owning operation must explicitly review its committed consumers to rebind.
    return true;
}
bool ConnectionXml::setPasswordInheritance(const Secret& master, const QString& owner, bool inherited, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || stopped(canceled) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return false;
    auto candidate = std::make_unique<Impl>(*impl_); auto* node = find(candidate->tree.root, owner);
    if (!node || (node->attribute("Type") != "Connection" && node->attribute("Type") != "Container")) return false;
    node->setAttribute("InheritPassword", inherited ? "true" : "false");
    if (!invalidateBindings(impl_->tree.root, candidate->tree.root, master, impl_->iterations, candidate->bindings, canceled)) return false;
    candidate->encrypted.clear();
    ConnectionXml prepared(std::move(candidate));
    if (!prepared.serialize(master, canceled) || stopped(canceled)) return false;
    impl_ = std::move(prepared.impl_); return true;
}
bool ConnectionXml::setFullEncryption(const Secret& master, bool enabled, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || stopped(canceled) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return false;
    auto candidate = std::make_unique<Impl>(*impl_);
    candidate->full = enabled; candidate->tree.root.setAttribute("FullFileEncryption", enabled ? "true" : "false"); candidate->encrypted.clear();
    ConnectionXml prepared(std::move(candidate));
    if (!prepared.serialize(master, canceled) || stopped(canceled)) return false;
    impl_ = std::move(prepared.impl_); return true;
}
bool ConnectionXml::changeMaster(const Secret& previous, const Secret& replacement, const Canceled& canceled) {
    return ConnectionCrypto::validNewMaster(replacement) && rekeyMaster(previous, replacement, canceled);
}
bool ConnectionXml::resetMaster(const Secret& previous, const Canceled& canceled) {
    return rekeyMaster(previous, Secret(QByteArray("mR3m")), canceled);
}
bool ConnectionXml::rekeyMaster(const Secret& previous, const Secret& replacement, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing ||
        !rekeyable(impl_->tree.root, true, canceled)) return false;
    for (const auto& node : impl_->tree.before) if (node.kind != Node::Comment) return false;
    for (const auto& node : impl_->tree.after) if (node.kind != Node::Comment) return false;
    if (!canary(impl_->tree.root, previous, impl_->iterations, canceled)) return false;
    std::optional<ConnectionBindings> bindings;
    if (!metadata(impl_->tree.root, previous, impl_->iterations, bindings, canceled)) return false;
    quint32 count = 0; if (!secrets(impl_->tree.root, count, &previous, impl_->iterations, canceled)) return false;
    std::vector<bool> active;
    if (bindings) for (const auto& record : bindings->records) {
        if (stopped(canceled)) return false;
        active.push_back(association(impl_->tree.root, *bindings, record.consumer, record.target));
    }
    auto candidate = std::make_unique<Impl>(*impl_);
    if (!reencrypt(candidate->tree.root, previous, replacement, impl_->iterations, canceled)) return false;
    if (bindings) {
        for (size_t i = 0; i < bindings->records.size(); ++i) {
            if (stopped(canceled)) return false;
            if (!active[i]) continue;
            auto* owner = find(candidate->tree.root, bindings->records[i].owner); if (!owner) return false;
            bindings->records[i].passwordHash = QCryptographicHash::hash(QByteArray::fromBase64(owner->attribute("Password").toLatin1()), QCryptographicHash::Sha256);
        }
        auto payload = ConnectionMetadata::encodeBindings(*bindings, canceled); if (!payload) return false;
        const auto cipher = ConnectionCrypto::encrypt(replacement, impl_->iterations, *payload, ConnectionMetadata::MaxPayload);
        if (!cipher || stopped(canceled)) return false;
        candidate->tree.root.setAttribute("RmCredentialBindings", QString::fromLatin1(*cipher));
    }
    candidate->passwordProtected = view(replacement) != "mR3m";
    const auto protectedValue = ConnectionCrypto::encrypt(replacement, impl_->iterations,
        Secret(QByteArray(*candidate->passwordProtected ? "ThisIsProtected" : "ThisIsNotProtected")), 4096);
    if (!protectedValue || stopped(canceled)) return false;
    candidate->tree.root.setAttribute("Protected", QString::fromLatin1(*protectedValue));
    candidate->bindings = std::move(bindings); candidate->encrypted.clear();
    // Validate the complete serializable candidate, including full-inner bounds,
    // before changing even in-memory ownership. The store still owns disk commit.
    ConnectionXml prepared(std::move(candidate));
    if (!prepared.serialize(replacement, canceled) || stopped(canceled)) return false;
    impl_ = std::move(prepared.impl_); return true;
}
QString ConnectionXml::fileId() const { return impl_->tree.root.attribute("RmFileId").toLower(); }
bool ConnectionXml::credentialReviewRequired() const { return impl_->ready && !impl_->tree.root.hasAttribute("RmFormatVersion"); }
bool ConnectionXml::hasCredentials() const {
    if (!impl_->ready) return false;
    quint32 count = 0;
    return !secrets(impl_->tree.root, count, nullptr, impl_->iterations, {}) || count != 0 || impl_->tree.root.hasAttribute("RmFormatVersion");
}
QHash<QString, PasswordState> ConnectionXml::passwordStates(const Canceled& canceled) const {
    QHash<QString, PasswordState> result;
    if (!impl_->ready) return result;
    std::vector<const Node*> nodes;
    std::function<bool(const Node&)> visit = [&](const Node& parent) {
        for (const auto& node : parent.children) if (node.profile()) {
            if (stopped(canceled)) return false;
            nodes.push_back(&node); PasswordState state;
            state.local = !node.attribute("Password").isEmpty();
            bool inherited = false;
            state.inherited = nodes.size() > 1 && boolean(node.attribute("InheritPassword").trimmed(), inherited) && inherited;
            // Keep the prospective source even when it has no password. No
            // password text or length is included in the presentation state.
            for (size_t i = nodes.size(); i > 0; --i) {
                bool inherit = false;
                if (i == 1 || !boolean(nodes[i - 1]->attribute("InheritPassword").trimmed(), inherit) || !inherit) {
                    state.source = nodes[i - 1]->id; state.present = !nodes[i - 1]->attribute("Password").isEmpty(); break;
                }
            }
            if (state.present && impl_->authenticated && impl_->bindings) {
                const auto target = account(nodes); const auto* owner = passwordOwner(nodes);
                state.authorized = target && owner && ConnectionMetadata::matches(*impl_->bindings, fileId(), node.id,
                    owner->id, owner->attribute("Password").toLatin1(), *target);
            }
            result.insert(node.id, state);
            if (!visit(node)) return false;
            nodes.pop_back();
        }
        return true;
    };
    if (!visit(impl_->tree.root)) result.clear();
    return result;
}
bool ConnectionXml::authorizePasswords(const Secret& master, const QString& editedOwner, const std::vector<PasswordAuthorization>& reviewed, const Canceled& canceled) {
    if (reviewed.size() > 1024 || stopped(canceled)) return false;
    for (const auto& consent : reviewed) {
        if (stopped(canceled)) return false;
        std::vector<const Node*> nodes; if (!path(impl_->tree.root, consent.consumer, nodes)) return false;
        bool affected = false;
        for (size_t i = nodes.size(); i > 0; --i) {
            if (nodes[i - 1]->id == editedOwner) { affected = true; break; }
            bool inherited = false;
            if (i == 1 || !boolean(nodes[i - 1]->attribute("InheritPassword").trimmed(), inherited) || !inherited) break;
        }
        if (!affected) return false;
    }
    return reviewPasswords(master, reviewed, canceled);
}
bool ConnectionXml::reviewPasswords(const Secret& master, const std::vector<PasswordAuthorization>& reviewed, const Canceled& canceled) {
    if (reviewed.size() > 1024 || stopped(canceled)) return false;
    ConnectionBindings bindings; bindings.fileId = fileId().isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : fileId();
    if (impl_->bindings) bindings.version = impl_->bindings->version;
    if (impl_->bindings) for (const auto& record : impl_->bindings->records) {
        if (stopped(canceled)) return false;
        if (association(impl_->tree.root, *impl_->bindings, record.consumer, record.target)) bindings.records.push_back(record);
    }
    QSet<QString> consumers;
    for (const auto& consent : reviewed) {
        if (stopped(canceled) || consumers.contains(consent.consumer)) return false;
        consumers.insert(consent.consumer);
        std::vector<const Node*> nodes; if (!path(impl_->tree.root, consent.consumer, nodes) || nodes.back()->attribute("Type") != "Connection") return false;
        const auto target = account(nodes); const auto* owner = passwordOwner(nodes);
        if (!target || !owner || !target->sameAccount(consent.target)) return false;
        auto& records = bindings.records;
        records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& record) { return record.consumer == consent.consumer; }), records.end());
        records.push_back({consent.consumer, owner->id,
            QCryptographicHash::hash(QByteArray::fromBase64(owner->attribute("Password").toLatin1()), QCryptographicHash::Sha256), *target});
    }
    return reviewCredentials(master, bindings, canceled);
}
bool ConnectionXml::reviewCredentials(const Secret& master, const ConnectionBindings& reviewed, const Canceled& canceled) {
    if (!impl_->ready || !impl_->authenticated || impl_->missing ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return false;
    if (!fileId().isEmpty() && QUuid(fileId()) != QUuid(reviewed.fileId)) return false;
    auto preserved = reviewed;
    if (impl_->bindings) preserved.version = std::max(preserved.version, impl_->bindings->version);
    auto payload = ConnectionMetadata::encodeBindings(preserved, canceled); if (!payload) return false;
    auto bindings = ConnectionMetadata::decodeBindings(*payload, reviewed.fileId, canceled); if (!bindings) return false;
    auto candidate = impl_->tree;
    candidate.root.setAttribute("RmFileId", bindings->fileId);
    for (const auto& record : bindings->records)
        if (stopped(canceled) || !association(candidate.root, *bindings, record.consumer, record.target)) return false;
    quint32 count = 0; if (!secrets(candidate.root, count, &master, impl_->iterations, canceled)) return false;
    const auto cipher = ConnectionCrypto::encrypt(master, impl_->iterations, *payload, ConnectionMetadata::MaxPayload);
    if (!cipher || stopped(canceled)) return false;
    candidate.root.setAttribute("RmFormatVersion", QString::number(bindings->version)); candidate.root.setAttribute("RmCredentialBindings", QString::fromLatin1(*cipher));
    // In-memory reviewed candidate only. Disk changes belong to the next explicit save.
    impl_->tree = std::move(candidate); impl_->bindings = std::move(bindings); return true;
}
std::optional<SessionTarget> ConnectionXml::committedAccount(const QString& consumer) const {
    if (!impl_->ready || impl_->missing) return std::nullopt;
    std::vector<const Node*> nodes;
    return path(impl_->tree.root, consumer, nodes) ? account(nodes) : std::nullopt;
}
std::optional<Secret> ConnectionXml::password(const Secret& master, const QString& consumer, const SessionTarget& committedTarget, const Canceled& canceled) const {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || !impl_->bindings ||
        !association(impl_->tree.root, *impl_->bindings, consumer, committedTarget) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    std::vector<const Node*> nodes; if (!path(impl_->tree.root, consumer, nodes)) return std::nullopt;
    const auto* owner = passwordOwner(nodes); if (!owner || stopped(canceled)) return std::nullopt;
    auto plain = ConnectionCrypto::decrypt(master, impl_->iterations, owner->attribute("Password").toLatin1(), 4096);
    if (!plain || !ConnectionCrypto::validUtf8(*plain) || stopped(canceled)) return std::nullopt;
    return plain;
}
std::optional<Secret> ConnectionXml::folderPassword(const Secret& master, const QString& folder, const Canceled& canceled) const {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    std::vector<const Node*> nodes; if (!path(impl_->tree.root, folder, nodes) || nodes.back()->attribute("Type") != "Container") return std::nullopt;
    const auto* owner = passwordOwner(nodes); if (!owner || stopped(canceled)) return std::nullopt;
    auto plain = ConnectionCrypto::decrypt(master, impl_->iterations, owner->attribute("Password").toLatin1(), 4096);
    if (!plain || plain->size() == 0 || !ConnectionCrypto::validUtf8(*plain) || stopped(canceled)) return std::nullopt;
    return plain;
}
std::optional<QByteArray> ConnectionXml::serialize(const Secret& master, const Canceled& canceled) const {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || !bounded(impl_->tree, canceled) ||
        !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    std::optional<ConnectionBindings> bindings;
    if (!metadata(impl_->tree.root, master, impl_->iterations, bindings, canceled)) return std::nullopt;
    quint32 count = 0;
    if (!secrets(impl_->tree.root, count, &master, impl_->iterations, canceled)) return std::nullopt;
    auto candidate = impl_->tree;
    if (impl_->full) {
        QByteArray body; QXmlStreamWriter writer(&body);
        for (const auto& child : candidate.root.children) write(writer, child);
        if (writer.hasError() || body.size() > MaxInner || stopped(canceled)) return std::nullopt;
        // Native encryption maps empty plaintext to an empty value. Whitespace
        // keeps an empty file's inner XML authenticated and readable on reopen.
        if (body.isEmpty()) body.append(' ');
        Secret plain(body); OPENSSL_cleanse(body.data(), size_t(body.size()));
        const auto encrypted = ConnectionCrypto::encrypt(master, impl_->iterations, plain, MaxInner); if (!encrypted || stopped(canceled)) return std::nullopt;
        Node text; text.kind = Node::Text; text.value = QString::fromLatin1(*encrypted);
        candidate.root.children.clear(); candidate.root.children.push_back(std::move(text));
    }
    if (stopped(canceled)) return std::nullopt;
    return encode(candidate);
}
std::optional<ImportedPasswords> ConnectionXml::importPasswords(const Secret& master, const Canceled& canceled) const {
    if (!impl_->ready || !impl_->authenticated || !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    ImportedPasswords result; bool ok = true;
    std::function<void(const Node&, bool)> visit = [&](const Node& parent, bool nested) {
        for (const auto& node : parent.children) if (ok && node.profile()) {
            if (stopped(canceled)) { ok = false; return; }
            const auto id = node.id.toLower(); bool inherited = false;
            if (nested && boolean(node.attribute("InheritPassword").trimmed(), inherited) && inherited) result.inherited.insert(id);
            else if (const auto value = node.attribute("Password"); !value.isEmpty()) {
                auto plain = ConnectionCrypto::decrypt(master, impl_->iterations, value.toLatin1(), 4096);
                if (!plain || !ConnectionCrypto::validUtf8(*plain)) { ok = false; return; }
                if (plain->size()) result.local.emplace(id, std::move(*plain));
            }
            visit(node, true);
        }
    };
    visit(impl_->tree.root, false);
    return ok ? std::optional<ImportedPasswords>(std::move(result)) : std::nullopt;
}
bool ConnectionXml::authorizeConsumers(const Secret& master, const QStringList& consumers, const Canceled& canceled) {
    std::vector<PasswordAuthorization> reviewed;
    for (const auto& consumer : consumers) {
        if (stopped(canceled)) return false;
        std::vector<const Node*> nodes;
        if (!path(impl_->tree.root, consumer, nodes) || nodes.back()->attribute("Type") != "Connection" || !passwordOwner(nodes)) continue;
        const auto target = account(nodes); if (!target) continue;
        if (reviewed.size() == 1024) return false;
        reviewed.push_back({consumer, *target});
    }
    return reviewed.empty() || reviewPasswords(master, reviewed, canceled);
}
std::vector<ConnectionBinding> ConnectionXml::boundRecords(const Canceled& canceled) const {
    std::vector<ConnectionBinding> result;
    if (!impl_->ready || !impl_->authenticated || !impl_->bindings) return result;
    for (const auto& record : impl_->bindings->records) {
        if (stopped(canceled)) return {};
        if (association(impl_->tree.root, *impl_->bindings, record.consumer, record.target)) result.push_back(record);
    }
    return result;
}
bool ConnectionXml::followAccountEdits(const Secret& master, const std::vector<ConnectionBinding>& previouslyBound, const Canceled& canceled) {
    QStringList followed;
    for (const auto& record : previouslyBound) {
        if (stopped(canceled)) return false;
        std::vector<const Node*> nodes; if (!path(impl_->tree.root, record.consumer, nodes)) continue;
        const auto target = account(nodes); const auto* owner = passwordOwner(nodes);
        if (!target || !owner || target->sameAccount(record.target) || owner->id != record.owner) continue;
        // Only the endpoint moved: a different user, domain, protocol or SSH method is a different account.
        auto moved = record.target; moved.host = target->host; moved.port = target->port;
        if (!target->sameAccount(moved)) continue;
        const auto hash = QCryptographicHash::hash(QByteArray::fromBase64(owner->attribute("Password").toLatin1()), QCryptographicHash::Sha256);
        if (hash != record.passwordHash) continue;
        followed.append(record.consumer);
    }
    return followed.isEmpty() || authorizeConsumers(master, followed, canceled);
}
namespace {
QString csvField(QString value) {
    if (!value.contains(';') && !value.contains('"') && !value.contains('\n') && !value.contains('\r')) return value;
    return '"' + value.replace('"', "\"\"") + '"';
}
}
std::optional<QByteArray> ConnectionXml::exportFile(const Secret& master, const ExportOptions& options, const Canceled& canceled) const {
    if (!impl_->ready || !impl_->authenticated || impl_->missing || !canary(impl_->tree.root, master, impl_->iterations, canceled)) return std::nullopt;
    Node root = impl_->tree.root;
    if (!options.scope.isEmpty()) {
        std::vector<const Node*> nodes; if (!path(root, options.scope, nodes)) return std::nullopt;
        Node selected = *nodes.back();
        root.children.erase(std::remove_if(root.children.begin(), root.children.end(), [](const Node& n) { return n.profile(); }), root.children.end());
        root.children.push_back(std::move(selected));
    }
    const bool csv = options.format == ExportOptions::Format::Csv;
    const Secret defaultKey(QByteArray("mR3m"));
    // XML re-encrypts secrets with the public default key; CSV, like mRemoteNG, writes them as plain text.
    std::map<const Node*, std::map<QString, QString>> plainSecrets;
    std::function<bool(Node&)> clean = [&](Node& parent) {
        for (auto& node : parent.children) if (node.profile()) {
            if (stopped(canceled)) return false;
            for (const auto& name : SecretAttributes) {
                const auto value = node.attribute(name); if (value.isEmpty()) continue;
                if (name == "Password" && !options.password) { node.setAttribute(name, {}); continue; }
                auto plain = ConnectionCrypto::decrypt(master, impl_->iterations, value.toLatin1(), 4096);
                if (!plain || !ConnectionCrypto::validUtf8(*plain)) return false;
                if (csv) { plainSecrets[&node][name] = QString::fromUtf8(QByteArray(reinterpret_cast<const char*>(plain->data()), qsizetype(plain->size()))); continue; }
                const auto cipher = ConnectionCrypto::encrypt(defaultKey, impl_->iterations, *plain, 4096); if (!cipher) return false;
                node.setAttribute(name, QString::fromLatin1(*cipher));
            }
            if (!options.username) node.setAttribute("Username", {});
            if (!options.domain) node.setAttribute("Domain", {});
            if (!options.inheritance) for (auto& attribute : node.attributes)
                if (attribute.uri.isEmpty() && (attribute.name.startsWith("Inherit") || (attribute.name.startsWith("Rm") && attribute.name.endsWith("Inherited")))) attribute.value = "false";
            if (!clean(node)) return false;
        }
        return true;
    };
    if (!clean(root)) return std::nullopt;
    if (csv) {
        // mRemoteNG CSV: one row per node, structure in Id/Parent/NodeType, other columns are node attributes.
        QStringList columns{"Name", "Id", "Parent", "NodeType", "Description", "Icon", "Panel", "Username", "Password", "Domain", "Hostname", "Port", "Protocol"};
        const QSet<QString> fixed{"Name", "Id", "Type", "Descr", "Icon", "Panel", "Username", "Password", "Domain", "Hostname", "Port", "Protocol"};
        QStringList extra;
        std::function<void(const Node&)> gather = [&](const Node& parent) {
            for (const auto& node : parent.children) if (node.profile()) {
                for (const auto& attribute : node.attributes)
                    if (attribute.uri.isEmpty() && !fixed.contains(attribute.name) && !extra.contains(attribute.name)) extra << attribute.name;
                gather(node);
            }
        };
        gather(root); columns += extra;
        QStringList lines{columns.join(';')};
        std::function<void(const Node&, const QString&)> rows = [&](const Node& parent, const QString& parentId) {
            for (const auto& node : parent.children) if (node.profile()) {
                QStringList row;
                for (const auto& column : columns) {
                    QString value;
                    if (column == "Id") value = node.id;
                    else if (column == "Parent") value = parentId;
                    else if (column == "NodeType") value = node.attribute("Type");
                    else if (column == "Description") value = node.attribute("Descr");
                    else if (plainSecrets[&node].count(column)) value = plainSecrets[&node][column];
                    else if (!SecretAttributes.contains(column)) value = node.attribute(column);
                    row << csvField(value);
                }
                lines << row.join(';');
                rows(node, node.id);
            }
        };
        rows(root, {});
        return stopped(canceled) ? std::nullopt : std::optional<QByteArray>((lines.join("\r\n") + "\r\n").toUtf8());
    }
    root.attributes.erase(std::remove_if(root.attributes.begin(), root.attributes.end(), [](const Attribute& a) {
        return a.uri.isEmpty() && (a.name == "RmFormatVersion" || a.name == "RmFileId" || a.name == "RmCredentialBindings");
    }), root.attributes.end());
    const auto protectedValue = ConnectionCrypto::encrypt(defaultKey, impl_->iterations, Secret(QByteArray("ThisIsNotProtected")), 4096);
    if (!protectedValue) return std::nullopt;
    root.setAttribute("Protected", QString::fromLatin1(*protectedValue)); root.setAttribute("FullFileEncryption", "false");
    Tree tree{std::move(root), impl_->tree.before, impl_->tree.after};
    if (!bounded(tree, canceled) || stopped(canceled)) return std::nullopt;
    return encode(tree);
}
