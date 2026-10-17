#include "ConnectionImport.h"
#include "ConnectionCrypto.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStringDecoder>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <functional>
#include <map>

namespace {
constexpr int MaxNodes = 10000;

struct Synthetic {
    QString name;
    bool folder = false;
    std::vector<std::pair<QString, QString>> attributes;
    QString password;
    std::vector<Synthetic> children;
    void set(const QString& name, const QString& value) {
        if (value.isEmpty()) return;
        for (auto& attribute : attributes) if (attribute.first == name) { attribute.second = value; return; }
        attributes.emplace_back(name, value);
    }
};

int count(const std::vector<Synthetic>& nodes) {
    int total = 0; for (const auto& node : nodes) total += 1 + count(node.children); return total;
}

std::optional<QByteArray> encrypted(const QString& text) {
    const Secret key(QByteArray("mR3m")); const Secret plain(text.toUtf8());
    return ConnectionCrypto::encrypt(key, ConnectionCrypto::NewIterations, plain, 4096);
}

// An unprotected mRemoteNG 2.8 file, the same shape the native reader accepts.
std::optional<QByteArray> synthesize(const std::vector<Synthetic>& nodes, QString* error) {
    if (nodes.empty()) { if (error) *error = "The file contains no connections."; return std::nullopt; }
    if (count(nodes) > MaxNodes) { if (error) *error = "The file contains more than 10000 connections."; return std::nullopt; }
    const auto protectedValue = encrypted("ThisIsNotProtected"); if (!protectedValue) return std::nullopt;
    QByteArray bytes; QXmlStreamWriter writer(&bytes);
    writer.writeStartDocument();
    writer.writeNamespace("http://mremoteng.org", "mrng");
    writer.writeStartElement("http://mremoteng.org", "Connections");
    for (const auto& [name, value] : std::vector<std::pair<QString, QString>>{
             {"Name", "Connections"}, {"Export", "false"}, {"EncryptionEngine", "AES"}, {"BlockCipherMode", "GCM"},
             {"KdfIterations", QString::number(ConnectionCrypto::NewIterations)}, {"FullFileEncryption", "false"},
             {"Protected", QString::fromLatin1(*protectedValue)}, {"ConfVersion", "2.8"}}) writer.writeAttribute(name, value);
    bool ok = true;
    std::function<void(const Synthetic&)> write = [&](const Synthetic& node) {
        writer.writeStartElement("Node");
        writer.writeAttribute("Name", node.name.isEmpty() ? QStringLiteral("Imported") : node.name);
        writer.writeAttribute("Type", node.folder ? "Container" : "Connection");
        writer.writeAttribute("Id", QUuid::createUuid().toString(QUuid::WithoutBraces));
        for (const auto& [name, value] : node.attributes) writer.writeAttribute(name, value);
        if (!node.password.isEmpty()) {
            const auto cipher = encrypted(node.password);
            if (!cipher) ok = false; else writer.writeAttribute("Password", QString::fromLatin1(*cipher));
        }
        for (const auto& child : node.children) write(child);
        writer.writeEndElement();
    };
    for (const auto& node : nodes) write(node);
    writer.writeEndElement(); writer.writeEndDocument();
    if (!ok || writer.hasError()) { if (error) *error = "Cannot convert the file."; return std::nullopt; }
    return bytes;
}

// Splits host[:port], including [IPv6]:port.
void splitEndpoint(Synthetic& node, QString endpoint) {
    endpoint = endpoint.trimmed();
    if (endpoint.startsWith('[')) {
        const int close = endpoint.indexOf(']');
        if (close > 0) {
            node.set("Hostname", endpoint.mid(1, close - 1));
            if (endpoint.mid(close + 1).startsWith(':')) node.set("Port", endpoint.mid(close + 2));
            return;
        }
    }
    if (endpoint.count(':') == 1) { node.set("Hostname", endpoint.section(':', 0, 0)); node.set("Port", endpoint.section(':', 1)); return; }
    node.set("Hostname", endpoint);
}

void splitAccount(Synthetic& node, const QString& account) {
    if (account.contains('\\')) { node.set("Domain", account.section('\\', 0, 0)); node.set("Username", account.section('\\', 1)); }
    else node.set("Username", account);
}

QString text(const QByteArray& bytes) {
    // .rdp files written by mstsc are UTF-16LE with a BOM.
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        auto decoder = QStringDecoder(bytes.startsWith("\xFF\xFE") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        return QString(decoder(bytes.mid(2)));
    }
    return QString::fromUtf8(bytes.startsWith("\xEF\xBB\xBF") ? bytes.mid(3) : bytes);
}

std::optional<QByteArray> fromRdp(const QString& path, const QByteArray& bytes, QString* error) {
    Synthetic node; node.name = QFileInfo(path).completeBaseName(); node.set("Protocol", "RDP");
    QString account, domain;
    for (const auto& line : text(bytes).split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
        const auto key = line.section(':', 0, 0).trimmed().toLower(); const auto value = line.section(':', 2).trimmed();
        if (key == "full address") splitEndpoint(node, value);
        else if (key == "server port") node.set("Port", value);
        else if (key == "username") account = value;
        else if (key == "domain") domain = value;
        else if (key == "administrative session" || key == "connect to console") node.set("ConnectToConsole", value == "1" ? "True" : "False");
        else if (key == "gatewayhostname") node.set("RDGatewayHostname", value);
        else if (key == "gatewayusagemethod") node.set("RDGatewayUsageMethod", value == "1" ? "Always" : value == "2" ? "Detect" : "Never");
        else if (key == "loadbalanceinfo") node.set("LoadBalanceInfo", value);
    }
    splitAccount(node, account); if (!domain.isEmpty()) node.set("Domain", domain);
    if (!node.attributes.empty() && std::none_of(node.attributes.begin(), node.attributes.end(), [](const auto& a) { return a.first == "Hostname"; })) {
        if (error) *error = "The RDP file has no \"full address\"."; return std::nullopt;
    }
    return synthesize({node}, error);
}

// Splits one CSV record; quoted fields may contain the delimiter, quotes and newlines.
std::vector<QStringList> csvRecords(const QString& input, QChar delimiter) {
    std::vector<QStringList> records; QStringList record; QString field; bool quoted = false;
    for (int i = 0; i < input.size(); ++i) {
        const QChar c = input.at(i);
        if (quoted) {
            if (c == '"') { if (i + 1 < input.size() && input.at(i + 1) == '"') { field += '"'; ++i; } else quoted = false; }
            else field += c;
        } else if (c == '"') quoted = true;
        else if (c == delimiter) { record << field; field.clear(); }
        else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < input.size() && input.at(i + 1) == '\n') ++i;
            record << field; field.clear();
            if (!(record.size() == 1 && record.first().isEmpty())) records.push_back(record);
            record.clear();
        } else field += c;
    }
    if (!field.isEmpty() || !record.isEmpty()) { record << field; records.push_back(record); }
    return records;
}

