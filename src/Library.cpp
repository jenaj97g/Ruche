#include "Library.h"
#include "MetaParse.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <sqlite3.h>

#include "TagPath.h"
#include <fileref.h>
#include <tag.h>
#include <tpropertymap.h>

// ---------------------------------------------------------------------------
// Track helpers

QByteArray encodePath(const QString &path)
{
#ifdef Q_OS_WIN
    return path.toUtf8();
#else
    return QFile::encodeName(path);
#endif
}

QString decodePath(const QByteArray &path)
{
#ifdef Q_OS_WIN
    return QString::fromUtf8(path);
#else
    return QFile::decodeName(path);
#endif
}

QString Track::filePath() const { return decodePath(path); }

QString Track::displayTitle() const
{
    if (!title.isEmpty())
        return title;
    return QFileInfo(filePath()).completeBaseName();
}

QString Track::suffix() const { return QFileInfo(filePath()).suffix().toUpper(); }

namespace paths {
QString dataDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1String("/ruche");
    QDir().mkpath(d);
    return d;
}
QString configFile()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1String("/ruche");
    QDir().mkpath(d);
    return d + QLatin1String("/ruche.ini");
}
QString playlistDir()
{
    const QString d = dataDir() + QLatin1String("/playlists");
    QDir().mkpath(d);
    return d;
}
} // namespace paths

bool isAudioFile(const QString &name)
{
    static const QSet<QString> exts = {
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("m4b"), QStringLiteral("mp4"),
        QStringLiteral("aac"), QStringLiteral("wav"), QStringLiteral("wv"), QStringLiteral("ape"),
        QStringLiteral("aif"), QStringLiteral("aiff"), QStringLiteral("aifc"), QStringLiteral("wma"),
        QStringLiteral("asf"), QStringLiteral("mpc"), QStringLiteral("tta"), QStringLiteral("spx"),
        QStringLiteral("alac"), QStringLiteral("mka")};
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    return dot >= 0 && exts.contains(name.mid(dot + 1).toLower());
}

static QString tq(const TagLib::String &s) { return QString::fromUtf8(s.toCString(true)).trimmed(); }

bool readTrackTags(const QString &file, Track *t)
{
    t->path = encodePath(file);
    TagLib::FileRef f(TagPath(file), true, TagLib::AudioProperties::Fast);
    if (f.isNull())
        return false;
    if (TagLib::Tag *tag = f.tag()) {
        t->title = tq(tag->title());
        t->artist = tq(tag->artist());
        t->album = tq(tag->album());
        t->genre = tq(tag->genre());
        t->year = int(tag->year());
        t->trackNo = qint16(tag->track());
        const TagLib::PropertyMap pm = f.file()->properties();
        auto first = [&pm](const char *key) {
            const auto it = pm.find(key);
            return (it == pm.end() || it->second.isEmpty()) ? QString() : tq(it->second.front());
        };
        t->albumArtist = first("ALBUMARTIST");
        if (t->albumArtist.isEmpty())
            t->albumArtist = first("ALBUM ARTIST");
        t->discNo = qint16(first("DISCNUMBER").section(QLatin1Char('/'), 0, 0).toInt());
    }
    if (TagLib::AudioProperties *ap = f.audioProperties()) {
        t->durationMs = ap->lengthInMilliseconds();
        t->bitrate = ap->bitrate();
        t->sampleRate = ap->sampleRate();
        t->channels = qint8(ap->channels());
    }
    const QFileInfo fi(file);
    t->mtime = fi.lastModified().toSecsSinceEpoch();
    t->size = fi.size();
    return true;
}

// ---------------------------------------------------------------------------
// SQLite helpers

static bool sqlExec(sqlite3 *db, const char *sql)
{
    char *err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        qWarning("sqlite: %s (%s)", err ? err : "?", sql);
        sqlite3_free(err);
        return false;
    }
    return true;
}

static QString colText(sqlite3_stmt *st, int i)
{
    const auto *p = reinterpret_cast<const char *>(sqlite3_column_text(st, i));
    return p ? QString::fromUtf8(p, sqlite3_column_bytes(st, i)) : QString();
}

static void bindText(sqlite3_stmt *st, int i, const QString &s)
{
    const QByteArray u = s.toUtf8();
    sqlite3_bind_text(st, i, u.constData(), u.size(), SQLITE_TRANSIENT);
}

