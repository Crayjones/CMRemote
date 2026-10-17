#include "ConnectionXml.h"
#include "IdleTimeout.h"
#include "RdpSecurity.h"
#include "RdpWorker.h"
#include "remotemanager-core/src/bridge.rs.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QXmlStreamReader>
#include <freerdp/error.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <memory>

namespace {
using Settings = std::unique_ptr<rdpSettings, decltype(&freerdp_settings_free)>;
Settings fresh() { return Settings(freerdp_settings_new(0), freerdp_settings_free); }
bool flag(const Settings& s, FreeRDP_Settings_Keys_Bool key) { return freerdp_settings_get_bool(s.get(), key); }
struct Flags { bool rdp, tls, nla, ext; };
Flags flags(const Settings& s) { return {flag(s, FreeRDP_RdpSecurity), flag(s, FreeRDP_TlsSecurity), flag(s, FreeRDP_NlaSecurity), flag(s, FreeRDP_ExtSecurity)}; }

// mRemoteNG "Password" Protected canary (confCons_v2_6 test resources).
const QByteArray Canary("e/T6ajrPtNNlHreSeD4QBqToTuiqtNACKiPJv7vU+l6TWCu9JNsmL+Y8lJ4aTl5YVcstXpQjxsZ9i8+YV4Gs");
const QByteArray Id("0c7c3b02-1e3c-4b52-9b8f-1d6c2f6b2a01");
QByteArray xml(const QByteArray& attributes) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Connections Name=\"Connections\" Export=\"false\" ConfVersion=\"2.8\" "
        "EncryptionEngine=\"AES\" BlockCipherMode=\"GCM\" KdfIterations=\"1000\" FullFileEncryption=\"false\" Protected=\"" + Canary + "\">"
        "<Node Type=\"Connection\" Id=\"" + Id + "\" Name=\"krdp\" Protocol=\"RDP\" Hostname=\"krdp.example\" Port=\"3389\" Username=\"rdpuser\" "
        "Password=\"\" Colors=\"Colors32Bit\" Resolution=\"FitToWindow\" " + attributes + "/></Connections>";
}
QByteArray attribute(const QByteArray& document, const QString& name) {
    QXmlStreamReader reader(document);
    while (!reader.atEnd()) { reader.readNext(); if (reader.isStartElement() && reader.name() == "Node") return reader.attributes().value(name).toLatin1(); }
    return {};
}
QJsonObject profile(const QByteArray& projection) { return QJsonDocument::fromJson(projection).object().value("profiles").toArray()[0].toObject(); }
QJsonObject effective(const QByteArray& projection) {
    auto core = rm::detached_core(); core->load_document(rust::Str(projection.constData(), size_t(projection.size())));
    const auto snapshot = core->snapshot();
    return QJsonDocument::fromJson(QByteArray(snapshot.data(), qsizetype(snapshot.size()))).object().value("profiles").toArray()[0].toObject().value("effective").toObject();
}
}