std::optional<QByteArray> fromCsv(const QByteArray& bytes, QString* error) {
    const auto input = text(bytes);
    const auto header = input.section('\n', 0, 0);
    const QChar delimiter = header.count(';') >= header.count(',') ? ';' : ',';
    const auto records = csvRecords(input, delimiter);
    if (records.size() < 2) { if (error) *error = "The CSV file has no connections."; return std::nullopt; }
    const auto columns = records.front();
    const auto column = [&](const char* name) { return columns.indexOf(QString::fromLatin1(name)); };
    const int nameColumn = column("Name");
    if (nameColumn < 0) { if (error) *error = "The CSV file has no Name column."; return std::nullopt; }
    // mRemoteNG CSV columns are node attribute names, except these.
    const std::map<QString, QString> renamed{{"Description", "Descr"}, {"UseConsoleSession", "ConnectToConsole"}};
    const QSet<QString> structural{"Name", "Id", "Parent", "NodeType", "Password", "Type"};
    const QRegularExpression attributeName("^[A-Za-z_][A-Za-z0-9_.-]{0,63}$");
    struct Row { Synthetic node; QString id, parent; };
    std::vector<Row> rows;
    for (size_t r = 1; r < records.size(); ++r) {
        const auto& values = records[r];
        const auto value = [&](int index) { return index >= 0 && index < values.size() ? values.at(index) : QString(); };
        Row row; row.node.name = value(nameColumn); row.id = value(column("Id")); row.parent = value(column("Parent"));
        row.node.folder = value(column("NodeType")).compare("Container", Qt::CaseInsensitive) == 0;
        row.node.password = value(column("Password"));
        for (int c = 0; c < columns.size(); ++c) {
            const auto& name = columns.at(c); if (structural.contains(name) || !attributeName.match(name).hasMatch()) continue;
            const auto found = renamed.find(name);
            row.node.set(found == renamed.end() ? name : found->second, value(c));
        }
        rows.push_back(std::move(row));
    }
    // Rebuild the tree from Id/Parent; unknown parents become top-level.
    std::map<QString, size_t> index; for (size_t i = 0; i < rows.size(); ++i) if (!rows[i].id.isEmpty()) index.emplace(rows[i].id, i);
    std::function<Synthetic(size_t, int)> build = [&](size_t i, int depth) {
        Synthetic node = rows[i].node;
        if (depth < 64) for (size_t j = 0; j < rows.size(); ++j)
            if (j != i && !rows[i].id.isEmpty() && rows[j].parent == rows[i].id && rows[i].node.folder) node.children.push_back(build(j, depth + 1));
        return node;
    };
    std::vector<Synthetic> top;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto parent = index.find(rows[i].parent);
        if (rows[i].parent.isEmpty() || parent == index.end() || !rows[parent->second].node.folder) top.push_back(build(i, 0));
    }
    return synthesize(top, error);
}

