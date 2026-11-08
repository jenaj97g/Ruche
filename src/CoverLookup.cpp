#include "CoverLookup.h"
#include "Library.h" // paths

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

CoverLookup::CoverLookup(QObject *parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}


namespace {
// For comparing names: case, accents, punctuation and edition notes don't matter.
QString norm(QString s)
{
    static const QRegularExpression edition(QStringLiteral("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]"));
    s.remove(edition); // "(Deluxe Edition)", "[Remastered]"
    s = s.normalized(QString::NormalizationForm_KD);
    QString out;
    for (const QChar c : std::as_const(s))
        if (c.isLetterOrNumber())
            out += c.toCaseFolded();
    return out;
}

bool sameName(const QString &a, const QString &b)
{
    const QString x = norm(a), y = norm(b);
    if (x.isEmpty() || y.isEmpty())
        return false;
    return x == y || (x.size() >= 4 && y.startsWith(x)) || (y.size() >= 4 && x.startsWith(y));
}

// (named for Discord, which used it first; kept so earlier finds aren't looked up again)
QString coverCacheFile() { return paths::dataDir() + QStringLiteral("/discord-covers.json"); }
QString imageCacheDir() { return paths::dataDir() + QStringLiteral("/covers"); }
} // namespace

void CoverLookup::loadCache()
{
    m_coverCacheLoaded = true;
    QFile f(coverCacheFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it)
        m_covers.insert(it.key(), it.value().toString());
}

