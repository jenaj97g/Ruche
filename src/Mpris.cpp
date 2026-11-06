#ifdef RUCHE_DBUS
#include "Mpris.h"
#include "Library.h"
#include "PlaybackController.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QUrl>

Mpris::Mpris(PlaybackController *pc, Library *lib, QObject *parent) : QObject(parent), m_pc(pc), m_lib(lib)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);
    new RucheAppAdaptor(this);
    QString service = QStringLiteral("org.mpris.MediaPlayer2.ruche");
    if (bus.interface()->isServiceRegistered(service))
        service += QStringLiteral(".instance%1").arg(QCoreApplication::applicationPid());
    if (!bus.registerService(service))
        return;
    if (!bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), this, QDBusConnection::ExportAdaptors))
        return;
    m_registered = true;

    auto meta = [this] { notify({{QStringLiteral("Metadata"), metadata()}, {QStringLiteral("CanSeek"), m_pc->isSeekable()}}); };
    connect(pc, &PlaybackController::trackChanged, this, meta);
    connect(pc, &PlaybackController::streamTitleChanged, this, meta);
    connect(pc, &PlaybackController::stationChanged, this, meta);
    connect(pc, &PlaybackController::stateChanged, this, [this] {
        const QString s = playbackStatus();
        if (s != m_lastStatus) {
            m_lastStatus = s;
            notify({{QStringLiteral("PlaybackStatus"), s}});
        }
    });
}

void Mpris::setArtFile(const QString &file)
{
    m_artFile = file;
    if (m_registered)
        notify({{QStringLiteral("Metadata"), metadata()}});
}

QString Mpris::playbackStatus() const
{
    if (!m_pc->isActive())
        return QStringLiteral("Stopped");
    return m_pc->isPaused() ? QStringLiteral("Paused") : QStringLiteral("Playing");
}

QVariantMap Mpris::metadata() const
{
    QVariantMap m;
    if (m_pc->mode() == PlaybackController::Mode::Library) {
        const Track *t = m_lib->byId(m_pc->currentTrackId());
        if (!t)
            return m;
        const qint64 id = t->id;
        m.insert(QStringLiteral("mpris:trackid"),
                 QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/ruche/track/%1%2").arg(id < 0 ? "x" : "").arg(qAbs(id)))));
        m.insert(QStringLiteral("xesam:title"), t->displayTitle());
        if (!t->artist.isEmpty())
            m.insert(QStringLiteral("xesam:artist"), QStringList{t->artist});
        if (!t->album.isEmpty())
            m.insert(QStringLiteral("xesam:album"), t->album);
        if (!t->albumArtist.isEmpty())
            m.insert(QStringLiteral("xesam:albumArtist"), QStringList{t->albumArtist});
        if (t->trackNo > 0)
            m.insert(QStringLiteral("xesam:trackNumber"), int(t->trackNo));
        m.insert(QStringLiteral("mpris:length"), qlonglong(t->durationMs) * 1000);
        m.insert(QStringLiteral("xesam:url"), QUrl::fromLocalFile(t->filePath()).toString());
    } else if (m_pc->mode() == PlaybackController::Mode::Radio) {
        m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/ruche/radio"))));
        const QString title = m_pc->streamTitle();
        const int dash = title.indexOf(QLatin1String(" - "));
        if (dash > 0) {
            m.insert(QStringLiteral("xesam:artist"), QStringList{title.left(dash)});
            m.insert(QStringLiteral("xesam:title"), title.mid(dash + 3));
        } else {
            m.insert(QStringLiteral("xesam:title"), title.isEmpty() ? m_pc->stationName() : title);
        }
        m.insert(QStringLiteral("xesam:album"), m_pc->stationName());
        m.insert(QStringLiteral("xesam:url"), m_pc->stationUrl().toString());
    } else {
        return m;
    }
    if (!m_artFile.isEmpty())
        m.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(m_artFile).toString());
    return m;
}

void Mpris::notify(const QVariantMap &changed)
{
    if (!m_registered)
        return;
    QDBusMessage msg = QDBusMessage::createSignal(QStringLiteral("/org/mpris/MediaPlayer2"),
                                                  QStringLiteral("org.freedesktop.DBus.Properties"),
                                                  QStringLiteral("PropertiesChanged"));
    msg << QStringLiteral("org.mpris.MediaPlayer2.Player") << changed << QStringList();
    QDBusConnection::sessionBus().send(msg);
}