// A minimal element tree for the XML formats below.
struct Element { QString name; QXmlStreamAttributes attributes; QString text; std::vector<Element> children;
    const Element* child(const QString& n) const { for (const auto& c : children) if (c.name == n) return &c; return nullptr; }
    QString value(const QString& path) const {
        const Element* e = this; for (const auto& part : path.split('/')) { e = e->child(part); if (!e) return {}; }
        return e->text.trimmed();
    }
};
std::optional<Element> parseXml(const QByteArray& bytes) {
    QXmlStreamReader reader(bytes); std::vector<Element> stack; std::optional<Element> root; int elements = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (++elements > 200000 || stack.size() > 64) return std::nullopt;
            stack.push_back({reader.name().toString(), reader.attributes(), {}, {}});
        } else if (reader.isCharacters() && !stack.empty()) stack.back().text += reader.text();
        else if (reader.isEndElement() && !stack.empty()) {
            auto done = std::move(stack.back()); stack.pop_back();
            if (stack.empty()) root = std::move(done); else stack.back().children.push_back(std::move(done));
        }
    }
    if (reader.hasError() || !root) return std::nullopt;
    return root;
}

std::optional<QByteArray> fromRdg(const QByteArray& bytes, QString* error) {
    const auto root = parseXml(bytes);
    const Element* file = root && root->name == "RDCMan" ? root->child("file") : nullptr;
    if (!file) { if (error) *error = "Not a Remote Desktop Connection Manager file."; return std::nullopt; }
    const auto credentials = [](Synthetic& node, const Element& element) {
        const auto* logon = element.child("logonCredentials");
        if (!logon || logon->attributes.value("inherit") == QLatin1String("FromParent")) return;
        node.set("Username", logon->value("userName")); node.set("Domain", logon->value("domain"));
    };
    std::function<Synthetic(const Element&)> convert = [&](const Element& element) {
        Synthetic node;
        if (element.name == "server") {
            const auto host = element.value("properties/name").isEmpty() ? element.value("name") : element.value("properties/name");
            const auto display = element.value("properties/displayName").isEmpty() ? element.value("displayName") : element.value("properties/displayName");
            node.name = display.isEmpty() ? host : display; node.set("Protocol", "RDP"); node.set("Hostname", host);
            if (const auto* settings = element.child("connectionSettings"); settings && settings->attributes.value("inherit") != QLatin1String("FromParent")) {
                node.set("Port", settings->value("port"));
                if (!settings->value("connectToConsole").isEmpty()) node.set("ConnectToConsole", settings->value("connectToConsole").compare("True", Qt::CaseInsensitive) == 0 ? "True" : "False");
            }
            node.set("Descr", element.value("properties/comment"));
            credentials(node, element);
            return node;
        }
        node.folder = true; node.name = element.value("properties/name").isEmpty() ? element.value("name") : element.value("properties/name");
        credentials(node, element);
        for (const auto& child : element.children) if (child.name == "group" || child.name == "server") node.children.push_back(convert(child));
        return node;
    };
    return synthesize({convert(*file)}, error);
}