void CoverLookup::saveCache()
{
    // Keep it bounded: an old entry is cheap to look up again.
    while (m_covers.size() > 3000)
        m_covers.erase(m_covers.begin());
    QJsonObject o;
    for (auto it = m_covers.cbegin(); it != m_covers.cend(); ++it)
        o.insert(it.key(), it.value());
    QSaveFile f(coverCacheFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

QString CoverLookup::url(const QString &artist, const QString &album, const QString &title)
{
    if (artist.isEmpty() || (album.isEmpty() && title.isEmpty()))
        return QString();
    if (!m_coverCacheLoaded)
        loadCache();
    // Tracks without an album are looked up as songs.
    const QString key = norm(artist) + QLatin1Char('\n') + (album.isEmpty() ? QStringLiteral("song:") + norm(title) : norm(album));
    const auto it = m_covers.constFind(key);
    if (it != m_covers.constEnd())
        return *it;
    if (!m_coverPending.contains(key))
        lookUp(key, artist, album, title);
    return QString(); // Ruche's logo until the cover is found
}

void CoverLookup::coverFound(const QString &key, const QString &url)
{
    m_coverPending.remove(key);
    m_covers.insert(key, url);
    saveCache();
    if (!url.isEmpty())
        Q_EMIT found(key, url);
}

void CoverLookup::lookUp(const QString &key, const QString &artist, const QString &album, const QString &title)
{
    m_coverPending.insert(key);
    const bool song = album.isEmpty();
    QUrl url(qEnvironmentVariable("RUCHE_ITUNES_SEARCH", QStringLiteral("https://itunes.apple.com/search")));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("term"), artist + QLatin1Char(' ') + (song ? title : album));
    q.addQueryItem(QStringLiteral("media"), QStringLiteral("music"));
    q.addQueryItem(QStringLiteral("entity"), song ? QStringLiteral("song") : QStringLiteral("album"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("15"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setTransferTimeout(10000);
    req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://github.com/jenaj97g/Ruche )");
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [=] {
        r->deleteLater();
        const QJsonArray results = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("results")).toArray();
        for (const QJsonValue &v : results) {
            const QJsonObject o = v.toObject();
            if (!sameName(o.value(QStringLiteral("artistName")).toString(), artist))
                continue;
            const QString name = o.value(song ? QStringLiteral("trackName") : QStringLiteral("collectionName")).toString();
            if (!sameName(name, song ? title : album))
                continue;
            QString art = o.value(QStringLiteral("artworkUrl100")).toString();
            if (art.isEmpty())
                continue;
            art.replace(QStringLiteral("100x100bb"), QStringLiteral("512x512bb"));
            coverFound(key, art);
            return;
        }
        if (r->error() != QNetworkReply::NoError && results.isEmpty() && r->error() != QNetworkReply::ContentNotFoundError) {
            m_coverPending.remove(key); // offline: try again with the next song, don't remember "none"
            return;
        }
        if (song)
            coverFound(key, QString());
        else
            lookUpMusicBrainz(key, artist, album);
    });
}

void CoverLookup::lookUpMusicBrainz(const QString &key, const QString &artist, const QString &album)
{
    auto quoted = [](QString s) {
        s.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('"'), QLatin1String("\\\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    };
    QUrl url(qEnvironmentVariable("RUCHE_MUSICBRAINZ", QStringLiteral("https://musicbrainz.org")) + QStringLiteral("/ws/2/release-group/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("query"),
                   QStringLiteral("releasegroup:%1 AND artist:%2").arg(quoted(album), quoted(artist)));
    q.addQueryItem(QStringLiteral("fmt"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("5"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setTransferTimeout(10000);
    // MusicBrainz asks every client to say who it is.
    req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://github.com/jenaj97g/Ruche )");
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [=] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            m_coverPending.remove(key);
            return;
        }
        const QJsonArray groups = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("release-groups")).toArray();
        for (const QJsonValue &v : groups) {
            const QJsonObject g = v.toObject();
            if (g.value(QStringLiteral("score")).toInt() < 90 || !sameName(g.value(QStringLiteral("title")).toString(), album))
                continue;
            const QString id = g.value(QStringLiteral("id")).toString();
            if (id.isEmpty())
                continue;
            // Only if Cover Art Archive really has a front cover.
            const QString art = qEnvironmentVariable("RUCHE_COVERARTARCHIVE", QStringLiteral("https://coverartarchive.org")) +
                                QStringLiteral("/release-group/%1/front-500").arg(id);
            QNetworkRequest head{QUrl(art)};
            head.setTransferTimeout(10000);
            head.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://github.com/jenaj97g/Ruche )");
            head.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
            QNetworkReply *h = m_nam->head(head);
            connect(h, &QNetworkReply::finished, this, [=] {
                h->deleteLater();
                const int code = h->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                coverFound(key, code >= 200 && code < 400 ? art : QString());
            });
            return;
        }
        coverFound(key, QString());
    });
}

// ---------------------------------------------------------------- the pictures themselves

void CoverLookup::fetchImage(const QString &url)
{
    if (url.isEmpty())
        return;
    const QString file = imageCacheDir() + QLatin1Char('/') +
                         QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex()) +
                         QStringLiteral(".img");
    QImage cached(file);
    if (!cached.isNull()) {
        Q_EMIT imageReady(url, cached);
        return;
    }
    if (m_imagePending.contains(url))
        return;
    m_imagePending.insert(url);
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(15000);
    req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://github.com/jenaj97g/Ruche )");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [=] {
        r->deleteLater();
        m_imagePending.remove(url);
        const QByteArray data = r->readAll();
        QImage img;
        if (r->error() == QNetworkReply::NoError && data.size() < 8 * 1024 * 1024)
            img.loadFromData(data);
        if (img.isNull())
            return;
        if (img.width() > 600 || img.height() > 600)
            img = img.scaled(600, 600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QDir().mkpath(imageCacheDir());
        img.save(file, "JPG", 90);
        pruneImages();
        Q_EMIT imageReady(url, img);
    });
}

// Pictures of songs heard on the radio add up; keep the 300 most recent.
void CoverLookup::pruneImages()
{
    const QFileInfoList files = QDir(imageCacheDir()).entryInfoList({QStringLiteral("*.img")}, QDir::Files, QDir::Time);
    for (int i = 300; i < files.size(); ++i)
        QFile::remove(files[i].absoluteFilePath());
}
