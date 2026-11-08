#include "PlaybackController.h"
#include "HlsProxy.h"
#include "Library.h"
#include "Player.h"
#include "StreamResolver.h"

#include <QDebug>
#include <QNetworkAccessManager>
#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

PlaybackController::PlaybackController(Library *lib, const QStringList &mpvOptions, QObject *parent)
    : QObject(parent), m_lib(lib), m_player(new Player(mpvOptions, this)), m_nam(new QNetworkAccessManager(this)),
      m_proxy(new HlsProxy(m_nam, this)), m_resolver(new StreamResolver(m_nam, this)), m_mpvOptions(mpvOptions)
{
    m_fade.setInterval(40);
    connect(&m_fade, &QTimer::timeout, this, &PlaybackController::fadeStep);
    m_tailWait.setInterval(5);
    connect(&m_tailWait, &QTimer::timeout, this, [this] {
        // start the tail exactly where the main player has got to, so the outgoing track
        // carries on seamlessly in it
        if (m_tailReady && m_player->position() >= m_tailStart - 0.01) {
            m_tailWait.stop();
            beginFade();
        }
    });
    m_tailGuard.setSingleShot(true);
    connect(&m_tailGuard, &QTimer::timeout, this, [this] {
        if (!m_fading)
            endCrossfade(); // the tail never got going: let the track end as usual
    });
    m_nam->setAutoDeleteReplies(false);
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        if (m_mode == Mode::Radio)
            pollStream();
        else if (m_mode == Mode::Library)
            maybeStartCrossfade();
        Q_EMIT positionChanged(position(), duration());
    });
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, [this] {
        if (m_mode == Mode::Radio && !m_userStopped)
            m_resolver->resolve(m_stationUrl);
    });

    connect(m_player, &Player::fileStarted, this, &PlaybackController::onFileStarted);
    connect(m_player, &Player::fileEnded, this, &PlaybackController::onFileEnded);
    connect(m_player, &Player::pausedChanged, this, [this](bool) {
        if (isPaused() || m_mode == Mode::Idle)
            m_tick.stop();
        else
            m_tick.start();
        Q_EMIT stateChanged();
    });
    connect(m_player, &Player::idleChanged, this, [this](bool idle) {
        if (idle)
            m_tick.stop();
        Q_EMIT stateChanged();
    });
    connect(m_player, &Player::seekableChanged, this, [this](bool s) {
        m_seekable = s;
        Q_EMIT stateChanged();
    });
    connect(m_player, &Player::durationChanged, this, [this](double) { Q_EMIT positionChanged(position(), duration()); });
    connect(m_player, &Player::playbackRestarted, this, [this] {
        m_reconnectAttempts = 0;
        if (!isPaused())
            m_tick.start();
        Q_EMIT stateChanged();
    });
    connect(m_player, &Player::streamMetadataChanged, this, [this](const QString &title, const QString &name) {
        if (m_mode != Mode::Radio)
            return;
        if (m_stationName.isEmpty() && !name.isEmpty()) {
            m_stationName = name;
            Q_EMIT stationChanged(name);
        }
        m_mpvStreamTitle = title;
        // With HLS the relay knows better; mpv's title is only a fallback until it has something.
        if (!m_proxy->isActive() || m_proxy->titleAt(1e12).isEmpty())
            setStreamTitle(title);
    });

    connect(m_resolver, &StreamResolver::resolved, this, &PlaybackController::startResolved);
    connect(m_resolver, &StreamResolver::failed, this, [this](const QString &msg) { scheduleReconnect(msg); });
    connect(m_proxy, &HlsProxy::upstreamError, this, [this](const QString &msg) {
        if (!qEnvironmentVariableIsEmpty("RUCHE_DEBUG"))
            qWarning() << "[hls] upstream error" << msg;
    });
    connect(m_proxy, &HlsProxy::titleScheduled, this, [](double t, const QString &title, const QString &src) {
        if (!qEnvironmentVariableIsEmpty("RUCHE_DEBUG"))
            qInfo().noquote() << "[hls] title at" << t << "s (" + src + "):" << title;
    });
    m_player->setVolume(m_volume);
}

PlaybackController::~PlaybackController() = default;

// ---------------------------------------------------------------------------
// Library queue

void PlaybackController::playTracks(const QVector<qint64> &ids, int start)
{
    if (ids.isEmpty())
        return;
    m_resolver->cancel();
    m_proxy->stop();
    m_reconnect.stop();
    m_queue = ids;
    m_index = qBound(0, start, int(ids.size()) - 1);
    rebuildShuffle();
    Q_EMIT queueChanged();
    playQueueIndex(m_index);
}

