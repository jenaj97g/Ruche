// Unit tests for the stream metadata parsers. Builds synthetic ID3 / TS / fMP4 / playlists.
#include "../src/MetaParse.h"

#include <QCoreApplication>
#include <QDebug>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            ++g_fail;                                                                                \
        }                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        const auto _a = (a);                                                                         \
        const auto _b = (b);                                                                         \
        if (!(_a == _b)) {                                                                           \
            qWarning().noquote() << "FAIL" << __FILE__ << __LINE__ << #a << "=" << _a << "expected" << _b; \
            ++g_fail;                                                                                \
        }                                                                                            \
    } while (0)

static QByteArray ss(quint32 v)
{
    QByteArray b(4, 0);
    b[0] = char((v >> 21) & 0x7f);
    b[1] = char((v >> 14) & 0x7f);
    b[2] = char((v >> 7) & 0x7f);
    b[3] = char(v & 0x7f);
    return b;
}
static QByteArray be32(quint32 v)
{
    QByteArray b(4, 0);
    b[0] = char(v >> 24);
    b[1] = char(v >> 16);
    b[2] = char(v >> 8);
    b[3] = char(v);
    return b;
}
static QByteArray frame24(const char *id, const QByteArray &data)
{
    return QByteArray(id, 4) + ss(data.size()) + QByteArray(2, 0) + data;
}
static QByteArray frame23(const char *id, const QByteArray &data)
{
    return QByteArray(id, 4) + be32(data.size()) + QByteArray(2, 0) + data;
}
static QByteArray tag(int ver, const QByteArray &frames)
{
    QByteArray h("ID3");
    h.append(char(ver));
    h.append(char(0));
    h.append(char(0));
    return h + ss(frames.size()) + frames;
}
static QByteArray privTs(quint64 pts)
{
    QByteArray d("com.apple.streaming.transportStreamTimestamp");
    d.append('\0');
    for (int i = 7; i >= 0; --i)
        d.append(char(pts >> (8 * i)));
    return frame24("PRIV", d);
}
static QByteArray adtsFrames(int n)
{
    QByteArray f;
    for (int i = 0; i < n; ++i) {
        QByteArray fr(100, char(0x11));
        fr[0] = char(0xFF);
        fr[1] = char(0xF1);
        f += fr;
    }
    return f;
}

