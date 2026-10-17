# Connection XML passwords and session credentials

## Save and reuse

Edit **Password** in Config, between Username and Domain. The transient editor is masked and never preloads a saved password. Enter or focus loss submits a replacement; Escape cancels; an untouched empty editor does nothing. **Clear password** and password inheritance are explicit operations. A fixed eight-bullet saved indicator reveals neither the value nor its length. Folder passwords can be inherited within their owning file.

Before saving, review the committed endpoints and accounts allowed to reuse the password. Saving does not require a custom file password: unprotected files encrypt passwords with mRemoteNG's public `mR3m` default key. This is obfuscation, not confidentiality. For confidentiality, set an optional custom password through the root's **Password protect** field or **Set or change file password…**.

New custom passwords require exact matching confirmation, 1–4096 single-byte characters (U+0001–U+00FF), and at least one non-whitespace character; `mR3m` is reserved for the unprotected default. Use a long random password. Characters above U+00FF reject because they cannot use the original provider's encoding. Masters are never normalized or silently truncated. Connection passwords support validated UTF-8 up to 4096 bytes.

Reusable passwords are encrypted inside connection XML, normally `confCons.xml`, not in profile JSON or a separate vault. JSON is an internal password-free metadata projection. Ordinary Core/cxx metadata, item roles and Config drafts/logs remain secret-free. SSH alone has a separate session-only cxx authentication handoff to its Rust backend for an owned password or key-file path/passphrase; authentication is never serialized into Core/profile/session metadata. Document masters and document crypto keys remain C++-owned and never enter that handoff. Ordinary development data stays under `.dev/<platform>/data`; normal launches use Qt's application-data location.

Saved RDP or SSH Password Connect/Reconnect reuses a matching unlocked password. After Lock, it first unlocks the captured owning file: default-key files authenticate without a master dialog; custom files prompt again. Cancel stops the request without creating/replacing a session or falling through to a remote-login dialog. Missing passwords prompt for session-only credentials; there is no remember-password control or normal saved-credentials manager. **Quick Connect** and **Connect with different credentials…** never persist passwords. **Save as connection…** saves metadata without copying temporary authentication secrets or retargeting the active session. RDP clipboard sharing is off by default; SSH Copy/Paste requires a direct user action and never synchronizes remote output with the host clipboard. Password storage grants neither RDP certificate trust nor SSH host-key approval.

SSH sends Username literally. Domain stays preserved but inactive. **SSH authentication: Private key** bypasses saved Password lookup; its masked passphrase prompt is session-only and never treats an XML Password as a key passphrase. Key contents/passphrases are not saved. Supported formats, KDF/key-size limits and best-effort release are documented in [SSH API](ssh-api.md).

Private bindings check the portable file identity, consumer/owner UUIDs, actual password ciphertext hash, password source and committed normalized endpoint/account. Exact version-1 RDP bindings remain compatible. Version 2 adds a protocol discriminator for RDP or SSH2 Password, with an empty canonical SSH domain; mismatched/unknown versions reject. A write requiring SSH bindings upgrades the payload, not an ordinary load. RDP authorization cannot authorize SSH reuse. Under a custom key bindings are authenticated; under the public default key they provide no attacker-resistant authenticity. Endpoint/account/source/protocol changes remove reuse authorization without changing an active session. Switching away from SSH Password removes authorization but retains ciphertext; switching back requires replacement or explicit review. Deleted nodes lose their secrets/bindings in the same replacement; duplicate nodes do not implicitly copy credentials.

## Multiple files and locking

Each loaded file has an independent **Connections** root, runtime identity, master, revision, selection and save state. Copies with the same portable file/profile UUIDs remain separate runtime documents. Root path/filename context distinguishes them. Saved tabs, reconnect and worker replies resolve their owning file, not the selected root.

