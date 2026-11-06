#include "MetaParse.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringList>
#include <cstring>

namespace meta {

// ---------------------------------------------------------------------------
// Text decoding

static const char16_t kCp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static QString decodeCp1252(const QByteArray &raw)
{
    QString out(raw.size(), Qt::Uninitialized);
    QChar *d = out.data();
    for (int i = 0; i < raw.size(); ++i) {
        const uchar c = uchar(raw[i]);
        d[i] = (c >= 0x80 && c < 0xA0) ? QChar(kCp1252[c - 0x80]) : QChar(c);
    }
    return out;
}

static bool validUtf8(const QByteArray &raw, QString *out)
{
    QStringDecoder dec(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString s = dec(raw);
    if (dec.hasError())
        return false;
    if (out)
        *out = s;
    return true;
}

QString decodeLoose(const QByteArray &raw)
{
    QString s;
    if (validUtf8(raw, &s))
        return fixMojibake(s);
    return decodeCp1252(raw);
}

QString fixMojibake(const QString &s)
{
    // Text that was UTF-8, wrongly decoded as Windows-1252/Latin-1 and re-encoded as UTF-8
    // shows up as "CÃ©line". Undo it only when the result is valid UTF-8 with real
    // multi-byte sequences, so legitimate accented text is left alone.
    bool suspicious = false;
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u == 0xC3 || u == 0xC2 || u == 0xE2 || u == 0xC5) {
            suspicious = true;
            break;
        }
    }
    if (!suspicious)
        return s;
    QByteArray bytes;
    bytes.reserve(s.size());
    bool multibyte = false;
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x100) {
            bytes.append(char(u));
            if (u >= 0x80)
                multibyte = true;
            continue;
        }
        int found = -1;
        for (int i = 0; i < 32; ++i)
            if (kCp1252[i] == u) {
                found = i;
                break;
            }
        if (found < 0)
            return s;
        bytes.append(char(0x80 + found));
        multibyte = true;
    }
    QString repaired;
    if (multibyte && validUtf8(bytes, &repaired) && repaired.size() < s.size())
        return repaired;
    return s;
}

QString cleanStreamTitle(QString s)
{
    s = s.trimmed();
    if (s.startsWith(QLatin1String("StreamTitle='"))) {
        s = s.mid(13);
        const int end = s.indexOf(QLatin1String("';"));
        if (end >= 0)
            s = s.left(end);
        else if (s.endsWith(QLatin1Char('\'')))
            s.chop(1);
    }
    s = s.simplified();
    if (s.startsWith(QLatin1String("- ")))
        s = s.mid(2);
    if (s.endsWith(QLatin1String(" -")))
        s.chop(2);
    if (s == QLatin1String("-"))
        s.clear();
    return s;
}

// ---------------------------------------------------------------------------
// ID3v2

static inline quint32 be16(const uchar *p) { return quint32(p[0]) << 8 | p[1]; }
static inline quint32 be24(const uchar *p) { return quint32(p[0]) << 16 | quint32(p[1]) << 8 | p[2]; }
static inline quint32 be32(const uchar *p)
{
    return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
}
static inline quint64 be64(const uchar *p) { return quint64(be32(p)) << 32 | be32(p + 4); }
static inline quint32 ss32(const uchar *p)
{
    return (quint32(p[0] & 0x7f) << 21) | (quint32(p[1] & 0x7f) << 14) | (quint32(p[2] & 0x7f) << 7) |
           (p[3] & 0x7f);
}

static QByteArray deunsync(const QByteArray &in)
{
    QByteArray o;
    o.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        o.append(in[i]);
        if (uchar(in[i]) == 0xFF && i + 1 < in.size() && in[i + 1] == 0)
            ++i;
    }
    return o;
}

