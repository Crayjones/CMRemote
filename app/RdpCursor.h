#pragma once
#include "RemoteCursor.h"
#include <freerdp/graphics.h>

// position (optional) observes server-initiated cursor moves.
rdpPointer rdpCursorPrototype(pPointer_Set set, pPointer_SetNull hide, pPointer_SetDefault reset, pPointer_SetPosition position = nullptr);
RemoteCursor rdpCursorImage(const rdpPointer* pointer);
