#include "Stations.h"
#include "Library.h"
#include "Logo.h"
#include "MetaParse.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>

static QString stationsFile() { return paths::dataDir() + QLatin1String("/stations.json"); }

Stations::Stations(QObject *parent) : QObject(parent) {}

QNetworkAccessManager *Stations::nam()
{
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    return m_nam;
}

QString Stations::logoDir()
{
    const QString d = paths::dataDir() + QLatin1String("/logos");
    QDir().mkpath(d);
    return d;
}

void Stations::load()
{
    m_list.clear();
    QFile f(stationsFile());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
        for (const auto &v : arr) {
            const QJsonObject o = v.toObject();
            Station s;
            s.name = o.value(QLatin1String("name")).toString();
            s.url = o.value(QLatin1String("url")).toString();
            s.lastTitle = o.value(QLatin1String("last")).toString();
            s.logo = o.value(QLatin1String("logo")).toString();
            s.logoUrl = o.value(QLatin1String("logoUrl")).toString();
            m_list.push_back(s);
        }
    }
    // Logos that were never fetched (offline at import time) or whose file went missing.
    for (const Station &s : std::as_const(m_list))
        if (!s.logoUrl.isEmpty() && (s.logo.isEmpty() || !QFileInfo::exists(logoDir() + QLatin1Char('/') + s.logo)))
            queueLogo(s.url, QUrl(s.logoUrl), false);
    Q_EMIT changed();
}

void Stations::save() const
{
    QJsonArray arr;
    for (const Station &s : m_list) {
        QJsonObject o{{QStringLiteral("name"), s.name}, {QStringLiteral("url"), s.url}, {QStringLiteral("last"), s.lastTitle}};
        if (!s.logo.isEmpty())
            o.insert(QStringLiteral("logo"), s.logo);
        if (!s.logoUrl.isEmpty())
            o.insert(QStringLiteral("logoUrl"), s.logoUrl);
        arr.append(o);
    }
    QSaveFile f(stationsFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(arr).toJson());
        f.commit();
    }
}

int Stations::indexOfUrl(const QString &url) const
{
    for (int i = 0; i < m_list.size(); ++i)
        if (m_list[i].url == url)
            return i;
    return -1;
}

int Stations::add(const QString &name, const QString &url, const QString &logoUrl)
{
    const int existing = indexOfUrl(url);
    if (existing >= 0) {
        // Re-importing a list can bring a logo the station did not have yet.
        if (!logoUrl.isEmpty() && m_list[existing].logo.isEmpty() && m_list[existing].logoUrl.isEmpty()) {
            m_list[existing].logoUrl = logoUrl;
            save();
            queueLogo(url, QUrl(logoUrl), false);
        }
        return existing;
    }
    QString n = name.trimmed();
    if (n.isEmpty()) {
        const QUrl u(url);
        n = u.host().isEmpty() ? QFileInfo(u.path()).completeBaseName() : u.host();
    }
    Station s;
    s.name = n;
    s.url = url;
    s.logoUrl = logoUrl;
    m_list.push_back(s);
    save();
    Q_EMIT changed();
    if (!logoUrl.isEmpty())
        queueLogo(url, QUrl(logoUrl), false);
    return m_list.size() - 1;
}

void Stations::update(int i, const QString &name, const QString &url)
{
    if (i < 0 || i >= m_list.size())
        return;
    m_list[i].name = name.trimmed();
    m_list[i].url = url.trimmed();
    save();
    Q_EMIT changed();
}

void Stations::remove(int i)
{
    if (i < 0 || i >= m_list.size())
        return;
    const QString logo = m_list[i].logo;
    m_list.removeAt(i);
    dropLogoFileIfUnused(logo);
    save();
    Q_EMIT changed();
}

void Stations::setLastTitle(const QString &url, const QString &title)
{
    const int i = indexOfUrl(url);
    if (i < 0 || m_list[i].lastTitle == title)
        return;
    m_list[i].lastTitle = title;
    Q_EMIT lastTitleChanged(i);
}

// ---------------------------------------------------------------------------
// Logos

