// Works out what a station URL actually is before playing it: an HLS stream (goes through
// the title relay), a playlist wrapper (.m3u/.pls pointing at an Icecast mount) or a direct
// stream mpv can open itself.
#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

class StreamResolver : public QObject {
    Q_OBJECT
public:
    explicit StreamResolver(QNetworkAccessManager *nam, QObject *parent = nullptr);
    void resolve(const QUrl &url);
    void cancel();

Q_SIGNALS:
    void resolved(const QUrl &playUrl, bool isHls, const QString &titleHint);
    void failed(const QString &message);

private:
    void step(const QUrl &url, int depth);
    void decide(QNetworkReply *reply, int depth, bool finished);
    void fromPlaylistText(const QString &text, const QUrl &base, bool pls, int depth, const QString &hint);

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    int m_token = 0;
};
