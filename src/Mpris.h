// MPRIS2 on the session bus: media keys, desktop widgets, playerctl.
#pragma once

#ifndef RUCHE_DBUS
// No D-Bus (Windows): the same signals, raised by the single-instance pipe in main.cpp.
#include <QObject>
#include <QStringList>
class PlaybackController;
class Library;
class Mpris : public QObject {
    Q_OBJECT
public:
    Mpris(PlaybackController *, Library *, QObject *parent = nullptr) : QObject(parent) {}
    bool isRegistered() const { return false; }
    void setArtFile(const QString &) {}
Q_SIGNALS:
    void raiseRequested();
    void openRequested(const QString &uri);
    void openArgsRequested(const QStringList &args);
};
#else
#include <QDBusAbstractAdaptor>
#include <QGuiApplication>
#include <QDBusObjectPath>
#include <QObject>
#include <QVariantMap>

class PlaybackController;
class Library;

class Mpris : public QObject {
    Q_OBJECT
public:
    Mpris(PlaybackController *pc, Library *lib, QObject *parent = nullptr);
    bool isRegistered() const { return m_registered; }
    void setArtFile(const QString &file);

    PlaybackController *controller() const { return m_pc; }
    QVariantMap metadata() const;
    QString playbackStatus() const;

Q_SIGNALS:
    void raiseRequested();
    void openRequested(const QString &uri);
    void openArgsRequested(const QStringList &args); // from a second launch (taskbar, file manager)

private:
    void notify(const QVariantMap &changed);
    PlaybackController *m_pc;
    Library *m_lib;
    QString m_artFile;
    bool m_registered = false;
    QString m_lastStatus;
    friend class MprisPlayerAdaptor;
};

// Private interface used to hand a second launch over to the running window.
class RucheAppAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "fr.ruche.App")
public:
    explicit RucheAppAdaptor(Mpris *m);
public Q_SLOTS:
    void Open(const QStringList &args);

private:
    Mpris *m_m;
};

class MprisRootAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ uriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimeTypes)
public:
    explicit MprisRootAdaptor(Mpris *m);
    bool canQuit() const { return true; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("Ruche"); }
    QString desktopEntry() const { return QGuiApplication::desktopFileName(); }
    QStringList uriSchemes() const { return {QStringLiteral("file"), QStringLiteral("http"), QStringLiteral("https")}; }
    QStringList mimeTypes() const;
public Q_SLOTS:
    void Raise();
    void Quit();

private:
    Mpris *m_m;
};

class MprisPlayerAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate WRITE setRate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ rate)
    Q_PROPERTY(double MaximumRate READ rate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoNext)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ yes)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ yes)
public:
    explicit MprisPlayerAdaptor(Mpris *m);
    QString playbackStatus() const { return m_m->playbackStatus(); }
    QString loopStatus() const;
    void setLoopStatus(const QString &s);
    double rate() const { return 1.0; }
    void setRate(double) {}
    bool shuffle() const;
    void setShuffle(bool on);
    QVariantMap metadata() const { return m_m->metadata(); }
    double volume() const;
    void setVolume(double v);
    qlonglong position() const;
    bool canGoNext() const;
    bool canSeek() const;
    bool yes() const { return true; }
public Q_SLOTS:
    void Next();
    void Previous();
    void Pause();
    void PlayPause();
    void Stop();
    void Play();
    void Seek(qlonglong offsetUs);
    void SetPosition(const QDBusObjectPath &track, qlonglong positionUs);
    void OpenUri(const QString &uri);
Q_SIGNALS:
    void Seeked(qlonglong positionUs);

private:
    Mpris *m_m;
};

#endif // RUCHE_DBUS
