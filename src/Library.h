// Music library: SQLite on disk, a compact in-memory copy for browsing.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct sqlite3;
class QThread;

struct Track {
    qint64 id = 0; // negative for files opened directly that are not in the library
    QByteArray path; // raw file-system path
    QString title, artist, album, albumArtist, genre; // artist/album/genre strings are shared
    qint32 year = 0;
    qint16 trackNo = 0, discNo = 0;
    qint32 durationMs = 0;
    qint32 bitrate = 0;
    qint32 sampleRate = 0;
    qint8 channels = 0;
    qint64 mtime = 0, size = 0, added = 0;
    qint32 plays = 0;

    QString filePath() const;
    QString displayTitle() const;
    QString displayAlbumArtist() const { return albumArtist.isEmpty() ? artist : albumArtist; }
    QString suffix() const;
};

namespace paths {
QString dataDir();
QString configFile();
QString playlistDir();
}

// File names as the library stores them: the system's 8-bit encoding, which on Windows
// means UTF-8 (QFile::encodeName would use the ANSI code page there and lose characters).
QByteArray encodePath(const QString &path);
QString decodePath(const QByteArray &path);

bool readTrackTags(const QString &file, Track *t);
bool isAudioFile(const QString &name);

class Library : public QObject {
    Q_OBJECT
public:
    explicit Library(QObject *parent = nullptr);
    ~Library() override;

    bool open();
    void reload();
    const QVector<Track> &tracks() const { return m_tracks; }
    int indexOf(qint64 id) const { return m_index.value(id, -1); }
    const Track *byId(qint64 id) const;
    qint64 idForPath(const QString &path) const;
    qint64 addExternal(const QString &path);
    int count() const { return m_libraryCount; }
    qint64 totalDurationMs() const { return m_totalMs; }

    QStringList folders() const;
    void setFolders(const QStringList &folders);
    void rescan();
    bool isScanning() const { return m_scanThread != nullptr; }
    void bumpPlayCount(qint64 id);

    // Playlists live as .m3u8 files so other players can read them too.
    QStringList playlists() const;
    QVector<qint64> playlistTracks(const QString &name);
    bool renamePlaylist(const QString &from, const QString &to);
    bool savePlaylist(const QString &name, const QVector<qint64> &ids);
    bool appendToPlaylist(const QString &name, const QVector<qint64> &ids);
    bool deletePlaylist(const QString &name);
    // Imports a local-file playlist; returns its new name or empty if it held no local files.
    QString importPlaylist(const QString &file);

Q_SIGNALS:
    void scanProgress(int done, int total);
    void scanFinished(int added, int updated, int removed);
    void reloaded();
    void playlistsChanged();

private:
    sqlite3 *m_db = nullptr;
    QVector<Track> m_tracks;
    QHash<qint64, int> m_index;
    QHash<QByteArray, qint64> m_byPath;
    int m_libraryCount = 0;
    qint64 m_totalMs = 0;
    qint64 m_nextExternalId = -1;
    QThread *m_scanThread = nullptr;
};
