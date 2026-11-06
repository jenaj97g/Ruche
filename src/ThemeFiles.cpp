#include "ThemeFiles.h"
#include "Library.h" // paths::dataDir

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h> // Qt 6.7+
#else
#include <QtGui/private/qzipreader_p.h>
#endif

namespace Theme {

QString userThemesDir() { return paths::dataDir() + QStringLiteral("/themes"); }

QString bundledThemesDir()
{
    const QString app = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    return app + QStringLiteral("/themes");
#else
    return QDir::cleanPath(app + QStringLiteral("/../share/ruche/themes"));
#endif
}

File parseFile(const QString &path)
{
    File f;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return f;
    f.path = path;
    const QFileInfo fi(path);
    // theme.qss in a folder is named after the folder
    f.name = fi.fileName().compare(QLatin1String("theme.qss"), Qt::CaseInsensitive) == 0 ? fi.dir().dirName()
                                                                                        : fi.completeBaseName();
    // The header: the first /* ... */ comment, "@key value" lines
    const QString head = QString::fromUtf8(in.read(16 * 1024));
    const qsizetype open = head.indexOf(QLatin1String("/*"));
    const qsizetype close = open >= 0 ? head.indexOf(QLatin1String("*/"), open + 2) : -1;
    if (open < 0 || close < 0 || !head.left(open).trimmed().isEmpty())
        return f;
    static const QRegularExpression line(QStringLiteral(R"(^\s*\*?\s*@([A-Za-z]+)\s+(.+?)\s*$)"));
    const QStringList lines = head.mid(open + 2, close - open - 2).split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        const QRegularExpressionMatch m = line.match(l);
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1).toLower();
        const QString value = m.captured(2);
        if (key == QLatin1String("name"))
            f.name = value;
        else if (key == QLatin1String("author"))
            f.author = value;
        else if (key == QLatin1String("version"))
            f.version = value;
        else if (key == QLatin1String("description"))
            f.description = value;
        else if (key == QLatin1String("window") || key == QLatin1String("background"))
            f.window = QColor(value);
        else if (key == QLatin1String("text"))
            f.text = QColor(value);
        else if (key == QLatin1String("accent"))
            f.accent = QColor(value);
    }
    // a theme that sets one base colour but not the other can't be mixed from
    if (f.window.isValid() != f.text.isValid())
        f.window = f.text = QColor();
    return f;
}

