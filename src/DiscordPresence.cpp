#include "DiscordPresence.h"

#include "Library.h"
#include "NowPlaying.h"
#include "PlaybackController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>
#include <QUuid>
#include <QtEndian>
#include <algorithm>

#ifndef RUCHE_DISCORD_CLIENT_ID
#define RUCHE_DISCORD_CLIENT_ID ""
#endif

namespace {
enum Op : quint32 { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

// Discord wants 2 to 128 characters in its text fields.
QString field(QString s)
{
    s = s.simplified();
    if (s.size() > 128)
        s = s.left(127) + QChar(0x2026);
    while (!s.isEmpty() && s.size() < 2)
        s += QChar(0x200b); // zero-width space
    return s;
}
} // namespace

DiscordPresence::DiscordPresence(PlaybackController *pc, Library *lib, Stations *stations, QObject *parent)
    : QObject(parent), m_pc(pc), m_lib(lib), m_stations(stations), m_clientId(builtInClientId())
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &DiscordPresence::restart);
    m_throttle.setSingleShot(true);
    connect(&m_throttle, &QTimer::timeout, this, &DiscordPresence::pushActivity);

    connect(pc, &PlaybackController::trackChanged, this, &DiscordPresence::scheduleUpdate);
    connect(pc, &PlaybackController::stateChanged, this, &DiscordPresence::scheduleUpdate);
    connect(pc, &PlaybackController::stationChanged, this, [this] {
        m_stationSince = QDateTime::currentMSecsSinceEpoch();
        scheduleUpdate();
    });
    connect(pc, &PlaybackController::streamTitleChanged, this, [this] {
        m_titleSince = QDateTime::currentMSecsSinceEpoch();
        scheduleUpdate();
    });
    connect(pc, &PlaybackController::positionChanged, this, [this](double pos, double) {
        // A seek moves the progress bar's start; ordinary playback doesn't.
        if (!m_ready || m_sentStart == 0 || m_pc->mode() != PlaybackController::Mode::Library || m_pc->isPaused())
            return;
        const qint64 start = QDateTime::currentMSecsSinceEpoch() - qint64(pos * 1000);
        if (qAbs(start - m_sentStart) > 3000)
            scheduleUpdate();
    });
}

DiscordPresence::~DiscordPresence()
{
    if (m_sock && m_ready) {
        send(Close, QJsonObject());
        m_sock->flush();
    }
}

QString DiscordPresence::builtInClientId() { return QStringLiteral(RUCHE_DISCORD_CLIENT_ID); }

QStringList DiscordPresence::socketCandidates()
{
    QStringList out;
#ifdef Q_OS_WIN
    for (int i = 0; i < 10; ++i)
        out << QStringLiteral("discord-ipc-%1").arg(i); // \\.\pipe\discord-ipc-N
#else
    QStringList bases;
    for (const char *var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const QString v = qEnvironmentVariable(var);
        if (!v.isEmpty() && !bases.contains(v))
            bases << v;
    }
    if (!bases.contains(QLatin1String("/tmp")))
        bases << QStringLiteral("/tmp");
    // Native clients put the socket straight in the runtime directory; Flatpak and Snap
    // builds put it in their own corner of it (Vesktop's Flatpak under .flatpak/).
    const QStringList subs = {
        QString(),
        QStringLiteral("app/com.discordapp.Discord"),
        QStringLiteral("app/com.discordapp.DiscordCanary"),
        QStringLiteral(".flatpak/dev.vencord.Vesktop/xdg-run"),
        QStringLiteral(".flatpak/com.discordapp.Discord/xdg-run"),
        QStringLiteral("app/dev.vencord.Vesktop"),
        QStringLiteral("snap.discord"),
        QStringLiteral("snap.discord-canary"),
    };
    for (const QString &b : bases)
        for (const QString &s : subs)
            for (int i = 0; i < 10; ++i) {
                const QString p = QDir::cleanPath(b + QLatin1Char('/') + s) + QStringLiteral("/discord-ipc-%1").arg(i);
                if (!out.contains(p) && QFileInfo::exists(p))
                    out << p;
            }
#endif
    return out;
}

void DiscordPresence::setEnabled(bool on)
{
    if (on == m_enabled)
        return;
    m_enabled = on;
    restart();
}