Tools provides **Unlock passwords…**, **Lock passwords**, and **Set or change file password…** for the selected file. An unlocked file stays unlocked until you lock or close it; it never relocks on its own. Wrong/canceled masters do not reset or rewrite it. Lock clears that file's reusable master/cache and invalidates pending lookup; other files and active sessions remain usable.

**Lock does not conceal already opened metadata or active targets.** It is not a screen/privacy lock. Active sessions retain their transient credentials. Close stops their workers; a file cannot close while its saved tabs remain, including disconnected tabs.

Completed Config edits autosave. Failed edits block switch/connect/close with Retry/Revert/Cancel. Lock clears pending plaintext but retains a nonsecret recovery marker requiring re-entry or Revert. Physical replacement is the commit boundary: committed outcomes reconcile to the owning context even after cancellation or Lock. A later directory-sync failure reports a saved-with-durability-warning outcome and requires acknowledgement.

## Independent root controls

**Password protect** selects the key. Yes asks for a custom password twice and atomically re-encrypts all known secrets. No confirms the public-default risk and re-encrypts them under `mR3m`; saved passwords and the full-encryption setting are retained. An unauthenticated file displays unknown protection state until unlocked.

**Encrypt entire file** selects what is encrypted. Confirm the warning to encrypt/decrypt the inner XML with the current key, without changing Password protect or asking for a new master. Full mode includes connection names, hosts and accounts; public root attributes remain readable. Individual password values remain encrypted inside that payload. Full mode under `mR3m` still provides only obfuscation.

Opening a custom-protected XML file automatically prompts with its filename/path, with at most three submitted passwords. Multiple files queue independent prompts. Cancel leaves that file locked and unchanged. Full-encrypted metadata remains hidden until successful authentication and validation; password-only metadata can be displayed while saved passwords/writes are locked. Default-key files authenticate automatically in either mode. Opening/unlocking never rewrites bytes. Corrupt/unsupported files reject without an empty fallback. Disabling full mode requires confirmation and retains encrypted password attributes.

Master change authenticates and re-encrypts every known secret, canary, private bindings/archive and optional inner XML with fresh salts/nonces in one XML replacement. Unknown secret-bearing extensions with unsupported rekey semantics block the operation. There is no data-key rewrap or sidecar stage. Old copies remain readable with their old masters; master change does not revoke them.

## Native mRemoteNG compatibility and limits

Supported interchange is **ConfVersion 2.6, 2.7 and 2.8, AES/GCM**, in password-only and full-inner-XML modes. New files use 2.8 and 10000 iterations; supported imported version/iteration values remain unchanged. Accepted KDF iterations are 1000–1000000. Legacy CBC, unsupported versions/engines/modes, missing/malformed parameters and crypto-factory fallback reject without changing the file.

Compatible values use AES-256-GCM with a 16-byte salt, 16-byte nonce, 16-byte tag, salt as AAD and PBKDF2-HMAC-SHA1. The envelope is `base64(salt || nonce || ciphertext || tag)`. Masters follow the original single-byte character conversion; plaintext secrets and inner XML use validated UTF-8. This compatible KDF has weaker password-guessing resistance than the retired Argon2id vault. No security-equivalence claim is made.

Default-key files use `mR3m` for password values, private metadata and optional full-inner XML, with a `ThisIsNotProtected` canary. Custom passwords use a `ThisIsProtected` canary. Effective protection state comes from successful key authentication, not the full-encryption flag or an unauthenticated sentinel. Default-key imports need endpoint/account review before saved reuse, not custom-master conversion. Anyone who possesses a default-key file can recover its secrets.

Native password AEAD authenticates the value, **not public host/account/node associations or root metadata**. Private encrypted `RmCredentialBindings` supplies this app's association checks. Missing private extensions require explicit native-import review before reuse; present-invalid extensions reject rather than falling back to import. Stripping every private extension is indistinguishable from legitimate native input. Original mRemoteNG resaves can drop private bindings, app-only host/fixed-resolution preferences and inactive archives. Fixed resolution is an app preference with native FitToWindow fallback, not identical original sizing behavior.