QString Stations::logoPath(int i) const
{
    if (i < 0 || i >= m_list.size() || m_list[i].logo.isEmpty())
        return {};
    const QString p = logoDir() + QLatin1Char('/') + m_list[i].logo;
    return QFileInfo::exists(p) ? p : QString();
}

QString Stations::logoPathForUrl(const QString &stationUrl) const { return logoPath(indexOfUrl(stationUrl)); }

QString Stations::storeLogo(const QImage &img)
{
    // Content-addressed PNG, so the same logo used by several stations is stored once.
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG"))
        return {};
    const QString name = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex().left(20)) +
                         QLatin1String(".png");
    QSaveFile f(logoDir() + QLatin1Char('/') + name);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(png);
    return f.commit() ? name : QString();
}

void Stations::dropLogoFileIfUnused(const QString &file)
{
    if (file.isEmpty())
        return;
    for (const Station &s : std::as_const(m_list))
        if (s.logo == file)
            return;
    const QString path = logoDir() + QLatin1Char('/') + file;
    Logo::forget(path);
    QFile::remove(path);
}

bool Stations::setLogoFromImage(int i, const QImage &img, const QString &sourceUrl)
{
    if (i < 0 || i >= m_list.size() || img.isNull())
        return false;
    const QString name = storeLogo(Logo::normalise(img));
    if (name.isEmpty())
        return false;
    const QString old = m_list[i].logo;
    m_list[i].logo = name;
    m_list[i].logoUrl = sourceUrl;
    if (old != name)
        dropLogoFileIfUnused(old);
    Logo::forget(logoDir() + QLatin1Char('/') + name);
    save();
    Q_EMIT logoChanged(i);
    return true;
}

bool Stations::setLogoFromFile(int i, const QString &file, QString *error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QImage img = Logo::decode(f.read(16 * 1024 * 1024));
    if (img.isNull()) {
        if (error)
            *error = tr("%1 is not an image Ruche can read.").arg(QFileInfo(file).fileName());
        return false;
    }
    return setLogoFromImage(i, img);
}

void Stations::setLogoFromUrl(int i, const QUrl &url)
{
    if (i < 0 || i >= m_list.size())
        return;
    if (url.isLocalFile()) {
        QString err;
        if (!setLogoFromFile(i, url.toLocalFile(), &err))
            Q_EMIT logoFailed(i, err);
        return;
    }
    queueLogo(m_list[i].url, url, true);
}

void Stations::clearLogo(int i)
{
    if (i < 0 || i >= m_list.size())
        return;
    const QString old = m_list[i].logo;
    m_list[i].logo.clear();
    m_list[i].logoUrl.clear();
    dropLogoFileIfUnused(old);
    save();
    Q_EMIT logoChanged(i);
}

void Stations::queueLogo(const QString &stationUrl, const QUrl &logoUrl, bool userRequested)
{
    if (!logoUrl.isValid() || (logoUrl.scheme() != QLatin1String("http") && logoUrl.scheme() != QLatin1String("https"))) {
        if (logoUrl.isLocalFile()) {
            const int i = indexOfUrl(stationUrl);
            if (i >= 0)
                setLogoFromFile(i, logoUrl.toLocalFile());
        }
        return;
    }
    const QString key = stationUrl + QLatin1Char('\n') + logoUrl.toString();
    if (!userRequested && m_logoTried.contains(key))
        return; // already failed once this session
    m_logoTried.insert(key);
    m_logoQueue.append({stationUrl, logoUrl, userRequested});
    pumpLogos();
}

void Stations::pumpLogos()
{
    while (m_logoActive < 4 && !m_logoQueue.isEmpty()) {
        const PendingLogo job = m_logoQueue.takeFirst();
        ++m_logoActive;
        QNetworkRequest req(job.logoUrl);
        req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("Ruche/" RUCHE_VERSION));
        req.setTransferTimeout(20000);
        QNetworkReply *reply = nam()->get(req);
        connect(reply, &QNetworkReply::readyRead, reply, [reply] {
            if (reply->bytesAvailable() > 8 * 1024 * 1024)
                reply->abort(); // not a logo
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, job] {
            reply->deleteLater();
            --m_logoActive;
            const int row = indexOfUrl(job.stationUrl);
            QString error;
            if (reply->error() != QNetworkReply::NoError) {
                error = reply->errorString();
            } else {
                const QImage img = Logo::decode(reply->readAll());
                if (img.isNull())
                    error = tr("The address did not return an image Ruche can read.");
                else if (row >= 0 && !setLogoFromImage(row, img, job.logoUrl.toString()))
                    error = tr("Could not save the logo.");
            }
            if (!error.isEmpty() && row >= 0 && job.userRequested)
                Q_EMIT logoFailed(row, error);
            pumpLogos();
        });
    }
}

