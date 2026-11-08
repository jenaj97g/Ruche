#include "DiscordPresence.h"
#include "CoverLookup.h"

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

DiscordPresence::DiscordPresence(PlaybackController *pc, Library *lib, Stations *stations, CoverLookup *covers, QObject *parent)
    : QObject(parent), m_pc(pc), m_lib(lib), m_stations(stations), m_covers(covers), m_clientId(builtInClientId())
{
    connect(covers, &CoverLookup::found, this, [this] {
        m_lastSent.clear();
        scheduleUpdate(); // the current song may be the one that was looked up
    });
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
        const QString cover = m_lookUpCovers ? m_covers->url(albumArtist.isEmpty() ? artist : albumArtist, album, title) : QString();
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
        // Pictures: the song's cover (looked up like a library track without an album),
        // with the station's logo as the small badge; else the logo; else Ruche's.
        const QString logo = m_lookUpCovers ? np.value(QStringLiteral("logo_url")).toString() : QString();
        const QString cover = (m_lookUpCovers && song) ? m_covers->url(artist, QString(), title) : QString();
        if (!cover.isEmpty()) {
            assets.insert(QStringLiteral("large_image"), cover);
            assets.insert(QStringLiteral("large_text"), field(title));
            assets.insert(QStringLiteral("small_image"), logo.isEmpty() ? QStringLiteral("ruche") : logo);
            assets.insert(QStringLiteral("small_text"), field(station));
        } else {
            assets.insert(QStringLiteral("large_image"), logo.isEmpty() ? QStringLiteral("ruche") : logo);
            assets.insert(QStringLiteral("large_text"), field(station));
            if (!logo.isEmpty()) {
                assets.insert(QStringLiteral("small_image"), QStringLiteral("ruche"));
                assets.insert(QStringLiteral("small_text"), QStringLiteral("Ruche"));
            }
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
