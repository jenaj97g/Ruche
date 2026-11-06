#include "StreamResolver.h"
#include "MetaParse.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

StreamResolver::StreamResolver(QNetworkAccessManager *nam, QObject *parent) : QObject(parent), m_nam(nam) {}

void StreamResolver::cancel()
{
    ++m_token;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
}

void StreamResolver::resolve(const QUrl &url)
{
    cancel();
    step(url, 0);
}

static bool pathLooksLikeList(const QUrl &u, bool *pls)
{
    const QString p = u.path().toLower();
    *pls = p.endsWith(QLatin1String(".pls"));
    return *pls || p.endsWith(QLatin1String(".m3u8")) || p.endsWith(QLatin1String(".m3u"));
}

void StreamResolver::fromPlaylistText(const QString &text, const QUrl &base, bool pls, int depth, const QString &hint)
{
    if (!pls && meta::looksLikeHls(text)) {
        if (base.isLocalFile())
            Q_EMIT resolved(base, false, hint); // local HLS: let mpv read it directly
        else
            Q_EMIT resolved(base, true, hint);
        return;
    }
    const auto entries = pls ? meta::parsePls(text, base) : meta::parseM3u(text, base);
    if (entries.isEmpty()) {
        Q_EMIT failed(tr("The playlist has no playable entries."));
        return;
    }
    const auto &e = entries.first();
    const QString title = e.title.isEmpty() ? hint : e.title;
    if (depth >= 3) {
        Q_EMIT resolved(e.url, false, title);
        return;
    }
    // The entry itself may be HLS or another wrapper; check it too.
    const int token = m_token;
    QTimer::singleShot(0, this, [this, e, depth, token] {
        if (token == m_token)
            step(e.url, depth + 1);
    });
}

void StreamResolver::step(const QUrl &url, int depth)
{
    if (url.isLocalFile()) {
        bool pls = false;
        if (!pathLooksLikeList(url, &pls)) {
            Q_EMIT resolved(url, false, QString());
            return;
        }
        QFile f(url.toLocalFile());
        if (!f.open(QIODevice::ReadOnly)) {
            Q_EMIT failed(tr("Cannot open %1").arg(url.toLocalFile()));
            return;
        }
        fromPlaylistText(meta::decodeLoose(f.read(4 * 1024 * 1024)), url, pls, depth, QString());
        return;
    }
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        Q_EMIT resolved(url, false, QString()); // rtmp, mms, ... let mpv try
        return;
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("Ruche/" RUCHE_VERSION));
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_nam->get(req);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, depth] { decide(reply, depth, false); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, depth] { decide(reply, depth, true); });
}

void StreamResolver::decide(QNetworkReply *reply, int depth, bool finished)
{
    if (reply != m_reply)
        return;
    const QUrl finalUrl = reply->url();
    const QByteArray ctype = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().toLower();
    const QByteArray peek = reply->peek(64);
    bool pls = false;
    const bool listByName = pathLooksLikeList(finalUrl, &pls);
    const bool listByType = ctype.contains("mpegurl") || ctype.contains("scpls") || ctype.contains("x-mpegurl");
    const bool listByContent = peek.startsWith("#EXTM3U") || peek.startsWith("[playlist]") ||
                               peek.startsWith("\xEF\xBB\xBF#EXTM3U") || peek.startsWith("http");
    if (!finished && peek.size() < 16 && (listByName || listByType))
        return; // need a few bytes to be sure

    if (listByType || listByContent || (listByName && !ctype.startsWith("audio/"))) {
        if (!finished)
            return; // playlists are small: wait for the whole thing
        m_reply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT failed(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        const QString hint = QString::fromUtf8(reply->rawHeader("icy-name"));
        fromPlaylistText(meta::decodeLoose(body), finalUrl, pls || peek.startsWith("[playlist]") || ctype.contains("scpls"),
                         depth, hint);
        return;
    }
    // A real audio stream (Icecast/Shoutcast/progressive file): mpv takes it from here.
    // Errors land here too: old Shoutcast servers answer "ICY 200 OK", which Qt rejects but
    // FFmpeg understands, so mpv gets the final say.
    m_reply = nullptr;
    const QString hint = meta::decodeLoose(reply->rawHeader("icy-name")).trimmed();
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    Q_EMIT resolved(finalUrl, false, hint);
}
