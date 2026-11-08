// Thin libmpv wrapper configured for audio only and a small memory footprint.
#pragma once

#include <QObject>
#include <QVector>
#include <QStringList>
#include <QVariantMap>
#include <atomic>

struct mpv_handle;

class Player : public QObject {
    Q_OBJECT
public:
    explicit Player(const QStringList &extraOptions = {}, QObject *parent = nullptr);
    ~Player() override;

    // Returns mpv's playlist entry id of the loaded item (or -1).
    qint64 load(const QString &urlOrPath, bool append = false);
    void clearOthers(); // drop every playlist entry except the current one
    void stop();
    void setPaused(bool paused);
    void togglePause();
    void seek(double seconds);
    void setVolume(int percent);
    void setVolumeF(double percent); // for fades
    void setReplayGain(const QString &mode);
    // 10 bands (31 Hz … 16 kHz), in dB; empty turns the equalizer off.
    void setEqualizer(const QVector<double> &gains);
    void setStartAt(double seconds); // where the next loaded file starts (< 0: the beginning)
    void playlistNext();             // move on to the appended entry now

    bool isPaused() const { return m_paused; }
    bool isIdle() const { return m_idle; }
    double duration() const { return m_duration; }
    double position() const;     // time-pos
    double playbackTime() const; // playback-time (content clock, used for HLS title sync)
    QVariantMap audioParams() const;

Q_SIGNALS:
    void fileStarted(qint64 entryId);
    void fileLoaded();
    void fileEnded(qint64 entryId, int reason, const QString &error); // reason: 0 eof, 2 stop, 4 error, 5 redirect
    void playbackRestarted();
    void pausedChanged(bool paused);
    void idleChanged(bool idle);
    void durationChanged(double seconds);
    void streamMetadataChanged(const QString &title, const QString &stationName);
    void seekableChanged(bool seekable);

private Q_SLOTS:
    void drainEvents();

private:
    void setOpt(const char *name, const char *value);
    void handleMetadata();

    mpv_handle *m_mpv = nullptr;
    std::atomic_bool m_wakeupPending{false};
    bool m_paused = false;
    bool m_idle = true;
    bool m_seekable = false;
    double m_duration = 0;
    QString m_lastStreamTitle;
    QString m_lastStationName;
};
