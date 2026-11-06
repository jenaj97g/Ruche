#include "NowPlaying.h"

#include "Library.h"
#include "PlaybackController.h"
#include "Stations.h"

#include <QRegularExpression>

namespace nowplaying {

QString stateName(const PlaybackController *pc)
{
    if (!pc->isActive())
        return QStringLiteral("stopped");
    return pc->isPaused() ? QStringLiteral("paused") : QStringLiteral("playing");
}

bool splitStreamTitle(const QString &streamTitle, QString *artist, QString *title)
{
    const int dash = streamTitle.indexOf(QLatin1String(" - "));
    if (dash <= 0)
        return false;
    *artist = streamTitle.left(dash).trimmed();
    *title = streamTitle.mid(dash + 3).trimmed();
    return !artist->isEmpty() && !title->isEmpty();
}

static QString letters(const QString &s)
{
    QString out;
    for (const QChar c : s.normalized(QString::NormalizationForm_KD))
        if (c.isLetterOrNumber())
            out += c.toCaseFolded();
    return out;
}

bool looksLikeSong(const QString &artist, const QString &title, const QString &station)
{
    if (artist.isEmpty() || title.isEmpty())
        return false;
    static const QRegularExpression web(QStringLiteral("(https?://|www\\.|\\.(com|net|org|fr|be|de|nl|uk|ru|fm|radio|info|eu)\\b)"),
                                        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression frequency(QStringLiteral("\\b\\d{2,3}[.,]\\d\\b"));
    const QString both = artist + QLatin1Char(' ') + title;
    if (web.match(both).hasMatch() || frequency.match(artist).hasMatch())
        return false;
    const QString a = letters(artist), s = letters(station);
    // "ONDE DOUCE 101.3" on Onde Douce: the station naming itself. (Not the other way
    // round: "Queen" on a station called Queen FM is a song.)
    if (s.size() >= 3 && a.contains(s))
        return false;
    return true;
}

QVariantMap snapshot(const PlaybackController *pc, const Library *lib, const Stations *stations)
{
    QVariantMap m;
    m.insert(QStringLiteral("state"), stateName(pc));
    switch (pc->mode()) {
    case PlaybackController::Mode::Library: {
        m.insert(QStringLiteral("source"), QStringLiteral("library"));
        const Track *t = lib->byId(pc->currentTrackId());
        if (!t)
            break;
        auto put = [&m](const char *k, const QString &v) {
            if (!v.isEmpty())
                m.insert(QLatin1String(k), v);
        };
        put("title", t->displayTitle());
        put("artist", t->artist);
        put("album", t->album);
        put("album_artist", t->albumArtist);
        put("genre", t->genre);
        put("path", t->filePath());
        if (t->year > 0)
            m.insert(QStringLiteral("year"), t->year);
        if (t->trackNo > 0)
            m.insert(QStringLiteral("track_number"), int(t->trackNo));
        if (t->discNo > 0)
            m.insert(QStringLiteral("disc_number"), int(t->discNo));
        const double dur = pc->duration() > 0 ? pc->duration() : t->durationMs / 1000.0;
        if (dur > 0)
            m.insert(QStringLiteral("duration"), dur);
        m.insert(QStringLiteral("position"), pc->position());
        m.insert(QStringLiteral("id"), t->id);
        break;
    }
    case PlaybackController::Mode::Radio: {
        m.insert(QStringLiteral("source"), QStringLiteral("radio"));
        const QString url = pc->stationUrl().toString();
        m.insert(QStringLiteral("station"), pc->stationName().isEmpty() ? pc->stationUrl().host() : pc->stationName());
        m.insert(QStringLiteral("station_url"), url);
        const QString st = pc->streamTitle();
        if (!st.isEmpty()) {
            m.insert(QStringLiteral("stream_title"), st);
            QString artist, title;
            if (splitStreamTitle(st, &artist, &title)) {
                m.insert(QStringLiteral("artist"), artist);
                m.insert(QStringLiteral("title"), title);
            } else {
                m.insert(QStringLiteral("title"), st);
            }
            m.insert(QStringLiteral("is_song"), looksLikeSong(artist, title, m.value(QStringLiteral("station")).toString()));
        }
        m.insert(QStringLiteral("position"), pc->position());
        if (stations) {
            const int i = stations->indexOfUrl(url);
            if (i >= 0) {
                const QString lu = stations->list().at(i).logoUrl;
                if (lu.startsWith(QLatin1String("http://")) || lu.startsWith(QLatin1String("https://")))
                    m.insert(QStringLiteral("logo_url"), lu);
            }
            const QString lf = stations->logoPathForUrl(url);
            if (!lf.isEmpty())
                m.insert(QStringLiteral("logo_file"), lf);
        }
        break;
    }
    case PlaybackController::Mode::Idle:
        m.insert(QStringLiteral("source"), QStringLiteral("none"));
        break;
    }
    return m;
}

} // namespace nowplaying
