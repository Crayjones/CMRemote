#pragma once
#include "Secret.h"
#include "remotemanager-core/src/ssh.rs.h"
#include <QString>

// Dedicated session-only handoff. Never put Authentication in Core/profile metadata.
rust::Box<rm::ssh::Authentication> sshPasswordAuthentication(Secret password);
rust::Box<rm::ssh::Authentication> sshPrivateKeyAuthentication(const QString& path, Secret passphrase);
