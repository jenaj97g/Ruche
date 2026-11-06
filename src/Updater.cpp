#include "Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#ifndef RUCHE_UPDATE_REPO
#define RUCHE_UPDATE_REPO "jenaj97g/Ruche"
#endif

namespace {
QNetworkRequest request(const QUrl &url)
{
    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " (update check)");
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}

// The release file for each kind of install.
bool matches(Updater::Kind k, const QString &name)
{
    switch (k) {
    case Updater::Kind::AppImage:
        return name.endsWith(QLatin1String("-x86_64.AppImage"), Qt::CaseInsensitive);
    case Updater::Kind::Flatpak:
        return name.endsWith(QLatin1String(".flatpak"));
    case Updater::Kind::Deb:
        return name.endsWith(QLatin1String("_amd64.deb"));
    case Updater::Kind::Rpm:
        return name.endsWith(QLatin1String(".x86_64.rpm"));
    case Updater::Kind::WindowsInstaller:
        return name.endsWith(QLatin1String("-windows-x64-setup.exe"), Qt::CaseInsensitive);
    case Updater::Kind::WindowsPortable:
        return name.endsWith(QLatin1String("-windows-x64.zip"), Qt::CaseInsensitive);
    case Updater::Kind::Other:
        return false;
    }
    return false;
}
} // namespace

Updater::Updater(QObject *parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

QString Updater::repo() { return qEnvironmentVariable("RUCHE_UPDATE_REPO", QStringLiteral(RUCHE_UPDATE_REPO)); }
QString Updater::releasesPage() { return QStringLiteral("https://github.com/%1/releases").arg(repo()); }

Updater::Kind Updater::kind()
{
    if (!qEnvironmentVariableIsEmpty("APPIMAGE"))
        return Kind::AppImage;
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID"))
        return Kind::Flatpak;
    const QString dir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    return QFileInfo::exists(dir + QStringLiteral("/Uninstall.exe")) ? Kind::WindowsInstaller : Kind::WindowsPortable;
#else
    if (dir == QLatin1String("/opt/ruche/bin")) {
        if (QFileInfo::exists(QStringLiteral("/var/lib/dpkg/info/ruche.list")))
            return Kind::Deb;
        return Kind::Rpm;
    }
    return Kind::Other;
#endif
}

QString Updater::kindName(Kind k)
{
    switch (k) {
    case Kind::AppImage:
        return QStringLiteral("AppImage");
    case Kind::Flatpak:
        return QStringLiteral("Flatpak");
    case Kind::Deb:
        return QStringLiteral(".deb");
    case Kind::Rpm:
        return QStringLiteral(".rpm");
    case Kind::WindowsInstaller:
        return tr("Windows installer");
    case Kind::WindowsPortable:
        return tr("portable zip");
    case Kind::Other:
        return tr("own build");
    }
    return QString();
}

int Updater::compareVersions(const QString &a, const QString &b)
{
    auto parse = [](QString s) {
        s = s.trimmed();
        if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
            s.remove(0, 1);
        return QVersionNumber::fromString(s);
    };
    return QVersionNumber::compare(parse(a), parse(b)) < 0 ? -1 : (parse(a) == parse(b) ? 0 : 1);
}

void Updater::check()
{
    const QString api = qEnvironmentVariable("RUCHE_UPDATE_API", QStringLiteral("https://api.github.com"));
    QNetworkRequest req = request(QUrl(QStringLiteral("%1/repos/%2/releases/latest").arg(api, repo())));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(15000);
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        Release rel;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) { // no release published yet
            Q_EMIT checked(false, rel, QString());
            return;
        }
        if (r->error() != QNetworkReply::NoError) {
            Q_EMIT checked(false, rel, r->errorString());
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        QString tag = o.value(QStringLiteral("tag_name")).toString();
        // tags like "v0.1.7", "0.1.7" or "ruche-0.1.7": keep the version part
        const qsizetype digit = tag.indexOf(QRegularExpression(QStringLiteral("\\d")));
        rel.version = digit >= 0 ? tag.mid(digit) : tag;
        rel.notes = o.value(QStringLiteral("body")).toString();
        rel.page = o.value(QStringLiteral("html_url")).toString(releasesPage());
        const Kind k = kind();
        for (const QJsonValue &v : o.value(QStringLiteral("assets")).toArray()) {
            const QJsonObject a = v.toObject();
            const QString name = a.value(QStringLiteral("name")).toString();
            if (name == QLatin1String("SHA256SUMS"))
                rel.sumsUrl = a.value(QStringLiteral("browser_download_url")).toString();
            else if (rel.assetUrl.isEmpty() && matches(k, name)) {
                rel.assetName = name;
                rel.assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
                rel.assetSize = a.value(QStringLiteral("size")).toInteger();
            }
        }
        if (QVersionNumber::fromString(rel.version).isNull()) {
            Q_EMIT checked(false, rel, tr("The newest release (%1) has no version number in its tag.").arg(tag));
            return;
        }
        Q_EMIT checked(compareVersions(rel.version, QStringLiteral(RUCHE_VERSION)) > 0, rel, QString());
    });
}