// ---------------------------------------------------------------------------

RucheAppAdaptor::RucheAppAdaptor(Mpris *m) : QDBusAbstractAdaptor(m), m_m(m) {}

void RucheAppAdaptor::Open(const QStringList &args)
{
    Q_EMIT m_m->openArgsRequested(args);
    Q_EMIT m_m->raiseRequested();
}

MprisRootAdaptor::MprisRootAdaptor(Mpris *m) : QDBusAbstractAdaptor(m), m_m(m) {}

QStringList MprisRootAdaptor::mimeTypes() const
{
    return {QStringLiteral("audio/mpeg"), QStringLiteral("audio/flac"), QStringLiteral("audio/ogg"),
            QStringLiteral("audio/mp4"), QStringLiteral("audio/x-wav"), QStringLiteral("audio/x-mpegurl"),
            QStringLiteral("application/vnd.apple.mpegurl"), QStringLiteral("audio/x-scpls")};
}

void MprisRootAdaptor::Raise() { Q_EMIT m_m->raiseRequested(); }
void MprisRootAdaptor::Quit() { QCoreApplication::quit(); }

MprisPlayerAdaptor::MprisPlayerAdaptor(Mpris *m) : QDBusAbstractAdaptor(m), m_m(m)
{
    setAutoRelaySignals(false);
}

QString MprisPlayerAdaptor::loopStatus() const
{
    switch (m_m->controller()->repeat()) {
    case PlaybackController::Repeat::All:
        return QStringLiteral("Playlist");
    case PlaybackController::Repeat::One:
        return QStringLiteral("Track");
    default:
        return QStringLiteral("None");
    }
}

void MprisPlayerAdaptor::setLoopStatus(const QString &s)
{
    m_m->controller()->setRepeat(s == QLatin1String("Playlist") ? PlaybackController::Repeat::All
                                 : s == QLatin1String("Track")  ? PlaybackController::Repeat::One
                                                                : PlaybackController::Repeat::Off);
}

bool MprisPlayerAdaptor::shuffle() const { return m_m->controller()->shuffle(); }
void MprisPlayerAdaptor::setShuffle(bool on) { m_m->controller()->setShuffle(on); }
double MprisPlayerAdaptor::volume() const { return m_m->controller()->volume() / 100.0; }
void MprisPlayerAdaptor::setVolume(double v) { m_m->controller()->setVolume(int(v * 100 + 0.5)); }
qlonglong MprisPlayerAdaptor::position() const { return qlonglong(m_m->controller()->position() * 1e6); }
bool MprisPlayerAdaptor::canGoNext() const { return m_m->controller()->mode() == PlaybackController::Mode::Library; }
bool MprisPlayerAdaptor::canSeek() const { return m_m->controller()->isSeekable(); }

void MprisPlayerAdaptor::Next() { m_m->controller()->next(); }
void MprisPlayerAdaptor::Previous() { m_m->controller()->previous(); }
void MprisPlayerAdaptor::Pause() { m_m->controller()->pause(); }
void MprisPlayerAdaptor::PlayPause() { m_m->controller()->togglePause(); }
void MprisPlayerAdaptor::Stop() { m_m->controller()->stop(); }
void MprisPlayerAdaptor::Play() { m_m->controller()->play(); }

void MprisPlayerAdaptor::Seek(qlonglong offsetUs)
{
    auto *pc = m_m->controller();
    if (!pc->isSeekable())
        return;
    const double target = qMax(0.0, pc->position() + offsetUs / 1e6);
    pc->seek(target);
    Q_EMIT Seeked(qlonglong(target * 1e6));
}

void MprisPlayerAdaptor::SetPosition(const QDBusObjectPath &, qlonglong positionUs)
{
    auto *pc = m_m->controller();
    if (!pc->isSeekable())
        return;
    pc->seek(positionUs / 1e6);
    Q_EMIT Seeked(positionUs);
}

void MprisPlayerAdaptor::OpenUri(const QString &uri) { Q_EMIT m_m->openRequested(uri); }

#endif // RUCHE_DBUS
