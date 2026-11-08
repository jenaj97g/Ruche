#include "Player.h"
#include "MetaParse.h"

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QMetaObject>
#include <clocale>
#include <mpv/client.h>

namespace {
enum ObserveId : quint64 { ObsPause = 1, ObsIdle, ObsDuration, ObsMetadata, ObsSeekable };
}

Player::Player(const QStringList &extraOptions, QObject *parent) : QObject(parent)
{
    // libmpv refuses to start unless LC_NUMERIC is "C".
    std::setlocale(LC_NUMERIC, "C");
    m_mpv = mpv_create();
    if (!m_mpv) {
        qCritical("mpv_create failed");
        return;
    }
    // Audio-only, no scripts, no OSD, no config files: the less mpv loads, the less RAM it keeps.
    setOpt("config", "no");
    setOpt("terminal", "no");
    setOpt("video", "no");
    setOpt("vo", "null");
    setOpt("audio-display", "no");
    setOpt("cover-art-auto", "no");
    setOpt("sub-auto", "no");
    setOpt("audio-file-auto", "no");
    setOpt("ytdl", "no");
    setOpt("load-scripts", "no");
    setOpt("osc", "no");
    setOpt("load-stats-overlay", "no");
    setOpt("load-osd-console", "no");
    setOpt("load-auto-profiles", "no");
    setOpt("input-default-bindings", "no");
    setOpt("input-vo-keyboard", "no");
    setOpt("idle", "yes");
    setOpt("keep-open", "no");
    setOpt("gapless-audio", "weak");
    setOpt("audio-client-name", "Ruche");
    setOpt("user-agent", "Ruche/" RUCHE_VERSION);
    setOpt("network-timeout", "20");
    setOpt("volume-max", "100");
    // Hand us the raw tag bytes. mpv's own charset guess turns Latin-1 "Céline" into
    // "CÚline"; meta::decodeLoose() gets UTF-8 vs Windows-1252 right instead.
    setOpt("metadata-codepage", "utf-8");
    // mpv's defaults keep up to 150 MiB ahead and 50 MiB behind per stream. A radio stream
    // left on for an hour would sit on ~50 MiB of already-played audio. Keep it tight.
    // (A small back buffer has to stay: with none at all mpv drops ICY title updates.)
    setOpt("demuxer-max-bytes", "4MiB");
    setOpt("demuxer-max-back-bytes", "256KiB");
    setOpt("demuxer-readahead-secs", "10");
    setOpt("cache-secs", "20");
    setOpt("audio-buffer", "0.25");

    for (const QString &opt : extraOptions) {
        const int eq = opt.indexOf(QLatin1Char('='));
        if (eq > 0)
            setOpt(opt.left(eq).toUtf8().constData(), opt.mid(eq + 1).toUtf8().constData());
    }

    if (!qEnvironmentVariableIsEmpty("RUCHE_MPV_LOG"))
        mpv_request_log_messages(m_mpv, qgetenv("RUCHE_MPV_LOG").constData());

    if (mpv_initialize(m_mpv) < 0) {
        qCritical("mpv_initialize failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }
    mpv_observe_property(m_mpv, ObsPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, ObsIdle, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, ObsDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, ObsMetadata, "metadata", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, ObsSeekable, "seekable", MPV_FORMAT_FLAG);

    mpv_set_wakeup_callback(
        m_mpv,
        [](void *ctx) {
            auto *self = static_cast<Player *>(ctx);
            // Coalesce bursts of wakeups into one queued drain.
            if (!self->m_wakeupPending.exchange(true))
                QMetaObject::invokeMethod(self, "drainEvents", Qt::QueuedConnection);
        },
        this);
}

Player::~Player()
{
    if (m_mpv)
        mpv_terminate_destroy(m_mpv);
}

void Player::setOpt(const char *name, const char *value)
{
    const int r = mpv_set_option_string(m_mpv, name, value);
    if (r < 0 && !qEnvironmentVariableIsEmpty("RUCHE_DEBUG"))
        qWarning("mpv option %s=%s: %s", name, value, mpv_error_string(r));
}

qint64 Player::load(const QString &urlOrPath, bool append)
{
    if (!m_mpv)
        return -1;
    const QByteArray u = urlOrPath.toUtf8();
    const char *args[] = {"loadfile", u.constData(), append ? "append" : "replace", nullptr};
    mpv_node result;
    qint64 id = -1;
    if (mpv_command_ret(m_mpv, args, &result) >= 0) {
        if (result.format == MPV_FORMAT_NODE_MAP) {
            for (int i = 0; i < result.u.list->num; ++i) {
                if (qstrcmp(result.u.list->keys[i], "playlist_entry_id") == 0 &&
                    result.u.list->values[i].format == MPV_FORMAT_INT64)
                    id = result.u.list->values[i].u.int64;
            }
        }
        mpv_free_node_contents(&result);
    }
    if (!append) {
        m_lastStreamTitle.clear();
        m_lastStationName.clear();
        setPaused(false);
    }
    return id;
}

void Player::clearOthers()
{
    if (!m_mpv)
        return;
    const char *args[] = {"playlist-clear", nullptr};
    mpv_command(m_mpv, args);
}

void Player::stop()
{
    if (!m_mpv)
        return;
    const char *args[] = {"stop", nullptr};
    mpv_command(m_mpv, args);
}

void Player::setPaused(bool paused)
{
    if (!m_mpv)
        return;
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void Player::togglePause() { setPaused(!m_paused); }

void Player::seek(double seconds)
{
    if (!m_mpv)
        return;
    const QByteArray s = QByteArray::number(qMax(0.0, seconds), 'f', 3);
    const char *args[] = {"seek", s.constData(), "absolute", nullptr};
    mpv_command_async(m_mpv, 0, args);
}

void Player::setVolume(int percent)
{
    if (!m_mpv)
        return;
    double v = qBound(0, percent, 100);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void Player::setVolumeF(double percent)
{
    if (!m_mpv)
        return;
    double v = qBound(0.0, percent, 100.0);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void Player::setEqualizer(const QVector<double> &gains)
{
    if (!m_mpv)
        return;
    static const int kFreq[] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    QByteArray af;
    bool any = false;
    double top = 0;
    for (double g : gains) {
        any = any || qAbs(g) >= 0.05;
        top = qMax(top, g);
    }
    if (any && gains.size() == 10) {
        // Peaking filters an octave wide; the volume step before them keeps boosts from clipping.
        QByteArrayList chain;
        if (top > 0)
            chain << "volume=" + QByteArray::number(-top, 'f', 1) + "dB";
        for (int i = 0; i < 10; ++i)
            if (qAbs(gains[i]) >= 0.05)
                chain << "equalizer=f=" + QByteArray::number(kFreq[i]) + ":t=o:w=1:g=" + QByteArray::number(gains[i], 'f', 1);
        af = "@eq:lavfi=[" + chain.join(',') + "]";
    }
    mpv_set_property_string(m_mpv, "af", af.constData());
}

void Player::setStartAt(double seconds)
{
    if (!m_mpv)
        return;
    const QByteArray v = seconds < 0 ? QByteArray("none") : QByteArray::number(seconds, 'f', 3);
    mpv_set_property_string(m_mpv, "start", v.constData());
}

void Player::playlistNext()
{
    if (!m_mpv)
        return;
    const char *args[] = {"playlist-next", "force", nullptr};
    mpv_command(m_mpv, args);
}

void Player::setReplayGain(const QString &mode)
{
    if (!m_mpv)
        return;
    const QByteArray m = mode.toUtf8();
    mpv_set_property_string(m_mpv, "replaygain", m.constData());
}

double Player::position() const
{
    double v = 0;
    if (m_mpv && mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &v) < 0)
        v = 0;
    return v;
}

double Player::playbackTime() const
{
    double v = -1;
    if (m_mpv && mpv_get_property(m_mpv, "playback-time", MPV_FORMAT_DOUBLE, &v) < 0)
        v = -1;
    return v;
}

QVariantMap Player::audioParams() const
{
    QVariantMap out;
    if (!m_mpv)
        return out;
    char *codec = mpv_get_property_string(m_mpv, "audio-codec-name");
    if (codec) {
        out.insert(QStringLiteral("codec"), QString::fromUtf8(codec));
        mpv_free(codec);
    }
    double br = 0;
    if (mpv_get_property(m_mpv, "audio-bitrate", MPV_FORMAT_DOUBLE, &br) >= 0)
        out.insert(QStringLiteral("bitrate"), br);
    int64_t sr = 0, ch = 0;
    if (mpv_get_property(m_mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &sr) >= 0)
        out.insert(QStringLiteral("samplerate"), qint64(sr));
    if (mpv_get_property(m_mpv, "audio-params/channel-count", MPV_FORMAT_INT64, &ch) >= 0)
        out.insert(QStringLiteral("channels"), qint64(ch));
    return out;
}

void Player::handleMetadata()
{
    mpv_node node;
    if (mpv_get_property(m_mpv, "metadata", MPV_FORMAT_NODE, &node) < 0)
        return;
    QHash<QByteArray, QByteArray> tags;
    if (node.format == MPV_FORMAT_NODE_MAP) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const mpv_node &v = node.u.list->values[i];
            if (v.format == MPV_FORMAT_STRING)
                tags.insert(QByteArray(node.u.list->keys[i]).toLower(), QByteArray(v.u.string));
        }
    }
    mpv_free_node_contents(&node);
    if (!qEnvironmentVariableIsEmpty("RUCHE_DEBUG"))
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it)
            qInfo("[mpv metadata] %s = %s", it.key().constData(), it.value().toPercentEncoding(" -'=;/:").constData());

    // ICY first (Icecast/Shoutcast), then in-band tags (chained Ogg, etc.).
    QString title = meta::cleanStreamTitle(meta::decodeLoose(tags.value("icy-title")));
    if (title.isEmpty()) {
        const QString t = meta::cleanStreamTitle(meta::decodeLoose(tags.value("title")));
        const QString a = meta::cleanStreamTitle(meta::decodeLoose(tags.value("artist")));
        title = (!a.isEmpty() && !t.isEmpty()) ? a + QLatin1String(" - ") + t : t;
    }
    const QString station = meta::decodeLoose(tags.value("icy-name")).trimmed();
    if (title != m_lastStreamTitle || station != m_lastStationName) {
        m_lastStreamTitle = title;
        m_lastStationName = station;
        Q_EMIT streamMetadataChanged(title, station);
    }
}

void Player::drainEvents()
{
    m_wakeupPending = false;
    if (!m_mpv)
        return;
    for (;;) {
        mpv_event *ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id) {
        case MPV_EVENT_START_FILE: {
            const auto *sf = static_cast<mpv_event_start_file *>(ev->data);
            Q_EMIT fileStarted(sf ? sf->playlist_entry_id : -1);
            break;
        }
        case MPV_EVENT_FILE_LOADED:
            Q_EMIT fileLoaded();
            break;
        case MPV_EVENT_END_FILE: {
            const auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            QString err;
            if (ef->reason == MPV_END_FILE_REASON_ERROR)
                err = QString::fromUtf8(mpv_error_string(ef->error));
            Q_EMIT fileEnded(ef->playlist_entry_id, int(ef->reason), err);
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            Q_EMIT playbackRestarted();
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            const auto *p = static_cast<mpv_event_property *>(ev->data);
            switch (ev->reply_userdata) {
            case ObsPause:
                if (p->format == MPV_FORMAT_FLAG) {
                    m_paused = *static_cast<int *>(p->data);
                    Q_EMIT pausedChanged(m_paused);
                }
                break;
            case ObsIdle:
                if (p->format == MPV_FORMAT_FLAG) {
                    m_idle = *static_cast<int *>(p->data);
                    Q_EMIT idleChanged(m_idle);
                }
                break;
            case ObsDuration:
                m_duration = p->format == MPV_FORMAT_DOUBLE ? *static_cast<double *>(p->data) : 0;
                Q_EMIT durationChanged(m_duration);
                break;
            case ObsSeekable:
                m_seekable = p->format == MPV_FORMAT_FLAG && *static_cast<int *>(p->data);
                Q_EMIT seekableChanged(m_seekable);
                break;
            case ObsMetadata:
                handleMetadata();
                break;
            }
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            const auto *m = static_cast<mpv_event_log_message *>(ev->data);
            qInfo("[mpv/%s] %s: %s", m->level, m->prefix, QByteArray(m->text).trimmed().constData());
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            return;
        default:
            break;
        }
    }
}