QString Updater::targetFor(const Release &r) const
{
    switch (kind()) {
    case Kind::AppImage: {
        // next to the running AppImage, if that folder is writable
        const QFileInfo cur(qEnvironmentVariable("APPIMAGE"));
        if (QFileInfo(cur.absolutePath()).isWritable())
            return cur.absolutePath() + QLatin1Char('/') + r.assetName;
        const QString apps = QDir::homePath() + QStringLiteral("/Applications");
        QDir().mkpath(apps);
        return apps + QLatin1Char('/') + r.assetName;
    }
    case Kind::Flatpak:
    case Kind::WindowsPortable: {
        QString dl = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (dl.isEmpty())
            dl = QDir::homePath();
        return dl + QLatin1Char('/') + r.assetName;
    }
    default: {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/ruche-update");
        QDir().mkpath(dir);
        return dir + QLatin1Char('/') + r.assetName;
    }
    }
}

void Updater::cancel()
{
    if (m_reply)
        m_reply->abort();
}

void Updater::download(const Release &r)
{
    if (r.assetUrl.isEmpty()) {
        Q_EMIT failed(tr("This release has no file for this kind of install (%1).").arg(kindName(kind())));
        return;
    }
    const QString target = targetFor(r);
    auto *file = new QFile(target + QStringLiteral(".part"), this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Q_EMIT failed(tr("Can't write %1").arg(QDir::toNativeSeparators(file->fileName())));
        delete file;
        return;
    }
    QNetworkRequest req = request(QUrl(r.assetUrl));
    req.setTransferTimeout(60000);
    m_reply = m_nam->get(req);
    QNetworkReply *reply = m_reply;
    connect(reply, &QNetworkReply::readyRead, file, [file, reply] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, &Updater::progress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, target, r] {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        const QString part = file->fileName();
        file->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(part);
            Q_EMIT failed(reply->error() == QNetworkReply::OperationCanceledError ? QString() : reply->errorString());
            return;
        }
        if (r.assetSize > 0 && QFileInfo(part).size() != r.assetSize) {
            QFile::remove(part);
            Q_EMIT failed(tr("The download is incomplete."));
            return;
        }
        QFile::remove(target);
        if (!QFile::rename(part, target)) {
            Q_EMIT failed(tr("Can't write %1").arg(QDir::toNativeSeparators(target)));
            return;
        }
        if (r.sumsUrl.isEmpty())
            Q_EMIT downloaded(target);
        else
            fetchSums(r, target);
    });
}

void Updater::fetchSums(const Release &r, const QString &file)
{
    QNetworkReply *reply = m_nam->get(request(QUrl(r.sumsUrl)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, r] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(file);
            Q_EMIT failed(tr("Couldn't get the checksums to verify the download: %1").arg(reply->errorString()));
            return;
        }
        QString expected;
        for (const QByteArray &line : reply->readAll().split('\n')) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            if (parts.size() == 2 && QString::fromUtf8(parts.at(1)).remove(QLatin1Char('*')) == r.assetName)
                expected = QString::fromLatin1(parts.at(0)).toLower();
        }
        QFile f(file);
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly) || !h.addData(&f)) {
            Q_EMIT failed(tr("Can't read the download."));
            return;
        }
        f.close();
        if (expected.isEmpty() || QString::fromLatin1(h.result().toHex()) != expected) {
            QFile::remove(file);
            Q_EMIT failed(expected.isEmpty() ? tr("The release's checksums don't list %1.").arg(r.assetName)
                                             : tr("The download doesn't match the release's checksum; it was deleted."));
            return;
        }
        Q_EMIT downloaded(file);
    });
}

bool Updater::startAfterExit(const QString &program, const QStringList &args)
{
    const qint64 pid = QCoreApplication::applicationPid();
#ifdef Q_OS_WIN
    // PowerShell waits for this process, then starts the program.
    auto quote = [](QString x) { return QLatin1Char('\'') + x.replace(QLatin1Char('\''), QLatin1String("''")) + QLatin1Char('\''); };
    QStringList quoted;
    for (const QString &a : args)
        quoted << quote(a);
    QString cmd = QStringLiteral("Wait-Process -Id %1 -ErrorAction SilentlyContinue; Start-Process -FilePath %2")
                      .arg(pid)
                      .arg(quote(QDir::toNativeSeparators(program)));
    if (!quoted.isEmpty())
        cmd += QStringLiteral(" -ArgumentList ") + quoted.join(QLatin1Char(','));
    return QProcess::startDetached(QStringLiteral("powershell.exe"),
                                   {QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                                    QStringLiteral("-Command"), cmd});
#else
    const QString script = QStringLiteral("while kill -0 %1 2>/dev/null; do sleep 0.2; done; exec \"$0\" \"$@\"").arg(pid);
    return QProcess::startDetached(QStringLiteral("/bin/sh"), QStringList{QStringLiteral("-c"), script, program} + args);
#endif
}