static QString decodeUtf16(const QByteArray &b, int enc)
{
    bool le = false;
    int off = 0;
    if (enc == 1) {
        le = true; // no BOM: most writers that omit it are little-endian
        if (b.size() >= 2) {
            if (uchar(b[0]) == 0xFF && uchar(b[1]) == 0xFE) {
                le = true;
                off = 2;
            } else if (uchar(b[0]) == 0xFE && uchar(b[1]) == 0xFF) {
                le = false;
                off = 2;
            }
        }
    }
    const int n = (b.size() - off) / 2;
    QString s(qMax(n, 0), Qt::Uninitialized);
    for (int i = 0; i < n; ++i) {
        const uchar a = uchar(b[off + 2 * i]), c = uchar(b[off + 2 * i + 1]);
        s[i] = QChar(char16_t(le ? (a | c << 8) : (a << 8 | c)));
    }
    return s;
}

static QString decodeId3String(int enc, const QByteArray &b)
{
    switch (enc) {
    case 1:
    case 2:
        return decodeUtf16(b, enc);
    case 3:
        return fixMojibake(QString::fromUtf8(b));
    default:
        // Encoding 0 is supposed to be Latin-1, but plenty of encoders put UTF-8 in it.
        return decodeLoose(b);
    }
}

// Splits on the encoding's terminator (one NUL, or two for UTF-16).
static QStringList splitId3Strings(int enc, const QByteArray &b)
{
    QStringList out;
    if (enc == 1 || enc == 2) {
        int start = 0;
        for (int i = 0; i + 1 < b.size(); i += 2) {
            if (b[i] == 0 && b[i + 1] == 0) {
                out << decodeUtf16(b.mid(start, i - start), enc);
                start = i + 2;
            }
        }
        if (start < b.size())
            out << decodeUtf16(b.mid(start), enc);
    } else {
        for (const QByteArray &part : b.split('\0'))
            out << decodeId3String(enc, part);
    }
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    return out;
}

static bool id3HeaderLooksValid(const QByteArray &d, int pos)
{
    if (pos < 0 || pos + 10 > d.size())
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(d.constData()) + pos;
    if (std::memcmp(p, "ID3", 3) != 0)
        return false;
    if (p[3] < 2 || p[3] > 4 || p[4] == 0xFF)
        return false;
    if (p[6] & 0x80 || p[7] & 0x80 || p[8] & 0x80 || p[9] & 0x80)
        return false;
    if (p[3] == 4 && (p[5] & 0x0F))
        return false;
    return ss32(p + 6) > 0;
}

QString Id3Info::display() const
{
    const QString t = cleanStreamTitle(title), a = cleanStreamTitle(artist), s = cleanStreamTitle(streamTitle);
    if (!a.isEmpty() && !t.isEmpty()) {
        if (t.startsWith(a + QLatin1String(" - ")))
            return t;
        return a + QLatin1String(" - ") + t;
    }
    if (!t.isEmpty())
        return t;
    if (!s.isEmpty())
        return s;
    return a;
}

