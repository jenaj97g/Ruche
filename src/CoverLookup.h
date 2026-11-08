// Album and song covers from the web: iTunes Search first, then MusicBrainz / Cover Art
// Archive, by artist and album (or artist and song title for radio). What's found is
// remembered, including "nothing", so each album is looked up once. Used by the
// now-playing panel for radio songs and by Discord presence.
#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

class QNetworkAccessManager;

class CoverLookup : public QObject {
    Q_OBJECT
public:
    explicit CoverLookup(QObject *parent = nullptr);
    // The cover's web address if known; else empty, and a lookup starts (found() follows).
    // An empty album looks the title up as a song.
    QString url(const QString &artist, const QString &album, const QString &title);
    // Downloads (or reads back from disk) the picture at a cover address: imageReady() follows.
    void fetchImage(const QString &url);

Q_SIGNALS:
    void found(const QString &key, const QString &url);
    void imageReady(const QString &url, const QImage &image);

private:
    void lookUp(const QString &key, const QString &artist, const QString &album, const QString &title);
    void lookUpMusicBrainz(const QString &key, const QString &artist, const QString &album);
    void coverFound(const QString &key, const QString &url);
    void loadCache();
    void saveCache();
    void pruneImages();

    QNetworkAccessManager *m_nam;
    QHash<QString, QString> m_covers; // "artist\nalbum" or "artist\nsong:title" -> URL ("" = none found)
    QSet<QString> m_coverPending;
    QSet<QString> m_imagePending;
    bool m_coverCacheLoaded = false;
};
