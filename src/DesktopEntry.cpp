#include "DesktopEntry.h"
#include "Icons.h"
#include "Theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {
const char kMarker[] = "X-Ruche-AppImage=";
const int kIconSizes[] = {16, 22, 24, 32, 48, 64, 128, 256};

// Same drawing as Icons::Logo, as a vector file for crisp taskbar icons at any scale.
const char kSvg[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" width=\"256\" height=\"256\">\n"
    "  <polygon points=\"12,2 20.66,7 20.66,17 12,22 3.34,17 3.34,7\" fill=\"#f5a524\"/>\n"
    "  <path d=\"M5.5 12 C8 6.5 10 6.5 12 12 C14 17.5 16 17.5 18.5 12\" fill=\"none\" stroke=\"#1c1d20\" "
    "stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n"
    "</svg>\n";

QString applicationsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
}

QString entryFile() { return applicationsDir() + QLatin1String("/ruche.desktop"); }

QString iconsBase()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/icons/hicolor");
}

QString iconFile(int size)
{
    return QStringLiteral("%1/%2x%2/apps/ruche.png").arg(iconsBase()).arg(size);
}

QString svgFile() { return iconsBase() + QLatin1String("/scalable/apps/ruche.svg"); }

// The launcher runs this script rather than the AppImage itself, so swapping in a newer
// AppImage (or moving it) doesn't leave a dead menu entry or taskbar pin behind.
QString launcherFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/ruche/launch");
}

QString shellQuoted(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

QString launcherText(const QString &appImage)
{
    return QStringLiteral(
               "#!/bin/sh\n"
               "# Written by Ruche for its menu entry. Starts the AppImage below; if that file was\n"
               "# moved, renamed or replaced by a newer version, starts the newest Ruche AppImage\n"
               "# found next to it or in the usual folders. Running an AppImage once updates this file.\n"
               "want=%1\n"
               "if [ ! -f \"$want\" ]; then\n"
               "  dl=$(xdg-user-dir DOWNLOAD 2>/dev/null)\n"
               "  want=\n"
               "  for d in \"$(dirname %1)\" \"$HOME/Applications\" \"$HOME/.local/bin\" \"$HOME/bin\" \"$dl\" \"$HOME/Downloads\" \"$HOME\"; do\n"
               "    [ -n \"$d\" ] && [ -d \"$d\" ] || continue\n"
               "    want=$(find \"$d\" -maxdepth 1 -type f -iname 'ruche*.appimage' 2>/dev/null | sort -V | tail -n 1)\n"
               "    [ -n \"$want\" ] && break\n"
               "  done\n"
               "fi\n"
               "if [ -z \"$want\" ]; then\n"
               "  msg='The Ruche AppImage was not found. Start the AppImage once from where you keep it and the launcher will follow it.'\n"
               "  notify-send -a Ruche -i ruche 'Ruche' \"$msg\" 2>/dev/null || kdialog --error \"$msg\" 2>/dev/null || echo \"$msg\" >&2\n"
               "  exit 1\n"
               "fi\n"
               "[ -x \"$want\" ] || chmod +x \"$want\" 2>/dev/null\n"
               "exec \"$want\" \"$@\"\n")
        .arg(shellQuoted(appImage));
}

// Desktop Entry spec: quote the program path, escaping the reserved characters.
QString quotedExec(const QString &path)
{
    QString q = path;
    q.replace(QLatin1Char('\\'), QLatin1String("\\\\\\\\"));
    for (const char c : {'"', '`', '$'})
        q.replace(QLatin1Char(c), QStringLiteral("\\\\") + QLatin1Char(c));
    return QLatin1Char('"') + q + QLatin1Char('"');
}

QString entryText(const QString &appImage)
{
    const QString exec = quotedExec(launcherFile());
    return QStringLiteral(
               "[Desktop Entry]\n"
               "Type=Application\n"
               "Name=Ruche\n"
               "GenericName=Music Player\n"
               "Comment=Music player with a MusicBee-style layout and accurate live radio titles\n"
               "Exec=%1 %U\n"
               "TryExec=%2\n"
               "Icon=%4\n"
               "Terminal=false\n"
               "StartupNotify=true\n"
               "StartupWMClass=ruche\n"
               "Categories=AudioVideo;Audio;Player;\n"
               "MimeType=audio/mpeg;audio/flac;audio/ogg;audio/x-vorbis+ogg;audio/opus;audio/mp4;audio/aac;"
               "audio/x-wav;audio/x-mpegurl;audio/mpegurl;application/vnd.apple.mpegurl;application/x-mpegurl;"
               "audio/x-scpls;\n"
               "Keywords=music;player;radio;stream;m3u8;hls;icecast;\n"
               "Actions=PlayPause;Next;Previous;\n"
               "%3\n"
               "\n"
               "[Desktop Action PlayPause]\n"
               "Name=Play / Pause\n"
               "Exec=%1 --play-pause\n"
               "\n"
               "[Desktop Action Next]\n"
               "Name=Next track\n"
               "Exec=%1 --next\n"
               "\n"
               "[Desktop Action Previous]\n"
               "Name=Previous track\n"
               "Exec=%1 --previous\n")
        .arg(exec, launcherFile(), QLatin1String(kMarker) + appImage, svgFile());
}

QString installedAppImage()
{
    QFile f(entryFile());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith(QLatin1String(kMarker)))
            return line.mid(int(sizeof(kMarker)) - 1);
    }
    return {};
}