// --- MPEG-TS builder -------------------------------------------------------
static quint32 crc32mpeg(const QByteArray &d)
{
    quint32 crc = 0xFFFFFFFF;
    for (uchar c : d) {
        crc ^= quint32(c) << 24;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}
static QByteArray tsPacket(int pid, bool pusi, const QByteArray &payload, int cc)
{
    QByteArray p;
    p.append(char(0x47));
    p.append(char((pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F)));
    p.append(char(pid & 0xFF));
    const int room = 184;
    if (payload.size() >= room) {
        p.append(char(0x10 | (cc & 0xF)));
        p += payload.left(room);
    } else {
        // adaptation field stuffing
        p.append(char(0x30 | (cc & 0xF)));
        const int afLen = room - payload.size() - 1;
        p.append(char(afLen));
        if (afLen > 0) {
            p.append(char(0x00));
            p += QByteArray(afLen - 1, char(0xFF));
        }
        p += payload;
    }
    return p;
}
static QByteArray section(const QByteArray &body)
{
    QByteArray s = body;
    s += be32(crc32mpeg(body));
    return QByteArray(1, 0) + s; // pointer field
}
static QByteArray pesWithPts(int streamId, quint64 pts, const QByteArray &payload)
{
    QByteArray h;
    h.append(char(0));
    h.append(char(0));
    h.append(char(1));
    h.append(char(streamId));
    const int len = payload.size() + 8;
    h.append(char(len >> 8));
    h.append(char(len & 0xFF));
    h.append(char(0x84));
    h.append(char(0x80));
    h.append(char(5));
    h.append(char(0x21 | ((pts >> 29) & 0x0E)));
    h.append(char(pts >> 22));
    h.append(char(0x01 | ((pts >> 14) & 0xFE)));
    h.append(char(pts >> 7));
    h.append(char(0x01 | ((pts << 1) & 0xFE)));
    return h + payload;
}
static QByteArray packetize(int pid, const QByteArray &pes, int *cc)
{
    QByteArray out;
    for (int off = 0; off < pes.size(); off += 184)
        out += tsPacket(pid, off == 0, pes.mid(off, 184), (*cc)++);
    return out;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace meta;

    // Text decoding
    CHECK_EQ(decodeLoose(QByteArray("C\xe9line Dion")), QString::fromUtf8("Céline Dion"));
    CHECK_EQ(decodeLoose(QByteArray("C\xc3\xa9line")), QString::fromUtf8("Céline"));
    CHECK_EQ(decodeLoose(QByteArray("It\x92s")), QString::fromUtf8("It’s"));
    CHECK_EQ(fixMojibake(QString::fromUtf8("CÃ©line - Ã‡a va")), QString::fromUtf8("Céline - Ça va"));
    CHECK_EQ(fixMojibake(QString::fromUtf8("Beyoncé")), QString::fromUtf8("Beyoncé"));
    CHECK_EQ(fixMojibake(QString::fromUtf8("Don’t")), QString::fromUtf8("Don’t"));
    CHECK_EQ(fixMojibake(QString::fromUtf8("Donâ€™t")), QString::fromUtf8("Don’t"));
    CHECK_EQ(cleanStreamTitle(QStringLiteral("StreamTitle='A - B';")), QStringLiteral("A - B"));
    CHECK_EQ(cleanStreamTitle(QStringLiteral(" - ")), QString());

    // ID3 v2.4 UTF-8 + PRIV
    {
        const QByteArray t = tag(4, privTs(123456) + frame24("TIT2", QByteArray("\x03") + "Pour que tu m'aimes encore") +
                                        frame24("TPE1", QByteArray("\x03") + "C\xc3\xa9line Dion"));
        Id3Info info;
        int sz = 0;
        CHECK(parseId3(t, 0, &info, &sz));
        CHECK_EQ(sz, int(t.size()));
        CHECK_EQ(info.privPts, qint64(123456));
        CHECK_EQ(info.display(), QString::fromUtf8("Céline Dion - Pour que tu m'aimes encore"));
    }
    // ID3 v2.3 UTF-16 with BOM, TXXX StreamTitle
    {
        QByteArray u16("\x01\xFF\xFE", 3);
        for (QChar c : QStringLiteral("Daft Punk - One More Time")) {
            u16.append(char(c.unicode() & 0xFF));
            u16.append(char(c.unicode() >> 8));
        }
        QByteArray txxx("\x00StreamTitle\x00", 13);
        txxx += "Mylene Farmer - Desenchantee";
        const QByteArray t = tag(3, frame23("TIT2", u16) + frame23("TXXX", txxx));
        Id3Info info;
        CHECK(parseId3(t, 0, &info, nullptr));
        CHECK_EQ(info.title, QStringLiteral("Daft Punk - One More Time"));
        CHECK_EQ(info.streamTitle, QStringLiteral("Mylene Farmer - Desenchantee"));
    }
    // Packed audio: leading PRIV-only tag, then a mid-stream title change
    {
        QByteArray seg = tag(4, privTs(0)) + adtsFrames(50) +
                         tag(4, frame24("TIT2", QByteArray("\x03") + "Second Song")) + adtsFrames(50);
        SegmentKind k;
        const auto titles = extractSegmentTitles(seg, 10.0, &k);
        CHECK(k == SegmentKind::PackedAudio);
        CHECK_EQ(titles.size(), qsizetype(1));
        if (titles.size() == 1) {
            CHECK_EQ(titles[0].title, QStringLiteral("Second Song"));
            CHECK(titles[0].offset > 4.0 && titles[0].offset < 6.0);
        }
        // Leading tag with text -> offset 0
        QByteArray seg2 = tag(4, privTs(0) + frame24("TIT2", QByteArray("\x03") + "First")) + adtsFrames(10);
        const auto t2 = extractSegmentTitles(seg2, 6.0);
        CHECK_EQ(t2.size(), qsizetype(1));
        if (!t2.isEmpty())
            CHECK_EQ(t2[0].offset, 0.0);
    }
    // MPEG-TS with timed ID3 on a 0x15 PID
    {
        int cc0 = 0, cc1 = 0, ccA = 0, ccM = 0;
        QByteArray pat;
        pat.append(char(0x00));
        pat.append(char(0xB0));
        pat.append(char(13));
        pat += QByteArray("\x00\x01\xC1\x00\x00", 5);
        pat += QByteArray("\x00\x01\xF0\x00", 4); // program 1 -> PMT PID 0x1000
        QByteArray pmt;
        pmt.append(char(0x02));
        QByteArray pmtBody;
        pmtBody += QByteArray("\x00\x01\xC1\x00\x00", 5);
        pmtBody += QByteArray("\xE1\x01\xF0\x00", 4); // PCR PID 0x101, no program info
        pmtBody += QByteArray("\x0F\xE1\x01\xF0\x00", 5); // ADTS AAC on 0x101
        pmtBody += QByteArray("\x15\xE1\x02\xF0\x00", 5); // ID3 metadata on 0x102
        const int secLen = pmtBody.size() + 4;
        pmt.append(char(0xB0 | ((secLen >> 8) & 0x0F)));
        pmt.append(char(secLen & 0xFF));
        pmt += pmtBody;
        QByteArray ts;
        ts += tsPacket(0, true, section(pat), cc0++);
        ts += tsPacket(0x1000, true, section(pmt), cc1++);
        const quint64 base = 900000;
        ts += packetize(0x101, pesWithPts(0xC0, base, adtsFrames(3)), &ccA);
        ts += packetize(0x101, pesWithPts(0xC0, base + 90000, adtsFrames(3)), &ccA);
        const QByteArray id3 = tag(4, frame24("TIT2", QByteArray("\x03") + "TS Title") +
                                         frame24("TPE1", QByteArray("\x03") + "TS Artist"));
        ts += packetize(0x102, pesWithPts(0xBD, base + 225000, id3), &ccM);
        ts += packetize(0x101, pesWithPts(0xC0, base + 270000, adtsFrames(3)), &ccA);
        SegmentKind k;
        const auto titles = extractSegmentTitles(ts, 6.0, &k);
        CHECK(k == SegmentKind::TransportStream);
        CHECK_EQ(titles.size(), qsizetype(1));
        if (titles.size() == 1) {
            CHECK_EQ(titles[0].title, QStringLiteral("TS Artist - TS Title"));
            CHECK(qAbs(titles[0].offset - 2.5) < 0.001);
        }
    }
    // fMP4 emsg v0 carrying ID3
    {
        const QByteArray id3 = tag(4, frame24("TIT2", QByteArray("\x03") + "Emsg Song"));
        QByteArray body;
        body += QByteArray(4, 0); // version 0, flags
        body += "https://aomedia.org/emsg/ID3";
        body.append('\0');
        body.append('\0'); // value
        body += be32(1000) + be32(1500) + be32(0) + be32(1);
        body += id3;
        QByteArray emsg = be32(8 + body.size()) + "emsg" + body;
        QByteArray styp = be32(16) + "styp" + "msdh" + be32(0);
        QByteArray moof = be32(8) + "moof";
        const auto titles = extractSegmentTitles(styp + emsg + moof, 4.0);
        CHECK_EQ(titles.size(), qsizetype(1));
        if (titles.size() == 1) {
            CHECK_EQ(titles[0].title, QStringLiteral("Emsg Song"));
            CHECK(qAbs(titles[0].offset - 1.5) < 0.001);
        }
    }
    // Playlist rewriting
    {
        const QString media = QStringLiteral(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:100\n"
            "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
            "#EXTINF:6.000,Artist A - Song A\nseg100.aac\n"
            "#EXTINF:5.5,\nhttps://cdn.example.com/x/seg101.aac?tok=1\n");
        int n = 0;
        PlaylistInfo pi;
        const QString out = rewritePlaylist(media, QUrl(QStringLiteral("http://radio.example/live/stream.m3u8")),
                                            [&](const QUrl &u, UriKind kind, const PlaylistSegment *seg) {
                                                ++n;
                                                if (kind == UriKind::Key)
                                                    CHECK_EQ(u.toString(), QStringLiteral("http://radio.example/live/key.bin"));
                                                if (seg && seg->seq == 101)
                                                    CHECK_EQ(u.toString(), QStringLiteral("https://cdn.example.com/x/seg101.aac?tok=1"));
                                                return QStringLiteral("P%1").arg(n);
                                            },
                                            &pi);
        CHECK(pi.isHls);
        CHECK(!pi.isMaster);
        CHECK_EQ(pi.mediaSequence, qint64(100));
        CHECK_EQ(pi.targetDuration, 6.0);
        CHECK_EQ(pi.segments.size(), qsizetype(2));
        if (pi.segments.size() == 2) {
            CHECK_EQ(pi.segments[0].title, QStringLiteral("Artist A - Song A"));
            CHECK_EQ(pi.segments[1].seq, qint64(101));
            CHECK_EQ(pi.segments[1].duration, 5.5);
            CHECK(pi.segments[1].title.isEmpty());
        }
        CHECK(out.contains(QStringLiteral("URI=\"P1\"")));
        CHECK(out.contains(QStringLiteral("\nP2\n")));
        CHECK(out.endsWith(QStringLiteral("P3\n")) || out.endsWith(QStringLiteral("P3")) || out.contains(QStringLiteral("P3")));

        const QString master = QStringLiteral(
            "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nhi/index.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=64000\nlo/index.m3u8\n");
        PlaylistInfo mi;
        QVector<UriKind> kinds;
        rewritePlaylist(master, QUrl(QStringLiteral("http://radio.example/live/master.m3u8")),
                        [&](const QUrl &, UriKind k, const PlaylistSegment *) {
                            kinds << k;
                            return QStringLiteral("x");
                        },
                        &mi);
        CHECK(mi.isMaster);
        CHECK_EQ(kinds.size(), qsizetype(2));
        CHECK(kinds.value(0) == UriKind::Playlist);
    }
    // Plain M3U station list
    {
        const QString list = QStringLiteral(
            "#EXTM3U\n#EXTINF:-1 tvg-logo=\"http://x/l.png\" group-title=\"Pop, Dance\",NRJ\nhttp://cdn.nrjaudio.fm/audio1/fr/30001/mp3_128.mp3\n"
            "#EXTINF:-1,Noël FM\nhttps://noelfm.fr/live.m3u8\n");
        const auto e = parseM3u(list, QUrl());
        CHECK_EQ(e.size(), qsizetype(2));
        if (e.size() == 2) {
            CHECK_EQ(e[0].title, QStringLiteral("NRJ"));
            CHECK_EQ(e[0].logo, QStringLiteral("http://x/l.png"));
            CHECK_EQ(e[1].title, QString::fromUtf8("Noël FM"));
            CHECK_EQ(e[1].url.toString(), QStringLiteral("https://noelfm.fr/live.m3u8"));
        }
        CHECK(!looksLikeHls(list));
        const auto pls = parsePls(QStringLiteral("[playlist]\nFile1=http://a/b\nTitle1=Radio B\nNumberOfEntries=1\n"), QUrl());
        CHECK_EQ(pls.size(), qsizetype(1));
    }

    if (g_fail) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all metadata parser tests passed\n");
    return 0;
}
