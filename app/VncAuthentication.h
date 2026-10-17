#pragma once
#include "Secret.h"
#include "remotemanager-core/src/vnc.rs.h"

// Session-only authentication: never retained in targets, Core, or XML metadata.
// A saved password is sent only if the server requires VncAuth; Prompt asks the
// user only then. Servers offering no authentication never receive a password.
rust::Box<rm::vnc::Authentication> vncPasswordAuthentication(Secret password);
rust::Box<rm::vnc::Authentication> vncPromptAuthentication();