bool iconsPresent()
{
    if (!QFileInfo::exists(svgFile()))
        return false;
    for (const int size : kIconSizes)
        if (!QFileInfo::exists(iconFile(size)))
            return false;
    return true;
}

bool writeIcons(QString *error)
{
    QDir().mkpath(QFileInfo(svgFile()).absolutePath());
    QSaveFile svg(svgFile());
    if (!svg.open(QIODevice::WriteOnly) || svg.write(kSvg) < 0 || !svg.commit()) {
        if (error)
            *error = QObject::tr("Could not write %1").arg(svgFile());
        return false;
    }
    for (const int size : kIconSizes) {
        const QString file = iconFile(size);
        QDir().mkpath(QFileInfo(file).absolutePath());
        QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        Icons::paint(&p, QRectF(0, 0, size, size), Icons::Logo, Theme::Brand);
        p.end();
        if (!img.save(file)) {
            if (error)
                *error = QObject::tr("Could not write %1").arg(file);
            return false;
        }
    }
    return true;
}

void notifyDesktop()
{
    // Desktops watch these folders, but nudging their caches makes the launcher and icon
    // show up straight away. Each tool is optional; missing ones are simply skipped.
    const QList<QStringList> tools = {{QStringLiteral("update-desktop-database"), applicationsDir()},
                                      {QStringLiteral("gtk-update-icon-cache"), QStringLiteral("-q"), QStringLiteral("-t"), iconsBase()},
                                      {QStringLiteral("kbuildsycoca6")},
                                      {QStringLiteral("kbuildsycoca5")}};
    for (const QStringList &cmd : tools) {
        QProcess p;
        p.setProgram(cmd.first());
        p.setArguments(cmd.mid(1));
        p.setStandardOutputFile(QProcess::nullDevice());
        p.setStandardErrorFile(QProcess::nullDevice());
        p.startDetached();
    }
}

bool writeLauncher(const QString &appImage, QString *error)
{
    QDir().mkpath(QFileInfo(launcherFile()).absolutePath());
    QSaveFile f(launcherFile());
    if (!f.open(QIODevice::WriteOnly) || f.write(launcherText(appImage).toUtf8()) < 0 || !f.commit()) {
        if (error)
            *error = QObject::tr("Could not write %1").arg(launcherFile());
        return false;
    }
    QFile::setPermissions(launcherFile(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                              QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                              QFileDevice::ExeOther);
    return true;
}

bool launcherCurrent(const QString &appImage)
{
    QFile f(launcherFile());
    return f.open(QIODevice::ReadOnly) && f.readAll() == launcherText(appImage).toUtf8() &&
           (QFileInfo(launcherFile()).permissions() & QFileDevice::ExeOwner);
}

bool writeEntry(const QString &appImage, QString *error)
{
    if (!writeLauncher(appImage, error))
        return false;
    QDir().mkpath(applicationsDir());
    QSaveFile f(entryFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(entryText(appImage).toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QFile::setPermissions(entryFile(), QFile::permissions(entryFile()) | QFileDevice::ExeOwner);
    return true;
}
} // namespace

bool DesktopEntry::available() { return !appImagePath().isEmpty(); }

QString DesktopEntry::appImagePath()
{
    const QString p = qEnvironmentVariable("APPIMAGE");
    return p.isEmpty() ? QString() : QFileInfo(p).absoluteFilePath();
}

bool DesktopEntry::isInstalled() { return !installedAppImage().isEmpty(); }

bool DesktopEntry::install(QString *error)
{
    if (!available()) {
        if (error)
            *error = QObject::tr("This only works when Ruche runs from its AppImage.");
        return false;
    }
    if (!writeIcons(error) || !writeEntry(appImagePath(), error))
        return false;
    notifyDesktop();
    return true;
}

bool DesktopEntry::remove(QString *error)
{
    if (!installedAppImage().isEmpty() && !QFile::remove(entryFile())) {
        if (error)
            *error = QObject::tr("Could not remove %1").arg(entryFile());
        return false;
    }
    for (const int size : kIconSizes)
        QFile::remove(iconFile(size));
    QFile::remove(svgFile());
    QFile::remove(launcherFile());
    notifyDesktop();
    return true;
}

void DesktopEntry::refreshIfInstalled()
{
    // Keeps an existing launcher current: a moved or renamed AppImage, or a newer Ruche
    // whose launcher or icons differ from what an older version wrote.
    if (!available() || installedAppImage().isEmpty())
        return;
    QFile f(entryFile());
    const QByteArray current = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    const bool entryStale = current != entryText(appImagePath()).toUtf8() || !launcherCurrent(appImagePath());
    const bool iconsStale = !iconsPresent();
    if (!entryStale && !iconsStale)
        return;
    if (iconsStale || entryStale)
        writeIcons(nullptr);
    writeEntry(appImagePath(), nullptr);
    notifyDesktop();
}