void PlaybackController::playQueueIndex(int i)
{
    endCrossfade();
    if (i < 0 || i >= m_queue.size())
        return;
    const Track *t = m_lib->byId(m_queue[i]);
    if (!t)
        return;
    m_userStopped = false;
    if (m_mode != Mode::Library) {
        m_mode = Mode::Library;
        m_stationName.clear();
        m_streamTitle.clear();
    }
    m_index = i;
    m_entryIndex.clear();
    m_nextEntry = -1;
    m_currentEntry = m_player->load(t->filePath());
    m_entryIndex.insert(m_currentEntry, i);
    appendNext();
    Q_EMIT stateChanged();
}

void PlaybackController::enqueue(const QVector<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    if (m_mode != Mode::Library || m_queue.isEmpty()) {
        playTracks(ids, 0);
        return;
    }
    const int old = m_queue.size();
    m_queue += ids;
    if (m_shuffle)
        for (int i = old; i < m_queue.size(); ++i)
            m_order.insert(QRandomGenerator::global()->bounded(m_order.indexOf(m_index) + 1, int(m_order.size()) + 1), i);
    refreshNext();
    Q_EMIT queueChanged();
}

void PlaybackController::playNext(const QVector<qint64> &ids)
{
    if (ids.isEmpty())
        return;
    if (m_mode != Mode::Library || m_queue.isEmpty()) {
        playTracks(ids, 0);
        return;
    }
    const int at = m_index + 1;
    for (int k = 0; k < ids.size(); ++k)
        m_queue.insert(at + k, ids[k]);
    // shift shuffle order indexes and slot the new ones right after the current track
    for (int &o : m_order)
        if (o >= at)
            o += ids.size();
    if (m_shuffle) {
        const int pos = m_order.indexOf(m_index) + 1;
        for (int k = 0; k < ids.size(); ++k)
            m_order.insert(pos + k, at + k);
    }
    refreshNext();
    Q_EMIT queueChanged();
}

void PlaybackController::removeFromQueue(const QVector<int> &indexes)
{
    QVector<int> sorted = indexes;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int i : sorted) {
        if (i < 0 || i >= m_queue.size() || i == m_index)
            continue;
        m_queue.removeAt(i);
        if (i < m_index)
            --m_index;
    }
    rebuildShuffle();
    refreshNext();
    Q_EMIT queueChanged();
}

void PlaybackController::rebuildShuffle()
{
    m_order.clear();
    if (!m_shuffle)
        return;
    for (int i = 0; i < m_queue.size(); ++i)
        if (i != m_index)
            m_order.push_back(i);
    std::shuffle(m_order.begin(), m_order.end(), *QRandomGenerator::global());
    if (m_index >= 0)
        m_order.prepend(m_index);
}

int PlaybackController::peekNext() const
{
    if (m_queue.isEmpty() || m_index < 0)
        return -1;
    if (m_repeat == Repeat::One)
        return m_index;
    if (m_shuffle && m_order.size() == m_queue.size()) {
        const int pos = m_order.indexOf(m_index);
        if (pos + 1 < m_order.size())
            return m_order[pos + 1];
        return m_repeat == Repeat::All ? m_order.value(0, -1) : -1;
    }
    if (m_index + 1 < m_queue.size())
        return m_index + 1;
    return m_repeat == Repeat::All ? 0 : -1;
}

QVector<int> PlaybackController::upcomingIndexes(int max) const
{
    QVector<int> out;
    if (m_mode != Mode::Library || m_index < 0)
        return out;
    if (m_shuffle && m_order.size() == m_queue.size()) {
        for (int p = m_order.indexOf(m_index) + 1; p < m_order.size() && out.size() < max; ++p)
            out << m_order[p];
    } else {
        for (int i = m_index + 1; i < m_queue.size() && out.size() < max; ++i)
            out << i;
    }
    return out;
}

void PlaybackController::appendNext()
{
    const int ni = peekNext();
    if (ni < 0)
        return;
    const Track *t = m_lib->byId(m_queue[ni]);
    if (!t)
        return;
    m_nextEntry = m_player->load(t->filePath(), true);
    m_entryIndex.insert(m_nextEntry, ni);
}

void PlaybackController::refreshNext()
{
    if (m_mode != Mode::Library || m_currentEntry < 0)
        return;
    m_player->clearOthers();
    m_entryIndex.remove(m_nextEntry);
    m_nextEntry = -1;
    appendNext();
}

