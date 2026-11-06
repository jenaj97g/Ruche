// Vector icons painted with QPainter: crisp at any scale, no icon theme or SVG module needed.
#pragma once

#include <QColor>
#include <QIcon>

namespace Icons {
enum Name {
    Play, Pause, Next, Prev, Stop, Shuffle, Repeat, RepeatOne, Volume, Mute, Search, Music, Radio,
    Queue, Playlist, Add, Remove, Import, Menu, Settings, Folder, Edit, Logo, Close
};
void setColors(const QColor &normal, const QColor &active, const QColor &disabled, const QColor &accent);
QIcon icon(Name n);
QIcon accentIcon(Name n); // always drawn in the accent colour (play button, logo)
void paint(QPainter *p, const QRectF &r, Name n, const QColor &color);
} // namespace Icons