static const char *kSchema =
    "CREATE TABLE IF NOT EXISTS tracks("
    " id INTEGER PRIMARY KEY, path BLOB UNIQUE NOT NULL, mtime INTEGER, size INTEGER,"
    " title TEXT, artist TEXT, album TEXT, albumartist TEXT, genre TEXT, year INTEGER, track INTEGER,"
    " disc INTEGER, duration_ms INTEGER, bitrate INTEGER, samplerate INTEGER, channels INTEGER,"
    " added INTEGER, plays INTEGER DEFAULT 0, lastplayed INTEGER);"
    "CREATE TABLE IF NOT EXISTS folders(path TEXT PRIMARY KEY);";

static sqlite3 *openDb()
{
    sqlite3 *db = nullptr;
    // sqlite3_open takes UTF-8 on every platform.
    const QByteArray file = (paths::dataDir() + QLatin1String("/library.db")).toUtf8();
    if (sqlite3_open(file.constData(), &db) != SQLITE_OK) {
        qWarning("cannot open library database: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlExec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-512;");
    sqlExec(db, kSchema);
    return db;
}

// ---------------------------------------------------------------------------
// Scanner thread

namespace {
class ScanThread : public QThread {
    Q_OBJECT
public:
    explicit ScanThread(const QStringList &folders) : m_folders(folders) {}
Q_SIGNALS:
    void progress(int done, int total);
    void done(int added, int updated, int removed);

protected:
    void run() override
    {
        sqlite3 *db = openDb();
        if (!db) {
            Q_EMIT done(0, 0, 0);
            return;
        }
        QHash<QByteArray, QPair<qint64, qint64>> existing;
        {
            sqlite3_stmt *st = nullptr;
            sqlite3_prepare_v2(db, "SELECT path, mtime, size FROM tracks", -1, &st, nullptr);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const QByteArray p(static_cast<const char *>(sqlite3_column_blob(st, 0)), sqlite3_column_bytes(st, 0));
                existing.insert(p, {sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2)});
            }
            sqlite3_finalize(st);
        }

        struct Candidate {
            QString file;
            qint64 mtime, size;
        };
        QVector<Candidate> todo;
        QSet<QByteArray> seen;
        for (const QString &folder : m_folders) {
            QDirIterator it(folder, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                const QFileInfo fi = it.fileInfo();
                if (!isAudioFile(fi.fileName()))
                    continue;
                const QByteArray enc = encodePath(fi.absoluteFilePath());
                if (seen.contains(enc))
                    continue;
                seen.insert(enc);
                const qint64 mt = fi.lastModified().toSecsSinceEpoch();
                const auto ex = existing.constFind(enc);
                if (ex != existing.constEnd() && ex->first == mt && ex->second == fi.size())
                    continue;
                todo.push_back({fi.absoluteFilePath(), mt, fi.size()});
            }
        }

        sqlite3_stmt *up = nullptr;
        sqlite3_prepare_v2(db,
                           "INSERT INTO tracks(path,mtime,size,title,artist,album,albumartist,genre,year,track,disc,"
                           "duration_ms,bitrate,samplerate,channels,added) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                           "ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime,size=excluded.size,"
                           "title=excluded.title,artist=excluded.artist,album=excluded.album,"
                           "albumartist=excluded.albumartist,genre=excluded.genre,year=excluded.year,"
                           "track=excluded.track,disc=excluded.disc,duration_ms=excluded.duration_ms,"
                           "bitrate=excluded.bitrate,samplerate=excluded.samplerate,channels=excluded.channels",
                           -1, &up, nullptr);
        int added = 0, updated = 0, removed = 0;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        sqlExec(db, "BEGIN");
        for (int i = 0; i < todo.size(); ++i) {
            if (isInterruptionRequested())
                break;
            Track t;
            if (readTrackTags(todo[i].file, &t)) {
                sqlite3_reset(up);
                sqlite3_bind_blob(up, 1, t.path.constData(), t.path.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int64(up, 2, todo[i].mtime);
                sqlite3_bind_int64(up, 3, todo[i].size);
                bindText(up, 4, t.title);
                bindText(up, 5, t.artist);
                bindText(up, 6, t.album);
                bindText(up, 7, t.albumArtist);
                bindText(up, 8, t.genre);
                sqlite3_bind_int(up, 9, t.year);
                sqlite3_bind_int(up, 10, t.trackNo);
                sqlite3_bind_int(up, 11, t.discNo);
                sqlite3_bind_int(up, 12, t.durationMs);
                sqlite3_bind_int(up, 13, t.bitrate);
                sqlite3_bind_int(up, 14, t.sampleRate);
                sqlite3_bind_int(up, 15, t.channels);
                sqlite3_bind_int64(up, 16, now);
                if (sqlite3_step(up) == SQLITE_DONE)
                    (existing.contains(t.path) ? updated : added)++;
            }
            if (i % 200 == 199) {
                sqlExec(db, "COMMIT");
                sqlExec(db, "BEGIN");
                Q_EMIT progress(i + 1, todo.size());
            }
        }
        sqlite3_finalize(up);

        if (!isInterruptionRequested()) {
            sqlite3_stmt *del = nullptr;
            sqlite3_prepare_v2(db, "DELETE FROM tracks WHERE path=?", -1, &del, nullptr);
            for (auto it = existing.constBegin(); it != existing.constEnd(); ++it) {
                if (seen.contains(it.key()))
                    continue;
                sqlite3_reset(del);
                sqlite3_bind_blob(del, 1, it.key().constData(), it.key().size(), SQLITE_TRANSIENT);
                if (sqlite3_step(del) == SQLITE_DONE)
                    ++removed;
            }
            sqlite3_finalize(del);
        }
        sqlExec(db, "COMMIT");
        sqlExec(db, "PRAGMA wal_checkpoint(TRUNCATE)");
        sqlite3_close(db);
        Q_EMIT done(added, updated, removed);
    }

private:
    QStringList m_folders;
};
} // namespace