void DiscordPresence::setClientId(const QString &id)
{
    const QString t = id.trimmed();
    if (t == m_clientId)
        return;
    m_clientId = t;
    if (m_enabled)
        restart();
}

void DiscordPresence::setShowRadio(bool on)
{
    m_showRadio = on;
    scheduleUpdate();
}

void DiscordPresence::setStatus(Status s)
{
    if (s == m_status)
        return;
    m_status = s;
    Q_EMIT statusChanged();
}

QString DiscordPresence::statusText() const
{
    switch (m_status) {
    case Status::Off:
        return tr("Off");
    case Status::NoClientId:
        return tr("Needs a Discord application ID");
    case Status::Searching:
        return tr("Looking for Discord…");
    case Status::NotRunning:
        return tr("Discord or Vesktop isn't running (or its Rich Presence is off); trying again every 15 s");
    case Status::Connected:
        return m_user.isEmpty() ? tr("Connected to Discord") : tr("Connected to Discord as %1").arg(m_user);
    }
    return QString();
}

void DiscordPresence::restart()
{
    m_retry.stop();
    m_throttle.stop();
    if (m_sock) {
        if (m_ready)
            send(Close, QJsonObject());
        m_sock->disconnect(this);
        m_sock->abort();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
    m_ready = false;
    m_lastSent.clear();
    m_sentStart = 0;
    m_buf.clear();
    if (!m_enabled) {
        setStatus(Status::Off);
        return;
    }
    if (m_clientId.isEmpty() || !std::all_of(m_clientId.begin(), m_clientId.end(), [](QChar c) { return c.isDigit(); })) {
        setStatus(Status::NoClientId);
        return;
    }
    setStatus(Status::Searching);
    m_candidates = socketCandidates();
    m_candidate = 0;
    connectNext();
}

void DiscordPresence::connectNext()
{
    if (m_candidate >= m_candidates.size()) {
        setStatus(Status::NotRunning);
        m_retry.start(15000);
        return;
    }
    if (!m_sock) {
        m_sock = new QLocalSocket(this);
        connect(m_sock, &QLocalSocket::connected, this, &DiscordPresence::onConnected);
        connect(m_sock, &QLocalSocket::readyRead, this, &DiscordPresence::onReadable);
        connect(m_sock, &QLocalSocket::disconnected, this, &DiscordPresence::onDisconnected);
        connect(m_sock, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            if (m_ready || m_sock->state() == QLocalSocket::ConnectedState)
                return; // handled by disconnected()
            ++m_candidate;
            m_sock->abort();
            QTimer::singleShot(0, this, &DiscordPresence::connectNext);
        });
    }
    m_sock->connectToServer(m_candidates.at(m_candidate));
}

void DiscordPresence::onConnected()
{
    QJsonObject hello;
    hello.insert(QStringLiteral("v"), 1);
    hello.insert(QStringLiteral("client_id"), m_clientId);
    send(Handshake, hello);
}

void DiscordPresence::onDisconnected()
{
    const bool wasReady = m_ready;
    m_ready = false;
    if (!m_enabled)
        return;
    if (!wasReady && m_status == Status::Searching) {
        // The socket existed but the client hung up on the handshake (a stale socket, or a
        // wrong application ID); try the next one.
        ++m_candidate;
        QTimer::singleShot(0, this, &DiscordPresence::connectNext);
        return;
    }
    setStatus(Status::NotRunning);
    m_retry.start(5000);
}

void DiscordPresence::send(quint32 op, const QJsonObject &payload)
{
    if (!m_sock || m_sock->state() != QLocalSocket::ConnectedState)
        return;
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray frame(8, Qt::Uninitialized);
    qToLittleEndian<quint32>(op, frame.data());
    qToLittleEndian<quint32>(quint32(json.size()), frame.data() + 4);
    m_sock->write(frame + json);
}