void PlaybackController::onFileStarted(qint64 entry)
{
    if (m_mode != Mode::Library)
        return;
    const auto it = m_entryIndex.constFind(entry);
    if (it == m_entryIndex.constEnd())
        return;
    const bool advanced = entry == m_nextEntry;
    m_index = it.value();
    m_currentEntry = entry;
    if (advanced) {
        // gapless hand-over: mpv already started the appended entry
        m_entryIndex.clear();
        m_entryIndex.insert(entry, m_index);
        m_nextEntry = -1;
        m_player->clearOthers();
        appendNext();
    }
    const qint64 id = m_queue.value(m_index);
    m_lib->bumpPlayCount(id);
    Q_EMIT trackChanged(id);
    Q_EMIT queueChanged();
}

void PlaybackController::onFileEnded(qint64 entry, int reason, const QString &error)
{
    if (m_mode == Mode::Radio) {
        if (reason == 2 /*stop*/ || m_userStopped)
            return;
        scheduleReconnect(error.isEmpty() ? tr("stream ended") : error);
        return;
    }
    if (m_mode != Mode::Library || entry != m_currentEntry)
        return;
    if (reason == 4 /*error*/) {
        const Track *t = m_lib->byId(m_queue.value(m_index));
        Q_EMIT message(tr("Could not play %1: %2").arg(t ? t->displayTitle() : QString(), error));
        if (++m_consecutiveErrors < 5) {
            const int ni = peekNext();
            if (ni >= 0 && ni != m_index) {
                playQueueIndex(ni);
                return;
            }
        }
    } else {
        m_consecutiveErrors = 0;
    }
    if (reason == 0 /*eof*/ && m_nextEntry < 0) {
        // end of the queue
        m_mode = Mode::Idle;
        m_currentEntry = -1;
        m_tick.stop();
        Q_EMIT trackChanged(0);
        Q_EMIT stateChanged();
    }
}

void PlaybackController::next()
{
    if (m_mode == Mode::Radio)
        return;
    int ni = peekNext();
    if (m_repeat == Repeat::One) {
        // an explicit "next" moves on even when repeating one track
        ni = m_index + 1 < m_queue.size() ? m_index + 1 : (m_queue.isEmpty() ? -1 : 0);
        if (m_shuffle && m_order.size() == m_queue.size()) {
            const int pos = m_order.indexOf(m_index);
            ni = m_order.value(pos + 1, m_order.value(0, -1));
        }
    }
    if (ni < 0 && !m_queue.isEmpty())
        ni = m_shuffle && !m_order.isEmpty() ? m_order.first() : 0;
    playQueueIndex(ni);
}

void PlaybackController::previous()
{
    if (m_mode == Mode::Radio)
        return;
    if (position() > 3.0) {
        seek(0);
        return;
    }
    int pi = m_index - 1;
    if (m_shuffle && m_order.size() == m_queue.size()) {
        const int pos = m_order.indexOf(m_index);
        pi = pos > 0 ? m_order[pos - 1] : m_index;
    }
    playQueueIndex(qMax(0, pi));
}

// ---------------------------------------------------------------------------
// Radio

void PlaybackController::playStation(const QString &name, const QUrl &url)
{
    endCrossfade();
    m_reconnect.stop();
    m_reconnectAttempts = 0;
    m_userStopped = false;
    m_mode = Mode::Radio;
    m_queue.clear();
    m_index = -1;
    m_entryIndex.clear();
    m_currentEntry = m_nextEntry = -1;
    m_stationName = name;
    m_stationUrl = url;
    m_history.clear();
    m_streamTitle.clear();
    m_mpvStreamTitle.clear();
    m_stationClock.start();
    m_player->stop();
    m_proxy->stop();
    Q_EMIT trackChanged(0);
    Q_EMIT stationChanged(name);
    Q_EMIT streamTitleChanged(QString());
    Q_EMIT historyChanged();
    Q_EMIT queueChanged();
    Q_EMIT stateChanged();
    m_resolver->resolve(url);
}

bool PlaybackController::streamIsHls() const { return m_mode == Mode::Radio && m_proxy->isActive(); }

void PlaybackController::startResolved(const QUrl &url, bool hlsIn, const QString &hint)
{
    if (m_mode != Mode::Radio)
        return;
    if (m_stationName.isEmpty() && !hint.isEmpty()) {
        m_stationName = hint;
        Q_EMIT stationChanged(hint);
    }
    bool hls = hlsIn;
    QString target = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (hls && !qEnvironmentVariableIsEmpty("RUCHE_NO_HLS_RELAY"))
        hls = false; // for comparison tests only
    if (hls)
        target = m_proxy->start(url);
    else
        m_proxy->stop();
    m_currentEntry = m_player->load(target);
    Q_EMIT stateChanged();
}

