// Menu/taskbar integration for the AppImage: writes ~/.local/share/applications/ruche.desktop
// and the icon, so Ruche shows up in the applications menu and can be pinned.
#pragma once

#include <QString>

namespace DesktopEntry {
bool available();       // only meaningful when running from the AppImage
QString appImagePath();
bool isInstalled();
bool install(QString *error = nullptr);
bool remove(QString *error = nullptr);
// If the launcher exists but the AppImage was moved or renamed, point it at the new place.
void refreshIfInstalled();
} // namespace DesktopEntry