void DiscordPresence::onReadable()
{
    m_buf += m_sock->readAll();
    while (m_buf.size() >= 8) {
        const quint32 op = qFromLittleEndian<quint32>(m_buf.constData());
        const quint32 len = qFromLittleEndian<quint32>(m_buf.constData() + 4);
        if (len > 1 << 20) { // nonsense; start over
            restart();
            return;
        }
        if (quint32(m_buf.size()) < 8 + len)
            return;
        const QByteArray body = m_buf.mid(8, len);
        m_buf.remove(0, 8 + len);
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        if (op == Ping) {
            send(Pong, o);
        } else if (op == Close) {
            qInfo("Discord closed the connection: %s", qPrintable(o.value(QStringLiteral("message")).toString()));
            m_sock->disconnectFromServer();
        } else if (op == Frame) {
            if (o.value(QStringLiteral("evt")).toString() == QLatin1String("READY")) {
                m_ready = true;
                const QJsonObject user = o.value(QStringLiteral("data")).toObject().value(QStringLiteral("user")).toObject();
                m_user = user.value(QStringLiteral("global_name")).toString();
                if (m_user.isEmpty())
                    m_user = user.value(QStringLiteral("username")).toString();
                setStatus(Status::Connected);
                m_lastSent.clear();
                pushActivity();
            } else if (o.value(QStringLiteral("evt")).toString() == QLatin1String("ERROR")) {
                qWarning("Discord presence: %s",
                         qPrintable(o.value(QStringLiteral("data")).toObject().value(QStringLiteral("message")).toString()));
            }
        }
    }
}

void DiscordPresence::scheduleUpdate()
{
    if (!m_ready || m_throttle.isActive())
        return;
    // At most one update every 4 s; changes in between are folded into the next one.
    const qint64 wait = m_sinceSend.isValid() ? qMax<qint64>(0, 4000 - m_sinceSend.elapsed()) : 0;
    m_throttle.start(int(qMax<qint64>(wait, 250)));
}

QJsonObject DiscordPresence::buildActivity()
{
    if (m_pc->isPaused() || !m_pc->isActive())
        return QJsonObject(); // nothing while paused or stopped
    const QVariantMap np = nowplaying::snapshot(m_pc, m_lib, m_stations);
    const QString source = np.value(QStringLiteral("source")).toString();
    if (source == QLatin1String("radio") && !m_showRadio)
        return QJsonObject();
    const QString title = np.value(QStringLiteral("title")).toString();
    const QString artist = np.value(QStringLiteral("artist")).toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QJsonObject a;
    a.insert(QStringLiteral("type"), 2); // "Listening to"
    QJsonObject assets;
    QJsonObject times;
    if (source == QLatin1String("library")) {
        if (title.isEmpty())
            return QJsonObject();
        a.insert(QStringLiteral("details"), field(title));
        if (!artist.isEmpty())
            a.insert(QStringLiteral("state"), field(artist));
        const QString album = np.value(QStringLiteral("album")).toString();
        const QString albumArtist = np.value(QStringLiteral("album_artist")).toString();
        const QString cover = m_lookUpCovers ? coverFor(albumArtist.isEmpty() ? artist : albumArtist, album, title) : QString();
        assets.insert(QStringLiteral("large_image"), cover.isEmpty() ? QStringLiteral("ruche") : cover);
        assets.insert(QStringLiteral("large_text"), field(album.isEmpty() ? QStringLiteral("Ruche") : album));
        if (!cover.isEmpty()) {
            assets.insert(QStringLiteral("small_image"), QStringLiteral("ruche"));
            assets.insert(QStringLiteral("small_text"), QStringLiteral("Ruche"));
        }
        const double pos = np.value(QStringLiteral("position")).toDouble();
        const double dur = np.value(QStringLiteral("duration")).toDouble();
        const qint64 start = now - qint64(pos * 1000);
        times.insert(QStringLiteral("start"), start);
        if (dur > 0)
            times.insert(QStringLiteral("end"), start + qint64(dur * 1000));
    } else {
        // Only a clean "Artist - Title" is shown; anything else a station sends as its title
        // (ads, slogans, show names, URLs) stays out, and Discord shows the station instead.
        const QString station = np.value(QStringLiteral("station")).toString();
        const bool song = np.value(QStringLiteral("is_song")).toBool();
        if (song) {
            a.insert(QStringLiteral("details"), field(title));
            a.insert(QStringLiteral("state"), field(artist));
        } else {
            a.insert(QStringLiteral("details"), field(station));
            a.insert(QStringLiteral("state"), field(tr("Live radio")));
        }
        const QString logo = np.value(QStringLiteral("logo_url")).toString();
        assets.insert(QStringLiteral("large_image"), logo.isEmpty() ? QStringLiteral("ruche") : logo);
        assets.insert(QStringLiteral("large_text"), field(station));
        if (!logo.isEmpty()) {
            assets.insert(QStringLiteral("small_image"), QStringLiteral("ruche"));
            assets.insert(QStringLiteral("small_text"), QStringLiteral("Ruche"));
        }
        // a song counts from when it started; the station alone, from when it was tuned in
        const qint64 since = song ? m_titleSince : m_stationSince;
        times.insert(QStringLiteral("start"), since > 0 ? since : now);
    }
    a.insert(QStringLiteral("assets"), assets);
    a.insert(QStringLiteral("timestamps"), times);
    a.insert(QStringLiteral("instance"), false);
    return a;
}