std::optional<QByteArray> fromPuttyCm(const QByteArray& bytes, QString* error) {
    const auto root = parseXml(bytes);
    if (!root || root->name != "configuration") { if (error) *error = "Not a PuTTY Connection Manager file."; return std::nullopt; }
    const auto protocol = [](const QString& value) {
        const auto lower = value.toLower();
        return lower == "ssh" ? "SSH2" : lower == "rdp" ? "RDP" : lower == "vnc" ? "VNC" : lower == "telnet" ? "Telnet" : lower == "rlogin" ? "Rlogin" : "RAW";
    };
    std::function<void(const Element&, std::vector<Synthetic>&)> convert = [&](const Element& element, std::vector<Synthetic>& into) {
        for (const auto& child : element.children) {
            const auto type = child.attributes.value("type").toString();
            if (child.name == "connection") {
                Synthetic node; node.name = child.attributes.value("name").toString();
                node.set("Protocol", protocol(child.value("connection_info/protocol")));
                node.set("Hostname", child.value("connection_info/host")); node.set("Port", child.value("connection_info/port"));
                node.set("Descr", child.value("connection_info/description"));
                node.set("Username", child.value("login/login")); node.password = child.value("login/password");
                into.push_back(std::move(node));
            } else if (child.name == "container" || child.name == "root") {
                Synthetic folder; folder.folder = true; folder.name = child.attributes.value("name").toString();
                convert(child, folder.children);
                if (type == "database" && folder.name.isEmpty()) folder.name = "PuTTY Connection Manager";
                into.push_back(std::move(folder));
            }
        }
    };
    std::vector<Synthetic> top; convert(*root, top);
    return synthesize(top, error);
}

Synthetic puttySession(const QString& name, const std::function<QString(const QString&)>& value) {
    Synthetic node; node.name = name;
    const auto kind = value("Protocol").toLower();
    node.set("Protocol", kind == "ssh" || kind.isEmpty() ? "SSH2" : kind == "telnet" ? "Telnet" : kind == "rlogin" ? "Rlogin" : "RAW");
    auto host = value("HostName");
    if (host.contains('@')) { node.set("Username", host.section('@', 0, -2)); host = host.section('@', -1); }
    node.set("Hostname", host); node.set("Port", value("PortNumber")); node.set("Username", value("UserName"));
    return node;
}
}

namespace ConnectionImport {
QString fileFilter() {
    return "All importable files (*.xml *.rdp *.rdg *.dat *.csv);;mRemote XML Files (*.xml);;mRemote CSV Files (*.csv);;"
           "RDP Files (*.rdp);;Remote Desktop Connection Manager files (*.rdg);;PuTTY Connection Manager files (*.dat);;All Files (*)";
}

std::optional<QByteArray> fromFile(const QString& path, const QByteArray& bytes, QString* error) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "rdp") return fromRdp(path, bytes, error);
    if (suffix == "csv") return fromCsv(bytes, error);
    if (suffix == "rdg") return fromRdg(bytes, error);
    if (suffix == "dat") return fromPuttyCm(bytes, error);
    return bytes; // mRemoteNG XML, read natively.
}

std::optional<QByteArray> fromPuttySessions(QString* error) {
    std::vector<Synthetic> sessions;
#ifdef Q_OS_WIN
    QSettings registry("HKEY_CURRENT_USER\\Software\\SimonTatham\\PuTTY\\Sessions", QSettings::NativeFormat);
    for (const auto& group : registry.childGroups()) {
        registry.beginGroup(group);
        const auto name = QUrl::fromPercentEncoding(group.toUtf8());
        auto node = puttySession(name, [&](const QString& key) { return registry.value(key).toString(); });
        registry.endGroup();
        if (name != "Default Settings" && std::any_of(node.attributes.begin(), node.attributes.end(), [](const auto& a) { return a.first == "Hostname"; }))
            sessions.push_back(std::move(node));
    }
#else
    const QDir directory(QDir::homePath() + "/.putty/sessions");
    for (const auto& entry : directory.entryInfoList(QDir::Files)) {
        QFile file(entry.filePath()); if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) continue;
        std::map<QString, QString> values;
        for (const auto& line : QString::fromUtf8(file.readAll()).split('\n'))
            if (line.contains('=')) values[line.section('=', 0, 0).trimmed()] = line.section('=', 1).trimmed();
        const auto name = QUrl::fromPercentEncoding(entry.fileName().toUtf8());
        auto node = puttySession(name, [&](const QString& key) { const auto found = values.find(key); return found == values.end() ? QString() : found->second; });
        if (name != "Default Settings" && std::any_of(node.attributes.begin(), node.attributes.end(), [](const auto& a) { return a.first == "Hostname"; }))
            sessions.push_back(std::move(node));
    }
#endif
    if (sessions.empty()) { if (error) *error = "No saved PuTTY sessions were found."; return std::nullopt; }
    Synthetic folder; folder.folder = true; folder.name = "PuTTY sessions"; folder.children = std::move(sessions);
    return synthesize({folder}, error);
}
}
