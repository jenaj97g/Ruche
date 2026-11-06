// Local HTTP relay between mpv and an HLS server.
//
// mpv (via FFmpeg) plays HLS fine but drops most of the title information: per-segment
// #EXTINF titles are ignored, and ID3 timed metadata only registers once. Routing the
// stream through this relay lets us read every playlist and segment as mpv fetches it,
// without downloading anything twice, and schedule each title at the content time where
// it occurs. The UI then shows the title that matches what you actually hear.
#pragma once

#include "MetaParse.h"

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QTcpServer>
#include <QUrl>

class QNetworkAccessManager;
class QTcpSocket;

class HlsProxy : public QObject {
    Q_OBJECT
public:
    explicit HlsProxy(QNetworkAccessManager *nam, QObject *parent = nullptr);

    // Starts relaying `upstream` and returns the local URL to hand to mpv.
    QString start(const QUrl &upstream);
    void stop();
    bool isActive() const { return m_active; }

    // Title in effect at `contentTime` seconds into the stream (mpv's playback-time).
    QString titleAt(double contentTime) const;

Q_SIGNALS:
    void titleScheduled(double contentTime, const QString &title, const QString &source);
    void upstreamError(const QString &message);

private Q_SLOTS:
    void onNewConnection();

private:
    struct Resource {
        QUrl url;
        meta::UriKind kind;
        QString name;
    };
    void onReadyRead(QTcpSocket *s);
    void serve(QTcpSocket *s, const QByteArray &path, const QByteArray &range);
    void respond(QTcpSocket *s, int status, const QByteArray &reason, const QByteArray &contentType,
                 const QByteArray &body, const QByteArray &contentRange = {});
    QString registerUri(const QUrl &url, meta::UriKind kind, const meta::PlaylistSegment *seg);
    void handleSegment(int id, qint64 rangeStart, const QByteArray &body);
    double offsetForSeq(qint64 seq) const;
    void addTitle(double t, const QString &title, const QString &source);
    void prune();

    QNetworkAccessManager *m_nam;
    QTcpServer m_server;
    bool m_active = false;
    int m_gen = 0;
    int m_nextId = 1;

    QHash<int, Resource> m_res;
    QHash<QString, int> m_idByUrl;
    QHash<QPair<int, qint64>, qint64> m_seqByRes; // (resource id, byte offset or -1) -> media sequence
    QMap<qint64, double> m_durations;
    QHash<qint64, QString> m_extinf;
    QSet<qint64> m_processed;
    double m_targetDuration = 0;
    qint64 m_firstSeq = -1;
    QMap<double, QString> m_schedule;
    QHash<QTcpSocket *, QByteArray> m_pending;
};
