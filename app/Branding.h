#pragma once
#include <QIcon>

namespace Branding {
// Registers the static resources and applies visible branding without changing
// the application/organization names used by settings and data paths.
void initialize();
QIcon applicationIcon();
}