// ---------------------------------------------------------------------------
// Import / export

int Stations::importText(const QString &text, const QUrl &source)
{
    if (meta::looksLikeHls(text)) {
        add(QString(), source.toString());
        return 1;
    }
    const bool pls = text.trimmed().startsWith(QLatin1String("[playlist]"), Qt::CaseInsensitive);
    const auto entries = pls ? meta::parsePls(text, source) : meta::parseM3u(text, source);
    int n = 0;
    for (const auto &e : entries) {
        if (e.url.isLocalFile())
            continue; // local files belong in a music playlist, not the radio list
        const int before = m_list.size();
        QString logo = e.logo.trimmed();
        if (!logo.isEmpty() && !logo.contains(QLatin1String("://")) && source.isLocalFile())
            logo = QUrl::fromLocalFile(QDir(QFileInfo(source.toLocalFile()).absolutePath()).absoluteFilePath(logo)).toString();
        else if (!logo.isEmpty() && !logo.contains(QLatin1String("://")))
            logo = source.resolved(QUrl(logo)).toString();
        add(e.title, e.url.toString(), logo);
        if (m_list.size() > before)
            ++n;
    }
    return n;
}

int Stations::importFile(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    return importText(meta::decodeLoose(f.read(8 * 1024 * 1024)), QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()));
}

void Stations::importUrl(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("Ruche/" RUCHE_VERSION));
    req.setTransferTimeout(15000);
    QNetworkReply *reply = nam()->get(req);
    // A live audio stream never finishes downloading: stop as soon as the headers say so.
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] {
        const QByteArray ct = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().toLower();
        if ((ct.startsWith("audio/") || ct.startsWith("application/ogg")) && !ct.contains("mpegurl") && !ct.contains("scpls"))
            reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > 2 * 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        const QByteArray ctype = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (reply->error() != QNetworkReply::NoError) {
            // An audio stream itself: just add it as a station.
            if (ctype.startsWith("audio/") || ctype.startsWith("application/ogg") ||
                reply->error() == QNetworkReply::OperationCanceledError) {
                add(QString::fromUtf8(reply->rawHeader("icy-name")), url.toString());
                Q_EMIT imported(1, QString());
            } else {
                Q_EMIT imported(0, reply->errorString());
            }
            return;
        }
        const QByteArray body = reply->readAll();
        if (ctype.startsWith("audio/") && !ctype.contains("mpegurl") && !ctype.contains("scpls")) {
            add(QString::fromUtf8(reply->rawHeader("icy-name")), url.toString());
            Q_EMIT imported(1, QString());
            return;
        }
        const int n = importText(meta::decodeLoose(body), reply->url());
        Q_EMIT imported(n, n ? QString() : tr("No stations found at that address."));
    });
}

bool Stations::exportM3u(const QString &file, QString *error) const
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QByteArray out = "#EXTM3U\n";
    for (int i = 0; i < m_list.size(); ++i) {
        const Station &s = m_list[i];
        QString logo = s.logoUrl;
        if (logo.isEmpty() || logo.startsWith(QLatin1String("file:")))
            logo = logoPath(i).isEmpty() ? QString() : QUrl::fromLocalFile(logoPath(i)).toString();
        QString name = s.name;
        name.replace(QLatin1Char('\n'), QLatin1Char(' '));
        out += "#EXTINF:-1";
        if (!logo.isEmpty())
            out += " tvg-logo=\"" + logo.toUtf8().replace('"', "%22") + '"';
        out += ',' + name.toUtf8() + '\n' + s.url.toUtf8() + '\n';
    }
    f.write(out);
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}
