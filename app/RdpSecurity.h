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
// client loader register rdpdr (and its rdpsnd) for every session (V102).
bool requestRdpMessageChannel(rdp_settings* settings);

// Sounds: local → rdpsnd playback here (when built with an audio backend),
// remote → server plays on its own device, none → no audio.
bool applyRdpAudio(rdp_settings* settings, const QString& sound);
bool rdpAudioPlaybackAvailable();

// Shared folder: one existing host folder redirected as a drive named after it.
// Empty shares nothing and keeps device redirection off. Never redirects other drives.
bool applyRdpSharedFolder(rdp_settings* settings, const QString& folder);
// Empty when the folder can be shared, otherwise why not.
QString rdpSharedFolderProblem(const QString& folder);
// The name the remote computer shows ("<name> on <client>").
QString rdpSharedFolderName(const QString& folder);

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