void PlaybackController::pollStream()
{
    if (!m_proxy->isActive())
        return;
    const double t = m_player->playbackTime();
    static const bool debug = !qEnvironmentVariableIsEmpty("RUCHE_DEBUG");
    if (debug) {
        static int n = 0;
        if (n++ % 4 == 0)
            qInfo() << "[hls] playback-time" << t << "time-pos" << m_player->position() << "->" << m_proxy->titleAt(t);
    }
    if (t < 0)
        return;
    const QString title = m_proxy->titleAt(t);
    if (!title.isEmpty())
        setStreamTitle(title);
    else if (!m_mpvStreamTitle.isEmpty() && m_streamTitle.isEmpty())
        setStreamTitle(m_mpvStreamTitle);
}

void PlaybackController::setStreamTitle(const QString &title)
{
    if (title == m_streamTitle || title.isEmpty())
        return;
    m_streamTitle = title;
    if (m_history.isEmpty() || m_history.first().title != title) {
        m_history.prepend({QDateTime::currentDateTime(), title});
        if (m_history.size() > 200)
            m_history.removeLast();
        Q_EMIT historyChanged();
    }
    if (m_titleLog.isOpen()) {
        m_titleLog.write(QStringLiteral("%1\t%2\t%3\n")
                             .arg(m_stationClock.elapsed())
                             .arg(m_player->playbackTime(), 0, 'f', 2)
                             .arg(title)
                             .toUtf8());
        m_titleLog.flush();
    }
    Q_EMIT streamTitleChanged(title);
}

void PlaybackController::scheduleReconnect(const QString &why)
{
    if (m_mode != Mode::Radio || m_userStopped)
        return;
    if (m_reconnectAttempts >= 8) {
        Q_EMIT message(tr("Gave up on %1: %2").arg(m_stationName, why));
        stop();
        return;
    }
    const int delay = qMin(30000, 2000 << qMin(m_reconnectAttempts, 4));
    ++m_reconnectAttempts;
    Q_EMIT message(tr("Stream dropped (%1), reconnecting in %2 s").arg(why).arg(delay / 1000));
    m_proxy->stop();
    m_reconnect.start(delay);
}

void PlaybackController::setTitleLog(const QString &file)
{
    m_titleLog.setFileName(file);
    m_titleLog.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
}

// ---------------------------------------------------------------------------
// Transport

bool PlaybackController::isPaused() const { return m_player->isPaused(); }

void PlaybackController::togglePause()
{
    endCrossfade(); // pausing mid-fade: the outgoing track is cut, the new one stays
    if (m_mode == Mode::Idle) {
        if (!m_queue.isEmpty())
            playQueueIndex(qMax(0, m_index));
        return;
    }
    m_player->togglePause();
}

void PlaybackController::play()
{
    if (isPaused() || m_mode == Mode::Idle)
        togglePause();
}

void PlaybackController::pause()
{
    endCrossfade();
    if (m_mode != Mode::Idle && !isPaused())
        m_player->setPaused(true);
}

void PlaybackController::stop()
{
    endCrossfade();
    m_userStopped = true;
    m_reconnect.stop();
    m_resolver->cancel();
    m_player->stop();
    m_proxy->stop();
    m_mode = Mode::Idle;
    m_currentEntry = m_nextEntry = -1;
    m_tick.stop();
    Q_EMIT trackChanged(0);
    Q_EMIT stateChanged();
}

void PlaybackController::seek(double seconds)
{
    endCrossfade();
    if (m_mode == Mode::Library)
        m_player->seek(seconds);
}

void PlaybackController::setVolume(int v)
{
    m_volume = qBound(0, v, 100);
    if (!m_fading) // mid-fade, the next step picks the new volume up
        m_player->setVolume(m_volume);
}

void PlaybackController::setShuffle(bool on)
{
    m_shuffle = on;
    rebuildShuffle();
    refreshNext();
    Q_EMIT queueChanged();
    Q_EMIT stateChanged();
}

void PlaybackController::setRepeat(Repeat r)
{
    m_repeat = r;
    refreshNext();
    Q_EMIT stateChanged();
}

void PlaybackController::setReplayGain(const QString &mode)
{
    m_replayGain = mode;
    m_player->setReplayGain(mode);
}

void PlaybackController::setEqualizer(const QVector<double> &gains)
{
    m_eq = gains;
    m_player->setEqualizer(gains);
    if (m_tail)
        m_tail->setEqualizer(gains);
}

void PlaybackController::setCrossfade(bool on, int seconds, bool betweenAlbumTracks)
{
    m_xfade = on;
    m_xfadeSecs = qBound(1, seconds, 12);
    m_xfadeAlbums = betweenAlbumTracks;
    if (!on)
        endCrossfade();
}

