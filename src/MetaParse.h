// Stream metadata helpers: text decoding, ID3, HLS segment scanning, playlist parsing.
// Pure functions so they can be unit tested without network or audio.
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QVector>
#include <functional>

namespace meta {

// UTF-8 when the bytes are valid UTF-8, Windows-1252 otherwise (what ICY servers
// usually send when they are not sending UTF-8). Also repairs double-encoded UTF-8.
QString decodeLoose(const QByteArray &raw);
QString fixMojibake(const QString &s);
QString cleanStreamTitle(QString s);

struct Id3Info {
    QString title, artist, album, streamTitle;
    qint64 privPts = -1; // com.apple.streaming.transportStreamTimestamp, 90 kHz
    QString display() const;
};
// Parses one ID3v2 tag at data[pos]. tagSize receives header + body + footer.
bool parseId3(const QByteArray &data, int pos, Id3Info *out, int *tagSize);

enum class SegmentKind { Unknown, PackedAudio, TransportStream, Fmp4 };
SegmentKind sniffSegment(const QByteArray &data);

struct TimedTitle {
    double offset; // seconds from the start of the segment
    QString title;
};
QVector<TimedTitle> extractSegmentTitles(const QByteArray &data, double segDuration,
                                         SegmentKind *kindOut = nullptr);

enum class UriKind { Playlist, Segment, Key, Map, Other };

struct PlaylistSegment {
    qint64 seq = 0;
    double duration = 0;
    QString title;       // text after the comma in #EXTINF, if any
    qint64 byteOffset = -1;
    bool discontinuity = false;
};

struct PlaylistInfo {
    bool isHls = false;
    bool isMaster = false;
    bool endList = false;
    qint64 mediaSequence = 0;
    double targetDuration = 0;
    QVector<PlaylistSegment> segments;
};

using UriMapper = std::function<QString(const QUrl &resolved, UriKind kind, const PlaylistSegment *seg)>;
// Rewrites every URI in an HLS playlist through mapUri and collects segment info.
QString rewritePlaylist(const QString &text, const QUrl &base, const UriMapper &mapUri, PlaylistInfo *info);
bool looksLikeHls(const QString &text);

struct M3uEntry {
    QString title;
    QUrl url;
    QString logo;
};
// Plain (non-HLS) M3U/M3U8 lists: station lists or track playlists.
QVector<M3uEntry> parseM3u(const QString &text, const QUrl &base);
// .pls files ([playlist] File1=...)
QVector<M3uEntry> parsePls(const QString &text, const QUrl &base);

} // namespace meta
