#pragma once
#include "RdpTarget.h"
#include <QString>

// FreeRDP mapping for the RDP security/gateway profile settings (I.rdpsecurity).
struct rdp_settings;

// Connect-time compatibility check (V101). Empty when the combination is usable,
// otherwise a message naming the setting to change. Never weakens a setting.
QString rdpSecurityConflict(const RdpOptions& options);
// Security flags, certificate policy, load-balance routing token and
// non-secret gateway settings. Gateway passwords are supplied separately.
bool applyRdpSecurity(rdp_settings* settings, const RdpOptions& options);
// Join the MCS message channel and answer server auto-detect/heartbeat PDUs.
// Call after freerdp_client_load_channels: set earlier, these flags make the
// client loader require the unbuilt rdpdr channel (V102).
bool requestRdpMessageChannel(rdp_settings* settings);

// Sounds: local → rdpsnd playback here (when built with an audio backend),
// remote → server plays on its own device, none → no audio.
bool applyRdpAudio(rdp_settings* settings, const QString& sound);
bool rdpAudioPlaybackAvailable();

// NTLM/NLA (MD4) and legacy RDP encryption (RC4) work with no OpenSSL legacy provider
// installed. Empty when both pass known answers, otherwise the failing algorithm.
QString rdpLegacyCryptoFailure();

enum class RdpCertificateDecision { Reject, Session };
// Callback policy for certificates FreeRDP could not verify itself (no prompt, nothing stored).
RdpCertificateDecision rdpCertificateDecision(const QString& serverAuth);
// FreeRDP VerifyCertificateEx/VerifyChangedCertificateEx answers.
inline constexpr unsigned RdpCertificateReject = 0, RdpCertificateSession = 2;

// Failure text with the FreeRDP error name and an actionable hint (V103).
QString rdpFailureText(quint32 error, const RdpOptions& options, bool certificateRejected);
