#include "HlsProxy.h"

#include <QDebug>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpSocket>

static bool debugEnabled() { return !qEnvironmentVariableIsEmpty("RUCHE_DEBUG"); }

HlsProxy::HlsProxy(QNetworkAccessManager *nam, QObject *parent) : QObject(parent), m_nam(nam)
{
    connect(&m_server, &QTcpServer::newConnection, this, &HlsProxy::onNewConnection);
}

QString HlsProxy::start(const QUrl &upstream)
{
    stop();
    if (!m_server.isListening() && !m_server.listen(QHostAddress::LocalHost, 0)) {
        Q_EMIT upstreamError(tr("Could not open the local stream relay: %1").arg(m_server.errorString()));
        return upstream.toString();
    }
    m_active = true;
    return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()) +
           registerUri(upstream, meta::UriKind::Playlist, nullptr);
}

void HlsProxy::stop()
{
    ++m_gen;
    m_active = false;
    m_res.clear();
    m_idByUrl.clear();
    m_seqByRes.clear();
    m_durations.clear();
    m_extinf.clear();
    m_processed.clear();
    m_schedule.clear();
    m_targetDuration = 0;
    m_firstSeq = -1;
}

QString HlsProxy::titleAt(double t) const
{
    if (m_schedule.isEmpty())
        return {};
    auto it = m_schedule.upperBound(t + 0.05);
    if (it == m_schedule.begin())
        return {};
    --it;
    return it.value();
}

QString HlsProxy::registerUri(const QUrl &url, meta::UriKind kind, const meta::PlaylistSegment *seg)
{
    const QString key = url.toString();
    int id = m_idByUrl.value(key, 0);
    if (!id) {
        id = m_nextId++;
        QString name = QFileInfo(url.path()).fileName();
        name.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")));
        if (name.isEmpty())
            name = kind == meta::UriKind::Playlist ? QStringLiteral("index.m3u8") : QStringLiteral("data");
        m_res.insert(id, {url, kind, name});
        m_idByUrl.insert(key, id);
    }
    if (seg) {
        m_seqByRes.insert(qMakePair(id, seg->byteOffset), seg->seq);
        if (seg->duration > 0)
            m_durations.insert(seg->seq, seg->duration);
        const QString t = meta::cleanStreamTitle(meta::fixMojibake(seg->title));
        if (!t.isEmpty())
            m_extinf.insert(seg->seq, t);
    }
    return QStringLiteral("/%1/%2/%3").arg(m_gen).arg(id).arg(m_res.value(id).name);
}

void HlsProxy::onNewConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        m_pending.insert(s, QByteArray());
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_pending.remove(s);
            s->deleteLater();
        });
    }
}

void HlsProxy::onReadyRead(QTcpSocket *s)
{
    auto it = m_pending.find(s);
    if (it == m_pending.end())
        return;
    it->append(s->readAll());
    const int end = it->indexOf("\r\n\r\n");
    if (end < 0) {
        if (it->size() > 16384)
            s->abort();
        return;
    }
    const QByteArray head = it->left(end);
    m_pending.erase(it);
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> req = lines.value(0).trimmed().split(' ');
    QByteArray range;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray l = lines[i].trimmed();
        if (l.toLower().startsWith("range:"))
            range = l.mid(6).trimmed();
    }
    if (req.size() < 2 || (req[0] != "GET" && req[0] != "HEAD")) {
        respond(s, 405, "Method Not Allowed", "text/plain", "");
        return;
    }
    serve(s, req[1], range);
}

void HlsProxy::serve(QTcpSocket *s, const QByteArray &path, const QByteArray &range)
{
    const QList<QByteArray> parts = path.split('/');
    // "/<gen>/<id>/<name>"
    if (parts.size() < 4 || parts[1].toInt() != m_gen) {
        respond(s, 404, "Not Found", "text/plain", "stale");
        return;
    }
    const int id = parts[2].toInt();
    const auto res = m_res.constFind(id);
    if (res == m_res.constEnd()) {
        respond(s, 404, "Not Found", "text/plain", "unknown");
        return;
    }
    const Resource r = *res;
    QNetworkRequest req(r.url);
    req.setTransferTimeout(20000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("Ruche/" RUCHE_VERSION));
    if (r.kind == meta::UriKind::Playlist)
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    qint64 rangeStart = -1;
    if (!range.isEmpty() && range != "bytes=0-") {
        req.setRawHeader("Range", range);
        const QRegularExpression re(QStringLiteral("bytes=(\\d+)-"));
        const auto m = re.match(QString::fromLatin1(range));
        if (m.hasMatch())
            rangeStart = m.captured(1).toLongLong();
    }
    if (debugEnabled())
        qInfo() << "[hls] GET" << r.url.toString() << range;

    QNetworkReply *reply = m_nam->get(req);
    QPointer<QTcpSocket> sock(s);
    connect(s, &QTcpSocket::disconnected, reply, [reply] {
        if (reply->isRunning())
            reply->abort();
    });
    const int gen = m_gen;
    connect(reply, &QNetworkReply::finished, this, [this, reply, sock, r, id, gen, rangeStart] {
        reply->deleteLater();
        if (!sock)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status < 400) {
            if (gen == m_gen && r.kind == meta::UriKind::Playlist)
                Q_EMIT upstreamError(reply->errorString());
            respond(sock, 502, "Bad Gateway", "text/plain", reply->errorString().toUtf8());
            return;
        }
        if (status >= 400) {
            respond(sock, status, reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray(),
                    "text/plain", reply->readAll());
            return;
        }
        QByteArray body = reply->readAll();
        QByteArray ctype = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (gen != m_gen) {
            respond(sock, 404, "Not Found", "text/plain", "stale");
            return;
        }
        if (r.kind == meta::UriKind::Playlist) {
            meta::PlaylistInfo info;
            const QString text = QString::fromUtf8(body);
            const QString rewritten = meta::rewritePlaylist(
                text, reply->url(),
                [this](const QUrl &u, meta::UriKind k, const meta::PlaylistSegment *seg) {
                    return registerUri(u, k, seg);
                },
                &info);
            if (info.targetDuration > 0)
                m_targetDuration = info.targetDuration;
            prune();
            respond(sock, 200, "OK", "application/vnd.apple.mpegurl", rewritten.toUtf8());
            return;
        }
        if (r.kind == meta::UriKind::Segment)
            handleSegment(id, rangeStart, body);
        if (ctype.isEmpty())
            ctype = "application/octet-stream";
        respond(sock, status == 206 ? 206 : 200, status == 206 ? "Partial Content" : "OK", ctype, body,
                reply->rawHeader("Content-Range"));
    });
}