// ---------------------------------------------------------------------------
// Library

Library::Library(QObject *parent) : QObject(parent) {}

Library::~Library()
{
    if (m_scanThread) {
        m_scanThread->requestInterruption();
        m_scanThread->wait();
    }
    if (m_db)
        sqlite3_close(m_db);
}

bool Library::open()
{
    m_db = openDb();
    if (m_db)
        reload();
    return m_db != nullptr;
}

void Library::reload()
{
    if (!m_db)
        return;
    // Keep external tracks (opened from the command line) across reloads.
    QVector<Track> externals;
    for (const Track &t : std::as_const(m_tracks))
        if (t.id < 0)
            externals.push_back(t);

    QVector<Track> tracks;
    QHash<QString, QString> pool; // share repeated artist/album/genre strings
    auto intern = [&pool](QString s) {
        auto it = pool.constFind(s);
        if (it != pool.constEnd())
            return *it;
        pool.insert(s, s);
        return s;
    };
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db,
                       "SELECT id,path,mtime,size,title,artist,album,albumartist,genre,year,track,disc,duration_ms,"
                       "bitrate,samplerate,channels,added,plays FROM tracks",
                       -1, &st, nullptr);
    qint64 total = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Track t;
        t.id = sqlite3_column_int64(st, 0);
        t.path = QByteArray(static_cast<const char *>(sqlite3_column_blob(st, 1)), sqlite3_column_bytes(st, 1));
        t.mtime = sqlite3_column_int64(st, 2);
        t.size = sqlite3_column_int64(st, 3);
        t.title = colText(st, 4);
        t.artist = intern(colText(st, 5));
        t.album = intern(colText(st, 6));
        t.albumArtist = intern(colText(st, 7));
        t.genre = intern(colText(st, 8));
        t.year = sqlite3_column_int(st, 9);
        t.trackNo = qint16(sqlite3_column_int(st, 10));
        t.discNo = qint16(sqlite3_column_int(st, 11));
        t.durationMs = sqlite3_column_int(st, 12);
        t.bitrate = sqlite3_column_int(st, 13);
        t.sampleRate = sqlite3_column_int(st, 14);
        t.channels = qint8(sqlite3_column_int(st, 15));
        t.added = sqlite3_column_int64(st, 16);
        t.plays = sqlite3_column_int(st, 17);
        total += t.durationMs;
        tracks.push_back(std::move(t));
    }
    sqlite3_finalize(st);
    m_libraryCount = tracks.size();
    m_totalMs = total;
    tracks += externals;
    tracks.squeeze();
    m_tracks = std::move(tracks);
    m_index.clear();
    m_byPath.clear();
    m_index.reserve(m_tracks.size());
    m_byPath.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i) {
        m_index.insert(m_tracks[i].id, i);
        m_byPath.insert(m_tracks[i].path, m_tracks[i].id);
    }
    Q_EMIT reloaded();
}

const Track *Library::byId(qint64 id) const
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &m_tracks[i];
}

qint64 Library::idForPath(const QString &path) const
{
    return m_byPath.value(encodePath(QFileInfo(path).absoluteFilePath()), 0);
}

qint64 Library::addExternal(const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (const qint64 id = idForPath(abs))
        return id;
    Track t;
    readTrackTags(abs, &t);
    t.path = encodePath(abs);
    t.id = m_nextExternalId--;
    m_tracks.push_back(t);
    m_index.insert(t.id, m_tracks.size() - 1);
    m_byPath.insert(t.path, t.id);
    return t.id;
}