void DiscordPresence::setLookUpCovers(bool on)
{
    m_lookUpCovers = on;
    scheduleUpdate();
}

// ---------------------------------------------------------------- album covers

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

QString coverCacheFile() { return paths::dataDir() + QStringLiteral("/discord-covers.json"); }
} // namespace

void DiscordPresence::loadCoverCache()
{
    m_coverCacheLoaded = true;
    QFile f(coverCacheFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.begin(); it != o.end(); ++it)
        m_covers.insert(it.key(), it.value().toString());
}

void DiscordPresence::saveCoverCache()
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

QString DiscordPresence::coverFor(const QString &artist, const QString &album, const QString &title)
{
    if (artist.isEmpty() || (album.isEmpty() && title.isEmpty()))
        return QString();
    if (!m_coverCacheLoaded)
        loadCoverCache();
    // Tracks without an album are looked up as songs.
    const QString key = norm(artist) + QLatin1Char('\n') + (album.isEmpty() ? QStringLiteral("song:") + norm(title) : norm(album));
    const auto it = m_covers.constFind(key);
    if (it != m_covers.constEnd())
        return *it;
    if (!m_coverPending.contains(key))
        lookUpCover(key, artist, album, title);
    return QString(); // Ruche's logo until the cover is found
}

void DiscordPresence::coverFound(const QString &key, const QString &url)
{
    m_coverPending.remove(key);
    m_covers.insert(key, url);
    saveCoverCache();
    if (!url.isEmpty()) {
        m_lastSent.clear();
        scheduleUpdate(); // the current song may be the one that was looked up
    }
}

void DiscordPresence::lookUpCover(const QString &key, const QString &artist, const QString &album, const QString &title)
{
    m_coverPending.insert(key);
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
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
    req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://weizkule.be )");
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

void DiscordPresence::lookUpMusicBrainz(const QString &key, const QString &artist, const QString &album)
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
    req.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://weizkule.be )");
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
            // Only point Discord at it if Cover Art Archive really has a front cover.
            const QString art = qEnvironmentVariable("RUCHE_COVERARTARCHIVE", QStringLiteral("https://coverartarchive.org")) +
                                QStringLiteral("/release-group/%1/front-500").arg(id);
            QNetworkRequest head{QUrl(art)};
            head.setTransferTimeout(10000);
            head.setRawHeader("User-Agent", "Ruche/" RUCHE_VERSION " ( https://weizkule.be )");
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

void DiscordPresence::pushActivity()
{
    if (!m_ready)
        return;
    const QJsonObject activity = buildActivity();
    QJsonObject args;
    args.insert(QStringLiteral("pid"), qint64(QCoreApplication::applicationPid()));
    if (!activity.isEmpty())
        args.insert(QStringLiteral("activity"), activity);
    else
        args.insert(QStringLiteral("activity"), QJsonValue::Null); // clears it
    // Skip identical updates (the timestamps only move on a new track or a seek).
    const QByteArray key = QJsonDocument(args).toJson(QJsonDocument::Compact);
    if (key == m_lastSent)
        return;
    m_lastSent = key;
    m_sentStart = activity.value(QStringLiteral("timestamps")).toObject().value(QStringLiteral("start")).toInteger();
    if (m_pc->mode() != PlaybackController::Mode::Library)
        m_sentStart = 0;
    QJsonObject cmd;
    cmd.insert(QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY"));
    cmd.insert(QStringLiteral("args"), args);
    cmd.insert(QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    send(Frame, cmd);
    m_sinceSend.restart();
}