void HlsProxy::respond(QTcpSocket *s, int status, const QByteArray &reason, const QByteArray &contentType,
                       const QByteArray &body, const QByteArray &contentRange)
{
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + (reason.isEmpty() ? QByteArray("Status") : reason) +
                   "\r\nContent-Type: " + contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                   "\r\nCache-Control: no-cache\r\nConnection: close\r\n";
    if (!contentRange.isEmpty())
        h += "Content-Range: " + contentRange + "\r\n";
    h += "\r\n";
    s->write(h);
    s->write(body);
    s->disconnectFromHost();
}

double HlsProxy::offsetForSeq(qint64 seq) const
{
    if (m_firstSeq < 0)
        return 0;
    const double fallback = m_targetDuration > 0 ? m_targetDuration : 6.0;
    double t = 0;
    if (seq >= m_firstSeq) {
        for (qint64 s = m_firstSeq; s < seq; ++s)
            t += m_durations.value(s, fallback);
    } else {
        for (qint64 s = seq; s < m_firstSeq; ++s)
            t -= m_durations.value(s, fallback);
    }
    return t;
}

void HlsProxy::handleSegment(int id, qint64 rangeStart, const QByteArray &body)
{
    auto it = m_seqByRes.constFind(qMakePair(id, rangeStart));
    if (it == m_seqByRes.constEnd())
        it = m_seqByRes.constFind(qMakePair(id, qint64(-1)));
    if (it == m_seqByRes.constEnd())
        return;
    const qint64 seq = it.value();
    if (m_processed.contains(seq))
        return;
    m_processed.insert(seq);
    if (m_firstSeq < 0)
        m_firstSeq = seq;
    const double start = offsetForSeq(seq);
    const double dur = m_durations.value(seq, m_targetDuration);

    const QString extinf = m_extinf.value(seq);
    if (!extinf.isEmpty())
        addTitle(start, extinf, QStringLiteral("extinf"));

    meta::SegmentKind kind;
    const auto titles = meta::extractSegmentTitles(body, dur, &kind);
    for (const auto &t : titles)
        addTitle(start + t.offset, t.title, QStringLiteral("id3"));
    if (debugEnabled())
        qInfo() << "[hls] segment" << seq << "at" << start << "kind" << int(kind) << "titles" << titles.size();
}

void HlsProxy::addTitle(double t, const QString &title, const QString &source)
{
    // Repeated identical titles (EXTINF on every segment) don't need their own entries.
    auto next = m_schedule.upperBound(t);
    if (next != m_schedule.begin()) {
        auto prev = std::prev(next);
        if (prev.value() == title && (next == m_schedule.end() || next.value() == title))
            return;
    }
    m_schedule.insert(t, title);
    Q_EMIT titleScheduled(t, title, source);
}

void HlsProxy::prune()
{
    // Live playlists keep adding segments; drop bookkeeping for ones long gone.
    if (m_res.size() > 600) {
        QList<int> ids = m_res.keys();
        std::sort(ids.begin(), ids.end());
        for (int i = 1; i < ids.size() - 300; ++i) { // keep id 1 (the entry playlist)
            const int id = ids[i];
            if (m_res.value(id).kind == meta::UriKind::Playlist)
                continue;
            m_idByUrl.remove(m_res.value(id).url.toString());
            m_res.remove(id);
        }
        for (auto it = m_seqByRes.begin(); it != m_seqByRes.end();) {
            if (!m_res.contains(it.key().first))
                it = m_seqByRes.erase(it);
            else
                ++it;
        }
    }
    while (m_schedule.size() > 2000)
        m_schedule.erase(m_schedule.begin());
    if (m_processed.size() > 4000)
        m_processed.clear(); // seqs only grow; old ones never come back
}