bool parseId3(const QByteArray &data, int pos, Id3Info *out, int *tagSize)
{
    if (!id3HeaderLooksValid(data, pos))
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData()) + pos;
    const int ver = p[3];
    const int flags = p[5];
    int size = int(ss32(p + 6));
    const int footer = (ver == 4 && (flags & 0x10)) ? 10 : 0;
    if (tagSize)
        *tagSize = qMin(10 + size + footer, int(data.size()) - pos);
    size = qMin(size, int(data.size()) - pos - 10);
    QByteArray body = data.mid(pos + 10, size);
    if (ver < 4 && (flags & 0x80))
        body = deunsync(body);

    const uchar *b = reinterpret_cast<const uchar *>(body.constData());
    int i = 0;
    if (flags & 0x40) {
        if (ver == 3 && body.size() >= 4)
            i += int(be32(b)) + 4;
        else if (ver == 4 && body.size() >= 4)
            i += int(ss32(b));
    }
    const int hdr = ver == 2 ? 6 : 10;
    while (i + hdr <= body.size()) {
        if (b[i] == 0)
            break; // padding
        QByteArray id = body.mid(i, ver == 2 ? 3 : 4);
        quint32 fsz;
        quint32 fflags = 0;
        if (ver == 2)
            fsz = be24(b + i + 3);
        else {
            fsz = ver == 4 ? ss32(b + i + 4) : be32(b + i + 4);
            fflags = be16(b + i + 8);
        }
        if (fsz == 0 || qint64(i) + hdr + fsz > body.size())
            break;
        QByteArray fd = body.mid(i + hdr, int(fsz));
        i += hdr + int(fsz);

        if (ver == 4) {
            if (fflags & 0x000C)
                continue; // compressed or encrypted
            if (fflags & 0x0001)
                fd = fd.mid(4);
            if (fflags & 0x0002)
                fd = deunsync(fd);
        } else if (ver == 3) {
            if (fflags & 0x00C0)
                continue;
            if (fflags & 0x0020)
                fd = fd.mid(1);
        }
        if (ver == 2) {
            if (id == "TT2") id = "TIT2";
            else if (id == "TP1") id = "TPE1";
            else if (id == "TAL") id = "TALB";
            else if (id == "TXX") id = "TXXX";
        }
        if (fd.isEmpty())
            continue;

        if (id == "TXXX") {
            const int enc = uchar(fd[0]);
            const QStringList parts = splitId3Strings(enc, fd.mid(1));
            if (parts.size() >= 2) {
                const QString desc = parts[0].trimmed().toLower();
                const QString value = parts.mid(1).join(QLatin1String(" / "));
                if (desc == QLatin1String("streamtitle") || desc == QLatin1String("stream_title") ||
                    desc == QLatin1String("icy-title") || desc == QLatin1String("now_playing"))
                    out->streamTitle = value;
                else if (desc == QLatin1String("title") && out->title.isEmpty())
                    out->title = value;
                else if (desc == QLatin1String("artist") && out->artist.isEmpty())
                    out->artist = value;
            }
        } else if (id.startsWith('T')) {
            const int enc = uchar(fd[0]);
            const QString text = splitId3Strings(enc, fd.mid(1)).join(QLatin1String(" / "));
            if (id == "TIT2")
                out->title = text;
            else if (id == "TPE1")
                out->artist = text;
            else if (id == "TALB")
                out->album = text;
        } else if (id == "PRIV") {
            const int nul = fd.indexOf('\0');
            if (nul > 0 && fd.left(nul) == "com.apple.streaming.transportStreamTimestamp" && fd.size() - nul - 1 >= 8)
                out->privPts = qint64(be64(reinterpret_cast<const uchar *>(fd.constData()) + nul + 1) & 0x1FFFFFFFFULL);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Segments

SegmentKind sniffSegment(const QByteArray &d)
{
    if (d.size() >= 1 && uchar(d[0]) == 0x47 && (d.size() < 189 || uchar(d[188]) == 0x47))
        return SegmentKind::TransportStream;
    if (d.size() >= 8) {
        const QByteArray t = d.mid(4, 4);
        if (t == "ftyp" || t == "styp" || t == "moof" || t == "sidx" || t == "emsg" || t == "prft" || t == "moov")
            return SegmentKind::Fmp4;
    }
    if (d.startsWith("ID3"))
        return SegmentKind::PackedAudio;
    if (d.size() >= 2) {
        const uchar a = uchar(d[0]), b = uchar(d[1]);
        if (a == 0xFF && (b & 0xE0) == 0xE0)
            return SegmentKind::PackedAudio;
        if (a == 0x0B && b == 0x77)
            return SegmentKind::PackedAudio;
    }
    return SegmentKind::Unknown;
}

static QVector<TimedTitle> scanPacked(const QByteArray &d, double dur)
{
    QVector<TimedTitle> out;
    int pos = 0;
    while (pos < d.size()) {
        const int found = d.indexOf("ID3", pos);
        if (found < 0)
            break;
        Id3Info info;
        int sz = 0;
        if (!parseId3(d, found, &info, &sz)) {
            pos = found + 3;
            continue;
        }
        if (found > 0) {
            // A tag in the middle of audio data must be followed by a frame sync or another tag.
            const int after = found + sz;
            if (after < d.size()) {
                const uchar c = uchar(d[after]);
                if (!(c == 0xFF || c == 0x0B || d.mid(after, 3) == "ID3")) {
                    pos = found + 3;
                    continue;
                }
            }
        }
        const QString t = info.display();
        if (!t.isEmpty())
            out.push_back({d.isEmpty() ? 0.0 : dur * double(found) / double(d.size()), t});
        pos = found + qMax(sz, 3);
    }
    return out;
}

static qint64 pesPts(const uchar *p, int len, int *payloadStart)
{
    // p points at 00 00 01 <stream id> ...
    if (len < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1)
        return -2;
    const int hdrLen = p[8];
    *payloadStart = 9 + hdrLen;
    if ((p[7] & 0x80) && len >= 14) {
        return (qint64(p[9] & 0x0E) << 29) | (qint64(p[10]) << 22) | (qint64(p[11] & 0xFE) << 14) |
               (qint64(p[12]) << 7) | (qint64(p[13]) >> 1);
    }
    return -1;
}

static QVector<TimedTitle> scanTs(const QByteArray &d, double dur)
{
    QVector<TimedTitle> out;
    const uchar *base = reinterpret_cast<const uchar *>(d.constData());
    int start = 0;
    while (start < 188 && start < d.size() && base[start] != 0x47)
        ++start;
    int pmtPid = -1, audioPid = -1;
    QVector<int> metaPids;
    QMap<int, QByteArray> pes;
    qint64 firstAudioPts = -1;

    auto flush = [&](int pid) {
        auto it = pes.find(pid);
        if (it == pes.end() || it->isEmpty())
            return;
        const QByteArray buf = *it;
        it->clear();
        int payload = 0;
        const qint64 pts = pesPts(reinterpret_cast<const uchar *>(buf.constData()), buf.size(), &payload);
        if (pts == -2)
            return;
        if (pid == audioPid) {
            if (firstAudioPts < 0 && pts >= 0)
                firstAudioPts = pts;
            return;
        }
        const QByteArray tag = buf.mid(payload);
        const int at = tag.indexOf("ID3");
        if (at < 0)
            return;
        Id3Info info;
        int sz = 0;
        if (!parseId3(tag, at, &info, &sz))
            return;
        const QString t = info.display();
        if (t.isEmpty())
            return;
        double off = 0;
        if (pts >= 0 && firstAudioPts >= 0)
            off = qBound(0.0, double(pts - firstAudioPts) / 90000.0, qMax(dur, 0.0));
        out.push_back({off, t});
    };

    for (int i = start; i + 188 <= d.size(); i += 188) {
        const uchar *p = base + i;
        if (p[0] != 0x47)
            continue;
        const bool pusi = p[1] & 0x40;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        const int afc = (p[3] >> 4) & 3;
        int off = 4;
        if (afc & 2)
            off += 1 + p[4];
        if (!(afc & 1) || off >= 188)
            continue;
        const uchar *pl = p + off;
        const int plen = 188 - off;

        if (pid == 0 && pusi) {
            const int ptr = pl[0];
            const uchar *s = pl + 1 + ptr;
            if (1 + ptr + 8 > plen)
                continue;
            const int secLen = ((s[1] & 0x0F) << 8) | s[2];
            const int end = qMin(3 + secLen - 4, plen - 1 - ptr);
            for (int k = 8; k + 4 <= end; k += 4) {
                const int prog = int(be16(s + k));
                if (prog != 0) {
                    pmtPid = ((s[k + 2] & 0x1F) << 8) | s[k + 3];
                    break;
                }
            }
            continue;
        }
        if (pid == pmtPid && pusi) {
            const int ptr = pl[0];
            const uchar *s = pl + 1 + ptr;
            if (1 + ptr + 12 > plen)
                continue;
            const int secLen = ((s[1] & 0x0F) << 8) | s[2];
            const int progInfoLen = ((s[10] & 0x0F) << 8) | s[11];
            const int end = qMin(3 + secLen - 4, plen - 1 - ptr);
            int k = 12 + progInfoLen;
            while (k + 5 <= end) {
                const int type = s[k];
                const int epid = ((s[k + 1] & 0x1F) << 8) | s[k + 2];
                const int esLen = ((s[k + 3] & 0x0F) << 8) | s[k + 4];
                bool isMeta = type == 0x15;
                if (type == 0x06) {
                    // private data tagged as ID3 via metadata (0x26) or registration (0x05 'ID3 ') descriptor
                    for (int q = k + 5; q + 2 <= qMin(k + 5 + esLen, end);) {
                        const int tag = s[q], len = s[q + 1];
                        if (tag == 0x26 || (tag == 0x05 && len >= 4 && std::memcmp(s + q + 2, "ID3 ", 4) == 0))
                            isMeta = true;
                        q += 2 + len;
                    }
                }
                if (isMeta) {
                    if (!metaPids.contains(epid))
                        metaPids.push_back(epid);
                } else if (audioPid < 0 && (type == 0x0F || type == 0x03 || type == 0x04 || type == 0x11 ||
                                            type == 0x81 || type == 0x87 || type == 0x06)) {
                    audioPid = epid;
                }
                k += 5 + esLen;
            }
            continue;
        }
        const bool wanted = metaPids.contains(pid) || (pid == audioPid && firstAudioPts < 0);
        if (!wanted)
            continue;
        if (pusi) {
            flush(pid);
            pes[pid] = QByteArray(reinterpret_cast<const char *>(pl), plen);
        } else if (pes.contains(pid) && !pes[pid].isEmpty()) {
            pes[pid].append(reinterpret_cast<const char *>(pl), plen);
        }
        if (pid == audioPid && pusi)
            flush(pid); // the PTS sits in the first packet, no need to wait
    }
    for (int pid : metaPids)
        flush(pid);
    return out;
}

static QVector<TimedTitle> scanFmp4(const QByteArray &d, double dur)
{
    QVector<TimedTitle> out;
    const uchar *b = reinterpret_cast<const uchar *>(d.constData());
    qint64 pos = 0;
    while (pos + 8 <= d.size()) {
        qint64 size = be32(b + pos);
        const QByteArray type = d.mid(int(pos + 4), 4);
        int hdr = 8;
        if (size == 1) {
            if (pos + 16 > d.size())
                break;
            size = qint64(be64(b + pos + 8));
            hdr = 16;
        } else if (size == 0) {
            size = d.size() - pos;
        }
        if (size < hdr || pos + size > d.size())
            break;
        if (type == "emsg" && size > hdr + 4) {
            const QByteArray box = d.mid(int(pos + hdr), int(size - hdr));
            const uchar *e = reinterpret_cast<const uchar *>(box.constData());
            const int version = e[0];
            int q = 4;
            QByteArray scheme;
            double offset = 0;
            auto readCStr = [&](QByteArray *s) {
                const int nul = box.indexOf('\0', q);
                if (nul < 0)
                    return false;
                *s = box.mid(q, nul - q);
                q = nul + 1;
                return true;
            };
            QByteArray value;
            bool ok = true;
            if (version == 0) {
                ok = readCStr(&scheme) && readCStr(&value) && q + 16 <= box.size();
                if (ok) {
                    const quint32 timescale = be32(e + q);
                    const quint32 delta = be32(e + q + 4);
                    if (timescale)
                        offset = double(delta) / timescale;
                    q += 16;
                }
            } else if (version == 1) {
                ok = q + 20 <= box.size();
                if (ok) {
                    q += 20;
                    ok = readCStr(&scheme) && readCStr(&value);
                }
            } else {
                ok = false;
            }
            if (ok && scheme.contains("ID3") ) {
                const QByteArray msg = box.mid(q);
                Id3Info info;
                int sz = 0;
                if (parseId3(msg, 0, &info, &sz)) {
                    const QString t = info.display();
                    if (!t.isEmpty())
                        out.push_back({qBound(0.0, offset, qMax(dur, 0.0)), t});
                }
            }
        }
        pos += size;
    }
    return out;
}

QVector<TimedTitle> extractSegmentTitles(const QByteArray &data, double segDuration, SegmentKind *kindOut)
{
    const SegmentKind kind = sniffSegment(data);
    if (kindOut)
        *kindOut = kind;
    switch (kind) {
    case SegmentKind::TransportStream:
        return scanTs(data, segDuration);
    case SegmentKind::Fmp4:
        return scanFmp4(data, segDuration);
    case SegmentKind::PackedAudio:
        return scanPacked(data, segDuration);
    default:
        return {};
    }
}

// ---------------------------------------------------------------------------
// Playlists

static void parseExtinf(const QString &line, double *dur, QString *title, QMap<QString, QString> *attrs)
{
    const QString rest = line.mid(8); // after "#EXTINF:"
    bool inQuote = false;
    int comma = -1;
    for (int i = 0; i < rest.size(); ++i) {
        if (rest[i] == QLatin1Char('"'))
            inQuote = !inQuote;
        else if (rest[i] == QLatin1Char(',') && !inQuote) {
            comma = i;
            break;
        }
    }
    const QString head = comma < 0 ? rest : rest.left(comma);
    if (title)
        *title = comma < 0 ? QString() : rest.mid(comma + 1).trimmed();
    const int sp = head.indexOf(QLatin1Char(' '));
    if (dur)
        *dur = (sp < 0 ? head : head.left(sp)).trimmed().toDouble();
    if (attrs) {
        static const QRegularExpression re(QStringLiteral("([A-Za-z0-9_-]+)=\"([^\"]*)\""));
        auto it = re.globalMatch(head);
        while (it.hasNext()) {
            const auto m = it.next();
            attrs->insert(m.captured(1).toLower(), m.captured(2));
        }
    }
}

bool looksLikeHls(const QString &text)
{
    return text.contains(QLatin1String("#EXT-X-TARGETDURATION")) || text.contains(QLatin1String("#EXT-X-STREAM-INF")) ||
           text.contains(QLatin1String("#EXT-X-MEDIA-SEQUENCE")) || text.contains(QLatin1String("#EXT-X-PLAYLIST-TYPE")) ||
           text.contains(QLatin1String("#EXT-X-MEDIA:"));
}

static QStringList splitLines(const QString &text)
{
    QString t = text;
    if (t.startsWith(QChar(0xFEFF)))
        t.remove(0, 1);
    t.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t.split(QLatin1Char('\n'));
}

static QUrl resolveUri(const QUrl &base, const QString &ref)
{
    if (ref.contains(QLatin1String("://")))
        return QUrl(ref, QUrl::TolerantMode);
    if (base.isLocalFile()) {
        const QString dir = QFileInfo(base.toLocalFile()).absolutePath();
        return QUrl::fromLocalFile(QDir(dir).absoluteFilePath(ref));
    }
    return base.resolved(QUrl(ref, QUrl::TolerantMode));
}

QString rewritePlaylist(const QString &text, const QUrl &base, const UriMapper &mapUri, PlaylistInfo *info)
{
    PlaylistInfo local;
    PlaylistInfo &pi = info ? *info : local;
    pi = PlaylistInfo();
    pi.isHls = looksLikeHls(text);
    const QStringList lines = splitLines(text);
    for (const QString &l : lines)
        if (l.startsWith(QLatin1String("#EXT-X-STREAM-INF")) || l.startsWith(QLatin1String("#EXT-X-MEDIA:")))
            pi.isMaster = true;

    QStringList out;
    out.reserve(lines.size());
    qint64 seq = 0;
    double pendingDur = 0;
    QString pendingTitle;
    bool pendingDisc = false;
    qint64 pendingOffset = -1, nextOffset = 0;

    static const QRegularExpression uriAttr(QStringLiteral("URI=\"([^\"]*)\""));

    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            out << QString();
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            if (line.startsWith(QLatin1String("#EXT-X-MEDIA-SEQUENCE:"))) {
                seq = line.mid(22).trimmed().toLongLong();
                pi.mediaSequence = seq;
            } else if (line.startsWith(QLatin1String("#EXT-X-TARGETDURATION:"))) {
                pi.targetDuration = line.mid(22).trimmed().toDouble();
            } else if (line.startsWith(QLatin1String("#EXT-X-ENDLIST"))) {
                pi.endList = true;
            } else if (line.startsWith(QLatin1String("#EXTINF:"))) {
                parseExtinf(line, &pendingDur, &pendingTitle, nullptr);
            } else if (line.startsWith(QLatin1String("#EXT-X-DISCONTINUITY")) &&
                       !line.startsWith(QLatin1String("#EXT-X-DISCONTINUITY-SEQUENCE"))) {
                pendingDisc = true;
            } else if (line.startsWith(QLatin1String("#EXT-X-BYTERANGE:"))) {
                const QString v = line.mid(17).trimmed();
                const int at = v.indexOf(QLatin1Char('@'));
                const qint64 len = (at < 0 ? v : v.left(at)).toLongLong();
                pendingOffset = at < 0 ? nextOffset : v.mid(at + 1).toLongLong();
                nextOffset = pendingOffset + len;
            }
            const auto m = uriAttr.match(line);
            if (m.hasMatch()) {
                UriKind kind = UriKind::Other;
                if (line.startsWith(QLatin1String("#EXT-X-KEY")) || line.startsWith(QLatin1String("#EXT-X-SESSION-KEY")))
                    kind = UriKind::Key;
                else if (line.startsWith(QLatin1String("#EXT-X-MAP")))
                    kind = UriKind::Map;
                else if (line.startsWith(QLatin1String("#EXT-X-MEDIA")) ||
                         line.startsWith(QLatin1String("#EXT-X-I-FRAME-STREAM-INF")))
                    kind = UriKind::Playlist;
                const QString mapped = mapUri(resolveUri(base, m.captured(1)), kind, nullptr);
                QString copy = raw.trimmed();
                copy.replace(m.capturedStart(1), m.capturedLength(1), mapped);
                out << copy;
            } else {
                out << raw.trimmed();
            }
            continue;
        }
        // URI line
        const QUrl resolved = resolveUri(base, line);
        if (pi.isMaster) {
            out << mapUri(resolved, UriKind::Playlist, nullptr);
        } else {
            PlaylistSegment seg;
            seg.seq = seq++;
            seg.duration = pendingDur;
            seg.title = pendingTitle;
            seg.discontinuity = pendingDisc;
            seg.byteOffset = pendingOffset;
            if (pendingOffset < 0)
                nextOffset = 0;
            out << mapUri(resolved, UriKind::Segment, &seg);
            pi.segments.push_back(seg);
            pendingDur = 0;
            pendingTitle.clear();
            pendingDisc = false;
            pendingOffset = -1;
        }
    }
    return out.join(QLatin1Char('\n'));
}