QString Updater::selfProgram()
{
    if (!qEnvironmentVariableIsEmpty("APPIMAGE"))
        return qEnvironmentVariable("APPIMAGE");
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID"))
        return QString(); // flatpak-spawn can't relaunch us from inside; the user starts it again
    if (QCoreApplication::applicationDirPath() == QLatin1String("/opt/ruche/bin"))
        return QStringLiteral("/usr/bin/ruche"); // the launcher script, with its environment
    return QCoreApplication::applicationFilePath();
}

// Runs a command (pkexec ...) while a busy dialog shows; returns its exit code (-1: didn't start).
static int runBusy(const QString &program, const QStringList &args, QWidget *parent, QString *output)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QProgressDialog dlg(QObject::tr("Installing the update…"), QString(), 0, 0, parent);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setCancelButton(nullptr);
    QEventLoop loop;
    QObject::connect(&p, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&p, &QProcess::errorOccurred, &loop, [&loop](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            loop.quit();
    });
    p.start(program, args);
    dlg.show();
    if (p.state() != QProcess::NotRunning || p.error() != QProcess::FailedToStart)
        loop.exec();
    *output = QString::fromLocal8Bit(p.readAll()).trimmed();
    if (p.error() == QProcess::FailedToStart)
        return -1;
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

bool Updater::install(const Release &r, const QString &file, QWidget *parent, QString *message)
{
    switch (kind()) {
    case Kind::AppImage: {
        QFile::setPermissions(file, QFile::permissions(file) | QFileDevice::ExeOwner | QFileDevice::ExeGroup |
                                        QFileDevice::ExeOther);
        const QString old = qEnvironmentVariable("APPIMAGE");
        if (QFileInfo(old).canonicalFilePath() != QFileInfo(file).canonicalFilePath())
            QFile::remove(old); // the running copy keeps working until it exits
        if (!startAfterExit(file, {})) {
            *message = tr("Ruche %1 is in %2. Start it from there.").arg(r.version, QDir::toNativeSeparators(file));
            return false;
        }
        return true; // caller quits
    }
    case Kind::Deb:
    case Kind::Rpm: {
        QString program = QStringLiteral("pkexec");
        QStringList args;
        if (kind() == Kind::Deb) {
            args << QStringLiteral("apt-get") << QStringLiteral("install") << QStringLiteral("-y") << file;
        } else if (!QStandardPaths::findExecutable(QStringLiteral("dnf")).isEmpty()) {
            args << QStringLiteral("dnf") << QStringLiteral("install") << QStringLiteral("-y") << QStringLiteral("--nogpgcheck") << file;
        } else if (!QStandardPaths::findExecutable(QStringLiteral("zypper")).isEmpty()) {
            args << QStringLiteral("zypper") << QStringLiteral("--non-interactive") << QStringLiteral("install")
                 << QStringLiteral("--allow-unsigned-rpm") << file;
        } else {
            args << QStringLiteral("rpm") << QStringLiteral("-U") << file;
        }
        if (QStandardPaths::findExecutable(program).isEmpty()) {
            *message = tr("Downloaded to %1. Install it with: sudo %2").arg(QDir::toNativeSeparators(file), args.join(QLatin1Char(' ')));
            return false;
        }
        QString out;
        const int rc = runBusy(program, args, parent, &out);
        if (rc != 0) {
            *message = (rc == 126 || rc == 127 ? tr("The update wasn't installed (no permission was given).")
                                               : tr("The update couldn't be installed:\n%1").arg(out.right(800))) +
                       QStringLiteral("\n\n") +
                       tr("The file is %1; it can also be installed with: sudo %2")
                           .arg(QDir::toNativeSeparators(file), args.join(QLatin1Char(' ')));
            return false;
        }
        QFile::remove(file);
        startAfterExit(QStringLiteral("/usr/bin/ruche"), {});
        return true;
    }
    case Kind::Flatpak:
        // A sandboxed app can't install Flatpaks; the software centre opens the bundle.
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
        *message = tr("Ruche %1 was downloaded to %2 and opened with your software centre. If nothing opened, "
                      "install it with: flatpak install --user \"%2\"")
                       .arg(r.version, QDir::toNativeSeparators(file));
        return false;
    case Kind::WindowsInstaller:
        // Through the shell, so Windows can ask for administrator rights; the installer
        // closes Ruche itself and offers to start the new one when it's done.
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file))) {
            *message = tr("Couldn't start the installer %1.").arg(QDir::toNativeSeparators(file));
            return false;
        }
        return true;
    case Kind::WindowsPortable:
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
        *message = tr("Ruche %1 was downloaded to %2. Close Ruche and unzip it over this copy (%3).")
                       .arg(r.version, QDir::toNativeSeparators(file), QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
        return false;
    case Kind::Other:
        break;
    }
    QDesktopServices::openUrl(QUrl(r.page));
    *message = tr("This copy of Ruche wasn't installed from a release; the release page was opened instead.");
    return false;
}