Unknown attributes, namespaces, opaque children and unsupported settings are preserved as passive data. Unsupported launch-critical settings disable Connect instead of silently becoming different RDP settings. DTD/entities, malformed nesting/IDs and excessive sizes reject. Native fixtures lacking IDs open read-only with stable temporary IDs; explicit repair/save consent is required before writable adoption. No plugin, command or .NET object is executed.

Interoperability evidence is original source/fixtures and independent crypto vectors. Windows mRemoteNG has not been launched or tested. No original .NET build/run or object/ImageListStreamer deserialization is used.

## Backup and retired formats

Back up **XML plus its master**. Active connection credentials require no sidecar. Do not copy during writes or overwrite the only copy during diagnosis. Lost masters cannot be recovered. Old copies/snapshots remain sensitive; there is no rollback resistance, secure-erasure or revocation guarantee.

Current production startup/Open/Save use XML only. Legacy JSON/`RMCONS` readers, vault services and **Convert to connection XML…** are absent. Malformed/unsupported input rejects instead of creating an empty file or falling back to legacy storage. Older conversion results in validation are historical, not a current API promise.

Retain historical protected-document/vault backups as pairs. The retired formats used Argon2id plus AES-GCM and, in some versions, retained document keys. Rekeying or converting a later copy does not revoke old copies or their keys. Preserved `RmLegacyCredentials` extensions in existing XML are encrypted passive archives; they do not authorize ordinary password lookup. Original mRemoteNG resaves can drop those app-only extensions.

## Independent local preferences and host trust

SSH host approval is stored separately in per-user `ssh-host-keys.json`, never beside or inside the XML document and never in `~/.ssh/known_hosts`. Unknown/changed keys require an explicit save/replace, connect-once or Cancel decision before authentication. Durable decisions require a conflict-checked atomic commit; connect-once does not write. Connection Open/Save as/rekey does not move or modify trust. RDP certificate approval remains session-only.

App-wide terminal appearance lives in separate `terminal-appearance.json`. Its font/colors/cursor preferences contain no credentials or trust and do not modify XML or retarget/reconnect sessions. Both services have independently injected paths, bounded schemas, off-GUI storage and accurate committed-warning outcomes. See [SSH trust and appearance behavior](ssh-api.md).

## Ownership and evidence boundaries

One serialized `DocumentService` worker owns XML stores and reusable secrets. KDF/crypto/I/O run off the GUI thread. Each GUI-owned context owns its detached Rust Core; the worker never borrows it. Independent generation/commit gates and terminal outcomes keep replies/cancellation file-local. Document shutdown invalidates work, clears worker keys and joins; in-flight KDF can briefly delay close. SSH network/key work has a separate standalone backend; its retirement/join runs off the GUI, and reconnect/application close wait asynchronously for it.

`AtomicFile` checks safe paths, sidecar locks and revisions; it uses owner-only Unix permissions and QSaveFile with direct-write fallback disabled. These checks are not filesystem merging or protection against every malicious filesystem race. Use one writer per physical file.

Move-only C++ Secret buffers use OPENSSL_cleanse. Temporary SSH authentication/key material is released after use/failure/cancellation; Qt/OS/FreeRDP/Rust copies make clearing best-effort, not guaranteed zeroization. There is no locked-memory, malware-resistance, independent-security-audit or power-loss-durability claim. RDP Domain binding/forwarding tests use synthetic instrumentation, not enterprise-domain login. Native Cocoa and Linux ARM64 XCB SSH have passed owned OpenSSH/vim acceptance; Windows/AMD64/Wayland SSH remain untested. OS keychain and VNC networking remain deferred. Exact tested platforms, live acceptance, package scope and capture limits are recorded in [validation](validation.md).