QVector<M3uEntry> parseM3u(const QString &text, const QUrl &base)
{
    QVector<M3uEntry> out;
    QString title, logo;
    for (const QString &raw : splitLines(text)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            QMap<QString, QString> attrs;
            parseExtinf(line, nullptr, &title, &attrs);
            logo = attrs.value(QStringLiteral("tvg-logo"));
            if (title.isEmpty())
                title = attrs.value(QStringLiteral("tvg-name"));
            continue;
        }
        if (line.startsWith(QLatin1Char('#')))
            continue;
        M3uEntry e;
        e.title = title;
        e.logo = logo;
        e.url = resolveUri(base, line);
        if (e.url.isValid())
            out.push_back(e);
        title.clear();
        logo.clear();
    }
    return out;
}

QVector<M3uEntry> parsePls(const QString &text, const QUrl &base)
{
    QMap<int, M3uEntry> byIndex;
    static const QRegularExpression re(QStringLiteral("^(File|Title)(\\d+)=(.*)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    for (const QString &raw : splitLines(text)) {
        const auto m = re.match(raw.trimmed());
        if (!m.hasMatch())
            continue;
        const int idx = m.captured(2).toInt();
        if (m.captured(1).compare(QLatin1String("file"), Qt::CaseInsensitive) == 0)
            byIndex[idx].url = resolveUri(base, m.captured(3).trimmed());
        else
            byIndex[idx].title = m.captured(3).trimmed();
    }
    QVector<M3uEntry> out;
    for (const auto &e : std::as_const(byIndex))
        if (e.url.isValid())
            out.push_back(e);
    return out;
}

} // namespace meta
