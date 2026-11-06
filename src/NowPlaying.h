// One snapshot of what's playing, shared by plugins and Discord presence.
#pragma once

#include <QVariantMap>

class PlaybackController;
class Library;
class Stations;

namespace nowplaying {
// Keys (absent when unknown):
//   source       "library" | "radio" | "none"
//   state        "playing" | "paused" | "stopped"
//   title, artist, album, album_artist, genre   strings
//   year, track_number, disc_number             integers
//   duration, position                          seconds (library; position also for radio)
//   path                                        local file (library)
//   station, station_url, stream_title          radio
//   is_song                                     radio: artist and title are a real song
//   logo_url                                    http(s) address of the station logo, if known
//   logo_file, cover_file                       local image files, if any
QVariantMap snapshot(const PlaybackController *pc, const Library *lib, const Stations *stations);
QString stateName(const PlaybackController *pc);
// "Artist - Title" as most stations send it; false when there's no separator.
bool splitStreamTitle(const QString &streamTitle, QString *artist, QString *title);
// Whether a split stream title is a song rather than the station talking about itself
// ("STATION 101.3 - THE BEST MUSIC - www.station.fr"): no web address, no FM frequency,
// and the "artist" isn't the station's own name.
bool looksLikeSong(const QString &artist, const QString &title, const QString &station);
} // namespace nowplaying