QStringList Library::folders() const
{
    QStringList out;
    if (!m_db)
        return out;
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "SELECT path FROM folders ORDER BY path", -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW)
        out << colText(st, 0);
    sqlite3_finalize(st);
    return out;
}

void Library::setFolders(const QStringList &folders)
{
    if (!m_db)
        return;
    sqlExec(m_db, "BEGIN; DELETE FROM folders;");
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "INSERT OR IGNORE INTO folders(path) VALUES(?)", -1, &st, nullptr);
    for (const QString &f : folders) {
        sqlite3_reset(st);
        bindText(st, 1, QDir::cleanPath(f));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlExec(m_db, "COMMIT");
}

void Library::rescan()
{
    if (m_scanThread)
        return;
    auto *t = new ScanThread(folders());
    m_scanThread = t;
    connect(t, &ScanThread::progress, this, &Library::scanProgress);
    connect(t, &ScanThread::done, this, [this, t](int a, int u, int r) {
        t->wait();
        t->deleteLater();
        m_scanThread = nullptr;
        reload();
        Q_EMIT scanFinished(a, u, r);
    });
    t->start(QThread::LowPriority);
}

void Library::bumpPlayCount(qint64 id)
{
    const int i = indexOf(id);
    if (i < 0 || id < 0 || !m_db)
        return;
    m_tracks[i].plays++;
    sqlite3_stmt *st = nullptr;
    sqlite3_prepare_v2(m_db, "UPDATE tracks SET plays=plays+1, lastplayed=? WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, QDateTime::currentSecsSinceEpoch());
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ---------------------------------------------------------------------------
// Playlists

static QString playlistFile(const QString &name) { return paths::playlistDir() + QLatin1Char('/') + name + QLatin1String(".m3u8"); }

QStringList Library::playlists() const
{
    QStringList out;
    const auto files = QDir(paths::playlistDir()).entryInfoList({QStringLiteral("*.m3u8"), QStringLiteral("*.m3u")},
                                                                QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : files)
        out << fi.completeBaseName();
    return out;
}

QVector<qint64> Library::playlistTracks(const QString &name)
{
    QVector<qint64> ids;
    QFile f(playlistFile(name));
    if (!f.open(QIODevice::ReadOnly))
        return ids;
    const auto entries = meta::parseM3u(meta::decodeLoose(f.readAll()), QUrl::fromLocalFile(f.fileName()));
    for (const auto &e : entries) {
        if (!e.url.isLocalFile())
            continue;
        const QString p = e.url.toLocalFile();
        qint64 id = idForPath(p);
        if (!id && QFileInfo::exists(p))
            id = addExternal(p);
        if (id)
            ids.push_back(id);
    }
    return ids;
}

bool Library::savePlaylist(const QString &name, const QVector<qint64> &ids)
{
    QFile f(playlistFile(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QByteArray out = "#EXTM3U\n";
    for (qint64 id : ids) {
        const Track *t = byId(id);
        if (!t)
            continue;
        out += "#EXTINF:" + QByteArray::number(t->durationMs / 1000) + ',' +
               (t->artist.isEmpty() ? QString() : t->artist + QLatin1String(" - ")).toUtf8() + t->displayTitle().toUtf8() +
               '\n' + t->filePath().toUtf8() + '\n';
    }
    f.write(out);
    Q_EMIT playlistsChanged();
    return true;
}

bool Library::appendToPlaylist(const QString &name, const QVector<qint64> &ids)
{
    QVector<qint64> all = playlistTracks(name);
    all += ids;
    return savePlaylist(name, all);
}

bool Library::deletePlaylist(const QString &name)
{
    const bool ok = QFile::remove(playlistFile(name));
    Q_EMIT playlistsChanged();
    return ok;
}

QString Library::importPlaylist(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const auto entries = meta::parseM3u(meta::decodeLoose(f.readAll()), QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()));
    QVector<qint64> ids;
    for (const auto &e : entries) {
        if (!e.url.isLocalFile() || !QFileInfo::exists(e.url.toLocalFile()))
            continue;
        qint64 id = idForPath(e.url.toLocalFile());
        if (!id)
            id = addExternal(e.url.toLocalFile());
        ids.push_back(id);
    }
    if (ids.isEmpty())
        return {};
    QString base = QFileInfo(file).completeBaseName();
    QString name = base;
    for (int n = 2; QFileInfo::exists(playlistFile(name)); ++n)
        name = QStringLiteral("%1 (%2)").arg(base).arg(n);
    savePlaylist(name, ids);
    return name;
}

#include "Library.moc"