class RdpSecurityTest : public QObject {
    Q_OBJECT
private slots:
    void securityModesMapExclusiveFlags() {
        const struct { const char* mode; Flags expected; } cases[] = {
            {"nla", {false, false, true, false}}, {"ext", {false, false, false, true}},
            {"tls", {false, true, false, false}}, {"rdp", {true, false, false, false}}};
        for (const auto& c : cases) {
            auto s = fresh(); QVERIFY(s);
            RdpOptions o; o.securityProtocol = c.mode; if (QByteArray(c.mode) == "rdp") o.serverAuth = "warn";
            QVERIFY2(applyRdpSecurity(s.get(), o), c.mode);
            const auto f = flags(s);
            QCOMPARE(f.rdp, c.expected.rdp); QCOMPARE(f.tls, c.expected.tls); QCOMPARE(f.nla, c.expected.nla); QCOMPARE(f.ext, c.expected.ext);
            QVERIFY(flag(s, FreeRDP_NegotiateSecurityLayer));
        }
        // Switching an existing settings object from a permissive to an explicit mode clears the others.
        auto s = fresh(); RdpOptions o; QVERIFY(applyRdpSecurity(s.get(), o)); o.securityProtocol = "tls"; QVERIFY(applyRdpSecurity(s.get(), o));
        const auto f = flags(s); QVERIFY(f.tls && !f.rdp && !f.nla && !f.ext);
    }
    void automaticKeepsFreeRdpNegotiation() {
        // Existing Windows compatibility: Automatic + CredSSP leaves FreeRDP's defaults.
        auto reference = fresh(), s = fresh(); QVERIFY(applyRdpSecurity(s.get(), RdpOptions{}));
        const auto a = flags(reference), b = flags(s);
        QCOMPARE(b.rdp, a.rdp); QCOMPARE(b.tls, a.tls); QCOMPARE(b.nla, a.nla); QCOMPARE(b.ext, a.ext);
        QVERIFY(b.nla && b.tls);
    }
    void credsspOffClearsNla() {
        auto s = fresh(); RdpOptions o; o.useCredSsp = false; QVERIFY(applyRdpSecurity(s.get(), o));
        const auto f = flags(s); QVERIFY(!f.nla && !f.ext && f.tls);
        o.securityProtocol = "tls"; s = fresh(); QVERIFY(applyRdpSecurity(s.get(), o)); QVERIFY(flags(s).tls);
    }
    void incompatibleCombinationsRejected() {
        RdpOptions o; o.securityProtocol = "nla"; o.useCredSsp = false;
        QVERIFY(rdpSecurityConflict(o).contains("Use CredSSP")); auto s = fresh(); QVERIFY(!applyRdpSecurity(s.get(), o));
        o.securityProtocol = "ext"; QVERIFY(rdpSecurityConflict(o).contains("Use CredSSP"));
        o = {}; o.securityProtocol = "rdp"; o.serverAuth = "required"; QVERIFY(rdpSecurityConflict(o).contains("Legacy RDP Security"));
        o.serverAuth = "warn"; QVERIFY(rdpSecurityConflict(o).isEmpty());
        o = {}; o.gatewayUsage = "always"; QVERIFY(rdpSecurityConflict(o).contains("Gateway Hostname"));
        o.gatewayHost = "gw.example.test"; QVERIFY(rdpSecurityConflict(o).isEmpty());
        o = {}; o.securityProtocol = "bogus"; QVERIFY(!rdpSecurityConflict(o).isEmpty());
    }
    void serverAuthPolicies() {
        for (const auto& policy : {QString("always"), QString("required"), QString("warn")}) {
            auto s = fresh(); RdpOptions o; o.serverAuth = policy; QVERIFY(applyRdpSecurity(s.get(), o));
            QCOMPARE(flag(s, FreeRDP_IgnoreCertificate), policy == "always");
            QVERIFY(!flag(s, FreeRDP_AutoAcceptCertificate)); QVERIFY(!flag(s, FreeRDP_AutoDenyCertificate));
        }
        QCOMPARE(rdpCertificateDecision("warn"), RdpCertificateDecision::Session);
        QCOMPARE(rdpCertificateDecision("required"), RdpCertificateDecision::Reject);
        QCOMPARE(rdpCertificateDecision("always"), RdpCertificateDecision::Session);
        // Unsupported values are rejected by profile validation before a connection.
        RdpOptions invalid; invalid.serverAuth = "unexpected";
        auto s = fresh(); QVERIFY(!applyRdpSecurity(s.get(), invalid));
    }
    void workerProfileSettingsVerifyCertificates() {
        // Replaces the old unconditional IgnoreCertificate=TRUE.
        auto s = fresh(); SessionTarget target{"krdp.example", 3389, "rdpuser"};
        QVERIFY(RdpWorker::applyProfileSettings(s.get(), target, {720, 480})); QVERIFY(!flag(s, FreeRDP_IgnoreCertificate));
        target.options.securityProtocol = "nla"; target.options.useCredSsp = false;
        QVERIFY(!RdpWorker::applyProfileSettings(s.get(), target, {720, 480}));
    }
    void loadBalanceInfoVerbatim() {
        auto s = fresh(); RdpOptions o; QVERIFY(applyRdpSecurity(s.get(), o));
        QCOMPARE(freerdp_settings_get_uint32(s.get(), FreeRDP_LoadBalanceInfoLength), 0u);
        o.loadBalanceInfo = "tsv://MS Terminal Services Plugin.1.Pool_Ünïcode"; QVERIFY(applyRdpSecurity(s.get(), o));
        const auto expected = o.loadBalanceInfo.toUtf8();
        QCOMPARE(freerdp_settings_get_uint32(s.get(), FreeRDP_LoadBalanceInfoLength), quint32(expected.size()));
        const auto* bytes = static_cast<const char*>(freerdp_settings_get_pointer(s.get(), FreeRDP_LoadBalanceInfo));
        QCOMPARE(QByteArray(bytes, expected.size()), expected);
    }
    void gatewayNeverLeavesDirect() {
        auto s = fresh(); RdpOptions o; o.gatewayHost = "gw.example.test"; o.gatewayUsername = "gwuser";
        QVERIFY(applyRdpSecurity(s.get(), o));
        QVERIFY(!flag(s, FreeRDP_GatewayEnabled)); QVERIFY(!freerdp_settings_get_string(s.get(), FreeRDP_GatewayHostname));
        QVERIFY(!freerdp_settings_get_string(s.get(), FreeRDP_GatewayUsername));
    }
    void gatewaySettingsMapped() {
        auto s = fresh(); RdpOptions o; o.gatewayUsage = "always"; o.gatewayHost = "gw.example.test"; o.gatewayPort = 8443;
        QVERIFY(applyRdpSecurity(s.get(), o));
        QVERIFY(flag(s, FreeRDP_GatewayEnabled)); QVERIFY(!flag(s, FreeRDP_GatewayBypassLocal));
        QCOMPARE(freerdp_settings_get_uint32(s.get(), FreeRDP_GatewayUsageMethod), quint32(TSC_PROXY_MODE_DIRECT));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(s.get(), FreeRDP_GatewayHostname)), QString("gw.example.test"));
        QCOMPARE(freerdp_settings_get_uint32(s.get(), FreeRDP_GatewayPort), 8443u);
        QVERIFY(flag(s, FreeRDP_GatewayUseSameCredentials));
        o.gatewayUsage = "detect"; o.gatewayCredentials = "separate"; o.gatewayUsername = "gwuser"; o.gatewayDomain = "GW";
        s = fresh(); QVERIFY(applyRdpSecurity(s.get(), o));
        QVERIFY(flag(s, FreeRDP_GatewayEnabled)); QVERIFY(flag(s, FreeRDP_GatewayBypassLocal));
        QVERIFY(!flag(s, FreeRDP_GatewayUseSameCredentials));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(s.get(), FreeRDP_GatewayUsername)), QString("gwuser"));
        QCOMPARE(QString::fromUtf8(freerdp_settings_get_string(s.get(), FreeRDP_GatewayDomain)), QString("GW"));
        // Gateway secrets are never part of the profile mapping (V106).
        QVERIFY(!freerdp_settings_get_string(s.get(), FreeRDP_GatewayPassword));
    }
    void soundMapsToPlaybackAndConsoleAudio() {
        for (const auto& sound : {QString("local"), QString("remote"), QString("none")}) {
            auto s = fresh(); QVERIFY(applyRdpAudio(s.get(), sound));
            QCOMPARE(flag(s, FreeRDP_AudioPlayback), sound == "local" && rdpAudioPlaybackAvailable());
            QCOMPARE(flag(s, FreeRDP_RemoteConsoleAudio), sound == "remote");
        }
        auto s = fresh(); QVERIFY(!applyRdpAudio(s.get(), "loud"));
#ifdef Q_OS_MACOS
        QVERIFY(rdpAudioPlaybackAvailable()); // bundled FreeRDP: rdpsnd + rdpdr + CoreAudio
#endif
        SessionTarget target{"host.example", 3389, "alice"}; QCOMPARE(target.options.sound, QString("local")); QVERIFY(!target.options.clipboard);
        auto w = fresh(); QVERIFY(RdpWorker::applyProfileSettings(w.get(), target, {720, 480}));
        QCOMPARE(flag(w, FreeRDP_AudioPlayback), rdpAudioPlaybackAvailable());
        target.options.sound = "bogus"; QVERIFY(!normalizeRdpTarget(target));
    }
    void messageChannelAndAutodetectWithoutRdpdr() {
        // B38: KRdp sends message-channel auto-detect PDUs during connect and
        // continuously once active; FreeRDP fails the session on them unless
        // NetworkAutoDetect is on. Set after channel load so rdpdr stays out.
        auto s = fresh(); QVERIFY(freerdp_settings_set_bool(s.get(), FreeRDP_NetworkAutoDetect, FALSE));
        QVERIFY(freerdp_settings_set_bool(s.get(), FreeRDP_SupportMultitransport, FALSE));
        QVERIFY(requestRdpMessageChannel(s.get()));
        QVERIFY(flag(s, FreeRDP_SupportHeartbeatPdu));
        QVERIFY(flag(s, FreeRDP_NetworkAutoDetect)); QVERIFY(!flag(s, FreeRDP_SupportMultitransport));
        QVERIFY(!flag(s, FreeRDP_DeviceRedirection));
    }
    void failureTextNamesFreeRdpError() {
        RdpOptions o;
        const auto nego = rdpFailureText(FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED, o, false);
        QVERIFY(nego.contains("ERRCONNECT_SECURITY_NEGO_CONNECT_FAILED")); QVERIFY(nego.contains("TLS or NLA"));
        o.securityProtocol = "nla";
        QVERIFY(rdpFailureText(FREERDP_ERROR_SECURITY_NEGO_CONNECT_FAILED, o, false).contains("Security Protocol NLA"));
        QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_HYBRID_REQUIRED_BY_SERVER, o, false).contains("requires NLA"));
        QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_LOGON_FAILURE, o, false).contains("username, domain and password"));
        QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_TRANSPORT_FAILED, o, false).contains("ERRCONNECT_CONNECT_TRANSPORT_FAILED"));
        o.gatewayUsage = "always"; o.gatewayHost = "gw";
        QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_FAILED, o, false).contains("RDP gateway"));
        o.serverAuth = "required"; QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_CANCELLED, o, true).contains("Don't connect"));
        o.serverAuth = "warn"; QVERIFY(rdpFailureText(FREERDP_ERROR_CONNECT_CANCELLED, o, true).contains("not trusted"));
        for (const auto error : {FREERDP_ERROR_CONNECT_LOGON_FAILURE, FREERDP_ERROR_TLS_CONNECT_FAILED})
            QVERIFY(!rdpFailureText(error, o, false).contains("password=", Qt::CaseInsensitive));
    }
    void rdpPropertiesRoundTripXml() {
        const Secret master(QByteArray("Password"));
        const QByteArray attributes = "RmRdpSecurity=\"tls\" RDPAuthenticationLevel=\"AuthRequired\" UseCredSsp=\"false\" RDPMinutesToIdleTimeout=\"15\" "
            "LoadBalanceInfo=\"tsv://MS Terminal Services Plugin.1.Pool\" RDGatewayUsageMethod=\"Detect\" RDGatewayHostname=\"gw.example.test\" "
            "RmGatewayPort=\"8443\" RDGatewayUseConnectionCredentials=\"No\" RDGatewayUsername=\"gwuser\" RDGatewayDomain=\"GW\" RedirectSound=\"LeaveAtRemoteComputer\" RedirectClipboard=\"true\"";
        auto parsed = ConnectionXml::read(xml(attributes), &master); QVERIFY(parsed);
        const auto projection = parsed->projection(); QVERIFY(projection);
        const auto overrides = profile(*projection).value("overrides").toObject();
        QCOMPARE(overrides.value("security_protocol").toString(), QString("tls"));
        QCOMPARE(overrides.value("server_auth").toString(), QString("required"));
        QCOMPARE(overrides.value("use_credssp").toBool(true), false);
        QCOMPARE(overrides.value("idle_minutes").toInt(), 15);
        QCOMPARE(overrides.value("load_balance_info").toString(), QString("tsv://MS Terminal Services Plugin.1.Pool"));
        QCOMPARE(overrides.value("gateway_usage").toString(), QString("detect"));
        QCOMPARE(overrides.value("gateway_port").toInt(), 8443);
        QCOMPARE(overrides.value("gateway_credentials").toString(), QString("separate"));
        QCOMPARE(overrides.value("sound").toString(), QString("remote"));
        QCOMPARE(overrides.value("clipboard").toBool(), true);
        const auto e = effective(*projection); QVERIFY2(e.value("unsupported").toArray().isEmpty(), QJsonDocument(e.value("unsupported").toArray()).toJson().constData());
        const auto target = sessionTarget(e);
        QCOMPARE(target.options.securityProtocol, QString("tls")); QCOMPARE(target.options.serverAuth, QString("required"));
        QCOMPARE(target.options.idleMinutes, quint16(15)); QCOMPARE(target.options.gatewayUsername, QString("gwuser"));
        QVERIFY(target.options.clipboard);
        // Edit through the projection and confirm native mRemoteNG attribute values.
        auto document = QJsonDocument::fromJson(*projection).object(); auto profiles = document.value("profiles").toArray();
        auto edited = profiles[0].toObject(); auto o = edited.value("overrides").toObject();
        o["server_auth"] = "always"; o["security_protocol"] = "nla"; o["use_credssp"] = true; o["gateway_usage"] = "always";
        o["gateway_credentials"] = "same"; o["idle_minutes"] = 0; o["sound"] = "local"; o["clipboard"] = false; edited["overrides"] = o; profiles[0] = edited; document["profiles"] = profiles;
        const auto patched = parsed->patch(master, QJsonDocument(document).toJson(QJsonDocument::Compact)); QVERIFY(patched);
        const auto saved = parsed->serialize(master); QVERIFY(saved);
        QCOMPARE(attribute(*saved, "RDPAuthenticationLevel"), QByteArray("NoAuth"));
        QCOMPARE(attribute(*saved, "RmRdpSecurity"), QByteArray("nla"));
        QCOMPARE(attribute(*saved, "UseCredSsp"), QByteArray("true"));
        QCOMPARE(attribute(*saved, "RDGatewayUsageMethod"), QByteArray("Always"));
        QCOMPARE(attribute(*saved, "RDGatewayUseConnectionCredentials"), QByteArray("Yes"));
        QCOMPARE(attribute(*saved, "RDPMinutesToIdleTimeout"), QByteArray("0"));
        QCOMPARE(attribute(*saved, "RedirectSound"), QByteArray("BringToThisComputer"));
        QCOMPARE(attribute(*saved, "RedirectClipboard"), QByteArray("false"));
        QCOMPARE(attribute(*saved, "LoadBalanceInfo"), QByteArray("tsv://MS Terminal Services Plugin.1.Pool"));
        auto reopened = ConnectionXml::read(*saved, &master); QVERIFY(reopened);
        QCOMPARE(effective(*reopened->projection()).value("server_auth").toString(), QString("always"));
        QCOMPARE(effective(*reopened->projection()).value("clipboard").toBool(true), false);
    }
    void existingFilesUseSessionCertificateAcceptance() {
        // Old files retain the warn value for XML compatibility; certificate
        // callbacks accept for this session without prompting or storing trust.
        const Secret master(QByteArray("Password"));
        for (const QByteArray& attributes : {QByteArray(), QByteArray("UseCredSsp=\"true\" RDPAuthenticationLevel=\"WarnOnFailedAuth\"")}) {
            auto parsed = ConnectionXml::read(xml(attributes), &master); QVERIFY(parsed);
            const auto e = effective(*parsed->projection());
            QVERIFY2(e.value("unsupported").toArray().isEmpty(), QJsonDocument(e.value("unsupported").toArray()).toJson().constData());
            QCOMPARE(e.value("security_protocol").toString(), QString("auto")); QCOMPARE(e.value("server_auth").toString(), QString("warn"));
            QCOMPARE(rdpCertificateDecision(sessionTarget(e).options.serverAuth), RdpCertificateDecision::Session);
            QVERIFY(e.value("use_credssp").toBool()); QCOMPARE(e.value("idle_minutes").toInt(), 0);
            QCOMPARE(e.value("gateway_usage").toString(), QString("never")); QCOMPARE(e.value("gateway_port").toInt(), 443);
            QCOMPARE(e.value("gateway_credentials").toString(), QString("same"));
        }
    }
    void unsupportedNativeValuesStayLaunchDisabled() {
        const Secret master(QByteArray("Password"));
        const struct { QByteArray attributes; const char* reason; } cases[] = {
            {"RDGatewayUsageMethod=\"Always\" RDGatewayHostname=\"gw\" RDGatewayUseConnectionCredentials=\"SmartCard\"", "gateway"},
            {"RDGatewayUsageMethod=\"Always\" RDGatewayHostname=\"gw\" RDGatewayUseConnectionCredentials=\"AccessToken\"", "gateway"},
            {"RDPMinutesToIdleTimeout=\"500\"", "idle_timeout"}, {"RDPAlertIdleTimeout=\"true\"", "idle_timeout"},
            {"RDPAuthenticationLevel=\"Sometimes\"", "authentication"},
            // Not representable in the model: preserved in XML, file still loads.
            {"RDGatewayUsageMethod=\"Always\" RDGatewayHostname=\"gw.example.test:443\"", "gateway"},
            {"RDGatewayUsername=\"GW\\\\user\" RDGatewayDomain=\"GW\"", "gateway"}};
        for (const auto& c : cases) {
            auto parsed = ConnectionXml::read(xml(c.attributes), &master); QVERIFY2(parsed, c.attributes.constData());
            const auto projection = parsed->projection(); QVERIFY(projection);
            QVERIFY2(effective(*projection).value("unsupported").toArray().contains(QString(c.reason)), c.attributes.constData());
            const auto saved = parsed->serialize(master); QVERIFY(saved);
            for (const auto& name : {"RDGatewayHostname", "RDGatewayUsername", "RDGatewayUseConnectionCredentials", "RDPMinutesToIdleTimeout"})
                QCOMPARE(attribute(*saved, name), attribute(xml(c.attributes), name));
        }
    }
    void rustModelRejectsOutOfRangeValues() {
        SessionTarget target{"host.example", 3389, "alice"};
        target.options.idleMinutes = 241; QVERIFY(!normalizeRdpTarget(target));
        target.options.idleMinutes = 240; QVERIFY(normalizeRdpTarget(target));
        target.options.gatewayPort = 0; QVERIFY(!normalizeRdpTarget(target)); target.options.gatewayPort = 443;
        target.options.securityProtocol = "bogus"; QVERIFY(!normalizeRdpTarget(target)); target.options.securityProtocol = "tls";
        target.options.gatewayHost = "bad host"; QVERIFY(!normalizeRdpTarget(target)); target.options.gatewayHost = "gw.example";
        target.options.loadBalanceInfo = QString("a\nb"); QVERIFY(!normalizeRdpTarget(target)); target.options.loadBalanceInfo.clear();
        QVERIFY(normalizeRdpTarget(target));
    }
    void idleTimerDisconnectsAndResets() {
        IdleTimeout idle; QSignalSpy expired(&idle, &IdleTimeout::expired);
        idle.configure(0); idle.start(); QVERIFY(!idle.armed());
        idle.configure(1, 150); QVERIFY(!idle.armed());
        idle.activity(); QVERIFY(!idle.armed()); // input before connection does not arm
        idle.start(); QVERIFY(idle.armed());
        for (int i = 0; i < 4; ++i) { QTest::qWait(80); idle.activity(); }
        QCOMPARE(expired.count(), 0); // continuous input keeps the session alive
        QTRY_COMPARE_WITH_TIMEOUT(expired.count(), 1, 1000);
        idle.start(); idle.stop(); QTest::qWait(250); QCOMPARE(expired.count(), 1);
        QCOMPARE(idle.minutes(), 1);
    }
};

QTEST_GUILESS_MAIN(RdpSecurityTest)
#include "rdp_security.moc"
