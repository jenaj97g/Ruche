// Colours. A theme is three colours (background, text, accent); every other shade in the
// interface is mixed from them, so presets, light themes and custom palettes all share one
// stylesheet.
#pragma once

#include <QColor>
#include <QString>
#include <QVector>

class QApplication;

namespace Theme {
// The honeycomb logo keeps its colour whatever the theme (app icon, tray, launcher).
inline const QColor Brand(0xf5, 0xa5, 0x24);

struct Preset {
    QString id;
    QString name;
    QColor window, text, accent;
};
// "dark" first (the default), then "light", then the other palettes.
const QVector<Preset> &presets();

// What the user picked, as stored in the settings file.
struct Choice {
    QString theme = QStringLiteral("dark"); // a preset id, "system" or "custom"
    QColor accent;                          // invalid: the theme's own accent
    QColor customWindow, customText;        // for "custom"
};
Choice loadChoice();
void saveChoice(const Choice &c);
// Whether the desktop is in dark mode (portal on Linux, registry on Windows); true if unknown.
bool systemPrefersDark();

// Applies a choice to the whole application: palette, stylesheet, icon colours.
void apply(QApplication &app, const Choice &c);
// Bumped by every apply(); caches of themed pixmaps compare against it.
int generation();

// Current colours, for code that paints by hand.
bool isDark();
QColor window();
QColor text();
QColor accent();     // fills: play button, sliders, progress
QColor accentText(); // the accent adjusted to stay readable as text on the background
QColor onAccent();   // glyphs drawn on an accent fill
QColor dim();        // secondary text
QColor faint();      // hints, placeholders
QColor surface();    // raised tiles (placeholders, logo backdrops)
QColor mixed(double towardText); // background → text, 0..1
} // namespace Theme
