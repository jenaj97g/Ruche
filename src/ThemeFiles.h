// Theme files: Qt stylesheets (.qss) layered on top of Ruche's own, so a theme can
// restyle anything. A comment block at the top names the theme and may set the three
// base colours every other shade is mixed from; the rules below can use the same
// @tokens as the built-in stylesheet (@accent, @window, @dim…). See THEMES.md.
#pragma once

#include <QColor>
#include <QString>
#include <QVector>

namespace Theme {

struct File {
    QString id;   // "user/Neon.qss" or "bundled/glass/theme.qss": stable across runs
    QString path; // absolute; empty when the file is gone
    QString name, author, version, description;
    QColor window, text, accent; // invalid where the theme leaves the colour alone
    bool bundled = false;
};

QString userThemesDir();    // where imported themes go (created on demand)
QString bundledThemesDir(); // examples shipped with Ruche
QVector<File> files();      // every theme file, bundled ones first
File fileById(const QString &id);
File parseFile(const QString &path);

// The rules of a theme file, with relative url(...) made absolute so images next to
// the theme are found.
QString fileStylesheet(const File &f);

// Copies a .qss file, a theme folder or a downloaded .zip into userThemesDir().
// Returns the new theme's id, or empty with *error set.
QString importTheme(const QString &source, QString *error);

} // namespace Theme