static void scan(const QString &dir, const QString &prefix, bool bundled, QVector<File> &out)
{
    QStringList found;
    const QString root = QFileInfo(dir).absoluteFilePath();
    QDirIterator it(root, {QStringLiteral("*.qss")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        found << QDir(root).relativeFilePath(it.fileInfo().absoluteFilePath());
    }
    std::sort(found.begin(), found.end(), [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    for (const QString &rel : std::as_const(found)) {
        File f = parseFile(dir + QLatin1Char('/') + rel);
        if (f.path.isEmpty())
            continue;
        f.id = prefix + rel;
        f.bundled = bundled;
        out << f;
    }
}

QVector<File> files()
{
    QVector<File> out;
    scan(bundledThemesDir(), QStringLiteral("bundled/"), true, out);
    scan(userThemesDir(), QStringLiteral("user/"), false, out);
    return out;
}

File fileById(const QString &id)
{
    QString path;
    bool bundled = false;
    if (id.startsWith(QLatin1String("user/")))
        path = userThemesDir() + id.mid(4);
    else if (id.startsWith(QLatin1String("bundled/"))) {
        path = bundledThemesDir() + id.mid(7);
        bundled = true;
    }
    if (path.isEmpty() || id.contains(QLatin1String("..")) || !QFileInfo(path).isFile())
        return File();
    File f = parseFile(path);
    f.id = id;
    f.bundled = bundled;
    return f;
}

QString fileStylesheet(const File &f)
{
    QFile in(f.path);
    if (f.path.isEmpty() || !in.open(QIODevice::ReadOnly))
        return QString();
    QString css = QString::fromUtf8(in.readAll());
    // url(img.png) → url("/full/path/img.png"): Qt resolves relative URLs against the
    // working directory, not the stylesheet.
    const QDir dir = QFileInfo(f.path).dir();
    static const QRegularExpression url(QStringLiteral(R"re(url\(\s*(['"]?)([^'")]+)\1\s*\))re"));
    QString out;
    qsizetype last = 0;
    for (auto it = url.globalMatch(css); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const QString target = m.captured(2).trimmed();
        const bool absolute = target.startsWith(QLatin1Char('/')) || target.startsWith(QLatin1Char(':')) ||
                              target.contains(QLatin1String("://")) || QDir::isAbsolutePath(target);
        out += QStringView(css).mid(last, m.capturedStart() - last);
        out += absolute ? m.captured(0)
                        : QStringLiteral("url(\"%1\")").arg(QDir::cleanPath(dir.absoluteFilePath(target)));
        last = m.capturedEnd();
    }
    out += QStringView(css).mid(last);
    return out;
}

// A name that is safe as a file or folder name, and not taken unless `replace`.
static QString safeName(QString name)
{
    static const QRegularExpression bad(QStringLiteral(R"([/\\:*?"<>|\x00-\x1f]|^\.+)"));
    name.replace(bad, QStringLiteral("_"));
    name = name.trimmed();
    return name.isEmpty() ? QStringLiteral("theme") : name;
}

static bool copyDir(const QString &fromPath, const QString &to)
{
    QDir().mkpath(to);
    const QString from = QFileInfo(fromPath).absoluteFilePath();
    QDirIterator it(from, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(from).relativeFilePath(it.fileInfo().absoluteFilePath());
        const QString dst = to + QLatin1Char('/') + rel;
        if (it.fileInfo().isDir()) {
            QDir().mkpath(dst);
        } else {
            QFile::remove(dst);
            if (!QFile::copy(it.filePath(), dst))
                return false;
        }
    }
    return true;
}

// The theme in a folder: theme.qss, else the only/first .qss
static QString mainQss(const QString &dir)
{
    if (QFileInfo::exists(dir + QStringLiteral("/theme.qss")))
        return QStringLiteral("theme.qss");
    const QStringList q = QDir(dir).entryList({QStringLiteral("*.qss")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    return q.value(0);
}

QString importTheme(const QString &source, QString *error)
{
    const QFileInfo fi(source);
    const QString base = userThemesDir();
    QDir().mkpath(base);
    auto fail = [error](const QString &e) {
        if (error)
            *error = e;
        return QString();
    };

    if (fi.isDir()) {
        if (mainQss(source).isEmpty())
            return fail(QCoreApplication::translate("Theme", "There's no .qss file in that folder."));
        const QString name = safeName(fi.fileName());
        if (!copyDir(source, base + QLatin1Char('/') + name))
            return fail(QCoreApplication::translate("Theme", "Couldn't copy the theme."));
        return QStringLiteral("user/") + name + QLatin1Char('/') + mainQss(base + QLatin1Char('/') + name);
    }

    if (fi.suffix().compare(QLatin1String("qss"), Qt::CaseInsensitive) == 0) {
        const QString name = safeName(fi.fileName());
        const QString dst = base + QLatin1Char('/') + name;
        if (QFileInfo(dst).canonicalFilePath() != fi.canonicalFilePath()) {
            QFile::remove(dst); // importing again updates it
            if (!QFile::copy(source, dst))
                return fail(QCoreApplication::translate("Theme", "Couldn't copy the theme."));
        }
        return QStringLiteral("user/") + name;
    }

    if (fi.suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0) {
        QZipReader zip(source);
        if (!zip.isReadable() || zip.status() != QZipReader::NoError)
            return fail(QCoreApplication::translate("Theme", "That zip file can't be read."));
        const auto entries = zip.fileInfoList();
        // Zips usually hold one top folder; drop it so the theme isn't a folder in a folder.
        QString top;
        bool oneTop = true;
        qint64 total = 0;
        bool hasQss = false;
        for (const auto &e : entries) {
            const QString p = QDir::cleanPath(e.filePath);
            if (p.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(p) || p.contains(QLatin1String("/../")))
                return fail(QCoreApplication::translate("Theme", "That zip file has unsafe paths in it."));
            total += e.size;
            hasQss = hasQss || (e.isFile && p.endsWith(QLatin1String(".qss"), Qt::CaseInsensitive));
            const QString first = p.section(QLatin1Char('/'), 0, 0);
            if (!p.contains(QLatin1Char('/')) && e.isFile)
                oneTop = false;
            else if (top.isEmpty())
                top = first;
            else if (top != first)
                oneTop = false;
        }
        if (!hasQss)
            return fail(QCoreApplication::translate("Theme", "There's no .qss file in that zip."));
        if (total > 64 * 1024 * 1024)
            return fail(QCoreApplication::translate("Theme", "That zip is too big for a theme."));
        const QString name = safeName(oneTop && !top.isEmpty() ? top : fi.completeBaseName());
        const QString dst = base + QLatin1Char('/') + name;
        QDir(dst).removeRecursively(); // importing again replaces the old copy
        for (const auto &e : entries) {
            QString p = QDir::cleanPath(e.filePath);
            if (oneTop && !top.isEmpty())
                p = p.mid(top.size() + 1);
            if (p.isEmpty())
                continue;
            const QString out = QDir::cleanPath(dst + QLatin1Char('/') + p);
            if (!out.startsWith(dst + QLatin1Char('/')))
                return fail(QCoreApplication::translate("Theme", "That zip file has unsafe paths in it."));
            if (e.isDir) {
                QDir().mkpath(out);
            } else if (e.isFile) {
                QDir().mkpath(QFileInfo(out).absolutePath());
                QFile f(out);
                if (!f.open(QIODevice::WriteOnly) || f.write(zip.fileData(e.filePath)) < 0)
                    return fail(QCoreApplication::translate("Theme", "Couldn't unpack the theme."));
            }
        }
        const QString qss = mainQss(dst);
        if (qss.isEmpty()) {
            // the .qss sits deeper; take the first one found
            QDirIterator it(dst, {QStringLiteral("*.qss")}, QDir::Files, QDirIterator::Subdirectories);
            if (it.hasNext())
                return QStringLiteral("user/") + QDir(base).relativeFilePath(it.next());
            return fail(QCoreApplication::translate("Theme", "There's no .qss file in that zip."));
        }
        return QStringLiteral("user/") + name + QLatin1Char('/') + qss;
    }
    return fail(QCoreApplication::translate("Theme", "Pick a .qss file, a theme folder or a .zip."));
}

} // namespace Theme
