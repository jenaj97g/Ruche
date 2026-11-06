// Owns the player, the play queue and radio playback (including the HLS title relay).
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>

class Library;
class Player;
class HlsProxy;
class StreamResolver;
class QNetworkAccessManager;

struct HeardTitle {
    QDateTime when;
    QString title;
};

class PlaybackController : public QObject {
    Q_OBJECT
public:
    enum class Mode { Idle, Library, Radio };
    enum class Repeat { Off, All, One };

    PlaybackController(Library *lib, const QStringList &mpvOptions, QObject *parent = nullptr);
    ~PlaybackController() override;

    // Library playback
    void playTracks(const QVector<qint64> &ids, int start);
    void enqueue(const QVector<qint64> &ids);
    void playNext(const QVector<qint64> &ids);
    void next();
    void previous();
    void playQueueIndex(int i);
    void removeFromQueue(const QVector<int> &indexes);

    // Radio
    void playStation(const QString &name, const QUrl &url);
    QString stationName() const { return m_stationName; }
    QUrl stationUrl() const { return m_stationUrl; }
    QString streamTitle() const { return m_streamTitle; }
    const QVector<HeardTitle> &history() const { return m_history; }
    bool streamIsHls() const;

    void togglePause();
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setVolume(int v);
    int volume() const { return m_volume; }
    void setShuffle(bool on);
    bool shuffle() const { return m_shuffle; }
    void setRepeat(Repeat r);
    Repeat repeat() const { return m_repeat; }
    void setReplayGain(const QString &mode);
    void setTitleLog(const QString &file);

    Mode mode() const { return m_mode; }
    bool isPaused() const;
    bool isActive() const { return m_mode != Mode::Idle; }
    bool isSeekable() const { return m_seekable && m_mode == Mode::Library; }
    double position() const;
    double duration() const;
    QVariantMap audioParams() const;
    qint64 currentTrackId() const;
    const QVector<qint64> &queue() const { return m_queue; }
    int queueIndex() const { return m_index; }
    QVector<int> upcomingIndexes(int max) const;

Q_SIGNALS:
    void trackChanged(qint64 id);
    void stationChanged(const QString &name);
    void streamTitleChanged(const QString &title);
    void historyChanged();
    void stateChanged();
    void positionChanged(double pos, double dur);
    void queueChanged();
    void message(const QString &text);

private:
    void onFileStarted(qint64 entry);
    void onFileEnded(qint64 entry, int reason, const QString &error);
    void appendNext();
    void refreshNext();
    int peekNext() const;
    void rebuildShuffle();
    void startResolved(const QUrl &url, bool hls, const QString &hint);
    void setStreamTitle(const QString &title);
    void pollStream();
    void scheduleReconnect(const QString &why);

    Library *m_lib;
    Player *m_player;
    QNetworkAccessManager *m_nam;
    HlsProxy *m_proxy;
    StreamResolver *m_resolver;
    QTimer m_tick;
    QTimer m_reconnect;

    Mode m_mode = Mode::Idle;
    QVector<qint64> m_queue;
    int m_index = -1;
    QVector<int> m_order; // shuffle order (queue indexes)
    bool m_shuffle = false;
    Repeat m_repeat = Repeat::Off;
    int m_volume = 80;
    bool m_seekable = false;
    qint64 m_currentEntry = -1;
    qint64 m_nextEntry = -1;
    QHash<qint64, int> m_entryIndex;
    int m_consecutiveErrors = 0;

    QString m_stationName;
    QUrl m_stationUrl;
    QString m_streamTitle;
    QString m_mpvStreamTitle;
    QVector<HeardTitle> m_history;
    int m_reconnectAttempts = 0;
    bool m_userStopped = false;
    QFile m_titleLog;
    QElapsedTimer m_stationClock;
};
