// Discord Rich Presence over Discord's local IPC socket: "Listening to Ruche" with the song,
// artist and a progress bar. Works with the Discord apps and with Vesktop (its built-in
// arRPC), native, Flatpak or Snap, and on Windows. Off unless the user turns it on.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>

class QLocalSocket;
class CoverLookup;
class QNetworkAccessManager;
class PlaybackController;
class Library;
class Stations;

class DiscordPresence : public QObject {
    Q_OBJECT
public:
    DiscordPresence(PlaybackController *pc, Library *lib, Stations *stations, CoverLookup *covers, QObject *parent = nullptr);
    ~DiscordPresence() override;

    // The application registered at discord.com/developers; its name is what Discord shows
    // after "Listening to". Built in with -DRUCHE_DISCORD_CLIENT_ID=..., or set in Settings.
    static QString builtInClientId();

    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    void setClientId(const QString &id);
    QString clientId() const { return m_clientId; }
    void setShowRadio(bool on);
    // Pictures. On: album covers for library tracks and song covers for radio (Discord only
    // shows images from a web address, so they're looked up on iTunes Search, then
    // MusicBrainz / Cover Art Archive, and remembered), station logos as the fallback.
    // Off: Ruche's logo for everything.
    void setLookUpCovers(bool on);

    enum class Status { Off, NoClientId, Searching, NotRunning, Connected };
    Status status() const { return m_status; }
    QString statusText() const;
    // Every place a Discord client might have put its socket, in the order they're tried.
    static QStringList socketCandidates();

Q_SIGNALS:
    void statusChanged();

private:
    void setStatus(Status s);
    void connectNext();
    void onConnected();
    void onReadable();
    void onDisconnected();
    void send(quint32 op, const QJsonObject &payload);
    void scheduleUpdate();
    void pushActivity();
    QJsonObject buildActivity();
    void restart();

    PlaybackController *m_pc;
    Library *m_lib;
    Stations *m_stations;
    CoverLookup *m_covers;
    QLocalSocket *m_sock = nullptr;
    QStringList m_candidates;
    int m_candidate = 0;
    QByteArray m_buf;
    bool m_enabled = false;
    bool m_ready = false;
    bool m_showRadio = true;
    bool m_lookUpCovers = true;
    QString m_clientId;
    QString m_user;
    Status m_status = Status::Off;
    QTimer m_retry;    // looks for Discord again while it isn't running
    QTimer m_throttle; // Discord accepts about five updates per 20 s
    QElapsedTimer m_sinceSend;
    QByteArray m_lastSent;
    qint64 m_sentStart = 0; // the progress bar's start, to notice seeks
    qint64 m_titleSince = 0;   // when the current radio title started
    qint64 m_stationSince = 0; // when the current station started
};