// ---------------------------------------------------------------------------
// Crossfade

bool PlaybackController::crossfadeAllowed(int from, int to) const
{
    if (to < 0 || to == from) // repeat-one, or nothing next
        return false;
    if (m_xfadeAlbums)
        return true;
    const Track *a = m_lib->byId(m_queue.value(from));
    const Track *b = m_lib->byId(m_queue.value(to));
    if (!a || !b)
        return false;
    // the next track of the same album (live albums, DJ mixes, concept albums) stays gapless
    const bool sameAlbum = !a->album.isEmpty() && a->album.compare(b->album, Qt::CaseInsensitive) == 0 &&
                           a->displayAlbumArtist().compare(b->displayAlbumArtist(), Qt::CaseInsensitive) == 0;
    return !(sameAlbum && b->discNo == a->discNo && b->trackNo == a->trackNo + 1);
}

void PlaybackController::maybeStartCrossfade()
{
    if (!m_xfade || m_tail || isPaused() || m_nextEntry < 0)
        return;
    const double dur = m_player->duration();
    const double left = dur - m_player->position();
    // the tail needs about half a second to load, so it starts that much before the fade
    if (dur < m_xfadeSecs * 2 + 2 || left > m_xfadeSecs + 0.6 || left < 1.0)
        return;
    if (!crossfadeAllowed(m_index, m_entryIndex.value(m_nextEntry, -1)))
        return;
    const Track *t = m_lib->byId(m_queue.value(m_index));
    if (!t)
        return;
    // The tail starts silent, a little ahead of the main player (it needs a moment to load);
    // once it plays, the main player moves on and the two cross over.
    m_tail = new Player(m_mpvOptions, this);
    m_tail->setReplayGain(m_replayGain);
    m_tail->setEqualizer(m_eq);
    m_tail->setVolume(0);
    m_tailStart = m_player->position() + 0.4;
    m_tailReady = false;
    m_tail->setStartAt(m_tailStart);
    connect(m_tail, &Player::playbackRestarted, this, [this] {
        m_tailReady = true; // loaded, paused at m_tailStart
    });
    m_tail->load(t->filePath());
    m_tail->setPaused(true);
    m_tailWait.start();
    m_tailGuard.start(2000);
}

void PlaybackController::beginFade()
{
    if (!m_tail || m_mode != Mode::Library || m_nextEntry < 0) {
        endCrossfade();
        return;
    }
    m_fading = true;
    m_tailGuard.stop();
    m_tail->setPaused(false);
    if (!qEnvironmentVariableIsEmpty("RUCHE_DEBUG"))
        qInfo("[crossfade] %d s, from %.2f s before the end; outgoing track carries on at %.3f s (main at %.3f s)",
              m_xfadeSecs, m_player->duration() - m_player->position(), m_tail->position(), m_player->position());
    m_tail->setVolume(m_volume);
    m_player->setVolumeF(0);
    m_player->playlistNext(); // the usual hand-over to the appended track, a few seconds early
    m_fadeClock.start();
    m_fade.start();
}

void PlaybackController::fadeStep()
{
    const double t = m_fadeClock.elapsed() / (m_xfadeSecs * 1000.0);
    if (t >= 1 || !m_tail) {
        endCrossfade();
        return;
    }
    // Equal power: sin/cos amplitudes. mpv's volume is cubic, hence the cube roots.
    constexpr double kQuarter = 1.5707963267948966; // pi / 2
    const double in = std::sin(t * kQuarter), out = std::cos(t * kQuarter);
    m_player->setVolumeF(m_volume * std::cbrt(in));
    m_tail->setVolumeF(m_volume * std::cbrt(out));
}

void PlaybackController::endCrossfade()
{
    m_fade.stop();
    m_tailGuard.stop();
    m_tailWait.stop();
    const bool was = m_fading;
    m_fading = false;
    if (m_tail) {
        m_tail->disconnect(this);
        m_tail->stop();
        m_tail->deleteLater();
        m_tail = nullptr;
    }
    if (was)
        m_player->setVolume(m_volume);
}

double PlaybackController::position() const { return m_mode == Mode::Idle ? 0 : m_player->position(); }
double PlaybackController::duration() const { return m_mode == Mode::Library ? m_player->duration() : 0; }
QVariantMap PlaybackController::audioParams() const { return m_player->audioParams(); }
qint64 PlaybackController::currentTrackId() const
{
    return m_mode == Mode::Library ? m_queue.value(m_index, 0) : 0;
}
